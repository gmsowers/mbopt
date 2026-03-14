#include <sstream>
#include <fstream>
#include <regex>
#include <charconv>
#include <typeindex>
#include <cstdlib>
#include "scripter.hpp"
#include "Model.hpp"
#include "Mixer.hpp"
#include "Splitter.hpp"
#include "Separator.hpp"
#include "YieldReactor.hpp"
#include "MultiYieldReactor.hpp"
#include "StoicReactor.hpp"

static ostream*          OUT {&cout};
static ofstream          OUTFILE;
static lua_State*        scripter_lua_state;

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

const char* get_string_elem(lua_State* L, int index, lua_Integer n) {
    const char* s {nullptr};
    auto type = lua_rawgeti(L, index, n);
    if (type == LUA_TSTRING) s = lua_tostring(L, -1);
    lua_pop(L, 1);
    return s;
}

double get_double_elem(lua_State* L, int index, lua_Integer n, bool& ok) {
    double val {};
    ok = false;
    auto type = lua_rawgeti(L, index, n);
    if (type == LUA_TNUMBER) {
        val = lua_tonumber(L, -1);
        ok = true;
    }   
    lua_pop(L, 1);
    return val;
}

static const char* MT_UNIT        = "mbopt.Unit";
static const char* MT_UNITKIND    = "mbopt.UnitKind";
static const char* MT_UNITSET     = "mbopt.UnitSet";
static const char* MT_QUANTITY    = "mbopt.Quantity";
static const char* MT_MODEL       = "mbopt.Model";
static const char* MT_FLOWSHEET   = "mbopt.Flowsheet";
static const char* MT_STREAM      = "mbopt.Stream";
static const char* MT_BLOCK       = "mbopt.Block";
static const char* MT_CALC        = "mbopt.Calc";
static const char* MT_SOLVER      = "mbopt.Solver";
static const char* MT_CONSTRAINT  = "mbopt.Constraint";
static const char* MT_JACOBIAN_NZ = "mbopt.JacobianNZ";
static const char* MT_HESSIAN_NZ  = "mbopt.HessianNZ";
static const char* MT_CONNECTION  = "mbopt.Connection";

//---------------------------------------------------------

struct LuaObj {
    void*           obj_p;
    std::type_index base_type;
    std::type_index derived_type;

    template <typename T>
    bool isa() const { return (base_type == typeid(T) || derived_type == typeid(T)); }

    template <typename T>
    T* as() const { return isa<T>() ? static_cast<T*>(obj_p) : nullptr; }

};

template <typename BaseT, typename DerivedT = void>
void push_luaobj(lua_State* L, void* p, const char* mt) {
    if (!p) { lua_pushnil(L); return; }
    void* luaobj_p = lua_newuserdatauv(L, sizeof(LuaObj), 0);
    new (luaobj_p) LuaObj(p, typeid(BaseT), typeid(DerivedT));
    luaL_getmetatable(L, mt);
    lua_setmetatable(L, -2);
}

template <typename T>
T* check_luaobj(lua_State* L, const char* mt, int arg) {
    auto luaobj = static_cast<LuaObj*>(luaL_checkudata(L, arg, mt));
    T* cppobj = luaobj->as<T>();
    luaL_argcheck(L, cppobj != nullptr, arg, "failed to convert the object to the target type.");
    return cppobj;
}

LuaObj* get_luaobj(lua_State* L, int index) {
    return static_cast<LuaObj*>(lua_touserdata(L, index));
}

template <typename T>
T* get_luaobj_elem(lua_State* L, int index, lua_Integer n) {
    T* obj;
    if (lua_rawgeti(L, index, n) == LUA_TUSERDATA) {
        auto luaobj = get_luaobj(L, -1);
        if (luaobj != nullptr)
            obj = luaobj->as<T>();
    }
    lua_pop(L, 1);
    return obj;
}

//---------------------------------------------------------

const std::regex re_binop(R"((\S+)(=|<|>)([^\s_]+)(?:_(\S+))?)");
const std::regex re_spec(R"((fix|free)\s+(\S+))");

int eval_expr(lua_State* L) {
    auto M = check_luaobj<Model>(L, MT_MODEL, 1);
    string exprs = luaL_checkstring(L, 2);
    std::istringstream expr_stream {exprs};

    bool ok = true;
    int line_no = 0;
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
                lua_warning(L, format("on line {} in expression \"{}\", the variable \"{}\" is not in the model.", line_no, expr_ts, lhs).c_str(), 0);
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
                        lua_warning(L, format("on line {} in expression \"{}\", the right-hand side unit \"{}\" is not in the unitset.",
                            line_no, expr_ts, rhs_unit_str).c_str(), 0);
                        continue;
                    }
                    rhs_value = lhs_var->convert(rhs_value, rhs_unit);
                }
            }
            else {
                ok = false;
                lua_warning(L, format("on line {} the right-hand side \"{}\" of expression \"{}\" is invalid.", line_no, rhs, expr_ts).c_str(), 0);
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
                lua_warning(L, format("on line {} in expression \"{}\", the variable \"{}\" is not in the model.", line_no, expr_ts, rhs).c_str(), 0);
                continue;
            }
            
            if (lhs == "free")
                rhs_var->free();
            else 
                rhs_var->fix();
        }
        else {
            ok = false;
            lua_warning(L, format("on line {} the expression \"{}\" is invalid.", line_no, expr_ts).c_str(), 0);
        }
    }
    
    lua_pushboolean(L, ok);
    return 1;
}

int delegate(lua_State* L, auto f) {
    auto obj = get_luaobj(L, 1);
    if (obj->isa<Model>()) {
        auto p = static_cast<Model*>(obj->obj_p);
        if (p) f(p);
    }
    else if (obj->isa<Block>()) {
        if (obj->isa<Mixer>()) {
            auto p = static_cast<Mixer*>(obj->obj_p);
            if (p) f(p);
        } else if (obj->isa<Splitter>()) {
            auto p = static_cast<Splitter*>(obj->obj_p);
            if (p) f(p);
        } else if (obj->isa<Separator>()) {
            auto p = static_cast<Separator*>(obj->obj_p);
            if (p) f(p);
        } else if (obj->isa<YieldReactor>()) {
            auto p = static_cast<YieldReactor*>(obj->obj_p);
            if (p) f(p);
        } else if (obj->isa<MultiYieldReactor>()) {
            auto p = static_cast<MultiYieldReactor*>(obj->obj_p);
            if (p) f(p);
        } else if (obj->isa<StoicReactor>()) {
            auto p = static_cast<StoicReactor*>(obj->obj_p);
            if (p) f(p);
        }
    }
    else if (obj->isa<Calc>()) {
        auto p = static_cast<Calc*>(obj->obj_p);
        if (p) f(p);
    }

    return 0;
}

