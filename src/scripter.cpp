#include <sstream>
#include <regex>
#include <charconv>
#include <typeindex>
#include "scripter.hpp"
#include "Model.hpp"
#include "Mixer.hpp"
#include "Splitter.hpp"

static unique_ptr<Model> M;
static Flowsheet*        FS;

static IpoptApplication* solver = IpoptApplicationFactory();

void check(lua_State* L, const bool cond, string_view err_msg) {
    if (!cond)
        luaL_error(L, err_msg.data());
}

string trim_all(const string_view s) {
    string res {};
    for (const auto& c : s) {
        if (c == ' ' || c == '\t' || c == '\n')
            continue;
        res += c;
    }
    return res;
}

string trim_sides(const string_view s) {
    const auto len = s.size();
    auto beg = len;
    for (size_t i = 0; i < len; i++) {
        if (const auto c = s[i]; c != ' ' && c != '\t' && c != '\n') {
            beg = i;
            break;
        }
    }
    auto end = len - 1;
    for (auto i = end; i >= beg; i--) {
        if (const auto c = s[i]; c != ' ' && c != '\t' && c != '\n') {
            end = i;
            break;
        }
    }
    string res {};
    for (auto i = beg; i <= end; i++)
        res += s[i];
    return res;
}

bool is_number(const string_view s, double& value) {
    const char *cb = s.data();
    const char *ce = cb + s.size();
    auto [p, e] = std::from_chars(cb, ce, value);
    return e == std::errc() && p == ce;
}

auto get_line_no(lua_State* L) {
    lua_Debug act_rec;
    int line_no {-1};
    if (lua_getstack(L, 1, &act_rec)) {
        lua_getinfo(L, "l", &act_rec);
        line_no = act_rec.currentline;
    }
    return line_no;
}

auto get_script_name(lua_State* L) {
    lua_Debug act_rec;
    string script_name {};
    if (lua_getstack(L, 1, &act_rec)) {
        lua_getinfo(L, "S", &act_rec);
        script_name = act_rec.short_src;
    }
    return script_name;
}

string get_string_elem(lua_State* L, int index, const string& err_msg) {
    check(L, lua_rawgeti(L, -1, index) == LUA_TSTRING, err_msg);
    string elem = lua_tostring(L, -1);
    lua_pop(L, 1);
    return elem;
}

double get_double_elem(lua_State* L, int index, const string& err_msg) {
    check(L, lua_rawgeti(L, -1, index) == LUA_TNUMBER, err_msg);
    double elem = lua_tonumber(L, -1);
    lua_pop(L, 1);
    return elem;
}

struct TypedPtr {
    void* ptr;
    std::type_index type_idx;
    std::type_index subtype_idx;
};

template <typename T>
T* get_pointer(lua_State* L) {
    auto tp = (TypedPtr*)lua_touserdata(L, -1);
    T* ptr {};
    if (tp->type_idx == typeid(T))
        ptr = static_cast<T*>(tp->ptr);
    lua_pop(L, 1);
    return ptr;
}

template <typename T>
T* get_pointer_elem(lua_State* L, int index, const string& err_msg) {
    check(L, lua_rawgeti(L, -1, index) == LUA_TUSERDATA, err_msg);
    return get_pointer<T>(L);
}

template <typename T, typename sub_T = void>
void push_pointer(lua_State* L, T* p) {
    auto tp = (TypedPtr*)lua_newuserdatauv(L, sizeof(TypedPtr), 0);
    tp->ptr = p;
    tp->type_idx = typeid(T);
    tp->subtype_idx = typeid(sub_T);
}

int solve_model(lua_State* L) {
    int retval {-1};
    if (M) retval = solver->OptimizeTNLP(M.get());
    lua_pushinteger(L, retval);
    return 1;
}

