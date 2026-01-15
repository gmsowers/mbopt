#include <cassert>
#include <sstream>
#include <regex>
#include <charconv>
#include <typeindex>
#include "scripter.hpp"
#include "Model.hpp"
#include "Mixer.hpp"

static lua_State*        L;
static unique_ptr<Model> M;
static Flowsheet*        FS;

void check(bool cond, string err_msg) {
    if (!cond)
        luaL_error(L, err_msg.c_str());
}

string trim_all(string_view s) {
    string res {};
    for (const auto& c : s) {
        if (c == ' ' || c == '\t' || c == '\n')
            continue;
        res += c;
    }
    return res;
}

string trim_sides(string_view s) {
    auto len = s.size();
    auto beg = len;
    for (int i = 0; i < len; i++) {
        auto c = s[i];
        if (c != ' ' && c != '\t' && c != '\n') {
            beg = i;
            break;
        }
    }
    auto end = len - 1;
    for (int i = end; i >= beg; i--) {
        auto c = s[i];
        if (c != ' ' && c != '\t' && c != '\n') {
            end = i;
            break;
        }
    }
    string res {};
    for (int i = beg; i <= end; i++)
        res += s[i];
    return res;
}

bool is_number(string_view s, double& value) {
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
    check(lua_rawgeti(L, -1, index) == LUA_TSTRING, err_msg);
    string elem = lua_tostring(L, -1);
    lua_pop(L, 1);
    return elem;
}

double get_double_elem(lua_State* L, int index, const string& err_msg) {
    check(lua_rawgeti(L, -1, index) == LUA_TNUMBER, err_msg);
    double elem = lua_tonumber(L, -1);
    lua_pop(L, 1);
    return elem;
}

struct TypedPtr {
    void* ptr;
    std::type_index type;
};

template <typename T>
T* get_pointer(lua_State* L) {
    auto tp = (TypedPtr*)lua_touserdata(L, -1);
    T* ptr {};
    if (tp->type == typeid(T))
        ptr = static_cast<T*>(tp->ptr);
    lua_pop(L, 1);
    return ptr;
}

template <typename T>
T* get_pointer_elem(lua_State* L, int index, const string& err_msg) {
    check(lua_rawgeti(L, -1, index) == LUA_TUSERDATA, err_msg);
    return get_pointer<T>(L);
}

template <typename T>
void push_pointer(lua_State* L, T* p) {
    TypedPtr* tp = (TypedPtr*)lua_newuserdatauv(L, sizeof(TypedPtr), 0);
    tp->ptr = p;
    tp->type = typeid(T);
}

#if 0

void lua_eval_constraints() {
    Model* M = lua["M"];
    if (M == nullptr) return;
    M->eval_constraints();
}

void lua_set_string_solver_option(const string& option, const string& val) {
    if (solver) solver->Options()->SetStringValue(option, val);
}

void lua_set_integer_solver_option(const string& option, int val) {
    if (solver) solver->Options()->SetIntegerValue(option, val);
}

void lua_set_numeric_solver_option(const string& option, double val) {
    if (solver) solver->Options()->SetNumericValue(option, val);
}

int lua_initialize_solver() {
    if (solver)
        return solver->Initialize();
    else
        return -1;
}

int lua_solve() {
    Model* M = lua["M"];
    if (M == nullptr) return -1;
    return solver->OptimizeTNLP(M);
}

void lua_show_variables() {
    Model* M = lua["M"];
    if (M == nullptr) return;
    M->show_variables();
}

void lua_show_model_variables(Model* m) {
    if (m == nullptr) return;
    m->show_variables();
}

void lua_show_block_variables(Block* blk) {
    if (blk == nullptr) return;
    blk->show_variables();
}

std::pair<Ndouble, sol::optional<string>> lua_get_value(const string& var_name) {
    Model* M = lua["M"];
    if (M == nullptr) return {sol::nullopt, sol::nullopt};
    if (M->x_map.contains(var_name))
        return {M->x_map[var_name]->value,
                M->x_map[var_name]->unit->str};
    return {sol::nullopt, sol::nullopt};
}

std::pair<Ndouble, sol::optional<string>> lua_get_lower(const string& var_name) {
    Model* M = lua["M"];
    if (M == nullptr) return {sol::nullopt, sol::nullopt};
    if (M->x_map.contains(var_name)) {
        Ndouble val {};
        if (M->x_map[var_name]->lower.has_value())
            val = M->x_map[var_name]->lower.value();
        return {val, M->x_map[var_name]->unit->str};
    }
    return {sol::nullopt, sol::nullopt};
}

