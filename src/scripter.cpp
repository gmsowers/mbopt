#include <sstream>
#include <fstream>
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
#include "StoicReactor.hpp"

static unique_ptr<Model> M;
static Flowsheet*        FS;
static ostream*          OUT {&cout};
static ofstream          OUTFILE;
static lua_State*        scripter_lua_state;

static IpoptApplication* solver = IpoptApplicationFactory();

static string const errM {"a valid Model."};
static string const errFS {"a valid Flowsheet."};

void check(lua_State* L, const bool cond, string_view err_msg) {
    if (!cond) luaL_error(L, err_msg.data());
}

void checkM(lua_State* L, string_view err_msg) {
    if (!M) luaL_error(L, err_msg.data());
}

void checkFS(lua_State* L, string_view err_msg) {
    if (!FS) luaL_error(L, err_msg.data());
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
    string const msg {"Solve: expected "};
    checkM(L, msg + errM);
    auto n_args = lua_gettop(L);
    check(L, n_args <= 1, format("{}0 or 1 argument, got {}.", msg, n_args));
    if (n_args == 1)
        M->obj = get_typed_ptr<Objective>(L, 1, msg + "argument to be an Objective.");
    int retval = solver->OptimizeTNLP(M.get());
    lua_pushinteger(L, retval);
    return 1;
}

int delegate(lua_State* L, string_view f_name, auto f) {
    checkM(L, format("{}: expected {}", f_name, errM));
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
                } else if (tp->subtype_idx == typeid(MultiYieldReactor)) {
                    auto p = static_cast<MultiYieldReactor*>(tp->ptr);
                    if (p) f(p);
                } else if (tp->subtype_idx == typeid(StoicReactor)) {
                    auto p = static_cast<StoicReactor*>(tp->ptr);
                    if (p) f(p);
                }
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
    return delegate(L, "EvalConstraints", [](auto* p) { p->eval_constraints(); });
}

int eval_jacobian(lua_State* L) {
    return delegate(L, "EvalJacobian", [](auto* p) { p->eval_jacobian(); });
}

int eval_hessian(lua_State* L) {
    return delegate(L, "EvalHessian", [](auto* p) { p->eval_hessian(); });
}

int eval_objective(lua_State* L) {
    string const msg {"EvalObjective: expected "};
    checkM(L, msg + errM);
    auto n_args = lua_gettop(L);
    check(L, n_args <= 1, format("{}0 or 1 argument, got {}.", msg, n_args));
    if (n_args == 0) {
        lua_pushnumber(L, M->eval_objective());
        return 1;
    }
    auto obj = get_typed_ptr<Objective>(L, 1, msg + "argument 1 to be an Objective.");
    lua_pushnumber(L, obj->eval());
    return 1;

}