int initialize(lua_State* L) {
    return delegate(L, [](auto* p) { p->initialize(); });
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

int show_variables(lua_State* L) {
    return delegate(L, [](auto* p) { p->show_variables(*OUT); });
}

int show_constraints(lua_State* L) {
    return delegate(L, [](auto* p) { p->show_constraints(*OUT); });
}

int show_jacobian(lua_State* L) {
    return delegate(L, [](auto* p) { p->show_jacobian(*OUT); });
}

int show_hessian(lua_State* L) {
    return delegate(L, [](auto* p) { p->show_hessian(*OUT); });
}

int show_connections(lua_State* L) {
    auto m = check_luaobj<Model>(L, MT_MODEL, 1);
    m->show_connections(*OUT);
    return 0;
}

int show_prices(lua_State* L) {
    auto m = check_luaobj<Model>(L, MT_MODEL, 1);
    m->show_prices(*OUT);
    return 0;
}

int get_var(lua_State* L) {
    auto m = check_luaobj<Model>(L, MT_MODEL, 1);
    string name = luaL_checkstring(L, 2);
    luaL_argcheck(L, m->x_map.contains(name), 2, format("\"{}\" not found in the model.", name).c_str());
    push_luaobj<Quantity, Variable>(L, m->x_map[name], MT_QUANTITY);
    return 1;
}

int change_unit(lua_State* L) {
    auto q = check_luaobj<Quantity>(L, MT_QUANTITY, 1);
    auto u = check_luaobj<Unit>(L, MT_UNIT, 2);
    q->change_unit(u);
    return 0;
}

//---------------------------------------------------------

int Unit_tostring(lua_State* L) {
    auto u = check_luaobj<Unit>(L, MT_UNIT, 1);
    lua_pushstring(L, u->str.c_str());
    return 1;
}

void Unit_register(lua_State* L) {
    luaL_newmetatable(L, MT_UNIT);
    lua_pushcfunction(L, Unit_tostring); lua_setfield(L, -2, "__tostring");
    lua_pop(L, 1);
}

//---------------------------------------------------------

int UnitKind_tostring(lua_State* L) {
    auto uk = check_luaobj<UnitKind>(L, MT_UNITKIND, 1);
    lua_pushstring(L, uk->to_str().c_str());
    return 1;
}

int UnitKind_index(lua_State* L) {
    auto uk = check_luaobj<UnitKind>(L, MT_UNITKIND, 1);
    string key = luaL_checkstring(L, 2);
    if      (key == "base_unit")    push_luaobj<Unit>(L, uk->base_unit, MT_UNIT);
    else if (key == "default_unit") push_luaobj<Unit>(L, uk->default_unit, MT_UNIT);
    else                            lua_pushnil(L);
    return 1;
}

void UnitKind_register(lua_State* L) {
    luaL_newmetatable(L, MT_UNITKIND);
    lua_pushcfunction(L, UnitKind_tostring); lua_setfield(L, -2, "__tostring");
    lua_pushcfunction(L, UnitKind_index);    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);
}

//---------------------------------------------------------

static vector<unique_ptr<UnitSet>> unitsets {};

int add_kind(lua_State* L) {
    auto us = check_luaobj<UnitSet>(L, MT_UNITSET, 1);                        // arg 1 is a UnitSet
    string kind_str = luaL_checkstring(L, 2);                                 // arg 2 is a kind string
    string base_unit_str = luaL_checkstring(L, 3);                            // arg 3 is a base unit string
    string default_unit_str = luaL_optstring(L, 4, base_unit_str.c_str());    // arg 4 is a default unit string
    auto ukind = us->add_kind(kind_str, base_unit_str, default_unit_str);
    if (ukind == nullptr)
        lua_pushnil(L);
    else
        push_luaobj<UnitKind>(L, ukind, MT_UNITKIND);
    return 1;
}

int add_unit(lua_State* L) {
    auto us = check_luaobj<UnitSet>(L, MT_UNITSET, 1);  // arg 1 is a UnitSet
    string kind_str = luaL_checkstring(L, 2);           // arg 2 is a kind string
    string unit_str = luaL_checkstring(L, 3);           // arg 3 is a unit string
    double unit_ratio = luaL_optnumber(L, 4, 1.0);      // arg 4 is a unit ratio
    double unit_offset = luaL_optnumber(L, 5, 0.0);     // arg 5 is a unit offset
    auto unit = us->add_unit(unit_str, kind_str, unit_ratio, unit_offset);
    if (unit == nullptr)
        lua_pushnil(L);
    else
        push_luaobj<Unit>(L, unit, MT_UNIT);
    return 1;
}

int get_kind(lua_State* L) {
    auto us = check_luaobj<UnitSet>(L, MT_UNITSET, 1);  // arg 1 is a UnitSet
    UnitKind* uk {nullptr};
    if (lua_isstring(L, 2)) {
        string unit_str = lua_tostring(L, -1);
        uk = (us->units.contains(unit_str) ? us->units[unit_str]->kind : nullptr);
    }
    else if (lua_isuserdata(L, 2)) {
        auto u = check_luaobj<Unit>(L, MT_UNIT, 2);
        uk = u->kind;
    }
    if (uk == nullptr)
        lua_pushnil(L);
    else
        push_luaobj<UnitKind>(L, uk, MT_UNITKIND);
    return 1;
}