template <typename Delegate_Func_T>
int delegate(lua_State* L, Delegate_Func_T f) {
    if (!M) return 0;
    auto n_args = lua_gettop(L);
    if (n_args == 0) {
        f(M.get());
        return 0;
    }
    for (int i = 1; i <= n_args; i++) {
        if (lua_isuserdata(L, i)) {
            auto tp = (TypedPtr*)lua_touserdata(L, -1);
            if (!tp) continue;
            if (tp->type_idx == typeid(Model)) {
                auto p = static_cast<Model*>(tp->ptr);
                if (p) f(p);
            }
            else if (tp->type_idx == typeid(Block)) {
                if (tp->subtype_idx == typeid(Mixer)) {
                    auto p = static_cast<Mixer*>(tp->ptr);
                    if (p) f(p);
                } else if (tp->subtype_idx == typeid(Splitter)) {
                    auto p = static_cast<Splitter*>(tp->ptr);
                    if (p) f(p);
                }
                // TODO: add Splitter, Separator, etc.
            }
        }
    }
    return 0;
}

int eval_constraints(lua_State* L) {
    return delegate(L, [](auto* p) { p->eval_constraints(); });
}

int eval_jacobian(lua_State* L) {
    return delegate(L, [](auto* p) { p->eval_jacobian(); });
}

int eval_hessian(lua_State* L) {
    return delegate(L, [](auto* p) { p->eval_hessian(); });
}

int initialize_solver(lua_State* L) {
    int retval {-1};
    if (solver) retval = solver->Initialize();
    lua_pushinteger(L, retval);
    return 1;
}

int set_solver_option(lua_State* L) {
    const string msg {"SolverOption: "};
    auto n_args = lua_gettop(L);
    check(L, n_args == 2, format("{}expected 2 arguments, got {}.", msg, n_args));
    check(L, lua_isstring(L, 1), msg + "expected argument 1 to be a string.");
    string option = lua_tostring(L, 1);
    if (lua_isinteger(L, 2)) {
        if (solver) solver->Options()->SetIntegerValue(option, lua_tointeger(L, 2));
    }
    else if (lua_isnumber(L, 2)) {
        if (solver) solver->Options()->SetNumericValue(option, lua_tonumber(L, 2));
    }
    else if (lua_isstring(L, 2)) {
        if (solver) solver->Options()->SetStringValue(option, lua_tostring(L, 2));
    }
    return 0;
}

template <typename Get_Attr_T>
int get_variable_attr(lua_State* L, Get_Attr_T get_attr) {
    std::pair<Ndouble, string> res {std::nullopt, {}};
    if (!M) return 0;
    if (lua_gettop(L) == 0) return 0;
    if (lua_isuserdata(L, -1)) {
        auto tp = (TypedPtr*)lua_touserdata(L, -1);
        if (!tp) return 0;
        if (tp->type_idx == typeid(Variable)) {
            auto p = static_cast<Variable*>(tp->ptr);
            res = {get_attr(p), p->unit->str};
        }
    }
    else if (lua_isstring(L, -1)) {
        string name = lua_tostring(L, -1);
        if (M->x_map.contains(name))
            res = {get_attr(M->x_map[name]), M->x_map[name]->unit->str};
    }

    if (std::get<0>(res).has_value()) {
        lua_pushnumber(L, std::get<0>(res).value());
        lua_pushstring(L, std::get<1>(res).c_str());
        return 2;
    }
    else
        return 0;
}

int get_value(lua_State* L) {
    return get_variable_attr(L, [](auto* p){ return p->value; });
}

int get_lower(lua_State* L) {
    return get_variable_attr(L, [](auto* p){ return p->lower; });
}

int get_upper(lua_State* L) {
    return get_variable_attr(L, [](auto* p){ return p->upper; });
}

int get_spec(lua_State* L) {
    if (!M) return 0;
    if (lua_gettop(L) == 0) return 0;
    if (lua_isstring(L, -1)) {
        string name = lua_tostring(L, -1);
        if (M->x_map.contains(name)) {
            lua_pushstring(L, M->x_map[name]->spec == VariableSpec::Fixed ? "Fixed" : "Free");
            return 1;
        }
    }
    return 0;
}

int get_var(lua_State* L) {
    if (!M) return 0;
    if (lua_gettop(L) == 0) return 0;
    if (lua_isstring(L, -1)) {
        string name = lua_tostring(L, -1);
        if (M->x_map.contains(name)) {
            push_pointer<Variable>(L, M->x_map[name]);
            return 1;
        }
    }
    return 0;
}