int eval_obj_grad(lua_State* L) {
    string const msg {"EvalObjGrad: expected "};
    checkM(L, msg + errM);
    check(L, lua_gettop(L) == 0, msg + "no arguments.");
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
    check(L, solver != nullptr, msg + "solver has not been created.");
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

int get_variable_attr(lua_State* L, string_view f_name, auto get_attr) {
    std::pair<Ndouble, string> res {std::nullopt, {}};
    checkM(L, format("{}: expected {}", f_name, errM));
    check(L, lua_gettop(L) == 1, format("{}: expected 1 argument.", f_name));
    if (lua_isuserdata(L, -1)) {
        auto tp = static_cast<TypedPtr*>(lua_touserdata(L, -1));
        check(L, tp != nullptr && tp->type_idx == typeid(Variable), format("{}: expected argument to be a Variable.", f_name));
        auto p = static_cast<Variable*>(tp->ptr);
        res = {get_attr(p), p->unit->str};
    }
    else if (lua_isstring(L, -1)) {
        string name = lua_tostring(L, -1);
        check(L, M->x_map.contains(name), format("{}: Variable named \"{}\" not found.", f_name, name));
        res = {get_attr(M->x_map[name]), M->x_map[name]->unit->str};
    }
    else
        luaL_error(L, format("{}: expected argument to be a Variable or a string.", f_name).c_str());

    if (std::get<0>(res).has_value()) {
        lua_pushnumber(L, std::get<0>(res).value());
        lua_pushstring(L, std::get<1>(res).c_str());
        return 2;
    }
    else
        return 0;
}

int get_value(lua_State* L) {
    return get_variable_attr(L, "Val", [](auto* p){ return p->value; });
}

int get_base_value(lua_State* L) {
    return get_variable_attr(L, "BaseVal", [](auto* p){ return p->convert_to_base(); });
}

int get_lower(lua_State* L) {
    return get_variable_attr(L, "LB", [](auto* p){ return p->lower; });
}

int get_upper(lua_State* L) {
    return get_variable_attr(L, "UB", [](auto* p){ return p->upper; });
}

int get_spec(lua_State* L) {
    string const msg {"Spec: expected "};
    checkM(L, msg + errM);
    check(L, lua_gettop(L) == 1, msg + "1 argument, a Variable or a string.");
    if (lua_isstring(L, -1)) {
        string name = lua_tostring(L, -1);
        check(L, M->x_map.contains(name), format("{}\"{}\" to be a valid Variable name.", msg, name));
        lua_pushstring(L, M->x_map[name]->spec == VariableSpec::Fixed ? "Fixed" : "Free");
        return 1;
    }
    else if (lua_isuserdata(L, -1)) {
        auto tp = static_cast<TypedPtr*>(lua_touserdata(L, -1));
        check(L, tp != nullptr && tp->type_idx == typeid(Variable), msg + "argument to be a Variable.");
        auto p = static_cast<Variable*>(tp->ptr);
        lua_pushstring(L, p->spec == VariableSpec::Fixed ? "Fixed" : "Free");
        return 1;
    }
    else
        luaL_error(L, format("{}argument to be a Variable or a string.", msg).c_str());

    return 0;
}

int get_var(lua_State* L) {
    string const msg {"Spec: expected "};
    checkM(L, msg + errM);
    check(L, lua_gettop(L) == 1 && lua_isstring(L, -1), msg + "1 argument, a string.");
    string name = lua_tostring(L, -1);
    check(L, M->x_map.contains(name), format("{}expected \"{}\" to be a valid Variable name.", msg, name));
    push_pointer<Variable>(L, M->x_map[name]);
    return 1;
}

int get_unit(lua_State* L) {
    string const msg {"Unit: expected "};
    checkM(L, msg + errM);
    check(L, lua_gettop(L) == 1, msg + "1 argument, one of: Variable, Price, Objective, string.");
    if (lua_isstring(L, 1)) {
        string arg = lua_tostring(L, 1);
        if (M->x_map.contains(arg)) {   // Variable name
            push_pointer<Unit>(L, M->x_map[arg]->unit);
            return 1;
        }
        else if (M->unit_set.units.contains(arg)) {     // Unit string
            push_pointer<Unit>(L, M->unit_set.units[arg].get());
            return 1;
        }
        else if (M->prices.contains(arg)) {     // Price name
            push_pointer<Unit>(L, M->prices[arg]->unit);
            return 1;
        }
        else if (M->objectives.contains(arg)) {     // Objective name
            push_pointer<Unit>(L, M->objectives[arg]->unit);
            return 1;
        }
        else
            luaL_error(L, format("{}argument to be one of: Variable, Price, Objective, string.", msg).c_str());
    }
    else if (lua_isuserdata(L, 1)) {
        auto tp = static_cast<TypedPtr*>(lua_touserdata(L, 1));
        Unit* u;
        check(L, tp != nullptr, msg + "{}argument to be one of: Variable, Price, Objective, string.");
        if (tp->type_idx == typeid(Variable))
            u = static_cast<Variable*>(tp->ptr)->unit;
        else if (tp->type_idx == typeid(Price))
            u = static_cast<Price*>(tp->ptr)->unit;
        else if (tp->type_idx == typeid(Objective))
            u = static_cast<Objective*>(tp->ptr)->unit;
        else
            luaL_error(L, format("{}argument to be one of: Variable, Price, Objective, string.", msg).c_str());

        push_pointer<Unit>(L, u);
        return 1;
    }
    else
        luaL_error(L, format("{}argument to be one of: Variable, Price, Objective, string.", msg).c_str());

    return 0;

}

int change_unit(lua_State* L) {
    string const msg {"ChangeUnit: expected "};
    checkM(L, msg + errM);
    check(L, lua_gettop(L) == 2, msg + "2 arguments, a Variable/Price/Objective and a Unit.");
    string unit_str;
    if (lua_isstring(L, 2))
        unit_str = lua_tostring(L, 2);
    else if (lua_isuserdata(L, 2)) {
        auto tp = static_cast<TypedPtr*>(lua_touserdata(L, 2));
        check(L, tp != nullptr, msg + "{}argument 2 to be a Unit or a string.");
        if (tp->type_idx == typeid(Unit))
            unit_str = static_cast<Unit*>(tp->ptr)->str;
        else
            luaL_error(L, format("{}argument 2 to be a Unit or a string.", msg).c_str());
    }
    else
        luaL_error(L, format("{}argument 2 to be a Unit or a string.", msg).c_str());

    if (lua_isstring(L, 1)) {
        string name = lua_tostring(L, 1);
        if (M->x_map.contains(name)) {
            Ndouble retval = M->x_map[name]->change_unit(M.get(), unit_str);
            if (retval.has_value()) {
                lua_pushnumber(L, retval.value());
                return 1;
            }
        } else if (M->prices.contains(name)) {
            Ndouble retval = M->prices[name]->change_unit(M.get(), unit_str);
            if (retval.has_value()) {
                lua_pushnumber(L, retval.value());
                return 1;
            }
        } else if (M->objectives.contains(name)) {
            Ndouble retval = M->objectives[name]->change_unit(M.get(), unit_str);
            if (retval.has_value()) {
                lua_pushnumber(L, retval.value());
                return 1;
            }
        }
        else
            luaL_error(L, format("{}argument 1 to be a Variable, Price, or Objective.", msg).c_str());
    }
    else if (lua_isuserdata(L, 1)) {
        auto tp = static_cast<TypedPtr*>(lua_touserdata(L, 1));
        check(L, tp != nullptr, format("{}argument 1 to be a Variable, Price, or Objective.", msg));
        if (tp->type_idx == typeid(Variable)) {
            auto p = static_cast<Variable*>(tp->ptr);
            Ndouble retval = p->change_unit(M.get(), unit_str);
            if (retval.has_value()) {
                lua_pushnumber(L, retval.value());
                return 1;
            }
        }
        else if (tp->type_idx == typeid(Price)) {
            auto p = static_cast<Price*>(tp->ptr);
            Ndouble retval = p->change_unit(M.get(), unit_str);
            if (retval.has_value()) {
                lua_pushnumber(L, retval.value());
                return 1;
            }
        }
        else if (tp->type_idx == typeid(Objective)) {
            auto p = static_cast<Objective*>(tp->ptr);
            Ndouble retval = p->change_unit(M.get(), unit_str);
            if (retval.has_value()) {
                lua_pushnumber(L, retval.value());
                return 1;
            }
        }
        else
            luaL_error(L, format("{}argument 1 to be a Variable, Price, or Objective.", msg).c_str());
    }
    else
        luaL_error(L, format("{}argument 1 to be a Variable, Price, or Objective.", msg).c_str());
        
    return 0;

}

int set_value(lua_State* L) {
    const string msg {"SetValue: expected "};
    checkM(L, msg + errM);
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
    return delegate(L, "ShowConstraints", [](auto* p) { p->show_constraints(*OUT); });
}

int show_variables(lua_State* L) {
    return delegate(L, "ShowVariables", [](auto* p) { p->show_variables(*OUT); });
}

int show_jacobian(lua_State* L) {
    return delegate(L, "ShowJacobian", [](auto* p) { p->show_jacobian(*OUT); });
}

int show_hessian(lua_State* L) {
    return delegate(L, "ShowHessian", [](auto* p) { p->show_hessian(*OUT); });
}

int show_model(lua_State* L) {
    return delegate(L, "ShowModel", [](auto* p) { p->show_model(*OUT); });
}

int show_connections(lua_State* L) {
    string const msg {"ShowConnections: expected "};
    checkM(L, msg + errM);
    check(L, lua_gettop(L) == 0, msg + "no arguments.");
    M->show_connections(*OUT);
    return 0;
}

int show_prices(lua_State* L) {
    string const msg {"ShowPrices: expected "};
    checkM(L, msg + errM);
    check(L, lua_gettop(L) == 0, msg + "no arguments.");
    M->show_prices(*OUT);
    return 0;
}

int show_objective(lua_State* L) {
    string const msg {"ShowObjective: expected "};
    checkM(L, msg + errM);
    auto n_args = lua_gettop(L);
    check(L, n_args <= 1, "0 or 1 argument.");
    if (n_args == 0) {
        M->show_objective(M->obj, *OUT);
        return 0;
    }
    auto obj = get_typed_ptr<Objective>(L, 1, msg + "argument 1 to be an Objective.");
    M->show_objective(obj, *OUT);
    return 0;
}

int show_obj_grad(lua_State* L) {
    string const msg {"ShowObjGrad: expected "};
    checkM(L, msg + errM);
    check(L, lua_gettop(L) == 0, msg + "no arguments.");
    M->show_obj_grad(*OUT);
    return 0;
}

int show_units(lua_State* L) {
    string const msg {"ShowUnits: expected "};
    auto n_args = lua_gettop(L);
    check(L, n_args <= 1, "0 or 1 argument.");
    if (n_args == 0) {
        checkM(L, msg + errM);
        M->show_units(*OUT);
        return 0;
    }
    auto unitset = get_typed_ptr<UnitSet>(L, 1, msg + "argument 1 to be a UnitSet.");
    unitset->show_units(*OUT);
    return 0;
}

int initialize(lua_State* L) {
    return delegate(L, "Init", [](auto* p) { p->initialize(); });
}

int connect(lua_State* L) {
    const string msg {"Connect: expected "};
    checkM(L, msg + errM);
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
    string const msg {"ConnectAll: expected "};
    checkM(L, msg + errM);
    check(L, lua_gettop(L) == 0, msg + "no arguments.");
    bool ok = M->index_fs->connect_streams();
    lua_pushboolean(L, ok);
    return 1;
}

const std::regex re_binop(R"((\S+)(=|<|>)([^\s_]+)(?:_(\S+))?)");
const std::regex re_spec(R"((fix|free)\s+(\S+))");

int eval_expr(lua_State* L) {
    string const msg {"Eval: expected "};
    checkM(L, msg + errM);
    auto line_no = get_line_no(L);
    string script_name {get_script_name(L) + ", "};
    auto n_args = lua_gettop(L);
    check(L, n_args == 1, format("{}1 argument, got {}.", msg, n_args));
    check(L, lua_isstring(L, 1), msg + "the argument to be a string.");

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
                cerr << format("{}line {}: in Eval expression \"{}\", the variable \"{}\" is not in the model.\n", script_name, line_no, expr_ts, lhs);
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
                        cerr << format("{}line {}: in Eval expression \"{}\", the right-hand side unit \"{}\" is not in the units table.\n",
                            script_name, line_no, expr_ts, rhs_unit_str);
                        continue;
                    }
                    rhs_value = lhs_var->convert(rhs_value, rhs_unit);
                }
            }
            else {
                ok = false;
                cerr << format("{}line {}: the right-hand side \"{}\" of Eval expression \"{}\" is invalid.\n", script_name, line_no, rhs, expr_ts);
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
                cerr << format("{}line {}: in Eval expression \"{}\", the variable \"{}\" is not in the model.\n", script_name, line_no, expr_ts, rhs);
                continue;
            }
            
            if (lhs == "free")
                rhs_var->free();
            else 
                rhs_var->fix();
        }
        else {
            ok = false;
            cerr << format("{}line {}: Eval expression \"{}\" is invalid.\n", script_name, line_no, expr_ts);
        }
    }
    
    lua_pushboolean(L, ok);
    return 1;
}

