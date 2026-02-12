#include <sstream>
#include <regex>
#include <charconv>
#include <typeindex>
#include "scripter.hpp"
#include "Model.hpp"
#include "Mixer.hpp"
#include "Splitter.hpp"
#include "Separator.hpp"
#include "YieldReactor.hpp"
#include "MultiYieldReactor.hpp"

static unique_ptr<Model> M;
static Flowsheet*        FS;
static lua_State*        scripter_lua_state;

static IpoptApplication* solver = IpoptApplicationFactory();

void check(lua_State* L, const bool cond, string_view err_msg) {
    if (!cond) luaL_error(L, err_msg.data());
}

void checkM(lua_State* L) {
    if (!M) luaL_error(L, "Model doesn't exist.");
}

void checkFS(lua_State* L) {
    if (!FS) luaL_error(L, "Flowsheet doesn't exist.");
}

string trim_all(const string_view s) {
    string res {};
    for (const auto& c : s) {
        if (c == ' ' || c == '\t' || c == '\n') continue;
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
T* get_typed_ptr(lua_State* L, int index, const string& err_msg) {
    if (index > 0) lua_pushvalue(L, index);
    auto tp = static_cast<TypedPtr*>(lua_touserdata(L, -1));
    check(L, tp != nullptr, err_msg);
    T* ptr {};
    if (tp->type_idx == typeid(T))
        ptr = static_cast<T*>(tp->ptr);
    check(L, ptr != nullptr, err_msg);
    lua_pop(L, 1);
    return ptr;
}

template <typename T>
T* get_typed_ptr_elem(lua_State* L, int index, const string& err_msg) {
    check(L, lua_rawgeti(L, -1, index) == LUA_TUSERDATA, err_msg);
    return get_typed_ptr<T>(L, 0, err_msg);
}

template <typename T, typename sub_T = void>
void push_pointer(lua_State* L, T* p) {
    if (!p)
        lua_pushnil(L);
    else {
        auto tp = static_cast<TypedPtr*>(lua_newuserdatauv(L, sizeof(TypedPtr), 0));
        tp->ptr = p;
        tp->type_idx = typeid(T);
        tp->subtype_idx = typeid(sub_T);
    }
}

int solve_model(lua_State* L) {
    checkM(L);
    auto n_args = lua_gettop(L);
    check(L, n_args <= 1, "Solve: expected 0 or 1 argument.");
    if (n_args == 1)
        M->obj = get_typed_ptr<Objective>(L, 1, "Solve: expected argument to be an Objective.");
    int retval = solver->OptimizeTNLP(M.get());
    lua_pushinteger(L, retval);
    return 1;
}

template <typename Delegate_Func_T>
int delegate(lua_State* L, Delegate_Func_T f) {
    checkM(L);
    auto n_args = lua_gettop(L);
    if (n_args == 0) {
        f(M.get());
        return 0;
    }
    for (int i = 1; i <= n_args; i++) {
        if (lua_isuserdata(L, i)) {
            auto tp = static_cast<TypedPtr*>(lua_touserdata(L, -1));
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
                } else if (tp->subtype_idx == typeid(Separator)) {
                    auto p = static_cast<Separator*>(tp->ptr);
                    if (p) f(p);
                } else if (tp->subtype_idx == typeid(YieldReactor)) {
                    auto p = static_cast<YieldReactor*>(tp->ptr);
                    if (p) f(p);
                }
                // TODO: add remaining Blocks.
            }
            else if (tp->type_idx == typeid(Calc)) {
                auto p = static_cast<Calc*>(tp->ptr);
                if (p) f(p);
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

int eval_objective(lua_State* L) {
    checkM(L);
    auto n_args = lua_gettop(L);
    check(L, n_args <= 1, "EvalObjective: expected 0 or 1 argument.");
    if (n_args == 0) {
        lua_pushnumber(L, M->eval_objective());
        return 1;
    }
    auto obj = get_typed_ptr<Objective>(L, 1, "EvalObjective: expected argument 1 to be an Objective.");
    lua_pushnumber(L, obj->eval());
    return 1;

}

int eval_obj_grad(lua_State* L) {
    checkM(L);
    check(L, lua_gettop(L) == 0, "EvalObjGrad: expected no arguments.");
    M->eval_obj_grad();
    return 0;

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
    checkM(L);
    if (lua_gettop(L) == 0) return 0;
    if (lua_isuserdata(L, -1)) {
        auto tp = static_cast<TypedPtr*>(lua_touserdata(L, -1));
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

int get_base_value(lua_State* L) {
    return get_variable_attr(L, [](auto* p){ return p->convert_to_base(); });
}

int get_lower(lua_State* L) {
    return get_variable_attr(L, [](auto* p){ return p->lower; });
}

int get_upper(lua_State* L) {
    return get_variable_attr(L, [](auto* p){ return p->upper; });
}

int get_spec(lua_State* L) {
    checkM(L);
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
    checkM(L);
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

int get_unit(lua_State* L) {
    checkM(L);
    auto n_args = lua_gettop(L);
    if (n_args == 0) return 0;
    if (n_args >= 1) {
        if (lua_isstring(L, -1)) {
            string arg = lua_tostring(L, -1);
            if (M->x_map.contains(arg)) {
                push_pointer<Unit>(L, M->x_map[arg]->unit);
                return 1;
            }
            else if (M->unit_set.units.contains(arg)) {
                push_pointer<Unit>(L, M->unit_set.units[arg].get());
                return 1;
            }
        }
    }
    return 0;

}

int change_unit(lua_State* L) {
    checkM(L);
    if (lua_gettop(L) != 2) return 0;
    if (lua_isstring(L, 1)) {
        string name = lua_tostring(L, 1);
        string unit_str = lua_tostring(L, 2);
        if (M->x_map.contains(name)) {
            Ndouble retval = M->x_map[name]->change_unit(M.get(), unit_str);
            if (retval.has_value()) {
                lua_pushnumber(L, retval.value());
                return 1;
            }
            else
                return 0;
        } else if (M->prices.contains(name)) {
            Ndouble retval = M->prices[name]->change_unit(M.get(), unit_str);
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
    else if (lua_isuserdata(L, 1)) {
        auto tp = static_cast<TypedPtr*>(lua_touserdata(L, 1));
        if (!tp) return 0;
        string unit_str = lua_tostring(L, 2);
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
        else if (tp->type_idx == typeid(Price)) {
            auto p = static_cast<Price*>(tp->ptr);
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

int set_value(lua_State* L) {
    checkM(L);
    const string msg {"SetValue: expected "};
    auto n_args = lua_gettop(L);
    check(L, n_args == 2, msg + format("{}2 arguments, got {}.", msg, n_args));
    check(L, lua_isuserdata(L, 1), msg + "argument 1 to be one of: Variable, Constraint, JacobianNZ, HessianNZ, Price.");
    check(L, lua_isnumber(L, 2), msg + "argument 2 to be a number.");
    auto tp = static_cast<TypedPtr*>(lua_touserdata(L, 1));
    double val = lua_tonumber(L, 2);
    check(L, tp != nullptr, msg + "argument 1 not to be null."); 
    if (tp->type_idx == typeid(Variable)) {
        auto var = static_cast<Variable*>(tp->ptr);
        var->convert_and_set(val);
    }
    else if (tp->type_idx == typeid(Constraint)) {
        auto con = static_cast<Constraint*>(tp->ptr);
        con->value = val;
    }
    else if (tp->type_idx == typeid(JacobianNZ)) {
        auto jnz = static_cast<JacobianNZ*>(tp->ptr);
        jnz->value = val;
    }
    else if (tp->type_idx == typeid(HessianNZ)) {
        auto hnz = static_cast<HessianNZ*>(tp->ptr);
        hnz->value = val;
    }
    else if (tp->type_idx == typeid(Price)) {
        auto price = static_cast<Price*>(tp->ptr);
        price->convert_and_set(val);
    }
    else
        check(L, false, msg + "argument 1 to be one of: Variable, Constraint, JacobianNZ, HessianNZ, Price.");

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

int show_summary(lua_State* L) {
    return delegate(L, [](auto* p) { p->show_summary(); });
}

int show_connections(lua_State* L) {
    checkM(L);
    check(L, lua_gettop(L) == 0, "ShowConnections: expected no arguments.");
    M->show_connections();
    return 0;
}

int show_prices(lua_State* L) {
    checkM(L);
    check(L, lua_gettop(L) == 0, "ShowPrices: expected no arguments.");
    M->show_prices();
    return 0;
}

int show_objective(lua_State* L) {
    checkM(L);
    auto n_args = lua_gettop(L);
    check(L, n_args <= 1, "ShowObjective: expected 0 or 1 argument.");
    if (n_args == 0) {
        M->show_objective();
        return 0;
    }
    auto obj = get_typed_ptr<Objective>(L, 1, "ShowObjective: expected argument 1 to be an Objective.");
    M->show_objective(obj);
    return 0;
}

int show_obj_grad(lua_State* L) {
    checkM(L);
    check(L, lua_gettop(L) == 0, "ShowObjGrad: expected no arguments.");
    M->show_obj_grad();
    return 0;
}

int initialize(lua_State* L) {
    return delegate(L, [](auto* p) { p->initialize(); });
}

int connect(lua_State* L) {
    checkM(L);
    const string msg {"Connect: expected "};
    auto n_args = lua_gettop(L);
    check(L, n_args == 1 || n_args == 2, format("{}1 or 2 arguments, got {}.", msg, n_args));
    check(L, lua_isuserdata(L, 1), msg + "argument 1 to be a Variable or a Stream.");
    auto tp = static_cast<TypedPtr*>(lua_touserdata(L, 1));
    check(L, tp != nullptr, msg + "argument 1 to be a Variable or a Stream.");
    if (tp->type_idx == typeid(Variable)) {
        auto var1 = static_cast<Variable*>(tp->ptr);
        check(L, n_args == 2, msg + "2 arguments, both Variables.");
        tp = static_cast<TypedPtr*>(lua_touserdata(L, 2));
        check(L, (tp != nullptr) && (tp->type_idx == typeid(Variable)), msg + "argument 2 to be a Variable or a Stream.");
        auto var2 = static_cast<Variable*>(tp->ptr);
        auto conn = M->add_connection(var1, var2);
        push_pointer<Connection>(L, conn);
        return 1;
    }
    else if (tp->type_idx == typeid(Stream)) {
        auto strm = static_cast<Stream*>(tp->ptr);
        auto conn = strm->connect();
        push_pointer<Connection>(L, conn);
        return 1;
    }
    return 0;
}

int connect_streams(lua_State* L) {
    checkM(L);
    check(L, lua_gettop(L) == 0, "ConnectAll: expected no arguments.");
    bool ok = M->index_fs->connect_streams();
    lua_pushboolean(L, ok);
    return 1;
}

const std::regex re_binop(R"((\S+)(=|<|>)([^\s_]+)(?:_(\S+))?)");
const std::regex re_spec(R"((fix|free)\s+(\S+))");

int eval_expr(lua_State* L) {
    checkM(L);
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

template <typename T>
void start_Block(lua_State* L, const string& blk_type, string& blk_name, vector<Stream*>& inlets, vector<Stream*>& outlets) {
    checkFS(L);
    const string msg {blk_type + ": expected "};
    auto n_args = lua_gettop(L);
    check(L, n_args >= 3, format("{}at least 3 arguments, got {}.", msg, n_args));
    check(L, lua_isstring(L, 1) && !lua_isnumber(L, 1), msg + "argument 1 to be a string.");
    check(L, lua_istable(L, 2), msg + "argument 2 to be a table of Streams.");
    check(L, lua_istable(L, 3), msg + "argument 3 to be a table of Streams.");

    // Block name,
    blk_name = lua_tostring(L, 1);               
    check(L, !blk_name.empty(), msg + "block name to be a non-empty string.");

    // Table of inlet streams.
    auto n_inlets = lua_rawlen(L, 2);
    check(L, n_inlets > 0, msg + "argument 2 to be a table of at least one Stream.");
    inlets.resize(n_inlets);
    lua_pushvalue(L, 2);    // Push argument 2 onto the stack.
    for (lua_Unsigned i = 1; i <= n_inlets; i++) {
        inlets[i - 1] = get_typed_ptr_elem<Stream>(L, i,
            format("{} element {} of argument 2 to be a Stream", msg, i));
    }
    lua_pop(L, 1);  // Pop argument 2

    // Table of outlet streams.
    auto n_outlets = lua_rawlen(L, 3);
    check(L, n_outlets > 0, msg + "argument 3 to be a table of at least one Stream.");
    outlets.resize(n_outlets);
    lua_pushvalue(L, 3);    // Push argument 3 onto the stack.
    for (lua_Unsigned i = 1; i <= n_outlets; i++) {
        outlets[i - 1] = get_typed_ptr_elem<Stream>(L, i,
            format("{} element {} of argument 3 to be a Stream.", msg, i));
    }
    lua_pop(L, 1);  // Pop argument 3
}

template <typename T, typename ...blk_params_T>
int finish_Block(lua_State* L, string& blk_name, vector<Stream*>& inlets, vector<Stream*>& outlets, blk_params_T& ...blk_params) {
    auto blk_p = FS->add_block<T>(blk_name, std::move(inlets), std::move(outlets), blk_params...);
    push_pointer<Block, T>(L, blk_p);
    return 1;
}

template <typename T>
int add_Block(lua_State* L, const string& blk_type) {
    string blk_name;
    vector<Stream*> inlets, outlets;
    start_Block<T>(L, blk_type, blk_name, inlets, outlets);
    return finish_Block<T>(L, blk_name, inlets, outlets);
}

int add_Mixer(lua_State* L) {
    return add_Block<Mixer>(L, "Mixer");
}

int add_Splitter(lua_State* L) {
    return add_Block<Splitter>(L, "Splitter");
}

int add_Separator(lua_State* L) {
    return add_Block<Separator>(L, "Separator");
}

int add_YieldReactor(lua_State* L) {
    string blk_name;
    vector<Stream*> inlets, outlets;
    start_Block<YieldReactor>(L, "YieldReactor", blk_name, inlets, outlets);
    return finish_Block<YieldReactor>(L, blk_name, inlets, outlets);
}

int add_MultiYieldReactor(lua_State* L) {
    string blk_name;
    vector<Stream*> inlets, outlets;
    start_Block<YieldReactor>(L, "YieldReactor", blk_name, inlets, outlets);

    const string msg {"MultiYieldReactor: expected "};

    auto n_args = lua_gettop(L);
    auto n_feeds = inlets.size();
    check(L, n_args >= n_feeds + 4, format("{}{} arguments, got {}.", msg, n_feeds + 4, n_args));
    check(L, lua_isstring(L, 4) && !lua_isnumber(L, 4), msg + "argument 4 to be a string.");

    // Reactor name,
    string reactor_name = lua_tostring(L, 4);
    check(L, !reactor_name.empty(), msg + "reactor name to be a non-empty string.");

    // Feed names.
    vector<string> feed_names(n_feeds);
    for (int i = 0, j = 5; i < n_feeds; i++, j++) {
        check(L, lua_isstring(L, j) && !lua_isnumber(L, j), format("{}argument {} to be a string.", msg, j));
        feed_names[i] = lua_tostring(L, j);
        check(L, !feed_names[i].empty(), format("{}argument {} to be a non-empty string.", msg, j));
    }

    return finish_Block<MultiYieldReactor, vector<string>&, const string&>(L, blk_name, inlets, outlets, feed_names, reactor_name);
}

int add_Calc(lua_State* L) {
    checkFS(L);
    const string msg {"Calc: expected "};
    auto n_args = lua_gettop(L);
    check(L, n_args == 1, format("{}1 argument, got {}.", msg, n_args));
    check(L, lua_isstring(L, 1) && !lua_isnumber(L, 1), msg + "argument 1 to be a string.");

    // Calc name,
    string calc_name = lua_tostring(L, 1);               
    check(L, !calc_name.empty(), msg + "argument 1 to be a non-empty string.");

    // Create the calc.
    auto calc_p = FS->add_calc(calc_name);

    // Push a pointer to a Calc onto the stack.
    push_pointer<Calc>(L, calc_p);

    return 1;
}

int add_variables(lua_State* L) {
    checkM(L);
    const string msg {"Variables: expected "};
    auto n_args = lua_gettop(L);
    check(L, n_args >= 2, format("{}at least 2 arguments, got {}.", msg, n_args));
    auto calc_p = get_typed_ptr<Calc>(L, 1, msg + "argument 1 to be a Calc.");
    auto n_vars = n_args - 1;
    vector<Variable*> vars(n_vars);
    for (lua_Unsigned i = 2; i <= n_args; i++) {
        check(L, lua_istable(L, i), format("{}argument {} to be a table.", msg, i));
        auto n_elem = lua_rawlen(L, i);    // number of elements in the ith argument table
        check(L, n_elem == 2, format("{}argument {} to look like {{string, Unit}}.", msg, i));
        lua_pushvalue(L, i);    // Push ith arg onto stack, where arg = {var_name, unit}
        auto var_name = get_string_elem(L, 1, format("{}argument {} to look like {{string, Unit}}.", msg, i));  // element 1 is variable name
        auto unit = get_typed_ptr_elem<Unit>(L, 2, format("{}argument {} to look like {{string, Unit}}.", msg, i));  // element 2 is variable name
        lua_pop(L, 1);
        auto v = M->add_var(calc_p->prefix + var_name, unit);
        calc_p->x.push_back(v);
        vars[i - 2] = v;
    }

    // Push pointers to the created variables onto the stack.
    for (int i = 1; i <= n_vars; i++)
        push_pointer<Variable>(L, vars[i - 1]);

    return n_vars;
}

int add_constraints(lua_State* L) {
    checkM(L);
    const string msg {"Constraints: expected "};
    auto n_args = lua_gettop(L);
    check(L, n_args >= 2, format("{}at least 2 arguments, got {}.", msg, n_args));
    auto calc_p = get_typed_ptr<Calc>(L, 1, msg + "argument 1 to be a Calc.");
    auto n_eqs = n_args - 1;
    vector<Constraint*> eqs(n_eqs);
    for (lua_Unsigned i = 2; i <= n_args; i++) {
        check(L, lua_isstring(L, i), format("{}argument {} to be a string.", msg, i));
        auto eq_name = lua_tostring(L, i);
        auto eq = M->add_constraint(calc_p->prefix + eq_name);
        calc_p->g.push_back(eq);
        eqs[i - 2] = eq;
    }

    // Push pointers to the created constraints onto the stack.
    for (int i = 1; i <= n_eqs; i++)
        push_pointer<Constraint>(L, eqs[i - 1]);

    return n_eqs;
}

int add_jacobian_nzs(lua_State* L) {
    checkM(L);
    const string msg {"JacobianNZs: expected "};
    auto n_args = lua_gettop(L);
    check(L, n_args >= 2, format("{}at least 2 arguments, got {}.", msg, n_args));
    auto calc_p = get_typed_ptr<Calc>(L, 1, msg + "argument 1 to be a Calc.");
    auto n_jnzs = n_args - 1;
    vector<JacobianNZ*> jnzs(n_jnzs);
    for (lua_Unsigned i = 2; i <= n_args; i++) {
        check(L, lua_istable(L, i), format("{}argument {} to be a table.", msg, i));
        auto n_elem = lua_rawlen(L, i);    // number of elements in the ith JNZ list
        check(L, n_elem == 2, format("{}argument {} to look like {{Constraint, Variable}}.", msg, i));
        lua_pushvalue(L, i);    // Push ith arg onto stack, where arg = {con, var}
        auto con = get_typed_ptr_elem<Constraint>(L, 1, format("{}argument {} to look like {{Constraint, Variable}}.", msg, i));  // element 1 is a Constraint
        auto var = get_typed_ptr_elem<Variable>(L, 2, format("{}argument {} to look like {{Constraint, Variable}}.", msg, i));  // element 2 is a Variable
        lua_pop(L, 1);
        auto jnz = M->add_J_NZ(con, var);
        calc_p->J.push_back(jnz);
        jnzs[i - 2] = jnz;
    }

    // Push pointers to the created variables onto the stack.
    for (int i = 1; i <= n_jnzs; i++)
        push_pointer<JacobianNZ>(L, jnzs[i - 1]);

    return n_jnzs;
}

int add_streams(lua_State* L) {
    checkFS(L);
    const string msg {"Streams: expected "};
    int n_strms = lua_gettop(L);
    check(L, n_strms > 0, format("{}at least one argument.", msg));

    vector<Stream*> strms(n_strms);
    for (int i = 1; i <= n_strms; i++) {
        // ith arg looks like {"Name", {"Comp1", "Comp2", etc}}
        check(L, lua_istable(L, i), format("{}argument {} to be a table with 2 elements.", msg, i));
        check(L, lua_rawlen(L, i) == 2, format("{}argument {} to be a table with 2 elements.", msg, i));
        lua_pushvalue(L, i);    // Push ith arg
        string strm_name = get_string_elem(L, 1, format("{}element 1 of argument {} to be a string.", msg, i));

        check(L, lua_rawgeti(L, i, 2) == LUA_TTABLE,
            format("{}element 2 of argument {} to be a table.", msg, i)); // Push elem 2 of ith arg, e.g., {"Comp1", "Comp2"}
        int n_comps = lua_rawlen(L, -1);
        check(L, n_comps > 0, format("{}at least one component in element 2 of argument {}.", msg, i));
        vector<string> comps(n_comps);
        for (int j = 1; j <= n_comps; j++)
            comps[j - 1] = get_string_elem(L, j, format("{}component {} in element 2 of argument {} to be a string.", msg, j, i));

        lua_pop(L, 2); // Pop ith arg and elem 2 of ith arg
        strms[i - 1] = FS->add_stream(strm_name, std::move(comps));
    }

    // Push pointers to the created streams.
    for (int i = 1; i <= n_strms; i++)
        push_pointer<Stream>(L, strms[i - 1]);

    return n_strms;
}

int flowsheet(lua_State* L) {
    checkFS(L);
    const string msg {"Flowsheet: expected "};
    int n_args = lua_gettop(L);
    check(L, n_args < 2, format("{}0 or 1 argument, got {}.", msg, n_args));
    if (n_args == 0) {
        push_pointer<Flowsheet>(L, FS);
        return 1;
    }
    check(L, lua_isstring(L, 1) || lua_isuserdata(L, 1), msg + "argument to be a string or a Flowsheet.");
    if (lua_isstring(L, 1)) {
        string fs_name = lua_tostring(L, 1);
        auto fs = FS->add_flowsheet(fs_name);
        FS = fs;
        push_pointer<Flowsheet>(L, fs);
        return 1;
    }
    else if (lua_isuserdata(L, 1)) {
        FS = get_typed_ptr<Flowsheet>(L, 1, msg + "argument to be a string or a Flowsheet.");
        return 0;
    }
    return 0;
}

int is_same_ptr(lua_State* L) {
    const string msg {"Same: expected "};
    int n_args = lua_gettop(L);
    check(L, n_args == 2, format("{}2 arguments, got {}.", msg, n_args));
    TypedPtr* tp1 {}, *tp2 {};
    if (lua_isuserdata(L, 1)) {
        tp1 = static_cast<TypedPtr*>(lua_touserdata(L, 1));
        check(L, tp1, msg + "argument 1 to be an object (e.g., Variable, Flowsheet, Objective, etc.).");        
    }
    if (lua_isuserdata(L, 2)) {
        tp2 = static_cast<TypedPtr*>(lua_touserdata(L, 2));
        check(L, tp2, msg + "argument 2 to be an object (e.g., Variable, Flowsheet, Objective, etc.).");        
    }
    bool is_same = (tp1->ptr == tp2->ptr);
    lua_pushboolean(L, is_same);
    return 1;
}

int add_bridge(lua_State* L) {
    checkM(L);
    const string msg {"Bridge: expected "};
    int n_args = lua_gettop(L);
    check(L, n_args == 2, format("{}2 arguments, got {}.", msg, n_args));
    Stream* strm_from {}, *strm_to {};
    strm_from = get_typed_ptr<Stream>(L, 1, msg + "argument 1 to be a Stream.");
    strm_to   = get_typed_ptr<Stream>(L, 2, msg + "argument 2 to be a Stream.");
    bool ok = M->add_bridge(strm_from, strm_to);
    lua_pushboolean(L, ok);
    return 1;
}

int add_prices(lua_State* L) {
    checkM(L);
    const string msg {"Prices: expected "};
    auto n_args = lua_gettop(L);
    check(L, n_args >= 1, format("{}at least 1 argument, got {}.", msg, n_args));
    vector<Price*> prices(n_args);
    for (lua_Unsigned i = 1; i <= n_args; i++) {
        check(L, lua_istable(L, i), format("{}argument {} to be a table.", msg, i));
        auto n_elem = lua_rawlen(L, i);    // number of elements in the ith arg, where arg = {price_name, value, unit}
        check(L, n_elem == 3, format("{}argument {} to look like {{string, number, Unit}}.", msg, i));
        
        lua_pushvalue(L, i);               // push ith arg
        auto price_name = get_string_elem(L, 1, format("{}argument {} to look like {{string, number, Unit}}.", msg, i));
        auto value      = get_double_elem(L, 2, format("{}argument {} to look like {{string, number, Unit}}.", msg, i));
        auto unit       = get_typed_ptr_elem<Unit>(L, 3, format("{}argument {} to look like {{string, number, Unit}}.", msg, i));
        lua_pop(L, 1);                     // pop ith arg

        auto p = M->add_price(price_name, value, unit);
        prices[i - 1] = p;
    }

    // Push pointers to the created prices.
    for (int i = 1; i <= n_args; i++)
        push_pointer<Price>(L, prices[i - 1]);

    return n_args;
}

int set_objective(lua_State* L) {
    checkM(L);
    check(L, lua_gettop(L) == 1, "SetObjective: expected 1 argument that is one of: (nil, Objective, name of an Objective).");
    if (lua_isnil(L, 1))
        M->obj = nullptr;
    else if (lua_isuserdata(L, 1))
        M->obj = get_typed_ptr<Objective>(L, 1, "SetObjective: expected argument to be one of: (nil, Objective, name of an Objective).");
    else if (lua_isstring(L, 1)) {
        string obj_name = lua_tostring(L, 1);
        check(L, M->objectives.contains(obj_name), format("SetObjective: expected argument \"{}\" to be a name of an existing Objective", obj_name));
        M->obj = M->objectives[obj_name].get();
    }
    else
        luaL_error(L, "SetObjective: expected argument to be one of: (nil, Objective, name of an Objective).");

    return 0;
}

int add_objective(lua_State* L) {
    checkM(L);
    const string msg {"Objective: expected "};
    auto n_args = lua_gettop(L);
    check(L, n_args > 1, msg + "at least two arguments.");
    int n_start = 1;

    // arg 1 is the objective name or an existing Objective.
    check(L, lua_isstring(L, 1) || lua_isuserdata(L, 1), msg + "argument 1 to be a string or an Objective.");
    Objective* obj {nullptr};
    string obj_name;
    if (lua_isstring(L, 1)) {
        obj_name = lua_tostring(L, 1);
        if (M->objectives.contains(obj_name))
            obj = M->objectives[obj_name].get();
    }
    else if (lua_isuserdata(L, 1)) {
        obj = get_typed_ptr<Objective>(L, 1, msg + "argument 1 to be an Objective.");
    }
    n_start++;

    if (obj == nullptr) {
        // arg 2 is a unit string or a Unit. 
        check(L, lua_isstring(L, 2) || lua_isuserdata(L, 2), msg + "argument 2 to be a unit string or a Unit.");
        Unit* obj_unit;
        if (lua_isstring(L, 2)) {
            string unit_str = lua_tostring(L, 2);
            check(L, M->unit_set.units.contains(unit_str), msg + unit_str + " to be a valid unit string.");
            obj_unit = M->unit_set.units[unit_str].get();
        }
        else {
            obj_unit = get_typed_ptr<Unit>(L, 2, msg + "argument 2 to be a valid Unit.");
        }
        n_start++;

        // If arg3 is a number, it's the objective scale factor.
        double obj_scale {1.0};
        if (n_args > 2 && lua_isnumber(L, 3)) {
            obj_scale = lua_tonumber(L, 3);
            n_start++;
        }

        // Create a new objective function.
        obj = M->add_objective(obj_name, obj_unit, obj_scale);
    }

    // Remaining args are either ObjTerms or already created Objectives.
    int n_terms = n_args - n_start + 1;
    vector<std::variant<ObjTerm*, Objective*>> terms(n_terms);
    for (lua_Unsigned i = n_start; i <= n_args; i++) {
        // ith arg is either a table or an Objective
        if (lua_istable(L, i)) {
            const string msg2 {format("{}argument {} to look like {{string, Variable, Price, Unit, <number>}}.", msg, i)};
            auto n_elem = lua_rawlen(L, i);    // number of elements in the ith arg, where arg = {term_name, Variable, Price, Unit, <scale>}
            check(L, n_elem == 4 || n_elem == 5, msg2);
            lua_pushvalue(L, i);               // push ith arg 
            
            auto term_name = get_string_elem(L, 1, msg2);   // element 1 is term name

            Variable* var;
            if (lua_rawgeti(L, -1, 2) == LUA_TSTRING) {     // element 2 is a variable name or a Variable
                lua_pop(L, 1);
                auto var_name = get_string_elem(L, 2, msg2);
                check(L, M->x_map.contains(var_name), msg2);
                var = M->x_map[var_name];
            }
            else
                var = get_typed_ptr_elem<Variable>(L, 2, msg2);

            Price* price;
            if (lua_rawgeti(L, -1, 3) == LUA_TSTRING) {     // element 3 is a price name or a Price
                lua_pop(L, 1);
                auto price_name = get_string_elem(L, 3, msg2);
                check(L, M->prices.contains(price_name), msg2);
                price = M->prices[price_name].get();
            }
            else
                price = get_typed_ptr_elem<Price>(L, 3, msg2);

            Unit* unit;
            if (lua_rawgeti(L, -1, 4) == LUA_TSTRING) {     // element 4 is a unit string or a Unit
                lua_pop(L, 1);
                auto unit_str = get_string_elem(L, 4, msg2);
                check(L, M->unit_set.units.contains(unit_str), msg2);
                unit = M->unit_set.units[unit_str].get();
            }
            else
                unit = get_typed_ptr_elem<Unit>(L, 4, msg2);

            double scale {1.0};
            if (n_elem == 5)                                // element 5, if present, is a scale factor
                scale = get_double_elem(L, 5, msg2);

            lua_pop(L, 1);                     // pop ith arg

            terms[i - n_start] = obj->add_objterm(term_name, var, price, unit, scale);
        }
        else {
            // ith arg is an Objective or a name of an Objective
            Objective* child_obj {};
            if (lua_isuserdata(L, i))
                child_obj = get_typed_ptr<Objective>(L, i, format("{}argument {} to be an Objective.", msg, i));
            else if (lua_isstring(L, i)) {
                string child_obj_name = lua_tostring(L, i);
                check(L, M->objectives.contains(child_obj_name), format("{}argument {} \"{}\"to be the name of an existing Objective.", msg, i, child_obj_name));
                child_obj = M->objectives[child_obj_name].get();
            }
            else
                luaL_error(L, format("{}argument {} to be an Objective or the name of an Objective.", msg, i).c_str());

            obj->add_objective(child_obj);
            terms[i - n_start] = child_obj;
        }         
    }

    // Push a pointer to the objective.
    push_pointer<Objective>(L, obj);

    // Push pointers to the objective terms.
    for (int i = 0; i < n_terms; i++)
        if (std::holds_alternative<ObjTerm*>(terms[i]))
            push_pointer<ObjTerm>(L, std::get<ObjTerm*>(terms[i]));
        else
            push_pointer<Objective>(L, std::get<Objective*>(terms[i]));

    return n_terms + 1;
}

int create_model(lua_State* L) {
    const string msg_expected {"Model: expected "};
    const string msg_kinds {"Model: in the \"kinds\" table, expected "};
    const string msg_units {"Model: in the \"units\" table, expected "};
    auto n_args = lua_gettop(L);
    check(L, n_args == 4, format("{}4 arguments, got {}.", msg_expected, n_args));
    check(L, lua_isstring(L, 1) && !lua_isnumber(L, 1), msg_expected + "argument 1 to be a string.");  // arg 1 is the model name
    check(L, lua_isstring(L, 2) && !lua_isnumber(L, 2), msg_expected + "argument 2 to be a string.");  // arg 2 is the index flowsheet name
    check(L, lua_istable(L, 3),  msg_expected + "argument 3 to be a table.");  // arg 3 is a kinds table
    check(L, lua_istable(L, 4),  msg_expected + "argument 4 to be a table.");  // arg 4 is a units table

    UnitSet u {};

    string name = lua_tostring(L, 1);   // Model name
    check(L, !name.empty(), msg_expected + "argument 1 to be a non-empty string.");

    string index_fs_name = lua_tostring(L, 2);  // Index Flowsheet name
    check(L, !index_fs_name.empty(), msg_expected + "argument 2 to be a non-empty string.");

    // kinds table:
    lua_pushnil(L);                             // push a nil key to start
    while (lua_next(L, 3) != 0) {               // pops the key, then pushes next key-value pair
        string kind_str = lua_tostring(L, -2);  // key is kind_str
        check(L, lua_istable(L, -1), format("{}key \"{}\" to reference a table.", msg_kinds, kind_str)); // value is table with 1 or 2 strings

        auto n_str = lua_rawlen(L, -1);         // number of strings in the table
        check(L, n_str > 0 && n_str < 3, format("{}1 or 2 strings in the \"{}\" definition.", msg_kinds, kind_str));

        string base_unit_str = get_string_elem(L, 1,
            format("{}element 1 in the \"{}\" definition to be a string.", msg_kinds, kind_str)); // element 1 is base_unit_str

        string default_unit_str = (n_str == 2 ? get_string_elem(L, 2,
            format("{}element 2 in the \"{}\" definition to be a string.", msg_kinds, kind_str)) : base_unit_str); // element 2 (optional) is default_unit_str

        lua_pop(L, 1);  // pop the value (the {base_unit_str, default_unit_str} table)
        u.add_kind(kind_str, base_unit_str, default_unit_str);
    }

    // units table:
    lua_pushnil(L);                             // push a nil key to start
    while (lua_next(L, 4) != 0) {               // pops the key, then pushes next key-value pair
        string kind_str = lua_tostring(L, -2);  // key is kind_str
        check(L, lua_istable(L, -1), format("{}key \"{}\" to reference a table.", msg_units, kind_str)); // value is a table of n_units tables
        auto n_units = lua_rawlen(L, -1);       // value is a list of units for this kind

        for (lua_Unsigned i = 1; i <= n_units; i++) {
            check(L, lua_rawgeti(L, -1, i) == LUA_TTABLE, format("{}unit {} in \"{}\" definition to be a list.", msg_units, i, kind_str)); // push the ith unit list
            auto n_elem = lua_rawlen(L, -1);    // number of elements in the ith unit list
            check(L, n_elem == 2 || n_elem == 3, format("{}unit {} in \"{}\" definition to look like {{string, number, number}}.", msg_units, i, kind_str));

            string unit_str = get_string_elem(L, 1, format("{}unit {} in \"{}\" definition to look like {{string, number, number}}.", msg_units, i, kind_str));     // element 1 is unit_str
            double unit_ratio = get_double_elem(L, 2, format("{}unit {} in \"{}\" definition to look like {{string, number, number}}.", msg_units, i, kind_str));   // element 2 is unit_ratio
            double unit_offset = (n_elem == 3 ? 
                get_double_elem(L, 3, format("{}unit {} in \"{}\" definition to look like {{string, number, number}}.", msg_units, i, kind_str)) : 0.0); // element 3 (optional) is unit_offset

            lua_pop(L, 1);  // pop the ith unit list
            u.add_unit(unit_str, kind_str, unit_ratio, unit_offset);
        }

        lua_pop(L, 1);  // pop the value (a list of unit lists) of the current key-value pair
    }

    M = make_unique<Model>(name, index_fs_name, std::move(u));
    FS = M->index_fs.get();

    // Push pointers to the Model and the index Flowsheet.
    push_pointer<Model>(L, M.get());
    push_pointer<Flowsheet>(L, FS);

    return 2;
}

void call_lua_function(const string& func_name) {
    auto L = scripter_lua_state;
    lua_getglobal(L, func_name.c_str());
    if (lua_pcall(L, 0, 0, 0) != LUA_OK)
        luaL_error(L, format("{}: {}", func_name, lua_tostring(L, -1)).c_str());
}

LuaResult run_lua_script(lua_State* L, const char* script_file_name) {
    int result = luaL_dofile(L, script_file_name);
    
    if (result != LUA_OK) {
        string err_str = lua_tostring(L, -1);
        lua_pop(L, 1);
        lua_close(L);
        scripter_lua_state = nullptr;
        return {.ok = false, .err_str = err_str};
    }
    return {.ok = true, .err_str = ""};
}

lua_State* start_lua() {
    auto L = luaL_newstate();
    if (!L) return nullptr;
    luaL_openlibs(L);
    scripter_lua_state = L;

    lua_register(L, "Model",             create_model);
    lua_register(L, "Flowsheet",         flowsheet);
    lua_register(L, "Streams",           add_streams);
    lua_register(L, "Eval",              eval_expr);
    lua_register(L, "Init",              initialize);
    lua_register(L, "ShowModel",         show_summary);
    lua_register(L, "ShowVariables",     show_variables);
    lua_register(L, "ShowConstraints",   show_constraints);
    lua_register(L, "ShowJacobian",      show_jacobian);
    lua_register(L, "ShowHessian",       show_hessian);
    lua_register(L, "ShowConnections",   show_connections);
    lua_register(L, "ShowPrices",        show_prices);
    lua_register(L, "ShowObjective",     show_objective);
    lua_register(L, "ShowObjGrad",       show_obj_grad);
    lua_register(L, "Val",               get_value);
    lua_register(L, "BaseVal",           get_base_value);
    lua_register(L, "LB",                get_lower);
    lua_register(L, "UB",                get_upper);
    lua_register(L, "Spec",              get_spec);
    lua_register(L, "Var",               get_var);
    lua_register(L, "Unit",              get_unit);
    lua_register(L, "ChangeUnit",        change_unit);
    lua_register(L, "SetValue",          set_value);
    lua_register(L, "SolverOption",      set_solver_option);
    lua_register(L, "Solve",             solve_model);
    lua_register(L, "InitSolver",        initialize_solver);
    lua_register(L, "EvalConstraints",   eval_constraints);
    lua_register(L, "EvalJacobian",      eval_jacobian);
    lua_register(L, "EvalHessian",       eval_hessian);
    lua_register(L, "EvalObjective",     eval_objective);
    lua_register(L, "EvalObjGrad",       eval_obj_grad);
    lua_register(L, "Variables",         add_variables);
    lua_register(L, "Constraints",       add_constraints);
    lua_register(L, "JacobianNZs",       add_jacobian_nzs);
    lua_register(L, "Connect",           connect);
    lua_register(L, "ConnectAll",        connect_streams);
    lua_register(L, "Same",              is_same_ptr);
    lua_register(L, "Bridge",            add_bridge);
    lua_register(L, "Prices",            add_prices);
    lua_register(L, "Objective",         add_objective);
    lua_register(L, "SetObjective",      set_objective);

    lua_register(L, "Mixer",             add_Mixer);
    lua_register(L, "Splitter",          add_Splitter);
    lua_register(L, "Separator",         add_Separator);
    lua_register(L, "YieldReactor",      add_YieldReactor);
    lua_register(L, "MultiYieldReactor", add_MultiYieldReactor);
    lua_register(L, "Calc",              add_Calc);

    return L;
}