int change_unit(lua_State* L) {
    if (!M) return 0;
    if (lua_gettop(L) < 2) return 0;
    if (lua_isstring(L, -2)) {
        string name = lua_tostring(L, -2);
        string unit_str = lua_tostring(L, -1);
        if (M->x_map.contains(name)) {
            Ndouble retval = M->x_map[name]->change_unit(M.get(), unit_str);
            if (retval.has_value()) {
                lua_pushnumber(L, retval.value());
                return 1;
            }
            else
                return 0;
        }
        else
            return 0;
    }
    else if (lua_isuserdata(L, -2)) {
        auto tp = (TypedPtr*)lua_touserdata(L, -2);
        if (!tp) return 0;
        string unit_str = lua_tostring(L, -1);
        if (tp->type_idx == typeid(Variable)) {
            auto p = static_cast<Variable*>(tp->ptr);
            Ndouble retval = p->change_unit(M.get(), unit_str);
            if (retval.has_value()) {
                lua_pushnumber(L, retval.value());
                return 1;
            }
            else
                return 0;
        }
        else
            return 0;
    }
    else
        return 0;
}

int show_constraints(lua_State* L) {
    return delegate(L, [](auto* p) { p->show_constraints(); });
}

int show_variables(lua_State* L) {
    return delegate(L, [](auto* p) { p->show_variables(); });
}

int show_jacobian(lua_State* L) {
    return delegate(L, [](auto* p) { p->show_jacobian(); });
}

int show_hessian(lua_State* L) {
    return delegate(L, [](auto* p) { p->show_hessian(); });
}

int initialize(lua_State* L) {
    return delegate(L, [](auto* p) { p->initialize(); });
}

const std::regex re_binop(R"((\S+)(=|<|>)([^\s_]+)(?:_(\S+))?)");
const std::regex re_spec(R"((fix|free)\s+(\S+))");

int eval_expr(lua_State* L) {
    if (!M) return 0;
    auto line_no = get_line_no(L);
    string msg {get_script_name(L) + ", "};
    auto n_args = lua_gettop(L);
    check(L, n_args == 1, format("{}expected 1 argument, got {}.", msg, n_args));
    check(L, lua_isstring(L, 1), msg + "expected the argument to be a string.");

    std::istringstream expr_stream {lua_tostring(L, 1)};

    bool ok = true;
    string expr_raw;
    while(std::getline(expr_stream, expr_raw)) {
        line_no++;
        string expr_ta = trim_all(expr_raw);
        string expr_ts = trim_sides(expr_raw);
        if (expr_ta.empty()) continue;
        std::smatch m;
        if (std::regex_match(expr_ta, m, re_binop)) {
            string lhs = m[1].str(),
                   op  = m[2].str(),
                   rhs = m[3].str();

            auto lhs_var = M->var(lhs);
            if (lhs_var == nullptr) {
                ok = false;
                cerr << format("{}line {}: in expression \"{}\", the variable \"{}\" is not in the model.\n", msg, line_no, expr_ts, lhs);
                continue;
            }
            
            double rhs_value {};
            if (rhs == "Inf") {
                rhs_value = NO_BOUND;
            }
            else if (rhs == "-Inf") {
                rhs_value = -NO_BOUND;
            }
            else if (is_number(rhs, rhs_value)) {
                if (m[4].matched) {
                    auto rhs_unit_str = m[4].str();
                    auto rhs_unit = (M->unit_set.units.contains(rhs_unit_str) ? M->unit_set.units[rhs_unit_str].get() : nullptr);
                    if (rhs_unit == nullptr) {
                        ok = false;
                        cerr << format("{}line {}: in expression \"{}\", the right-hand side unit \"{}\" is not in the units table.\n",
                            msg, line_no, expr_ts, rhs_unit_str);
                        continue;
                    }
                    rhs_value = lhs_var->convert(rhs_value, rhs_unit);
                }
            }
            else {
                ok = false;
                cerr << format("{}line {}: the right-hand side \"{}\" of expression \"{}\" is invalid.\n", msg, line_no, rhs, expr_ts);
                continue;
            }

            if (op == "=")
                lhs_var->value = rhs_value;
            else if (op == "<")
                lhs_var->upper = rhs_value;
            else if (op == ">")
                lhs_var->lower = rhs_value;
        }
        else if (std::regex_match(expr_ts, m, re_spec)) {
            string lhs = m[1].str(),
                   rhs = m[2].str();

            auto rhs_var = M->var(rhs);
            if (rhs_var == nullptr) {
                ok = false;
                cerr << format("{}line {}: in expression \"{}\", the variable \"{}\" is not in the model.\n", msg, line_no, expr_ts, rhs);
                continue;
            }
            
            if (lhs == "free")
                rhs_var->free();
            else 
                rhs_var->fix();
        }
        else {
            ok = false;
            cerr << format("{}line {}: expression \"{}\" is invalid.\n", msg, line_no, expr_ts);
        }
    }
    
    lua_pushboolean(L, ok);
    return 1;
}