int UnitSet_new(lua_State* L) {
    auto us = make_unique<UnitSet>();
    if (!us) {
        lua_pushnil(L);
        return 1;
    }
    auto us_p = us.get();
    unitsets.push_back(std::move(us));
    auto n_args = lua_gettop(L);
    if (n_args == 0) {
        push_luaobj<UnitSet>(L, us_p, MT_UNITSET);
        return 1;
    }
    luaL_checktype(L, 1, LUA_TTABLE);
    luaL_checktype(L, 2, LUA_TTABLE);

    // kinds table:
    lua_pushnil(L);                             // push a nil key to start
    while (lua_next(L, 1) != 0) {               // pops the key, then pushes next key-value pair
        string kind_str = lua_tostring(L, -2);  // key is kind_str
        luaL_argcheck(L, !kind_str.empty(), 1, "kind string cannot be empty.");
        luaL_checktype(L, -1, LUA_TTABLE);      // value is a table the looks like {base_unit_str, <default_unit_str>}
        auto n_str = lua_rawlen(L, -1);         // number of strings in the table
        string base_unit_str = get_string_elem(L, -1, 1);                                   // element 1 is the base unit string
        luaL_argcheck(L, !base_unit_str.empty(), 1, "base unit string cannot be empty.");
        string default_unit_str = (n_str == 2 ? get_string_elem(L, -1, 2) : base_unit_str); // element 2 (optional) is the default unit string

        lua_pop(L, 1);                          // pop the value (the {base_unit_str, default_unit_str} table) of the current key-value pair
        us_p->add_kind(kind_str, base_unit_str, default_unit_str);
    }

    // units table:
    lua_pushnil(L);                             // push a nil key to start
    while (lua_next(L, 2) != 0) {               // pops the key, then pushes next key-value pair
        string kind_str = lua_tostring(L, -2);  // key is kind_str
        luaL_checktype(L, -1, LUA_TTABLE);      // value is a table of n_units tables
        auto n_units = lua_rawlen(L, -1);       // number of units with this kind

        for (lua_Unsigned i = 1; i <= n_units; i++) {
            auto type = lua_rawgeti(L, -1, i);      // push the ith unit table
            if (type == LUA_TTABLE) {
                auto n_elem = lua_rawlen(L, -1);    // number of elements in the ith unit table
                string unit_str = get_string_elem(L, -1, 1);                                // element 1 is unit_str
                luaL_argcheck(L, !unit_str.empty(), i, "unit string cannot be empty.");
                bool ok1 {true}, ok2 {true};
                double unit_ratio = (n_elem >= 2 ? get_double_elem(L, -1, 2, ok1) : 1.0);   // element 2 is unit_ratio
                luaL_argcheck(L, ok1, i, "expected the unit_ratio to be a number.");
                double unit_offset = (n_elem == 3 ? get_double_elem(L, -1, 3, ok2) : 0.0);  // element 3 (optional) is unit_offset
                luaL_argcheck(L, ok2, i, "expected the unit_offset to be a number.");
                us_p->add_unit(unit_str, kind_str, unit_ratio, unit_offset);
            }
            lua_pop(L, 1);                          // pop the ith unit table
        }

        lua_pop(L, 1);                              // pop the value (a list of unit lists) of the current key-value pair
    }

    push_luaobj<UnitSet>(L, us_p, MT_UNITSET);
    return 1;

}

int UnitSet_tostring(lua_State* L) {
    auto us = check_luaobj<UnitSet>(L, MT_UNITSET, 1);
    std::ostringstream oss;
    us->show_units(oss);
    lua_pushstring(L, oss.str().c_str());
    return 1;
}

int UnitSet_index(lua_State* L) {
    auto us = check_luaobj<UnitSet>(L, MT_UNITSET, 1);
    string key = luaL_checkstring(L, 2);
    if      (key == "add_kind") lua_pushcfunction(L, add_kind);
    else if (key == "add_unit") lua_pushcfunction(L, add_unit);
    else if (key == "get_kind") lua_pushcfunction(L, get_kind);
    else if (key == "kinds") {
        lua_newtable(L);
        for (const auto& [kind_str, unit_kind] : us->kinds) {
            lua_pushstring(L, kind_str.c_str());
            push_luaobj<UnitKind>(L, unit_kind.get(), MT_UNITKIND);
            lua_settable(L, -3);
        }            
    }
    else if (key == "units") {
        lua_newtable(L);
        for (const auto& [unit_str, unit] : us->units) {
            lua_pushstring(L, unit_str.c_str());
            push_luaobj<Unit>(L, unit.get(), MT_UNIT);
            lua_settable(L, -3);
        }            
    }
    else
        lua_pushnil(L);
    
    return 1;
}

void UnitSet_register(lua_State* L) {
    luaL_newmetatable(L, MT_UNITSET);
    lua_pushcfunction(L, UnitSet_tostring); lua_setfield(L, -2, "__tostring");
    lua_pushcfunction(L, UnitSet_index);    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);
}

//---------------------------------------------------------

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

struct Solver
{
    LuaStreambuf* lua_buf;
    IpoptApplication* ipopt_app;
};

static unique_ptr<Solver> solver;

int Solver_set_option(lua_State* L) {
    auto slv = check_luaobj<Solver>(L, MT_SOLVER, 1);
    luaL_checkany(L, 3);
    string option = luaL_checkstring(L, 2);
    if (!slv->ipopt_app) return 0;
    if (lua_isinteger(L, 3)) {
        slv->ipopt_app->Options()->SetIntegerValue(option, lua_tointeger(L, 3));
    }
    else if (lua_isnumber(L, 3)) {
        slv->ipopt_app->Options()->SetNumericValue(option, lua_tonumber(L, 3));
    }
    else if (lua_isstring(L, 3)) {
        slv->ipopt_app->Options()->SetStringValue(option, lua_tostring(L, 3));
    }
    return 0;
}

int Solver_initialize(lua_State* L) {
    auto slv = check_luaobj<Solver>(L, MT_SOLVER, 1);
    lua_pushinteger(L, slv->ipopt_app->Initialize());
    return 1;
}

int Solver_solve(lua_State* L) {
    auto slv = check_luaobj<Solver>(L, MT_SOLVER, 1);
    auto M = check_luaobj<Model>(L, MT_MODEL, 2);
    int retval = slv->ipopt_app->OptimizeTNLP(M);
    lua_pushinteger(L, retval);
    return 1;
}

int Solver_new(lua_State* L) {
    solver = make_unique<Solver>(new LuaStreambuf(L), IpoptApplicationFactory());
    if (std::getenv("JPY_PARENT_PID") != nullptr) {
        cout.rdbuf(solver->lua_buf);

        solver->ipopt_app->Jnlst()->DeleteAllJournals();
        solver->ipopt_app->Jnlst()->AddJournal(new LuaJournal(L));
    }
    push_luaobj<Solver>(L, solver.get(), MT_SOLVER);
    return 1;
}

int Solver_index(lua_State* L) {
    auto slv = check_luaobj<Solver>(L, MT_SOLVER, 1);
    string key = luaL_checkstring(L, 2);
    if (key == "init" || key == "initialize") lua_pushcfunction(L, Solver_initialize);
    else if (key == "set_option") lua_pushcfunction(L, Solver_set_option);
    else if (key == "solve") lua_pushcfunction(L, Solver_solve);
    else lua_pushnil(L);

    return 1;
}

void Solver_register(lua_State* L) {
    luaL_newmetatable(L, MT_SOLVER);
    lua_pushcfunction(L, Solver_index);    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);
}

//---------------------------------------------------------

static vector<unique_ptr<Quantity>> quantities {};

int Quantity_new(lua_State* L) {
    double value = luaL_checknumber(L, 1);
    auto u = check_luaobj<Unit>(L, MT_UNIT, 2);
    auto q = make_unique<Quantity>(value, u);
    if (q) {
        auto q_p = q.get();
        quantities.push_back(std::move(q)); 
        push_luaobj<Quantity>(L, q_p, MT_QUANTITY);
    }
    else
        lua_pushnil(L);

    return 1;
}