void start_Block(lua_State* L, const string& blk_type, string& blk_name, vector<Stream*>& inlets, vector<Stream*>& outlets) {
    const string msg {blk_type + ": expected "};
    checkFS(L, msg + errFS);
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
    const string msg {blk_name + ": expected "};
    checkFS(L, msg + errFS);
    auto blk_p = FS->add_block<T>(blk_name, std::move(inlets), std::move(outlets), blk_params...);
    push_pointer<Block, T>(L, blk_p);
    return 1;
}

template <typename T>
int add_Block(lua_State* L, const string& blk_type) {
    string blk_name;
    vector<Stream*> inlets, outlets;
    start_Block(L, blk_type, blk_name, inlets, outlets);
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
    start_Block(L, "YieldReactor", blk_name, inlets, outlets);
    return finish_Block<YieldReactor>(L, blk_name, inlets, outlets);
}

int add_MultiYieldReactor(lua_State* L) {
    string blk_name;
    vector<Stream*> inlets, outlets;
    start_Block(L, "MultiYieldReactor", blk_name, inlets, outlets);

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

    return finish_Block<MultiYieldReactor, const string&, const vector<string>&>(L, blk_name, inlets, outlets, reactor_name, feed_names);
}

int add_StoicReactor(lua_State* L) {
    string blk_name;
    vector<Stream*> inlets, outlets;
    start_Block(L, "StoicReactor", blk_name, inlets, outlets);

    const string msg {"StoicReactor: expected "};

    auto u_mw_def = M->unit_set.get_default_unit("molewt");
    Unit* u_mw;

    // Get table of molecular weights.
    check(L, lua_istable(L, 4) && lua_rawlen(L, 4) > 0,
        msg + "argument 4 to be a table of at least one molecular weight specification.");
    auto n_mw = lua_rawlen(L, 4);
    unordered_map<string, Quantity> mw {};
    for (lua_Unsigned i = 1; i <= n_mw; i++) {
        check(L, lua_rawgeti(L, 4, i) == LUA_TTABLE,
            format("{}element {} of argument 4 to be a table.", msg, i)); // Push elem i, e.g., {"H2", 2.0}
        int n_elem = lua_rawlen(L, -1);
        check(L, n_elem == 2 || n_elem == 3,
            format("{}element {} of argument 4 to look like {{string, number, <Unit>}}.", msg, i));
        string comp_name = get_string_elem(L, 1,
            format("{}element {} of argument 4 to look like {{string, number, <Unit>}}.", msg, i));
        double mw_val = get_double_elem(L, 2,
            format("{}element {} of argument 4 to look like {{string, number, <Unit>}}.", msg, i));
        if (n_elem == 3) {
            if (lua_rawgeti(L, -1, 3) == LUA_TSTRING) {
                string u_str = lua_tostring(L, -1);
                u_mw = M->unit_set.units[u_str].get();
                lua_pop(L, 1);
                check(L, u_mw, 
                    format("{}element {} of argument 4 to look like {{string, number, <Unit>}}.", msg, i));
            } else {
                u_mw = get_typed_ptr_elem<Unit>(L, 3,
                    format("{}element {} of argument 4 to look like {{string, number, <Unit>}}.", msg, i));
            }
        }
        else
            u_mw = u_mw_def;

        mw[comp_name] = {mw_val, u_mw};
        lua_pop(L, 1);   // Pop elem i
    }

    // Get table of stoichiometric coefficients.
    check(L, lua_istable(L, 5) && lua_rawlen(L, 5) > 0,
        msg + "argument 5 to be a table of stoichiometric coefficients.");
    auto n_rx = lua_rawlen(L, 5);
    vector<unordered_map<string, double>> stoic_coef(n_rx);
    for (lua_Unsigned i = 1; i <= n_rx; i++) {
        check(L, lua_rawgeti(L, 5, i) == LUA_TTABLE,     // Push reaction i, e.g., { {"H2", -1.0}, {"C2H2", -1.0}, {"C2H4", 1.0} }
            format("{}element {} of argument 5 to look like {{{{string, number}}, {{string, number}}, etc.}}.", msg, i));
        int n_coef = lua_rawlen(L, -1);
        if (n_coef == 0) continue;
        for (lua_Unsigned j = 1; j <= n_coef; j++) {
            check(L, lua_rawgeti(L, -1, j) == LUA_TTABLE,   // Push stoic coeff j, e.g., {"H2", -1.0}
                format("{}element {} of argument 5 to look like {{{{string, number}}, {{string, number}}, etc.}}.", msg, i));
            string comp_name = get_string_elem(L, 1,
                format("{}element {} of argument 5 to look like {{{{string, number}}, {{string, number}}, etc.}}.", msg, i));
            double coef = get_double_elem(L, 2,
                format("{}element {} of argument 5 to look like {{{{string, number}}, {{string, number}}, etc.}}.", msg, i));
            lua_pop(L, 1);   // Pop stoic coef j
            stoic_coef[i - 1][comp_name] = coef;
        }
        lua_pop(L, 1);  // Pop reaction i
    }
    
    // Get table of conversion specs.
    check(L, lua_istable(L, 6) && lua_rawlen(L, 6) > 0,
        msg + "argument 6 to be a table of at least one conversion specification.");
    auto n_keys = lua_rawlen(L, 6);
    check(L, n_keys == n_rx, 
        format("{}{} conversion specifications, got {}.", msg, n_rx, n_keys));
    vector<string> conversion_keys(n_keys);
    for (lua_Unsigned i = 1; i <= n_keys; i++)
        conversion_keys[i - 1] = get_string_elem(L, i,
            msg + "conversion specification to be a component name (a string).");

    return finish_Block<StoicReactor,
                        const unordered_map<string, Quantity>&,
                        const vector<unordered_map<string, double>>&,
                        const vector<string>&>(L, blk_name, inlets, outlets, mw, stoic_coef, conversion_keys);
}