int add_Mixer(lua_State* L) {
    if (!FS) return 0;
    const string msg {"Mixer: expected "};
    auto n_args = lua_gettop(L);
    check(L, n_args == 3, format("{}3 arguments, got {}.", msg, n_args));
    check(L, lua_isstring(L, 1) && !lua_isnumber(L, 1), msg + "argument 1 to be a string.");
    check(L, lua_istable(L, 2), msg + "argument 2 to be a table of Stream pointers.");
    check(L, lua_isuserdata(L, 3), msg + "argument 3 to be a Stream pointer.");

    // Block name,
    string blk_name = lua_tostring(L, 1);               
    check(L, !blk_name.empty(), msg + "argument 1 to be a non-empty string");

    // List of inlet streams.
    auto n_inlets = lua_rawlen(L, 2);
    check(L, n_inlets > 1, msg + "argument 2 to be a table of at least two pointers to Streams");
    vector<Stream*> inlets(n_inlets);
    lua_pushvalue(L, 2);    // Push argument 2 onto the stack.
    for (lua_Unsigned i = 1; i <= n_inlets; i++) {
        inlets[i - 1] = get_pointer_elem<Stream>(L, i,
            format("{} element {} of argument 2 to be a pointer to a Stream", msg, i));
        check(L, inlets[i - 1] != nullptr,
            format("{} element {} of argument 2 to be a pointer to a Stream", msg, i));
    }
    lua_pop(L, 1);  // Pop argument 2

    lua_pushvalue(L, 3);    // Push argument 3 onto the stack.
    auto outlet = get_pointer<Stream>(L);
    check(L, outlet != nullptr, format("{} argument 3 to be a pointer to a Stream", msg));

    // Create the block.
    auto blk_p = FS->add_block<Mixer>(blk_name, std::move(inlets), vector<Stream*>{outlet});

    // Push a pointer to a Block with subtype Mixer onto the stack.
    push_pointer<Block, Mixer>(L, blk_p);

    return 1;
}

int add_Splitter(lua_State* L) {
    if (!FS) return 0;
    const string msg {"Splitter: expected "};
    auto n_args = lua_gettop(L);
    check(L, n_args == 3, format("{}3 arguments, got {}.", msg, n_args));
    check(L, lua_isstring(L, 1) && !lua_isnumber(L, 1), msg + "argument 1 to be a string.");
    check(L, lua_isuserdata(L, 2), msg + "argument 2 to be a Stream pointer.");
    check(L, lua_istable(L, 3), msg + "argument 3 to be a table of Stream pointers.");

    // Block name,
    string blk_name = lua_tostring(L, 1);               
    check(L, !blk_name.empty(), msg + "argument 1 to be a non-empty string");

    // Inlet stream.
    lua_pushvalue(L, 2);
    auto inlet = get_pointer<Stream>(L);
    check(L, inlet != nullptr, format("{} argument 2 to be a pointer to a Stream", msg));

    // List of outlet streams.
    auto n_outlets = lua_rawlen(L, 3);
    check(L, n_outlets > 1, msg + "argument 3 to be a table of at least two pointers to Streams");
    vector<Stream*> outlets(n_outlets);
    lua_pushvalue(L, 3);  // Push argument 3
    for (lua_Unsigned i = 1; i <= n_outlets; i++) {
        outlets[i - 1] = get_pointer_elem<Stream>(L, i,
            format("{} element {} of argument 3 to be a pointer to a Stream", msg, i));
        check(L, outlets[i - 1] != nullptr,
            format("{} element {} of argument 3 to be a pointer to a Stream", msg, i));
    }
    lua_pop(L, 1);  // Pop argument 3

    // Create the block.
    auto blk_p = FS->add_block<Splitter>(blk_name, vector<Stream*>{inlet}, std::move(outlets));

    // Push a pointer to a Block with subtype Mixer onto the stack.
    push_pointer<Block, Splitter>(L, blk_p);

    return 1;
}