int Quantity_tostring(lua_State* L) {
    auto q = check_luaobj<Quantity>(L, MT_QUANTITY, 1);
    lua_pushstring(L, q->to_str().c_str());
    return 1;
}

int Quantity_index(lua_State* L) {
    auto obj = get_luaobj(L, 1);
    Quantity* q = obj->as<Quantity>();
    Variable* v = obj->as<Variable>();

    string key = luaL_checkstring(L, 2);
    if (key == "v" || key == "value") lua_pushnumber(L, q->value);
    else if (key == "u" || key == "unit") push_luaobj<Unit>(L, q->unit, MT_UNIT);
    else if (key == "bv" || key == "base_value") lua_pushnumber(L, q->convert_to_base());
    else if ((key == "lb" || key == "lower") && v) if (v->lower.has_value()) lua_pushnumber(L, v->lower.value()); else lua_pushnil(L);
    else if ((key == "ub" || key == "upper") && v) if (v->upper.has_value()) lua_pushnumber(L, v->upper.value()); else lua_pushnil(L);
    else if (key == "spec" && v) lua_pushstring(L, (v->is_fixed() ? "fixed" : "free"));
    else if (key == "change_unit") lua_pushcfunction(L, change_unit);
    else lua_pushnil(L);

    return 1;
}

int Quantity_newindex(lua_State* L) {
    auto q = check_luaobj<Quantity>(L, MT_QUANTITY, 1);
    string key = luaL_checkstring(L, 2);
    if (key == "bv" || key == "base_value") {
        double base_val = luaL_checknumber(L, 3);
        q->convert_and_set(base_val);
    }
    else if (key == "v" || key == "value") {
        double val = luaL_checknumber(L, 3);
        q->value = val;
    }
    return 0;
}

void Quantity_register(lua_State* L) {
    luaL_newmetatable(L, MT_QUANTITY);
    lua_pushcfunction(L, Quantity_tostring); lua_setfield(L, -2, "__tostring");
    lua_pushcfunction(L, Quantity_index);    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, Quantity_newindex); lua_setfield(L, -2, "__newindex");
    lua_pop(L, 1);
}

//---------------------------------------------------------

int Constraint_newindex(lua_State* L) {
    auto eq = check_luaobj<Constraint>(L, MT_CONSTRAINT, 1);
    string key = luaL_checkstring(L, 2);
    if (key == "v" || key == "value") {
        double val = luaL_checknumber(L, 3);
        eq->value = val;
    }
    return 0;
}

void Constraint_register(lua_State* L) {
    luaL_newmetatable(L, MT_CONSTRAINT);
    lua_pushcfunction(L, Constraint_newindex); lua_setfield(L, -2, "__newindex");
    lua_pop(L, 1);
}

//---------------------------------------------------------

int JacobianNZ_newindex(lua_State* L) {
    auto jnz = check_luaobj<JacobianNZ>(L, MT_JACOBIAN_NZ, 1);
    string key = luaL_checkstring(L, 2);
    if (key == "v" || key == "value") {
        double val = luaL_checknumber(L, 3);
        jnz->value = val;
    }
    return 0;
}

void JacobianNZ_register(lua_State* L) {
    luaL_newmetatable(L, MT_JACOBIAN_NZ);
    lua_pushcfunction(L, JacobianNZ_newindex); lua_setfield(L, -2, "__newindex");
    lua_pop(L, 1);
}

//---------------------------------------------------------

int HessianNZ_newindex(lua_State* L) {
    auto hnz = check_luaobj<HessianNZ>(L, MT_HESSIAN_NZ, 1);
    string key = luaL_checkstring(L, 2);
    if (key == "v" || key == "value") {
        double val = luaL_checknumber(L, 3);
        hnz->value = val;
    }
    return 0;
}

void HessianNZ_register(lua_State* L) {
    luaL_newmetatable(L, MT_HESSIAN_NZ);
    lua_pushcfunction(L, HessianNZ_newindex); lua_setfield(L, -2, "__newindex");
    lua_pop(L, 1);
}

//---------------------------------------------------------

static vector<unique_ptr<Model>> models {};

int Model_new(lua_State* L) {
    string name = luaL_checkstring(L, 1);
    string index_fs_name = luaL_checkstring(L, 2);
    auto us = check_luaobj<UnitSet>(L, MT_UNITSET, 3);
    auto m = make_unique<Model>(name, index_fs_name, std::move(*us));
    if (m) {
        auto m_p = m.get();
        models.push_back(std::move(m));
        push_luaobj<Model>(L, m_p, MT_MODEL);
    }
    else
        lua_pushnil(L);

    return 1;
}

int Model_tostring(lua_State* L) {
    std::ostringstream oss;
    auto m = check_luaobj<Model>(L, MT_MODEL, 1);
    m->show_model(oss);
    lua_pushstring(L, oss.str().c_str());
    return 1;
}

int connect(lua_State* L);
int connect_all(lua_State* L);
int add_bridge(lua_State* L);
int add_prices(lua_State* L);

int Model_index(lua_State* L) {
    auto m = check_luaobj<Model>(L, MT_MODEL, 1);
    string key = luaL_checkstring(L, 2);
    if      (key == "name")     lua_pushstring(L, m->name.c_str());
    else if (key == "index_fs") push_luaobj<Flowsheet>(L, m->index_fs.get(), MT_FLOWSHEET);
    else if (key == "unitset")  push_luaobj<UnitSet>(L, &m->unit_set, MT_UNITSET);
    else if (key == "eval")     lua_pushcfunction(L, eval_expr);
    else if (key == "get_var")  lua_pushcfunction(L, get_var);
    else if (key == "eval_constraints")  lua_pushcfunction(L, eval_constraints);
    else if (key == "eval_jacobian")  lua_pushcfunction(L, eval_jacobian);
    else if (key == "eval_hessian")  lua_pushcfunction(L, eval_hessian);
    else if (key == "show_constraints")  lua_pushcfunction(L, show_constraints);
    else if (key == "show_variables")  lua_pushcfunction(L, show_variables);
    else if (key == "show_jacobian")  lua_pushcfunction(L, show_jacobian);
    else if (key == "show_connections")  lua_pushcfunction(L, show_connections);
    else if (key == "show_prices")  lua_pushcfunction(L, show_prices);
    else if (key == "init" || key == "initialize")     lua_pushcfunction(L, initialize);
    else if (key == "connect")  lua_pushcfunction(L, connect);
    else if (key == "connect_all")  lua_pushcfunction(L, connect_all);
    else if (key == "add_bridge")  lua_pushcfunction(L, add_bridge);
    else if (key == "Prices")  lua_pushcfunction(L, add_prices);
    else                        lua_pushnil(L);
    return 1;
}