int add_Calc(lua_State* L) {
    const string msg {"Calc: expected "};
    checkFS(L, msg + errFS);
    auto n_args = lua_gettop(L);
    check(L, n_args == 1, format("{}1 argument, got {}.", msg, n_args));
    check(L, lua_isstring(L, 1) && !lua_isnumber(L, 1), msg + "argument 1 to be a string.");

    string calc_name = lua_tostring(L, 1);               
    check(L, !calc_name.empty(), msg + "argument 1 to be a non-empty string.");

    auto calc_p = FS->add_calc(calc_name);

    push_pointer<Calc>(L, calc_p);
    return 1;
}

int add_variables(lua_State* L) {
    const string msg {"Variables: expected "};
    checkM(L, msg + errM);
    auto n_args = lua_gettop(L);
    check(L, n_args >= 2, format("{}at least 2 arguments, got {}.", msg, n_args));
    auto calc_p = get_typed_ptr<Calc>(L, 1, msg + "argument 1 to be a Calc.");
    auto n_vars = n_args - 1;
    vector<Variable*> vars(n_vars);
    for (lua_Unsigned i = 2; i <= n_args; i++) {
        check(L, lua_istable(L, i), format("{}argument {} to be a table.", msg, i));
        auto n_elem = lua_rawlen(L, i);    // number of elements in the ith argument table
        check(L, n_elem == 2, format("{}argument {} to look like {{string, Unit}}.", msg, i));

        lua_pushvalue(L, i);    // Push ith arg, where arg = {var_name, unit}
        auto var_name = get_string_elem(L, 1, format("{}argument {} to look like {{string, Unit}}.", msg, i));  // element 1 is variable name
        auto unit = get_typed_ptr_elem<Unit>(L, 2, format("{}argument {} to look like {{string, Unit}}.", msg, i));  // element 2 is variable name
        lua_pop(L, 1);          // Pop ith arg

        auto v = M->add_var(calc_p->prefix + var_name, unit);
        calc_p->x.push_back(v);
        vars[i - 2] = v;
    }

    for (int i = 1; i <= n_vars; i++)
        push_pointer<Variable>(L, vars[i - 1]);

    return n_vars;
}