int add_streams(lua_State* L) {
    if (!FS) return 0;
    const string msg {"Streams: expected "};
    int n_strms = lua_gettop(L);
    check(L, n_strms > 0, format("{}at least one argument", msg));

    vector<Stream*> strms(n_strms);
    for (int i = 1; i <= n_strms; i++) {
        check(L, lua_istable(L, i), format("{}argument {} to be a table", msg, i));
        check(L, lua_rawlen(L, i) == 2, format("{}length of argument {} to be 2", msg, i));
        lua_pushvalue(L, i);    // Push ith arg onto stack, where arg = {"Name", {"Comp1", "Comp2", etc}}
        string strm_name = get_string_elem(L, 1, format("{}element 1 of argument {} to be a string", msg, i));

        check(L, lua_rawgeti(L, i, 2) == LUA_TTABLE,
            format("{}element 2 of argument {} to be a table", msg, i)); // Push elem 2 of ith arg onto stack, e.g., {"Comp1", "Comp2"}
        int n_comps = lua_rawlen(L, -1);
        check(L, n_comps > 0, format("{}at least one component in argument {}", msg, i));
        vector<string> comps(n_comps);
        for (int j = 1; j <= n_comps; j++)
            comps[j - 1] = get_string_elem(L, j, format("{}component {} in argument {} to be a string", msg, j, i));

        lua_pop(L, 2); // Pop ith arg and elem 2 of ith arg
        strms[i - 1] = FS->add_stream(strm_name, std::move(comps));
    }

    // Push pointers to the created streams onto the stack.
    for (int i = 1; i <= n_strms; i++)
        push_pointer<Stream>(L, strms[i - 1]);

    return n_strms;
}