void Model_register(lua_State* L) {
    luaL_newmetatable(L, MT_MODEL);
    lua_pushcfunction(L, Model_tostring); lua_setfield(L, -2, "__tostring");
    lua_pushcfunction(L, Model_index);    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);
}

//---------------------------------------------------------

int add_streams(lua_State* L) {
    auto fs = check_luaobj<Flowsheet>(L, MT_FLOWSHEET, 1);
    int n_strms = lua_gettop(L) - 1;
    if (n_strms == 0) {
        lua_pushnil(L);
        return 1;
    }

    vector<Stream*> strms(n_strms);
    for (int i = 2; i <= n_strms + 1; i++) {
        // ith arg looks like {"Name", {"Comp1", "Comp2", etc}}
        luaL_checktype(L, i, LUA_TTABLE);
        luaL_argcheck(L, lua_rawlen(L, i) == 2, i, format("expected argument {} to look like {{name, {{c1, c2, ...}}}}.", i).c_str());
        string strm_name = get_string_elem(L, i, 1); 
        luaL_argcheck(L, !strm_name.empty(), i, "stream name cannot be empty.");
        luaL_argcheck(L, lua_rawgeti(L, i, 2) == LUA_TTABLE, i,
            format("expected argument {} to look like {{name, {{c1, c2, ...}}}}.", i).c_str());; // Push elem 2 of ith arg, e.g., {"Comp1", "Comp2"}
        int n_comps = lua_rawlen(L, -1);
        luaL_argcheck(L, n_comps > 0, i, "expected at least one component.");
        vector<string> comps(n_comps);
        for (int j = 1; j <= n_comps; j++) {
            string cname = get_string_elem(L, -1, j);
            luaL_argcheck(L, !cname.empty(), i, "component name cannot be empty.");
            comps[j - 1] = cname;
        }
        lua_pop(L, 1); // Pop elem 2 of ith arg
        strms[i - 2] = fs->add_stream(strm_name, std::move(comps));
    }

    for (int i = 1; i <= n_strms; i++)
        push_luaobj<Stream>(L, strms[i - 1], MT_STREAM);

    return n_strms;
}

int add_Flowsheet(lua_State* L) {
    auto fs = check_luaobj<Flowsheet>(L, MT_FLOWSHEET, 1);
    string fs_name = luaL_checkstring(L, 2);
    luaL_argcheck(L, !fs_name.empty(), 2, "Flowsheet name cannot be empty.");
    auto fs_new = fs->add_flowsheet(fs_name);
    push_luaobj<Flowsheet>(L, fs_new, MT_FLOWSHEET);
    return 1;
}

int get_Flowsheet(lua_State* L) {
    auto fs = check_luaobj<Flowsheet>(L, MT_FLOWSHEET, 1);
    string fs_name = luaL_checkstring(L, 2);
    luaL_argcheck(L, !fs_name.empty(), 2, "Flowsheet name cannot be empty.");
    luaL_argcheck(L, fs->child_map.contains(fs_name), 2, format("Flowsheet \"{}\" not found.", fs_name).c_str());
    push_luaobj<Flowsheet>(L, fs->child_map[fs_name], MT_FLOWSHEET);
    return 1;
}

int Flowsheet_tostring(lua_State* L) {
    auto fs = check_luaobj<Flowsheet>(L, MT_FLOWSHEET, 1);
    lua_pushstring(L, fs->name.c_str());
    return 1;
}

int add_Mixer(lua_State* L);
int add_Splitter(lua_State* L);
int add_Separator(lua_State* L);
int add_YieldReactor(lua_State* L);
int add_MultiYieldReactor(lua_State* L);
int add_StoicReactor(lua_State* L);
int add_Calc(lua_State* L);

int Flowsheet_index(lua_State* L) {
    auto fs = check_luaobj<Flowsheet>(L, MT_FLOWSHEET, 1);
    string key = luaL_checkstring(L, 2);
    if      (key == "name")        lua_pushstring(L, fs->name.c_str());
    else if (key == "Streams")     lua_pushcfunction(L, add_streams);
    else if (key == "Mixer")       lua_pushcfunction(L, add_Mixer);
    else if (key == "Splitter")    lua_pushcfunction(L, add_Splitter);
    else if (key == "Separator")   lua_pushcfunction(L, add_Separator);
    else if (key == "YieldReactor")   lua_pushcfunction(L, add_YieldReactor);
    else if (key == "MultiYieldReactor")   lua_pushcfunction(L, add_MultiYieldReactor);
    else if (key == "StoicReactor")   lua_pushcfunction(L, add_StoicReactor);
    else if (key == "Calc")        lua_pushcfunction(L, add_Calc);
    else if (key == "Flowsheet")        lua_pushcfunction(L, add_Flowsheet);
    else if (key == "get_flowsheet")        lua_pushcfunction(L, get_Flowsheet);
    else if (key == "streams") {
        lua_newtable(L);
        for (const auto& [name, strm] : fs->streams) {
            lua_pushstring(L, name.c_str());
            push_luaobj<Stream>(L, strm.get(), MT_STREAM);
            lua_settable(L, -3);
        }            
    }
    else lua_pushnil(L);
    return 1;
}

void Flowsheet_register(lua_State* L) {
    luaL_newmetatable(L, MT_FLOWSHEET);
    lua_pushcfunction(L, Flowsheet_tostring); lua_setfield(L, -2, "__tostring");
    lua_pushcfunction(L, Flowsheet_index);    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);
}

//---------------------------------------------------------

int Stream_tostring(lua_State* L) {
    auto strm = check_luaobj<Stream>(L, MT_STREAM, 1);
    lua_pushstring(L, strm->name.c_str());
    return 1;
}

int Stream_index(lua_State* L) {
    auto strm = check_luaobj<Stream>(L, MT_STREAM, 1);
    string key = luaL_checkstring(L, 2);
    if      (key == "name")       lua_pushstring(L, strm->name.c_str());
    else if (key == "components") {
        lua_newtable(L);
        for (int i = 1; const auto& c : strm->comps) {
            lua_pushinteger(L, i++);
            lua_pushstring(L, c.c_str());
            lua_settable(L, -3);
        }            
    }
    else lua_pushnil(L);
    return 1;
}

void Stream_register(lua_State* L) {
    luaL_newmetatable(L, MT_STREAM);
    lua_pushcfunction(L, Stream_tostring); lua_setfield(L, -2, "__tostring");
    lua_pushcfunction(L, Stream_index);    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);
}

//---------------------------------------------------------