int add_constraints(lua_State* L) {
    const string msg {"Constraints: expected "};
    checkM(L, msg + errM);
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

    for (int i = 1; i <= n_eqs; i++)
        push_pointer<Constraint>(L, eqs[i - 1]);

    return n_eqs;
}

int add_jacobian_nzs(lua_State* L) {
    const string msg {"JacobianNZs: expected "};
    checkM(L, msg + errM);
    auto n_args = lua_gettop(L);
    check(L, n_args >= 2, format("{}at least 2 arguments, got {}.", msg, n_args));
    auto calc_p = get_typed_ptr<Calc>(L, 1, msg + "argument 1 to be a Calc.");
    auto n_jnzs = n_args - 1;
    vector<JacobianNZ*> jnzs(n_jnzs);
    for (lua_Unsigned i = 2; i <= n_args; i++) {
        check(L, lua_istable(L, i), format("{}argument {} to be a table.", msg, i));
        auto n_elem = lua_rawlen(L, i);    // number of elements in the ith JNZ list
        check(L, n_elem == 2, format("{}argument {} to look like {{Constraint, Variable}}.", msg, i));

        lua_pushvalue(L, i);    // Push ith arg, where arg = {con, var}
        auto con = get_typed_ptr_elem<Constraint>(L, 1, format("{}argument {} to look like {{Constraint, Variable}}.", msg, i));  // element 1 is a Constraint
        auto var = get_typed_ptr_elem<Variable>(L, 2, format("{}argument {} to look like {{Constraint, Variable}}.", msg, i));  // element 2 is a Variable
        lua_pop(L, 1);          // Pop ith arg

        auto jnz = M->add_J_NZ(con, var);
        calc_p->J.push_back(jnz);
        jnzs[i - 2] = jnz;
    }

    for (int i = 1; i <= n_jnzs; i++)
        push_pointer<JacobianNZ>(L, jnzs[i - 1]);

    return n_jnzs;
}

int add_streams(lua_State* L) {
    const string msg {"Streams: expected "};
    checkFS(L, msg + errFS);
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

    for (int i = 1; i <= n_strms; i++)
        push_pointer<Stream>(L, strms[i - 1]);

    return n_strms;
}

int flowsheet(lua_State* L) {
    const string msg {"Flowsheet: expected "};
    checkFS(L, msg + errFS);
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
    const string msg {"Bridge: expected "};
    checkM(L, msg + errM);
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
    const string msg {"Prices: expected "};
    checkM(L, msg + errM);
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

    for (int i = 1; i <= n_args; i++)
        push_pointer<Price>(L, prices[i - 1]);

    return n_args;
}