std::pair<Ndouble, sol::optional<string>> lua_get_upper(const string& var_name) {
    Model* M = lua["M"];
    if (M == nullptr) return {sol::nullopt, sol::nullopt};
    if (M->x_map.contains(var_name)) {
        Ndouble val {};
        if (M->x_map[var_name]->upper.has_value())
            val = M->x_map[var_name]->upper.value();
        return {val, M->x_map[var_name]->unit->str};
    }
    return {sol::nullopt, sol::nullopt};
}

sol::optional<string> lua_get_spec(const string& var_name) {
    using namespace std::literals;
    Model* M = lua["M"];
    if (M == nullptr) return sol::nullopt;
    if (M->x_map.contains(var_name))
        return (M->x_map[var_name]->spec == VariableSpec::Fixed ? "fixed"s : "free"s);
    return sol::nullopt;
}

Ndouble lua_change_unit(const string& var_name, const string& new_unit_str) {
    Model* M = lua["M"];
    if (M == nullptr) return sol::nullopt;
    if (M->x_map.contains(var_name))
        return M->x_map[var_name]->change_unit(M, new_unit_str);
    return sol::nullopt;
}
#endif

int initialize_model(lua_State* L) {
    if (M) M->initialize();
    return 0;
}

const std::regex re_binop(R"((\S+)(=|<|>)([^\s_]+)(?:_(\S+))?)");
const std::regex re_spec(R"((fix|free)\s+(\S+))");

int eval_expr(lua_State* L) {
    if (!M) return 0;
    auto line_no = get_line_no(L);
    string msg {get_script_name(L) + ", "};
    auto n_args = lua_gettop(L);
    check(n_args == 1, format("{}expected 1 argument, got {}.", msg, n_args));
    check(lua_isstring(L, 1), msg + "expected the argument to be a string.");

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
            
            rhs_var->spec = (lhs == "free" ? VariableSpec::Free : VariableSpec::Fixed);
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
    check(n_args == 3, format("{}3 arguments, got {}.", msg, n_args));
    check(lua_isstring(L, 1) && !lua_isnumber(L, 1), msg + "argument 1 to be a string.");
    check(lua_istable(L, 2), msg + "argument 2 to be a table of Stream pointers.");
    check(lua_isuserdata(L, 3), msg + "argument 3 to be a Stream pointer.");

    // Block name,
    string blk_name = lua_tostring(L, 1);               
    check(!blk_name.empty(), msg + "argument 1 to be a non-empty string");

    // List of inlet streams.
    auto n_inlets = lua_rawlen(L, 2);
    check(n_inlets > 1, msg + "argument 2 to be a table of at least two pointers to Streams");
    vector<Stream*> inlets(n_inlets);
    lua_pushvalue(L, 2);    // Push argument 2 onto the stack.
    for (int i = 1; i <= n_inlets; i++) {
        inlets[i - 1] = get_pointer_elem<Stream>(L, i,
            format("{} element {} of argument 2 to be a pointer to a Stream", msg, i));
        check(inlets[i - 1] != nullptr,
            format("{} element {} of argument 2 to be a pointer to a Stream", msg, i));
    }
    lua_pop(L, 1);  // Pop argument 2

    lua_pushvalue(L, 3);    // Push argument 3 onto the stack.
    auto outlet = get_pointer<Stream>(L);
    check(outlet != nullptr, format("{} argument 3 to be a pointer to a Stream", msg));

    // Create the block.
    auto blk_p = FS->add_block<Mixer>(blk_name, std::move(inlets), vector<Stream*>{outlet});

    // Push a pointer to the block onto the stack.
    push_pointer<Mixer>(L, blk_p);

    return 1;
}