void start_Block(lua_State* L, string& blk_name, vector<Stream*>& inlets, vector<Stream*>& outlets) {
    blk_name = luaL_checkstring(L, 2);               

    // Table of inlet streams.
    luaL_checktype(L, 3, LUA_TTABLE);
    auto n_inlets = lua_rawlen(L, 3);
    luaL_argcheck(L, n_inlets > 0, 3, "expected at least one inlet Stream.");
    inlets.resize(n_inlets);
    for (lua_Unsigned i = 1; i <= n_inlets; i++) {
        inlets[i - 1] = get_luaobj_elem<Stream>(L, 3, i);
        luaL_argcheck(L, inlets[i - 1] != nullptr, 3, format("element {} to be a Stream.", i).c_str());
    }

    // Table of outlet streams.
    luaL_checktype(L, 4, LUA_TTABLE);
    auto n_outlets = lua_rawlen(L, 4);
    luaL_argcheck(L, n_outlets > 0, 4, "expected at least one outlet Stream.");
    outlets.resize(n_outlets);
    for (lua_Unsigned i = 1; i <= n_outlets; i++) {
        outlets[i - 1] = get_luaobj_elem<Stream>(L, 4, i);
        luaL_argcheck(L, outlets[i - 1] != nullptr, 4, format("element {} to be a Stream.", i).c_str());
    }

}

template <typename T, typename ...blk_params_T>
int finish_Block(lua_State* L, string& blk_name, Flowsheet* fs, vector<Stream*>& inlets, vector<Stream*>& outlets, blk_params_T& ...blk_params) {
    auto blk_p = fs->add_block<T>(blk_name, std::move(inlets), std::move(outlets), blk_params...);
    push_luaobj<Block, T>(L, blk_p, MT_BLOCK);
    return 1;
}

template <typename T>
int add_Block(lua_State* L, Flowsheet* fs) {
    string blk_name;
    vector<Stream*> inlets, outlets;
    start_Block(L, blk_name, inlets, outlets);
    return finish_Block<T>(L, blk_name, fs, inlets, outlets);
}

int add_Mixer(lua_State* L) {
    auto fs = check_luaobj<Flowsheet>(L, MT_FLOWSHEET, 1);
    return add_Block<Mixer>(L, fs);
}

int add_Splitter(lua_State* L) {
    auto fs = check_luaobj<Flowsheet>(L, MT_FLOWSHEET, 1);
    return add_Block<Splitter>(L, fs);
}

int add_Separator(lua_State* L) {
    auto fs = check_luaobj<Flowsheet>(L, MT_FLOWSHEET, 1);
    return add_Block<Separator>(L, fs);
}

int add_YieldReactor(lua_State* L) {
    auto fs = check_luaobj<Flowsheet>(L, MT_FLOWSHEET, 1);
    return add_Block<YieldReactor>(L, fs);
}

int add_MultiYieldReactor(lua_State* L) {
    auto fs = check_luaobj<Flowsheet>(L, MT_FLOWSHEET, 1);
    string blk_name;
    vector<Stream*> inlets, outlets;
    start_Block(L, blk_name, inlets, outlets);
    auto n_args = lua_gettop(L);
    auto n_feeds = inlets.size();

    // Reactor name,
    string reactor_name = luaL_checkstring(L, 5);
    luaL_argcheck(L, !reactor_name.empty(), 5, "reactor name cannot be empty.");

    // Feed names.
    vector<string> feed_names(n_feeds);
    for (int i = 0, j = 6; i < n_feeds; i++, j++) {
        feed_names[i] = luaL_checkstring(L, j);
        luaL_argcheck(L, !feed_names[i].empty(), j, "feed name cannot be empty.");
    }

    return finish_Block<MultiYieldReactor, const string&, const vector<string>&>(L, blk_name, fs, inlets, outlets, reactor_name, feed_names);
}

int add_StoicReactor(lua_State* L) {
    auto fs = check_luaobj<Flowsheet>(L, MT_FLOWSHEET, 1);
    string blk_name;
    vector<Stream*> inlets, outlets;
    start_Block(L, blk_name, inlets, outlets);

    auto u_mw_def = fs->m->unit_set.get_default_unit("molewt");
    Unit* u_mw;
    bool ok {true};

    // Arg 5 is a table of molecular weights.
    luaL_argcheck(L, lua_istable(L, 5) && lua_rawlen(L, 5) > 0, 5,
        "expected a table of one or more molecular weight specifications.");
    auto n_mw = lua_rawlen(L, 5);
    unordered_map<string, Quantity> mw {};
    for (lua_Unsigned i = 1; i <= n_mw; i++) {
        luaL_argcheck(L, lua_rawgeti(L, 5, i) == LUA_TTABLE, 5,      // Push elem i of MW table, e.g., {"H2", 2.0, "kg/kmol"}
            format("expected element {} to be a table.", i).c_str());
        int n_elem = lua_rawlen(L, -1);
        luaL_argcheck(L, n_elem == 2 || n_elem == 3, 5,
            format("expected element {} to look like {{component_name, number, unit}}.", i).c_str());
        string comp_name = get_string_elem(L, -1, 1);
        luaL_argcheck(L, !comp_name.empty(), 5, format("component name in element {} cannot be empty.", i).c_str());
        double mw_val = get_double_elem(L, -1, 2, ok);
        luaL_argcheck(L, ok, 5, format("expected molecular weight in element {} to be a number.", i).c_str());
        if (n_elem == 3) {
            auto type = lua_rawgeti(L, -1, 3);
            if (type == LUA_TSTRING) {
                string u_str = lua_tostring(L, -1);
                luaL_argcheck(L, fs->m->unit_set.units.contains(u_str), 5, format("unit \"{}\" not in the unit set.", u_str).c_str());
                u_mw = fs->m->unit_set.units[u_str].get();
            } else {
                u_mw = get_luaobj_elem<Unit>(L, -1, 3);
                luaL_argcheck(L, u_mw != nullptr, 5, format("expected element {} to look like {{component_name, number, unit}}.", i).c_str());
            }
            lua_pop(L, 1);
        }
        else
            u_mw = u_mw_def;

        mw[comp_name] = {mw_val, u_mw};
        lua_pop(L, 1);   // Pop elem i
    }

    // Arg 6 is a table of stoichiometric coefficients.
    luaL_argcheck(L, lua_istable(L, 6) && lua_rawlen(L, 6) > 0, 6,
        "expected a table of one or more stoichiometric coefficients.");
    auto n_rx = lua_rawlen(L, 6);
    vector<unordered_map<string, double>> stoic_coef(n_rx);
    for (lua_Unsigned i = 1; i <= n_rx; i++) {
        luaL_argcheck(L, lua_rawgeti(L, 6, i) == LUA_TTABLE, 6,    // Push reaction i, e.g., { {"H2", -1.0}, {"C2H2", -1.0}, {"C2H4", 1.0} }
            format("expected element {} to be a table.", i).c_str());
        int n_coef = lua_rawlen(L, -1);
        luaL_argcheck(L, n_coef > 0, 6, format("expected at least one coefficient in element {}.", i).c_str());
        for (lua_Unsigned j = 1; j <= n_coef; j++) {
            luaL_argcheck(L, lua_rawgeti(L, -1, j) == LUA_TTABLE, 6,    // Push stoic coeff j, e.g., {"H2", -1.0}
                format("expected element {} to be a table.", i).c_str());
            string comp_name = get_string_elem(L, -1, 1);
            luaL_argcheck(L, !comp_name.empty(), 6,
                format("component name in element {} cannot be empty.", i).c_str());
            double coef = get_double_elem(L, -1, 2, ok);
            luaL_argcheck(L, ok, 6, format("coefficient in element {} to be a number.", i).c_str());
            lua_pop(L, 1);                                              // Pop stoic coef j
            stoic_coef[i - 1][comp_name] = coef;
        }
        lua_pop(L, 1);                                             // Pop reaction i
    }
    
    // Arg 7 is a table of conversion specs.
    luaL_argcheck(L, lua_istable(L, 7) && lua_rawlen(L, 7) > 0, 7,
        "expected a table of one or more conversion specifications.");
    auto n_keys = lua_rawlen(L, 7);
    luaL_argcheck(L, n_keys == n_rx, 7,
        format("expected {} conversion specifications, got {}.", n_rx, n_keys).c_str());
    vector<string> conversion_keys(n_keys);
    for (lua_Unsigned i = 1; i <= n_keys; i++) {
        string ckey = get_string_elem(L, 7, i);
        luaL_argcheck(L, !ckey.empty(), 7, "expected a component name.");
        conversion_keys[i - 1] = ckey;
    }

    return finish_Block<StoicReactor,
                        const unordered_map<string, Quantity>&,
                        const vector<unordered_map<string, double>>&,
                        const vector<string>&>(L, blk_name, fs, inlets, outlets, mw, stoic_coef, conversion_keys);
}