int set_objective(lua_State* L) {
    string const msg {"SetObjective: expected "};
    checkM(L, msg + errM);
    check(L, lua_gettop(L) == 1, msg + "1 argument that is one of: (nil, Objective, name of an Objective).");
    if (lua_isnil(L, 1))
        M->obj = nullptr;
    else if (lua_isuserdata(L, 1))
        M->obj = get_typed_ptr<Objective>(L, 1, msg + "argument to be one of: (nil, Objective, name of an Objective).");
    else if (lua_isstring(L, 1)) {
        string obj_name = lua_tostring(L, 1);
        check(L, M->objectives.contains(obj_name), format("{}argument \"{}\" to be a name of an existing Objective.", msg, obj_name));
        M->obj = M->objectives[obj_name].get();
    }
    else
        luaL_error(L, format("{}argument to be one of: (nil, Objective, name of an Objective).", msg).c_str());

    return 0;
}

int add_objective(lua_State* L) {
    const string msg {"Objective: expected "};
    checkM(L, msg + errM);
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
    auto n_args = lua_gettop(L);
    check(L, n_args == 3, format("{}3 arguments, got {}.", msg_expected, n_args));
    check(L, lua_isstring(L, 1) && !lua_isnumber(L, 1), msg_expected + "argument 1 to be a string.");  // arg 1 is the model name
    check(L, lua_isstring(L, 2) && !lua_isnumber(L, 2), msg_expected + "argument 2 to be a string.");  // arg 2 is the index flowsheet name
    check(L, lua_isuserdata(L, 3),  msg_expected + "argument 3 to be a UnitSet.");  // arg 3 is a Unitset

    string name = lua_tostring(L, 1);   // Model name
    check(L, !name.empty(), msg_expected + "argument 1 to be a non-empty string.");

    string index_fs_name = lua_tostring(L, 2);  // Index Flowsheet name
    check(L, !index_fs_name.empty(), msg_expected + "argument 2 to be a non-empty string.");

    auto u = get_typed_ptr<UnitSet>(L, 3, msg_expected + "argument 3 to be a UnitSet.");

    M = make_unique<Model>(name, index_fs_name, std::move(*u));
    delete u;
    FS = M->index_fs.get();

    push_pointer<Model>(L, M.get());
    push_pointer<Flowsheet>(L, FS);

    return 2;
}

int create_unitset(lua_State* L) {
    const string msg_expected {"UnitSet: expected "};
    const string msg_kinds {"UnitSet: in the kinds table, expected "};
    const string msg_units {"UnitSet: in the units table, expected "};
    auto u {new UnitSet};
    auto n_args = lua_gettop(L);
    if (n_args == 0) {
        push_pointer<UnitSet>(L, u);
        return 1;
    }
    check(L, n_args == 2, format("{}2 arguments, got {}.", msg_expected, n_args));
    check(L, lua_istable(L, 1),  msg_expected + "argument 1 to be a table.");  // arg 1 is a kinds table
    check(L, lua_istable(L, 2),  msg_expected + "argument 2 to be a table.");  // arg 2 is a units table

    // kinds table:
    lua_pushnil(L);                             // push a nil key to start
    while (lua_next(L, 1) != 0) {               // pops the key, then pushes next key-value pair
        string kind_str = lua_tostring(L, -2);  // key is kind_str
        check(L, lua_istable(L, -1), format("{}key \"{}\" to reference a table.", msg_kinds, kind_str)); // value is table containing 1 or 2 strings

        auto n_str = lua_rawlen(L, -1);         // number of strings in the table
        check(L, n_str == 1 || n_str == 2, format("{}1 or 2 strings in the \"{}\" definition.", msg_kinds, kind_str));

        string base_unit_str = get_string_elem(L, 1,
            format("{}element 1 in the \"{}\" definition to be a string.", msg_kinds, kind_str)); // element 1 is base_unit_str

        string default_unit_str = (n_str == 2 ? get_string_elem(L, 2,
            format("{}element 2 in the \"{}\" definition to be a string.", msg_kinds, kind_str)) : base_unit_str); // element 2 (optional) is default_unit_str

        lua_pop(L, 1);  // pop the value (the {base_unit_str, default_unit_str} table)
        u->add_kind(kind_str, base_unit_str, default_unit_str);
    }

    // units table:
    lua_pushnil(L);                             // push a nil key to start
    while (lua_next(L, 2) != 0) {               // pops the key, then pushes next key-value pair
        string kind_str = lua_tostring(L, -2);  // key is kind_str
        check(L, lua_istable(L, -1), format("{}kind string \"{}\" to reference a table.", msg_units, kind_str)); // value is a table of n_units tables
        auto n_units = lua_rawlen(L, -1);       // value is a list of units for this kind

        for (lua_Unsigned i = 1; i <= n_units; i++) {
            check(L, lua_rawgeti(L, -1, i) == LUA_TTABLE, format("{}unit {} in \"{}\" definition to be a list.", msg_units, i, kind_str)); // push the ith unit list
            auto n_elem = lua_rawlen(L, -1);    // number of elements in the ith unit list
            check(L, n_elem >= 1 && n_elem <= 3, format("{}unit {} in \"{}\" definition to look like {{string, <number>, <number>}}.", msg_units, i, kind_str));

            string unit_str = get_string_elem(L, 1, format("{}unit {} in \"{}\" definition to look like {{string, <number>, <number>}}.", msg_units, i, kind_str));     // element 1 is unit_str
            double unit_ratio = (n_elem >= 2 ?
                get_double_elem(L, 2, format("{}unit {} in \"{}\" definition to look like {{string, number, <number>}}.", msg_units, i, kind_str)) : 1.0);   // element 2 is unit_ratio
            double unit_offset = (n_elem == 3 ? 
                get_double_elem(L, 3, format("{}unit {} in \"{}\" definition to look like {{string, number, number}}.", msg_units, i, kind_str)) : 0.0); // element 3 (optional) is unit_offset

            lua_pop(L, 1);  // pop the ith unit list
            check(L, u->add_unit(unit_str, kind_str, unit_ratio, unit_offset) != nullptr,
                format("UnitSet: in the units table, kind \"{}\" has not been defined.", kind_str));
        }

        lua_pop(L, 1);  // pop the value (a list of unit lists) of the current key-value pair
    }

    push_pointer<UnitSet>(L, u);
    return 1;

}