int create_model(lua_State* L) {
    const string msg_expected {"Model: expected "};
    const string msg_kinds {"Model: in the \"kinds\" table, expected "};
    const string msg_units {"Model: in the \"units\" table, expected "};
    auto n_args = lua_gettop(L);
    check(L, n_args == 4, format("{}4 arguments, got {}", msg_expected, n_args));
    check(L, lua_isstring(L, 1) && !lua_isnumber(L, 1), msg_expected + "argument 1 to be a string");  // arg 1 is the model name
    check(L, lua_isstring(L, 2) && !lua_isnumber(L, 2), msg_expected + "argument 2 to be a string");  // arg 2 is the index flowsheet name
    check(L, lua_istable(L, 3),  msg_expected + "argument 3 to be a table");  // arg 3 is a kinds table
    check(L, lua_istable(L, 4),  msg_expected + "argument 4 to be a table");  // arg 4 is a units table

    UnitSet u {};

    string name = lua_tostring(L, 1);               
    check(L, !name.empty(), msg_expected + "argument 1 to be a non-empty string");
    string index_fs_name = lua_tostring(L, 2);      
    check(L, !index_fs_name.empty(), msg_expected + "argument 2 to be a non-empty string");

    // kinds table:
    lua_pushnil(L);     // push a nil key to start
    while (lua_next(L, 3) != 0) {                   // pops the key, then pushes next key-value pair
        string kind_str = lua_tostring(L, -2);      // key is kind_str
        check(L, lua_istable(L, -1), format("{}key \"{}\" to reference a table", msg_kinds, kind_str)); // value is table with 1 or 2 strings

        auto n_str = lua_rawlen(L, -1);             // number of strings in the table
        check(L, n_str > 0, format("{}1 or 2 strings in the \"{}\" definition", msg_kinds, kind_str));

        string base_unit_str = get_string_elem(L, 1,
            format("{}element 1 in the \"{}\" definition to be a string", msg_kinds, kind_str)); // element 1 is base_unit_str

        string default_unit_str = (n_str > 1 ? get_string_elem(L, 2,
            format("{}element 2 in the \"{}\" definition to be a string", msg_kinds, kind_str)) : base_unit_str); // element 2 (optional) is default_unit_str

        lua_pop(L, 1);  // pop the value (the {base_unit_str, default_unit_str} table)
        u.add_kind(kind_str, base_unit_str, default_unit_str);
    }

    // units table:
    lua_pushnil(L);     // push a nil key to start
    while (lua_next(L, 4) != 0) {                   // pops the key, then pushes next key-value pair
        string kind_str = lua_tostring(L, -2);      // key is kind_str
        check(L, lua_istable(L, -1), format("{}key \"{}\" to reference a table", msg_units, kind_str)); // value is a table of n_units tables
        auto n_units = lua_rawlen(L, -1);           // value is a list of unit lists for this kind

        for (lua_Unsigned i = 1; i <= n_units; i++) {
            check(L, lua_rawgeti(L, -1, i) == LUA_TTABLE, format("{}unit {} in \"{}\" definition to be a list", msg_units, i, kind_str)); // push the ith unit list on the stack
            auto n_elem = lua_rawlen(L, -1);                // number of elements in the ith unit list
            check(L, n_elem > 1 && n_elem < 4, format("{}unit {} in \"{}\" definition to look like {{string, number, number}}", msg_units, i, kind_str));

            string unit_str = get_string_elem(L, 1, format("{}unit {} in \"{}\" definition to look like {{string, number, number}}", msg_units, i, kind_str));     // element 1 is unit_str
            double unit_ratio = get_double_elem(L, 2, format("{}unit {} in \"{}\" definition to look like {{string, number, number}}", msg_units, i, kind_str));   // element 2 is unit_ratio
            double unit_offset = (n_elem > 2 ? 
                get_double_elem(L, 3, format("{}unit {} in \"{}\" definition to look like {{string, number, number}}", msg_units, i, kind_str)) : 0.0); // element 3 (optional) is unit_offset

            lua_pop(L, 1);  // pop the ith table
            u.add_unit(unit_str, kind_str, unit_ratio, unit_offset);
        }

        lua_pop(L, 1);  // pop the value (a list of unit lists)
    }

    // Create the model.
    M = make_unique<Model>(name, index_fs_name, std::move(u));
    FS = M->index_fs.get();

    // Push pointers to the Model and the index Flowsheet onto the stack.
    push_pointer<Model>(L, M.get());
    push_pointer<Flowsheet>(L, FS);

    return 2;
}

LuaResult run_lua_script(lua_State* L, const char* script_file_name) {
    int result = luaL_dofile(L, script_file_name);
    
    if (result != LUA_OK) {
        string err_str = lua_tostring(L, -1);
        lua_pop(L, 1);
        lua_close(L);
        return {.ok = false, .err_str = err_str};
    }
    return {.ok = true, .err_str = ""};
}

lua_State* start_lua() {
    auto L = luaL_newstate();
    if (!L) return nullptr;
    luaL_openlibs(L);
    
    lua_register(L, "Model",           create_model);
    lua_register(L, "Streams",         add_streams);
    lua_register(L, "Eval",            eval_expr);
    lua_register(L, "Init",            initialize);
    lua_register(L, "ShowVariables",   show_variables);
    lua_register(L, "ShowConstraints", show_constraints);
    lua_register(L, "ShowJacobian",    show_jacobian);
    lua_register(L, "ShowHessian",     show_hessian);
    lua_register(L, "Val",             get_value);
    lua_register(L, "LB",              get_lower);
    lua_register(L, "UB",              get_upper);
    lua_register(L, "Spec",            get_spec);
    lua_register(L, "Var",             get_var);
    lua_register(L, "ChangeUnit",      change_unit);
    lua_register(L, "SolverOption",    set_solver_option);
    lua_register(L, "Solve",           solve_model);
    lua_register(L, "InitSolver",      initialize_solver);
    lua_register(L, "EvalConstraints", eval_constraints);
    lua_register(L, "EvalJacobian",    eval_jacobian);
    lua_register(L, "EvalHessian",     eval_hessian);

    lua_register(L, "Mixer",           add_Mixer);
    lua_register(L, "Splitter",        add_Splitter);

    return L;
}