int add_Calc(lua_State* L) {
    auto fs = check_luaobj<Flowsheet>(L, MT_FLOWSHEET, 1);
    string calc_name = luaL_checkstring(L, 2);               

    auto calc_p = fs->add_calc(calc_name);

    push_luaobj<Calc>(L, calc_p, MT_CALC);
    return 1;
}

int add_bridge(lua_State* L) {
    auto m = check_luaobj<Model>(L, MT_MODEL, 1);
    Stream* strm_from {}, *strm_to {};
    strm_from = check_luaobj<Stream>(L, MT_STREAM, 2);
    strm_to   = check_luaobj<Stream>(L, MT_STREAM, 3);
    bool ok = m->add_bridge(strm_from, strm_to);
    lua_pushboolean(L, ok);
    return 1;
}

int connect(lua_State* L) {
    auto m = check_luaobj<Model>(L, MT_MODEL, 1);
    auto obj = get_luaobj(L, 2);
    luaL_argcheck(L, obj != nullptr, 2, "expected a Variable or a Stream.");
    if (obj->isa<Variable>()) {
        auto var1 = obj->as<Variable>();
        auto var2 = check_luaobj<Variable>(L, MT_QUANTITY, 3);
        luaL_argcheck(L, var2 != nullptr, 3, "expected a Variable.");
        auto conn = m->add_connection(var1, var2);
        push_luaobj<Connection>(L, conn, MT_CONNECTION);
        return 1;
    }
    else if (obj->isa<Stream>()) {
        auto strm = obj->as<Stream>();
        auto conn = strm->connect();
        push_luaobj<Connection>(L, conn, MT_CONNECTION);
        return 1;
    }
    return 0;
}

int connect_all(lua_State* L) {
    auto m = check_luaobj<Model>(L, MT_MODEL, 1);
    bool ok = m->index_fs->connect_streams();
    lua_pushboolean(L, ok);
    return 1;
}

int add_prices(lua_State* L) {
    auto m = check_luaobj<Model>(L, MT_MODEL, 1);
    auto n_args = lua_gettop(L);
    bool ok {true};
    for (lua_Unsigned i = 2; i <= n_args; i++) {
        luaL_argcheck(L, lua_istable(L, i), i, "expected a table.");
        auto n_elem = lua_rawlen(L, i);    // number of elements in the ith arg, where arg = {price_name, value, unit}
        luaL_argcheck(L, n_elem == 3, i, "expected a table that looks like {{string, number, Unit}}.");
        string price_name = get_string_elem(L, i, 1);
        luaL_argcheck(L, !price_name.empty(), i, "price name cannot be empty.");
        auto value = get_double_elem(L, i, 2, ok);
        luaL_argcheck(L, ok, i, "expecting price value to be a number.");
        auto unit = get_luaobj_elem<Unit>(L, i, 3);
        luaL_argcheck(L, unit != nullptr, i, "expecting element 3 to be a Unit.");

        auto p = m->add_price(price_name, value, unit);
        push_luaobj<Quantity, Price>(L, p, MT_QUANTITY);
    }

    return n_args - 1;
}

//---------------------------------------------------------

int Block_index(lua_State* L) {
    auto blk = check_luaobj<Block>(L, MT_BLOCK, 1);
    string key = luaL_checkstring(L, 2);
    if      (key == "name")        lua_pushstring(L, blk->name.c_str());
    else if (key == "init" || key == "initialize") lua_pushcfunction(L, initialize);
    else if (key == "eval_constraints") lua_pushcfunction(L, eval_constraints);
    else if (key == "eval_jacobian") lua_pushcfunction(L, eval_jacobian);
    else if (key == "eval_hessian") lua_pushcfunction(L, eval_hessian);
    else if (key == "show_variables") lua_pushcfunction(L, show_variables);
    else if (key == "show_constraints") lua_pushcfunction(L, show_constraints);
    else if (key == "show_jacobian") lua_pushcfunction(L, show_jacobian);
    else if (key == "show_hessian") lua_pushcfunction(L, show_hessian);
    else lua_pushnil(L);
    return 1;
}

void Block_register(lua_State* L) {
    luaL_newmetatable(L, MT_BLOCK);
    //lua_pushcfunction(L, Flowsheet_tostring); lua_setfield(L, -2, "__tostring");
    lua_pushcfunction(L, Block_index);    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);
}

//---------------------------------------------------------