int add_kind(lua_State* L) {
    const string msg {"AddKind: expected "};
    auto n_args = lua_gettop(L);
    check(L, n_args == 3 || n_args == 4, format("{}3 or 4 arguments, got {}.", msg, n_args));
    check(L, lua_isuserdata(L, 1),  msg + "argument 1 to be a UnitSet.");      // arg 1 is a UnitSet
    check(L, lua_isstring(L, 2),  msg + "argument 2 to be a string.");         // arg 2 is a kind string
    check(L, lua_isstring(L, 3),  msg + "argument 3 to be a string.");         // arg 3 is a base unit string
    if (n_args == 4)
        check(L, lua_isstring(L, 4),  msg + "argument 4 to be a string.");     // arg 4 is a default unit string
    auto u = get_typed_ptr<UnitSet>(L, 1, msg + "argument 1 to be a UnitSet.");
    check(L, u != nullptr, msg + "argument 1 to be a UnitSet.");
    string kind_str = lua_tostring(L, 2);
    string base_unit_str = lua_tostring(L, 3);
    string default_unit_str = (n_args == 4 ? lua_tostring(L, 4) : base_unit_str);
    u->add_kind(kind_str, base_unit_str, default_unit_str);
    return 0;
}

int add_unit(lua_State* L) {
    const string msg {"AddUnit: expected "};
    auto n_args = lua_gettop(L);
    check(L, n_args >= 3 && n_args <= 5, format("{}3, 4, or 5 arguments, got {}.", msg, n_args));
    check(L, lua_isuserdata(L, 1),  msg + "argument 1 to be a UnitSet.");      // arg 1 is a UnitSet
    check(L, lua_isstring(L, 2),  msg + "argument 2 to be a string.");         // arg 2 is a kind string
    check(L, lua_isstring(L, 3),  msg + "argument 3 to be a string.");         // arg 3 is a unit string
    if (n_args > 3)
        check(L, lua_isnumber(L, 4), msg + "argument 4 to be a number.");
    if (n_args > 4)
        check(L, lua_isnumber(L, 5), msg + "argument 5 to be a number.");
    auto u = get_typed_ptr<UnitSet>(L, 1, msg + "argument 1 to be a UnitSet.");
    check(L, u != nullptr, msg + "argument 1 to be a UnitSet.");
    string kind_str = lua_tostring(L, 2);
    string unit_str = lua_tostring(L, 3);
    double unit_ratio = (n_args > 3 ? lua_tonumber(L, 4) : 1.0);
    double unit_offset = (n_args > 4 ? lua_tonumber(L, 5) : 0.0); 
    check(L, u->add_unit(unit_str, kind_str, unit_ratio, unit_offset) != nullptr,
        format("AddUnit: kind \"{}\" has not been defined yet.", kind_str));
    return 0;
}

int write_variables(lua_State* L) {
    const string msg {"WriteVariables: expected "};
    checkM(L, msg + errM);
    auto n_args = lua_gettop(L);
    check(L, n_args <= 1, msg + "0 or 1 argument.");
    if (n_args == 0)
        M->write_variables();
    else {
        if (lua_isstring(L, 1)) {
            string filename = lua_tostring(L, 1);
            ofstream file;
            file.open(filename, std::ios::out);
            check(L, file.is_open(), format("WriteVariables: unable to open the file \"{}\".", filename));
            M->write_variables(file);
        }
        else
            luaL_error(L, format("{}argument 1 to be a file name.", msg).c_str());
    }

    return 0;
}