int add_streams(lua_State* L) {
    if (!FS) return 0;
    const string msg {"Streams: expected "};
    int n_strms = lua_gettop(L);
    check(n_strms > 0, format("{}at least one argument", msg));

    vector<Stream*> strms(n_strms);
    for (int i = 1; i <= n_strms; i++) {
        check(lua_istable(L, i), format("{}argument {} to be a table", msg, i));
        check(lua_rawlen(L, i) == 2, format("{}length of argument {} to be 2", msg, i));
        lua_pushvalue(L, i);    // Push ith arg onto stack, where arg = {"Name", {"Comp1", "Comp2", etc}}
        string strm_name = get_string_elem(L, 1, format("{}element 1 of argument {} to be a string", msg, i));

        check(lua_rawgeti(L, i, 2) == LUA_TTABLE,
            format("{}element 2 of argument {} to be a table", msg, i)); // Push elem 2 of ith arg onto stack, e.g., {"Comp1", "Comp2"}
        int n_comps = lua_rawlen(L, -1);
        check(n_comps > 0, format("{}at least one component in argument {}", msg, i));
        vector<string> comps(n_comps);
        for (int j = 1; j <= n_comps; j++)
            comps[j - 1] = get_string_elem(L, j, format("{}component {} in argument {} to be a string", msg, j, i));

        lua_pop(L, 2); // Pop ith arg and elem 2 of ith arg
        strms[i - 1] = FS->add_stream(strm_name, comps);
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
    check(n_args == 4, format("{}4 arguments, got {}", msg_expected, n_args));
    check(lua_isstring(L, 1) && !lua_isnumber(L, 1), msg_expected + "argument 1 to be a string");  // arg 1 is the model name
    check(lua_isstring(L, 2) && !lua_isnumber(L, 2), msg_expected + "argument 2 to be a string");  // arg 2 is the index flowsheet name
    check(lua_istable(L, 3),  msg_expected + "argument 3 to be a table");  // arg 3 is a kinds table
    check(lua_istable(L, 4),  msg_expected + "argument 4 to be a table");  // arg 4 is a units table

    UnitSet u {};

    string name = lua_tostring(L, 1);               
    check(!name.empty(), msg_expected + "argument 1 to be a non-empty string");
    string index_fs_name = lua_tostring(L, 2);      
    check(!index_fs_name.empty(), msg_expected + "argument 2 to be a non-empty string");

    // kinds table:
    lua_pushnil(L);     // push a nil key to start
    while (lua_next(L, 3) != 0) {                   // pops the key, then pushes next key-value pair
        string kind_str = lua_tostring(L, -2);      // key is kind_str
        check(lua_istable(L, -1), format("{}key \"{}\" to reference a table", msg_kinds, kind_str)); // value is table with 1 or 2 strings

        auto n_str = lua_rawlen(L, -1);             // number of strings in the table
        check(n_str > 0, format("{}1 or 2 strings in the \"{}\" definition", msg_kinds, kind_str));

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
        check(lua_istable(L, -1), format("{}key \"{}\" to reference a table", msg_units, kind_str)); // value is a table of n_units tables
        auto n_units = lua_rawlen(L, -1);           // value is a list of unit lists for this kind

        for (int i = 1; i <= n_units; i++) {
            check(lua_rawgeti(L, -1, i) == LUA_TTABLE, format("{}unit {} in \"{}\" definition to be a list", msg_units, i, kind_str)); // push the ith unit list on the stack
            auto n_elem = lua_rawlen(L, -1);                // number of elements in the ith unit list
            check(n_elem > 1 && n_elem < 4, format("{}unit {} in \"{}\" definition to look like {{string, number, number}}", msg_units, i, kind_str));

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
    M.reset(new Model {name, index_fs_name, std::move(u)});
    FS = M->index_fs.get();

    // Push pointers to the Model and the index Flowsheet onto the stack.
    push_pointer<Model>(L, M.get());
    push_pointer<Flowsheet>(L, FS);

    return 2;
}

LuaResult run_lua_script(const char* script_file_name) {
    int result = luaL_dofile(L, script_file_name);
    
    if (result != LUA_OK) {
        string err_str = lua_tostring(L, -1);
        lua_pop(L, 1);
        lua_close(L);
        return {false, err_str};
    }
    return {true, ""};
}

bool start_lua() {
    L = luaL_newstate();
    if (!L) return false;
    luaL_openlibs(L);
    
    lua_register(L, "Model",      create_model);
    lua_register(L, "Streams",    add_streams);
    lua_register(L, "Mixer",      add_Mixer);
    lua_register(L, "Eval",       eval_expr);
    lua_register(L, "InitModel",  initialize_model);

    return true;
}

#if 0
void start_lua() {
	lua.open_libraries(sol::lib::base,
                       sol::lib::string,
                       sol::lib::os,
                       sol::lib::math,
                       sol::lib::table);

    lua.set_function("UnitSet",          lua_unit_set);
    lua.set_function("Model",            lua_new_model);
    lua.set_function("IndexFS",          lua_get_index_fs);
    lua.set_function("Streams",          lua_add_streams);
    lua.set_function("Mixer",            lua_add_Mixer);
    lua.set_function("Set",              lua_set);
    lua.set_function("Specs",            lua_specs);
    lua.set_function("ShowVariables",    sol::overload(lua_show_variables,
                                                       lua_show_model_variables,
                                                       lua_show_block_variables));
    lua.set_function("InitializeModel",  lua_initialize_model);
    lua.set_function("EvalConstraints",  lua_eval_constraints);
    lua.set_function("Val",              lua_get_value);
    lua.set_function("LB",               lua_get_lower);
    lua.set_function("UB",               lua_get_upper);
    lua.set_function("Spec",             lua_get_spec);
    lua.set_function("ChangeUnit",       lua_change_unit);
    lua.set_function("SolverOption",     sol::overload(lua_set_string_solver_option,
                                                       lua_set_integer_solver_option,
                                                       lua_set_numeric_solver_option));
    lua.set_function("InitializeSolver", lua_initialize_solver);
    lua.set_function("Solve",            lua_solve);

}
#endif