int add_variables(lua_State* L) {
    auto calc_p = check_luaobj<Calc>(L, MT_CALC, 1);
    auto M = calc_p->fs->m;
    auto n_args = lua_gettop(L);
    for (lua_Unsigned i = 2; i <= n_args; i++) {
        luaL_checktype(L, i, LUA_TTABLE);
        auto n_elem = lua_rawlen(L, i);    // number of elements in the ith argument table
        luaL_argcheck(L, n_elem == 2, i, format("expected argument {} to look like {{string, Unit}}.", i).c_str());
        string var_name = get_string_elem(L, i, 1);  // element 1 is variable name
        luaL_argcheck(L, !var_name.empty(), i, "variable name cannot be empty.");
        auto unit = get_luaobj_elem<Unit>(L, i, 2);  // element 2 is Unit
        luaL_argcheck(L, unit != nullptr, i, format("expected element 2 of argument {} to be a Unit.", i).c_str());
        auto v = M->add_var(calc_p->prefix + var_name, unit);
        calc_p->x.push_back(v);
        push_luaobj<Quantity, Variable>(L, v, MT_QUANTITY);
    }

    return n_args - 1;
}

int add_constraints(lua_State* L) {
    auto calc_p = check_luaobj<Calc>(L, MT_CALC, 1);
    auto M = calc_p->fs->m;
    auto n_args = lua_gettop(L);
    for (lua_Unsigned i = 2; i <= n_args; i++) {
        string eq_name = luaL_checkstring(L, i);
        luaL_argcheck(L, !eq_name.empty(), i, "constraint name cannot be empty.");
        auto eq = M->add_constraint(calc_p->prefix + eq_name);
        calc_p->g.push_back(eq);
        push_luaobj<Constraint>(L, eq, MT_CONSTRAINT);
    }

    return n_args - 1;
}

int add_jacobian_nzs(lua_State* L) {
    auto calc_p = check_luaobj<Calc>(L, MT_CALC, 1);
    auto M = calc_p->fs->m;
    auto n_args = lua_gettop(L);
    for (lua_Unsigned i = 2; i <= n_args; i++) {
        luaL_checktype(L, i, LUA_TTABLE);
        auto n_elem = lua_rawlen(L, i);    // number of elements in the ith JNZ list
        luaL_argcheck(L, n_elem == 2, i, format("expected argument {} to look like {{Constraint, Variable}}.", i).c_str());
        auto con = get_luaobj_elem<Constraint>(L, i, 1); // element 1 is a Constraint
        luaL_argcheck(L, con != nullptr, i, format("expected element 1 of argument {} to be a Constraint.", i).c_str());
        auto var = get_luaobj_elem<Variable>(L, i, 2);   // element 2 is a Variable
        luaL_argcheck(L, var != nullptr, i, format("expected element 2 of argument {} to be a Variable.", i).c_str());
        auto jnz = M->add_J_NZ(con, var);
        calc_p->J.push_back(jnz);
        push_luaobj<JacobianNZ>(L, jnz, MT_JACOBIAN_NZ);
    }

    return n_args - 1;
}

int add_hessian_nzs(lua_State* L) {
    auto calc_p = check_luaobj<Calc>(L, MT_CALC, 1);
    auto M = calc_p->fs->m;
    auto n_args = lua_gettop(L);
    for (lua_Unsigned i = 2; i <= n_args; i++) {
        luaL_checktype(L, i, LUA_TTABLE);
        auto n_elem = lua_rawlen(L, i);    // number of elements in the ith HNZ list
        luaL_argcheck(L, n_elem == 3, i, format("expected argument {} to look like {{Constraint, Variable, Variable}}.", i).c_str());
        auto con = get_luaobj_elem<Constraint>(L, i, 1); // element 1 is a Constraint
        luaL_argcheck(L, con != nullptr, i, format("expected element 1 of argument {} to be a Constraint.", i).c_str());
        auto var1 = get_luaobj_elem<Variable>(L, i, 2);   // element 2 is a Variable
        luaL_argcheck(L, var1 != nullptr, i, format("expected element 2 of argument {} to be a Variable.", i).c_str());
        auto var2 = get_luaobj_elem<Variable>(L, i, 3);   // element 3 is a Variable
        luaL_argcheck(L, var2 != nullptr, i, format("expected element 3 of argument {} to be a Variable.", i).c_str());
        auto hnz = M->add_H_NZ(con, var1, var2);
        calc_p->H.push_back(hnz);
        push_luaobj<HessianNZ>(L, hnz, MT_HESSIAN_NZ);
    }

    return n_args - 1;
}

int Calc_index(lua_State* L) {
    auto calc = check_luaobj<Calc>(L, MT_CALC, 1);
    string key = luaL_checkstring(L, 2);
    if      (key == "name")        lua_pushstring(L, calc->name.c_str());
    else if (key == "eval_constraints") lua_pushcfunction(L, eval_constraints);
    else if (key == "show_constraints") lua_pushcfunction(L, show_constraints);
    else if (key == "show_variables") lua_pushcfunction(L, show_variables);
    else if (key == "show_jacobian") lua_pushcfunction(L, show_jacobian);
    else if (key == "add_vars" || key == "add_variables")  lua_pushcfunction(L, add_variables);
    else if (key == "add_cons" || key == "add_constraints")  lua_pushcfunction(L, add_constraints);
    else if (key == "add_jnzs" || key == "add_jacobian_nzs")  lua_pushcfunction(L, add_jacobian_nzs);
    else if (key == "add_hnzs" || key == "add_hessian_nzs")  lua_pushcfunction(L, add_hessian_nzs);
    else lua_pushnil(L);
    return 1;
}

void Calc_register(lua_State* L) {
    luaL_newmetatable(L, MT_CALC);
    //lua_pushcfunction(L, Flowsheet_tostring); lua_setfield(L, -2, "__tostring");
    lua_pushcfunction(L, Calc_index);    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);
}

//---------------------------------------------------------

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

int test(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    luaL_argcheck(L, lua_rawlen(L, 1) > 1, 1, "expected table to have more than 1 element");
    return 0;
}

static const luaL_Reg function_table[] {
    { "UnitSet",           UnitSet_new           },
    { "Quantity",          Quantity_new          },
    { "Model",             Model_new             },
    { "Solver",            Solver_new            },
    { "testerr",           test                  },
    { NULL,                NULL                  }   
};

void register_objs(lua_State* L) {
    Unit_register(L);
    UnitKind_register(L);
    UnitSet_register(L);
    Quantity_register(L);
    Constraint_register(L);
    JacobianNZ_register(L);
    HessianNZ_register(L);
    Model_register(L);
    Flowsheet_register(L);
    Stream_register(L);
    Block_register(L);
    Calc_register(L);
    Solver_register(L);
}

lua_State* start_lua() {
    auto L = luaL_newstate();
    if (!L) return nullptr;
    luaL_openlibs(L);
    scripter_lua_state = L;

    register_objs(L);
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


extern "C" EXPORT_LUAOPEN int luaopen_mboptlib(lua_State* L)
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    register_objs(L);
    lua_pushglobaltable(L);
    luaL_setfuncs(L, function_table, 0);

    return 0;                     
}