int set_output(lua_State* L) {
    const string msg {"Output: expected "};
    auto n_args = lua_gettop(L);
    check(L, n_args <= 1, msg + "0 or 1 argument.");
    if (n_args == 0) {
        if (OUTFILE.is_open()) OUTFILE.close();
        OUT = &cout;
    }
    else {
        if (lua_isuserdata(L, 1)) {
            auto out = get_typed_ptr<ostream>(L, 1, msg + "argument 1 to be a file name or an Output.");
            OUT = out;
        }
        else if (lua_isstring(L, 1)) {
            if (OUTFILE.is_open()) OUTFILE.close();
            string filename = lua_tostring(L, 1);
            OUTFILE.open(filename, std::ios::out);
            check(L, OUTFILE.is_open(), format("Output: unable to open the file \"{}\".", filename));
            OUT = &OUTFILE;
        }
        else
            luaL_error(L, format("{}argument 1 to be a file name or an Output.", msg).c_str());
    }

    push_pointer<ostream>(L, OUT);
    return 1;
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

class LuaStreambuf : public std::streambuf {
    lua_State* L;
    std::string buffer;

    void flush_to_lua() {
        if (buffer.empty()) return;
        lua_getglobal(L, "io");
        lua_getfield(L, -1, "write");
        lua_remove(L, -2);
        lua_pushstring(L, buffer.c_str());
        lua_pcall(L, 1, 0, 0);
        buffer.clear();
    }

protected:
    int overflow(int c) override {
        if (c != EOF)
            buffer += static_cast<char>(c);
        return c;
    }

    int sync() override {
        flush_to_lua();
        return 0;
    }
    
public:
    LuaStreambuf(lua_State* L_) : L(L_) {}
};

class LuaJournal : public Ipopt::Journal {
    lua_State* L;
protected:
    void PrintImpl(Ipopt::EJournalCategory, Ipopt::EJournalLevel,
                   const char* str) override {
        lua_getglobal(L, "io");
        lua_getfield(L, -1, "write");
        lua_remove(L, -2);
        lua_pushstring(L, str);
        lua_pcall(L, 1, 0, 0);
    }
    void PrintfImpl(Ipopt::EJournalCategory, Ipopt::EJournalLevel,
                    const char* fmt, va_list ap) override {
        char buf[4096];
        vsnprintf(buf, sizeof(buf), fmt, ap);
        PrintImpl({}, {}, buf);
    }
    void FlushBufferImpl() override {}
public:
    LuaJournal(lua_State* L_) : Journal("lua", Ipopt::J_ITERSUMMARY), L(L_) {}
};

static const luaL_Reg function_table[] {
    { "Output",            set_output            },
    { "UnitSet",           create_unitset        },
    { "AddKind",           add_kind              },
    { "AddUnit",           add_unit              },
    { "Model",             create_model          },
    { "Flowsheet",         flowsheet             },
    { "Streams",           add_streams           },
    { "Eval",              eval_expr             },
    { "Init",              initialize            },
    { "WriteVariables",    write_variables       },
    { "ShowModel",         show_model            },
    { "ShowUnits",         show_units            },
    { "ShowVariables",     show_variables        },
    { "ShowConstraints",   show_constraints      },
    { "ShowJacobian",      show_jacobian         },
    { "ShowHessian",       show_hessian          },
    { "ShowConnections",   show_connections      },
    { "ShowPrices",        show_prices           },
    { "ShowObjective",     show_objective        },
    { "ShowObjGrad",       show_obj_grad         },
    { "Val",               get_value             },
    { "BaseVal",           get_base_value        },
    { "LB",                get_lower             },
    { "UB",                get_upper             },
    { "Spec",              get_spec              },
    { "Var",               get_var               },
    { "Unit",              get_unit              },
    { "ChangeUnit",        change_unit           },
    { "SetValue",          set_value             },
    { "SolverOption",      set_solver_option     },
    { "Solve",             solve_model           },
    { "InitSolver",        initialize_solver     },
    { "EvalConstraints",   eval_constraints      },
    { "EvalJacobian",      eval_jacobian         },
    { "EvalHessian",       eval_hessian          },
    { "EvalObjective",     eval_objective        },
    { "EvalObjGrad",       eval_obj_grad         },
    { "Variables",         add_variables         },
    { "Constraints",       add_constraints       },
    { "JacobianNZs",       add_jacobian_nzs      },
    { "Connect",           connect               },
    { "ConnectAll",        connect_streams       },
    { "IsSame",            is_same_ptr           },
    { "Bridge",            add_bridge            },
    { "Prices",            add_prices            },
    { "Objective",         add_objective         },
    { "SetObjective",      set_objective         },
    { "Mixer",             add_Mixer             },
    { "Splitter",          add_Splitter          },
    { "Separator",         add_Separator         },
    { "YieldReactor",      add_YieldReactor      },
    { "MultiYieldReactor", add_MultiYieldReactor },
    { "StoicReactor",      add_StoicReactor      },
    { "Calc",              add_Calc              },
    { NULL,                NULL                  }   
};

lua_State* start_lua() {
    auto L = luaL_newstate();
    if (!L) return nullptr;
    luaL_openlibs(L);
    scripter_lua_state = L;

    lua_pushglobaltable(L);
    luaL_setfuncs(L, function_table, 0);

    return L;
}

#ifdef _WIN32
#define EXPORT_LUAOPEN __declspec(dllexport)
#include <windows.h>
#else
#define EXPORT_LUAOPEN
#endif

#include <cstdlib>

extern "C" EXPORT_LUAOPEN int luaopen_mboptlib(lua_State* L)
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    lua_pushglobaltable(L);
    luaL_setfuncs(L, function_table, 0);

    static LuaStreambuf lua_buf(L);
    if (std::getenv("JPY_PARENT_PID") != nullptr) {
        cout.rdbuf(&lua_buf);

        solver->Jnlst()->DeleteAllJournals();
        solver->Jnlst()->AddJournal(new LuaJournal(L));
    }

    return 0;                     
}