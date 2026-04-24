#include <sstream>
#include <fstream> // IWYU pragma: keep
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
#include "lua.h"

static ostream*          OUT {&cout};
static ofstream          OUTFILE;
static string            OUTFILE_NAME {"cout"};
static lua_State*        scripter_lua_state;

//---------------------------------------------------------

string strip_comment(const string_view s) {
    string res {};
    if (s.empty()) return res;
    for (const auto& c : s) {
        if (c == '-' && res.size() > 0 && res.back() == '-') {
            res.pop_back();
            break;
        }
        res += c;
    }
    return res;
}

string strip_space(const string_view s_) {
    string res {};
    string s {strip_comment(s_)};
    if (s.empty()) return res;
    for (const auto& c : s) {
        if (c == ' ' || c == '\t' || c == '\n') continue;
        res += c;
    }
    return res;
}

string trim_sides(const string_view s_) {
    string s {strip_comment(s_)};
    string res {};
    if (s.empty()) return res;
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

//---------------------------------------------------------

const char* get_string_elem(lua_State* L, int index, lua_Integer n) {
    const char* s {""};
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

//---------------------------------------------------------

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

using Solver = IpoptApplication;

// Wrapper for passing pointers to C++ objects to and from Lua while remembering their types
struct LuaObj {
    void*           obj_p;
    std::type_index base_type;
    std::type_index derived_type;

    template <typename T>
    bool isa() const { return (base_type == typeid(T) || derived_type == typeid(T)); }

    template <typename T>
    T* as() const { return isa<T>() ? static_cast<T*>(obj_p) : nullptr; }

};

template <typename T>
T* check_luaobj(lua_State* L, const char* mt, int arg) {
    auto luaobj = static_cast<LuaObj*>(luaL_checkudata(L, arg, mt));
    T* cppobj = luaobj->as<T>();
    luaL_argcheck(L, cppobj != nullptr, arg, format("failed to cast the Lua object to {}", mt).c_str());
    return cppobj;
}

LuaObj* get_luaobj(lua_State* L, int index) {
    return static_cast<LuaObj*>(lua_touserdata(L, index));
}

template <typename BaseT, typename DerivedT = void>
void push_luaobj(lua_State* L, void* p, const char* mt) {
    if (!p) { lua_pushnil(L); return; }
    void* luaobj_p = lua_newuserdatauv(L, sizeof(LuaObj), 0);   // push new userdata
    new (luaobj_p) LuaObj(p, typeid(BaseT), typeid(DerivedT));
    luaL_getmetatable(L, mt);                                   // push the metatable bound to mt
    lua_setmetatable(L, -2);                                    // pop the metatable bound to mt and set the userdata's metatable to it
                                                                // new userdata left on top of the stack
}

int LuaObj_eq(lua_State* L) {
    auto a = get_luaobj(L, 1);
    auto b = get_luaobj(L, 2);
    lua_pushboolean(L, a->obj_p == b->obj_p);
    return 1;
}

template <typename T>
T* get_luaobj_elem(lua_State* L, int index, lua_Integer n) {
    T* obj {nullptr};
    if (lua_rawgeti(L, index, n) == LUA_TUSERDATA) {    // push element n
        auto luaobj = get_luaobj(L, -1);
        if (luaobj != nullptr)
            obj = luaobj->as<T>();
    }
    lua_pop(L, 1);                                      // pop element n
    return obj;
}

int show_LuaObj_type(lua_State* L) {
    auto luaobj = get_luaobj(L, -1);
    const char* s {};
    if (luaobj->isa<Variable>())
        s = "Variable";
    else if (luaobj->isa<Objective>())
        s = "Objective";
    else if (luaobj->isa<ObjTerm>())
        s = "ObjTerm";
    else if (luaobj->isa<Quantity>() && luaobj->derived_type == typeid(Price))
        s = "Price";
    else if (luaobj->isa<Quantity>())
        s = "Quantity";
    else if (luaobj->isa<Unit>())
        s = "Unit";
    else if (luaobj->isa<UnitKind>())
        s = "UnitKind";
    else if (luaobj->isa<UnitSet>())
        s = "UnitSet";
    else if (luaobj->isa<Model>())
        s = "Model";
    else if (luaobj->isa<Flowsheet>())
        s = "Flowsheet";
    else if (luaobj->isa<Stream>())
        s = "Stream";
    else if (luaobj->isa<Calc>())
        s = "Calc";
    else if (luaobj->isa<Constraint>())
        s = "Constraint";
    else if (luaobj->isa<JacobianNZ>())
        s = "JacobianNZ";
    else if (luaobj->isa<HessianNZ>())
        s = "HessianNZ";
    else if (luaobj->isa<Connection>())
        s = "Connection";
    else if (luaobj->isa<Solver>())
        s = "Solver";
    else if (luaobj->isa<Mixer>())
        s = "Mixer";
    else if (luaobj->isa<Splitter>())
        s = "Splitter";
    else if (luaobj->isa<Separator>())
        s = "Separator";
    else if (luaobj->isa<YieldReactor>())
        s = "YieldReactor";
    else if (luaobj->isa<MultiYieldReactor>())
        s = "MultiYieldReactor";
    else if (luaobj->isa<StoicReactor>())
        s = "StoicReactor";
    else if (luaobj->isa<Block>())
        s = "Block";
    else
        s = "userdata";

    lua_pushstring(L, s);
    return 1;
}

//---------------------------------------------------------

bool split_reaction(const string& s, string& lhs, string& rhs) {
    const std::string coef    = R"((?:\d+(?:\.\d+)?)?)";
    const std::string species = R"([A-Za-z_][A-Za-z0-9_]*)";
    const std::string term    = coef + species;
    const std::string side    = term + R"((?:\+)" + term + ")*";
    const std::string arrow   = "->";
    const std::regex rxn {"^(" + side + ")" + arrow + "(" + side + ")$"};
    std::smatch m;
    if (!std::regex_match(s, m, rxn))
        return false;
    lhs = m[1].str();
    rhs = m[2].str();
    return true;
}

unordered_map<string, double> parse_reaction_side(const string& s) {
    unordered_map<string, double> stoic_coef {};
    const std::regex re_term {R"((\d+(?:\.\d+)?)?([A-Za-z_][A-Za-z0-9_]*))"};
    auto term_1 = std::sregex_iterator(s.begin(), s.end(), re_term);
    auto term_n = std::sregex_iterator();
    for (auto it = term_1; it != term_n; ++it) {
        const std::smatch& m = *it;
        double coef    = m[1].matched ? std::stod(m[1].str()) : 1.0;
        string species = m[2].str();
        stoic_coef[species] += coef;
    }
    return stoic_coef;
}

bool parse_reaction(const string& s, unordered_map<string, double>& stoic_coef) {
    string lhs {}, rhs{};
    if (!split_reaction(s, lhs, rhs)) return false;
    auto stoic_coef_lhs = parse_reaction_side(lhs);
    auto stoic_coef_rhs = parse_reaction_side(rhs);
    for (const auto& [species, coef] : stoic_coef_lhs)
        stoic_coef[species] = -coef;
    for (const auto& [species, coef] : stoic_coef_rhs)
        stoic_coef[species] += coef;
    return true;
}

int eval_reactions(lua_State* L) {
    string rxns = luaL_checkstring(L, 1);
    std::istringstream rxn_stream {rxns};
    vector<unordered_map<string, double>> rxn_stoic_coef {};

    string rxn_raw;
    while(std::getline(rxn_stream, rxn_raw)) {
        string rxn_ta = strip_space(rxn_raw);
        string rxn_ts = trim_sides(rxn_raw);
        if (rxn_ta.empty()) continue;
        unordered_map<string, double> stoic_coef {};
        if (!parse_reaction(rxn_ta, stoic_coef))
            lua_warning(L, format("syntax error in reaction \"{}\"", rxn_ts).c_str(), 0);
        else
            rxn_stoic_coef.push_back(std::move(stoic_coef));
    }

    lua_newtable(L);
    for (int i = 0; i < rxn_stoic_coef.size(); i++) {
        lua_newtable(L);  // ith reaction
        for (const auto& [species, coef] : rxn_stoic_coef[i]) {
            lua_pushnumber(L, coef);
            lua_setfield(L, -2, species.c_str());   // species = coef
        }
        lua_seti(L, -2, i + 1);
    }
    return 1;
}

//---------------------------------------------------------

int do_show_variables(lua_State* L, auto f) {
    auto obj = get_luaobj(L, 1);
    int n_args = lua_gettop(L);
    vector<string> var_names {};
    vector<string> glob_patterns {};
    for (int i = 2; i <= n_args; i++) {
        string s = luaL_checkstring(L, i);
        if (s.find('*') != string::npos) {
            for (size_t c = 0; (c = s.find('*', c)) != string::npos; c += 2)
                s.replace(c, 1, ".*");
            glob_patterns.push_back(s);
        }
        else
            var_names.push_back(s);
    }
    if (obj->isa<Model>())
        f(static_cast<Model*>(obj->obj_p), var_names, glob_patterns);
    else if (obj->isa<Block>()) {
        f(static_cast<Block*>(obj->obj_p), var_names, glob_patterns);
    }
    else if (obj->isa<Calc>())
        f(static_cast<Calc*>(obj->obj_p), var_names, glob_patterns);

    return 0;
}

int show_variables(lua_State* L) {
    return do_show_variables(L, [](auto* p, 
                                   const vector<string>& var_names,
                                   const vector<string> glob_patterns) 
                                { p->show_variables(*OUT, var_names, glob_patterns); });
}

int show_fixed(lua_State* L) {
    return do_show_variables(L, [](auto* p, 
                                   const vector<string>& var_names,
                                   const vector<string> glob_patterns) 
                                { p->show_fixed(*OUT, var_names, glob_patterns); });
}

int show_active(lua_State* L) {
    return do_show_variables(L, [](auto* p, 
                                   const vector<string>& var_names,
                                   const vector<string> glob_patterns) 
                                { p->show_active(*OUT, var_names, glob_patterns); });
}

//---------------------------------------------------------

int delegate(lua_State* L, auto f) {
    auto obj = get_luaobj(L, 1);
    if (obj->isa<Model>())
        f(static_cast<Model*>(obj->obj_p));
    else if (obj->isa<Block>()) {
        auto blk = obj->as<Block>();
        switch (blk->blk_type) {
            case BlockType::Mixer:
                f(static_cast<Mixer*>(obj->obj_p));
                break;
            case BlockType::Splitter:
                f(static_cast<Splitter*>(obj->obj_p));
                break;
            case BlockType::Separator:
                f(static_cast<Separator*>(obj->obj_p));
                break;
            case BlockType::YieldReactor:
                f(static_cast<YieldReactor*>(obj->obj_p));
                break;
            case BlockType::MultiYieldReactor:
                f(static_cast<MultiYieldReactor*>(obj->obj_p));
                break;
            case BlockType::StoicReactor:
                f(static_cast<StoicReactor*>(obj->obj_p));
                break;
        }
    }
    else if (obj->isa<Calc>())
        f(static_cast<Calc*>(obj->obj_p));

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

int show_constraints(lua_State* L) {
    return delegate(L, [](auto* p) { p->show_constraints(*OUT); });
}

int show_jacobian(lua_State* L) {
    return delegate(L, [](auto* p) { p->show_jacobian(*OUT); });
}

int show_hessian(lua_State* L) {
    return delegate(L, [](auto* p) { p->show_hessian(*OUT); });
}

//---------------------------------------------------------

int set_output(lua_State* L) {
    string filename;
    if (lua_gettop(L) == 0)
        filename = "cout";
    else
        filename = luaL_checkstring(L, 1);

    OUT = &cout;
    OUTFILE_NAME = "cout";
    if (OUTFILE.is_open()) OUTFILE.close();
    if (filename != "cout" && filename != "stdout" && filename != "terminal") {
        OUTFILE.open(filename, std::ios::out);
        if (!OUTFILE.is_open()) luaL_error(L, format("unable to open the file \"{}\"", filename).c_str());
        OUT = &OUTFILE;
        OUTFILE_NAME = filename;
    }

    lua_pushstring(L, OUTFILE_NAME.c_str());
    return 1;
}

int write_variables(lua_State* L) {
    auto obj = get_luaobj(L, 1);
    ostream* dest = OUT;
    ofstream file;
    if (lua_gettop(L) > 1) {
        string filename = luaL_checkstring(L, 2);
        file.open(filename, std::ios::out);
        if (!file.is_open())
            lua_warning(L, format("unable to open the file \"{}\"", filename).c_str(), 0);
        else
            dest = &file;
    }
    if (obj->isa<Model>())
        obj->as<Model>()->write_variables(*dest);
    else if (obj->isa<Calc>())
        obj->as<Calc>()->write_variables(*dest);
    else if (obj->isa<Block>())
        obj->as<Block>()->write_variables(*dest);

    file.close();
    return 0;
}

int read_variables(lua_State* L) {
    auto filename = luaL_checkstring(L, 1);
    if (luaL_loadfile(L, filename) != LUA_OK)   // pushes compiled chunk
        luaL_error(L, format("error executing Lua script \"{}\"", filename).c_str());
    else {
        if (lua_pcall(L, 0, 1, 0) != LUA_OK)  // pushes return value from the script
            luaL_error(L, format("error executing Lua script \"{}\"", filename).c_str());
        if (!lua_isstring(L, -1))
            luaL_error(L, format("expected Lua script \"{}\" to return a string", filename).c_str());
    }
    return 1;
}

//---------------------------------------------------------

int Unit_tostring(lua_State* L) {
    auto u = check_luaobj<Unit>(L, MT_UNIT, 1);
    lua_pushstring(L, format("{}, kind={}", u->str, u->kind->str).c_str());
    return 1;
}

int Unit_index(lua_State* L) {
    auto u = check_luaobj<Unit>(L, MT_UNIT, 1);
    string key = luaL_checkstring(L, 2);
    if      (key == "kind")    push_luaobj<UnitKind>(L, u->kind, MT_UNITKIND);
    else if (key == "unitset") push_luaobj<UnitSet>(L, u->unitset, MT_UNITSET);
    else if (key == "str")     lua_pushstring(L, u->str.c_str());
    else if (key == "ratio")   lua_pushnumber(L, u->ratio);
    else if (key == "offset")  lua_pushnumber(L, u->offset);
    else                       lua_pushnil(L);
    return 1;
}

void Unit_register(lua_State* L) {
    luaL_newmetatable(L, MT_UNIT);
    lua_pushcfunction(L, LuaObj_eq);     lua_setfield(L, -2, "__eq");
    lua_pushcfunction(L, Unit_tostring); lua_setfield(L, -2, "__tostring");
    lua_pushcfunction(L, Unit_index);    lua_setfield(L, -2, "__index");
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
    else if (key == "str")          lua_pushstring(L, uk->str.c_str());
    else                            lua_pushnil(L);
    return 1;
}

void UnitKind_register(lua_State* L) {
    luaL_newmetatable(L, MT_UNITKIND);
    lua_pushcfunction(L, LuaObj_eq);         lua_setfield(L, -2, "__eq");
    lua_pushcfunction(L, UnitKind_tostring); lua_setfield(L, -2, "__tostring");
    lua_pushcfunction(L, UnitKind_index);    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);
}

//---------------------------------------------------------

static vector<unique_ptr<UnitSet>> unitsets {};

int UnitSet_add_kind(lua_State* L) {
    auto us = check_luaobj<UnitSet>(L, MT_UNITSET, 1);                        // arg 1 is a UnitSet
    string kind_str = luaL_checkstring(L, 2);                                 // arg 2 is a kind string
    string base_unit_str = luaL_checkstring(L, 3);                            // arg 3 is a base unit string
    string default_unit_str = luaL_optstring(L, 4, base_unit_str.c_str());    // arg 4 is a default unit string
    auto ukind = us->add_kind(kind_str, base_unit_str, default_unit_str);
    if (ukind == nullptr)
        luaL_error(L, "failed to create a new UnitKind");
    else
        push_luaobj<UnitKind>(L, ukind, MT_UNITKIND);
    return 1;
}

int UnitSet_add_unit(lua_State* L) {
    auto us = check_luaobj<UnitSet>(L, MT_UNITSET, 1);  // arg 1 is a UnitSet
    string kind_str = luaL_checkstring(L, 2);           // arg 2 is a kind string
    string unit_str = luaL_checkstring(L, 3);           // arg 3 is a unit string
    double unit_ratio = luaL_optnumber(L, 4, 1.0);      // arg 4 is a unit ratio
    double unit_offset = luaL_optnumber(L, 5, 0.0);     // arg 5 is a unit offset
    auto unit = us->add_unit(unit_str, kind_str, unit_ratio, unit_offset);
    if (unit == nullptr)
        luaL_argerror(L, 2, format("kind \"{}\" is not in the UnitSet", kind_str).c_str());
    else
        push_luaobj<Unit>(L, unit, MT_UNIT);
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
        luaL_argcheck(L, !kind_str.empty(), 1, "kind string cannot be empty");
        luaL_checktype(L, -1, LUA_TTABLE);      // value is a table the looks like {base_unit_str, <default_unit_str>}
        auto n_str = lua_rawlen(L, -1);         // number of strings in the table
        string base_unit_str = get_string_elem(L, -1, 1);                                   // element 1 is the base unit string
        luaL_argcheck(L, !base_unit_str.empty(), 1, "base unit string cannot be empty");
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

        for (int i = 1; i <= n_units; i++) {
            auto type = lua_rawgeti(L, -1, i);      // push the ith unit table
            if (type == LUA_TTABLE) {
                auto n_elem = lua_rawlen(L, -1);    // number of elements in the ith unit table
                string unit_str = get_string_elem(L, -1, 1);                                // element 1 is unit_str
                luaL_argcheck(L, !unit_str.empty(), i, "unit string cannot be empty");
                bool ok1 {true}, ok2 {true};
                double unit_ratio = (n_elem >= 2 ? get_double_elem(L, -1, 2, ok1) : 1.0);   // element 2 is unit_ratio
                luaL_argcheck(L, ok1, i, "expected the unit_ratio to be a number");
                double unit_offset = (n_elem == 3 ? get_double_elem(L, -1, 3, ok2) : 0.0);  // element 3 (optional) is unit_offset
                luaL_argcheck(L, ok2, i, "expected the unit_offset to be a number");
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
    if      (key == "add_kind") lua_pushcfunction(L, UnitSet_add_kind);
    else if (key == "add_unit") lua_pushcfunction(L, UnitSet_add_unit);
    else if (key == "kinds") {
        lua_newtable(L);
        for (const auto& [kind_str, unit_kind] : us->kinds) {
            push_luaobj<UnitKind>(L, unit_kind.get(), MT_UNITKIND);
            lua_setfield(L, -2, kind_str.c_str());
        }            
    }
    else if (key == "units") {
        lua_newtable(L);
        for (const auto& [unit_str, unit] : us->units) {
            push_luaobj<Unit>(L, unit.get(), MT_UNIT);
            lua_setfield(L, -2, unit_str.c_str());
        }            
    }
    else
        lua_pushnil(L);
    
    return 1;
}

void UnitSet_register(lua_State* L) {
    luaL_newmetatable(L, MT_UNITSET);
    lua_pushcfunction(L, LuaObj_eq);        lua_setfield(L, -2, "__eq");
    lua_pushcfunction(L, UnitSet_tostring); lua_setfield(L, -2, "__tostring");
    lua_pushcfunction(L, UnitSet_index);    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);
}

//---------------------------------------------------------

// This makes Ipopt output show up in a Jupyter notebook output cell
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

static vector<Solver*> solvers {};

int Solver_new(lua_State* L) {
    auto solver = IpoptApplicationFactory();
    if (std::getenv("JPY_PARENT_PID") != nullptr) {
        solver->Jnlst()->DeleteAllJournals();
        solver->Jnlst()->AddJournal(new LuaJournal(L));
    }
    solvers.push_back(solver);
    push_luaobj<Solver>(L, solver, MT_SOLVER);
    return 1;
}

int Solver_set_option(lua_State* L) {
    auto slv = check_luaobj<Solver>(L, MT_SOLVER, 1);
    string option = luaL_checkstring(L, 2);
    luaL_checkany(L, 3);
    auto type = lua_type(L, 3);
    if (type == LUA_TNUMBER) {
        if (lua_isinteger(L, 3))
            slv->Options()->SetIntegerValue(option, lua_tointeger(L, 3));
        else
            slv->Options()->SetNumericValue(option, lua_tonumber(L, 3));
    }
    else if (type == LUA_TSTRING) {
        slv->Options()->SetStringValue(option, lua_tostring(L, 3));
    }
    else
        luaL_argerror(L, 3, "expected a string, integer, or double");

    return 0;
}

int Solver_initialize(lua_State* L) {
    auto slv = check_luaobj<Solver>(L, MT_SOLVER, 1);
    lua_pushinteger(L, slv->Initialize());
    return 1;
}

int Solver_tostring(lua_State* L) {
    auto slv = check_luaobj<Solver>(L, MT_SOLVER, 1);
    lua_pushstring(L, "Ipopt");
    return 1;
}

int Solver_solve(lua_State* L) {
    auto slv = check_luaobj<Solver>(L, MT_SOLVER, 1);
    auto M   = check_luaobj<Model>(L, MT_MODEL, 2);
    lua_pushinteger(L, slv->OptimizeTNLP(M));
    return 1;
}

int Solver_index(lua_State* L) {
    auto slv = check_luaobj<Solver>(L, MT_SOLVER, 1);
    string key = luaL_checkstring(L, 2);
    if      (key == "init" || key == "initialize") lua_pushcfunction(L, Solver_initialize);
    else if (key == "set_option")                  lua_pushcfunction(L, Solver_set_option);
    else if (key == "solve")                       lua_pushcfunction(L, Solver_solve);
    else                                           lua_pushnil(L);

    return 1;
}

void Solver_register(lua_State* L) {
    luaL_newmetatable(L, MT_SOLVER);
    lua_pushcfunction(L, LuaObj_eq);       lua_setfield(L, -2, "__eq");
    lua_pushcfunction(L, Solver_tostring); lua_setfield(L, -2, "__tostring");
    lua_pushcfunction(L, Solver_index);    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);
}

//---------------------------------------------------------

static vector<unique_ptr<Quantity>> quantities {};

int do_add_obj(lua_State* L, Objective* obj, int n_args, int n_start, int n_terms, bool new_obj = false) {
    // Args are either ObjTerms or existing Objectives or names of existing Objectives.
    int top1 = lua_gettop(L);
    vector<std::variant<ObjTerm*, Objective*>> terms(n_terms);
    for (int i = n_start; i <= n_args; i++) {
        if (lua_istable(L, i)) {    // ith arg is a table that defines an ObjTerm
            const char* msg {"expected a table that looks like {string, Variable, Price, Unit, <number>}"};
            auto n_elem = lua_rawlen(L, i);    // number of elements in the ith arg, where arg = {term_name, Variable, Price, Unit, <scale>}
            luaL_argcheck(L, n_elem == 4 || n_elem == 5, i, msg);
            string term_name = get_string_elem(L, i, 1);    // element 1 is an objective term name
            luaL_argcheck(L, !term_name.empty(), i, "ObjTerm name cannot be empty");
            Variable* var;
            auto type = lua_rawgeti(L, i, 2);  // push elem 2
            if (type == LUA_TSTRING) {     // element 2 is a variable name or a Variable
                string var_name = get_string_elem(L, i, 2);
                luaL_argcheck(L, !var_name.empty(), i, "Variable name cannot be empty");
                luaL_argcheck(L, obj->m->x_map.contains(var_name), i, format("Variable \"{}\" is not in the Model", var_name).c_str());
                var = obj->m->x_map[var_name];
            }
            else {
                var = get_luaobj_elem<Variable>(L, i, 2);
                luaL_argcheck(L, var != nullptr, i, "expected a Variable");
            }
            lua_pop(L, 1);                     // pop elem 2

            Price* price;
            type = lua_rawgeti(L, i, 3);       // push elem 3
            if (type == LUA_TSTRING) {     // element 3 is a price name or a Price
                string price_name = get_string_elem(L, i, 3);
                luaL_argcheck(L, !price_name.empty(), i, "Price name cannot be empty");
                luaL_argcheck(L, obj->m->prices.contains(price_name), i, format("Price \"{}\" is not in the Model", price_name).c_str());
                price = obj->m->prices[price_name].get();
            }
            else {
                price = get_luaobj_elem<Price>(L, i, 3);
                luaL_argcheck(L, price != nullptr, i, "expected a Price");
            }
            lua_pop(L, 1);                     // pop elem 3

            Unit* unit;
            type = lua_rawgeti(L, i, 4);       // push elem 4
            if (type == LUA_TSTRING) {     // element 4 is a unit string or a Unit
                string unit_str = get_string_elem(L, i, 4);
                luaL_argcheck(L, !unit_str.empty(), i, "Unit string cannot be empty");
                luaL_argcheck(L, obj->m->unitset->units.contains(unit_str), i, format("Unit \"{}\" is not in the UnitSet", unit_str).c_str());
                unit = obj->m->unitset->units[unit_str].get();
            }
            else {
                unit = get_luaobj_elem<Unit>(L, i, 4);
                luaL_argcheck(L, unit != nullptr, i, "expected a Unit");
            }
            lua_pop(L, 1);                     // pop elem 4

            double scale {1.0};
            if (n_elem == 5) {             // element 5, if present, is a scale factor
                bool ok {true};
                scale = get_double_elem(L, i, 5, ok);
                luaL_argcheck(L, ok, i, "expected scale factor to be a number");
            }

            terms[i - n_start] = obj->add_objterm(term_name, var, price, unit, scale);
        }
        else {  // ith arg is an Objective or a name of an Objective
            Objective* child_obj {};
            if (lua_isuserdata(L, i))
                child_obj = check_luaobj<Objective>(L, MT_QUANTITY, i);
            else if (lua_isstring(L, i)) {
                string child_obj_name = lua_tostring(L, i);
                luaL_argcheck(L, !child_obj_name.empty(), i, "objective name cannot be empty");
                luaL_argcheck(L, obj->m->objectives.contains(child_obj_name), i,
                    format("Objective \"{}\" is not in the Model", child_obj_name).c_str());
                child_obj = obj->m->objectives[child_obj_name].get();
            }
            else
                luaL_argerror(L, i, " expected an Objective or the name of an Objective");

            obj->add_objective(child_obj);
            terms[i - n_start] = child_obj;
        }         
    }
    int top2 = lua_gettop(L);
    if (top1 != top2)
        luaL_error(L, format("do_add_obj: starting stack top = {}, ending stack top = {}", top1, top2).c_str());

    // Push a pointer to the objective.
    if (new_obj) push_luaobj<Quantity, Objective>(L, obj, MT_QUANTITY);

    // Push pointers to the objective terms.
    for (int i = 0; i < n_terms; i++)
        if (std::holds_alternative<ObjTerm*>(terms[i]))
            push_luaobj<Quantity, ObjTerm>(L, std::get<ObjTerm*>(terms[i]), MT_QUANTITY);
        else
            push_luaobj<Quantity, Objective>(L, std::get<Objective*>(terms[i]), MT_QUANTITY);

    lua_checkstack(L, n_terms + 1);
    return n_terms + (new_obj ? 1 : 0);
}

int Objective_add_terms(lua_State* L) {
    auto obj = check_luaobj<Objective>(L, MT_QUANTITY, 1);
    auto n_args = lua_gettop(L);

    int n_terms = n_args - 1;
    int n_start = 2;

    return do_add_obj(L, obj, n_args, n_start, n_terms);

}

int Variable_fix(lua_State* L) {
    auto v = check_luaobj<Variable>(L, MT_QUANTITY, 1);
    v->fix();
    return 0;
}

int Variable_free(lua_State* L) {
    auto v = check_luaobj<Variable>(L, MT_QUANTITY, 1);
    v->free();
    return 0;
}

int Quantity_new(lua_State* L) {
    double value {0.0};
    Unit* u {nullptr};
    if (lua_isuserdata(L, 1)) {
        auto q_source = check_luaobj<Quantity>(L, MT_QUANTITY, 1);
        value = q_source->value;
        u = q_source->unit;
    }
    else {
        value = luaL_checknumber(L, 1);
        u = check_luaobj<Unit>(L, MT_UNIT, 2);
    }
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
    auto obj = get_luaobj(L, 1);
    if (obj->isa<Variable>())
        lua_pushstring(L, obj->as<Variable>()->to_str_2().c_str());
    else if (obj->isa<Quantity>())
        lua_pushstring(L, obj->as<Quantity>()->to_str_2().c_str());
    else
        lua_pushnil(L);

    return 1;
}

double Quantity_convert_value(lua_State* L, Quantity* q, double val, int index) {
    Unit* u {nullptr};
    if (lua_isstring(L, index)) {
        const auto& units_ = q->unit->unitset->units;
        string u_str = lua_tostring(L, index);
        luaL_argcheck(L, units_.contains(u_str), index, format("Unit \"{}\" is not in the UnitSet", u_str).c_str());
        u = units_.at(u_str).get();
    }
    else
        u = check_luaobj<Unit>(L, MT_UNIT, index);

    return q->convert(val, u);
}

int Quantity_value_in(lua_State* L) {
    auto q = check_luaobj<Quantity>(L, MT_QUANTITY, 1);
    lua_pushnumber(L, Quantity_convert_value(L, q, q->value, 2));
    return 1;
}

int Quantity_set_value(lua_State* L) {
    auto q = check_luaobj<Quantity>(L, MT_QUANTITY, 1);
    auto val = luaL_checknumber(L, 2);
    q->value = Quantity_convert_value(L, q, val, 3);
    return 0;
}

void Quantity_change_unit(lua_State* L, Quantity* q, int index) {
    Unit* u {nullptr};
    if (lua_isstring(L, index)) {
        string unit_str = lua_tostring(L, index);
        const auto& units_ = q->unit->unitset->units;
        luaL_argcheck(L, units_.contains(unit_str), index, format("Unit \"{}\" is not in the UnitSet", unit_str).c_str());
        u = units_.at(unit_str).get();
    }
    else
        u = check_luaobj<Unit>(L, MT_UNIT, index);
    q->change_unit(u);
}

int Variable_set_lower(lua_State* L) {
    auto v = check_luaobj<Variable>(L, MT_QUANTITY, 1);
    auto val = luaL_checknumber(L, 2);
    v->lower = Quantity_convert_value(L, v, val, 3);
    return 0;
}

int Variable_set_upper(lua_State* L) {
    auto v = check_luaobj<Variable>(L, MT_QUANTITY, 1);
    auto val = luaL_checknumber(L, 2);
    v->upper = Quantity_convert_value(L, v, val, 3);
    return 0;
}

int Quantity_index(lua_State* L) {
    const auto obj = get_luaobj(L, 1);
    const auto q = obj->as<Quantity>();
    const auto v = obj->as<Variable>();
    const auto o = obj->as<Objective>();

    string key = luaL_checkstring(L, 2);
    if      (key == "v" || key == "value")                 lua_pushnumber(L, q->value);
    else if (key == "name")
        if (q->name.empty()) lua_pushnil(L);
        else                 lua_pushstring(L, q->name.c_str());
    else if (key == "u" || key == "unit")                  push_luaobj<Unit>(L, q->unit, MT_UNIT);
    else if (key == "bv" || key == "base_value")           lua_pushnumber(L, q->convert_to_base());
    else if (key == "bu" || key == "base_unit")            push_luaobj<Unit>(L, q->unit->kind->base_unit, MT_UNIT);
    else if (key == "vin" || key == "value_in")            lua_pushcfunction(L, Quantity_value_in);
    else if ((key == "lb" || key == "lower") && v)         if (v->lower.has_value()) lua_pushnumber(L, v->lower.value()); else lua_pushnil(L);
    else if ((key == "ub" || key == "upper") && v)         if (v->upper.has_value()) lua_pushnumber(L, v->upper.value()); else lua_pushnil(L);
    else if (key == "spec" && v)                           lua_pushstring(L, (v->is_fixed() ? "fixed" : "free"));
    else if (key == "fix" && v)                            lua_pushcfunction(L, Variable_fix);
    else if (key == "free" && v)                           lua_pushcfunction(L, Variable_free);
    else if ((key == "add" || key == "add_terms") && o)    lua_pushcfunction(L, Objective_add_terms);
    else if (key == "set_v" || key == "set_value")         lua_pushcfunction(L, Quantity_set_value);
    else if ((key == "set_lb" || key == "set_lower") && v) lua_pushcfunction(L, Variable_set_lower);
    else if ((key == "set_ub" || key == "set_upper") && v) lua_pushcfunction(L, Variable_set_upper);
    else                                                   lua_pushnil(L);

    return 1;
}

int Quantity_newindex(lua_State* L) {
    auto obj = get_luaobj(L, 1);
    auto q = obj->as<Quantity>();
    auto v = obj->as<Variable>();

    string key = luaL_checkstring(L, 2);
    if (key == "bv" || key == "base_value")
        q->convert_and_set(luaL_checknumber(L, 3));
    else if (key == "v" || key == "value")
        q->value = luaL_checknumber(L, 3);
    else if (key == "u" || key == "unit")
        Quantity_change_unit(L, q, 3);
    else if (key == "spec" && v) {
        string s = luaL_checkstring(L, 3);
        if      (s == "fixed") v->fix();
        else if (s == "free") v->free();
        else    lua_warning(L, R"(variable spec must be "fixed" or "free".)", 0);
    }
    else if ((key == "lb" || key == "lower") && v) {
        if (lua_isnil(L, 3))
            v->lower = std::nullopt;
        else
            v->lower = luaL_checknumber(L, 3);
    }
    else if ((key == "ub" || key == "upper") && v) {
        if (lua_isnil(L, 3))
            v->upper = std::nullopt;
        else
            v->upper = luaL_checknumber(L, 3);
    }
    else
        luaL_error(L, format("no property named \"{}\"", key).c_str());

    return 0;
}

double Quantity_binop(lua_State* L, auto binop) {
    double val[2] {0.0, 0.0};
    for (int i = 0, j = 1; i < 2; i++, j++)
        if (lua_isnumber(L, j))
            val[i] = lua_tonumber(L, j);
        else {
            auto q = check_luaobj<Quantity>(L, MT_QUANTITY, j);
            val[i] = *q;
        }
    return binop(val[0], val[1]);
}
    
int Quantity_add(lua_State* L) {
    lua_pushnumber(L, Quantity_binop(L, [](double a, double b) { return a + b; }));
    return 1;
}

int Quantity_sub(lua_State* L) {
    lua_pushnumber(L, Quantity_binop(L, [](double a, double b) { return a - b; }));
    return 1;
}
    
int Quantity_mul(lua_State* L) {
    lua_pushnumber(L, Quantity_binop(L, [](double a, double b) { return a * b; }));
    return 1;
}

int Quantity_div(lua_State* L) {
    lua_pushnumber(L, Quantity_binop(L, [](double a, double b) { return a / b; }));
    return 1;
}

int Quantity_unm(lua_State* L) {
    auto q = check_luaobj<Quantity>(L, MT_QUANTITY, 1);
    double result = -(*q);
    lua_pushnumber(L, result);
    return 1;
}

void Quantity_register(lua_State* L) {
    luaL_newmetatable(L, MT_QUANTITY);
    lua_pushcfunction(L, LuaObj_eq);         lua_setfield(L, -2, "__eq");
    lua_pushcfunction(L, Quantity_tostring); lua_setfield(L, -2, "__tostring");
    lua_pushcfunction(L, Quantity_index);    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, Quantity_newindex); lua_setfield(L, -2, "__newindex");
    lua_pushcfunction(L, Quantity_add);      lua_setfield(L, -2, "__add");
    lua_pushcfunction(L, Quantity_sub);      lua_setfield(L, -2, "__sub");
    lua_pushcfunction(L, Quantity_mul);      lua_setfield(L, -2, "__mul");
    lua_pushcfunction(L, Quantity_div);      lua_setfield(L, -2, "__div");
    lua_pushcfunction(L, Quantity_unm);      lua_setfield(L, -2, "__unm");
    lua_pop(L, 1);
}

//---------------------------------------------------------

int Constraint_tostring(lua_State* L) {
    auto eq = check_luaobj<Constraint>(L, MT_CONSTRAINT, 1);
    lua_pushstring(L, eq->to_str_2().c_str());
    return 1;
}

int Constraint_index(lua_State* L) {
    auto eq = check_luaobj<Constraint>(L, MT_CONSTRAINT, 1);
    string key = luaL_checkstring(L, 2);
    if      (key == "v" || key == "value" || key == "bv") lua_pushnumber(L, eq->value);
    else if (key == "name")                               lua_pushstring(L, eq->name.c_str());
    else                                                  lua_pushnil(L);

    return 1;
}

int Constraint_newindex(lua_State* L) {
    auto eq = check_luaobj<Constraint>(L, MT_CONSTRAINT, 1);
    string key = luaL_checkstring(L, 2);
    if (key == "v" || key == "value" || key == "bv") eq->value = luaL_checknumber(L, 3);
    else
        luaL_error(L, format("no property named \"{}\"", key).c_str());

    return 0;
}

void Constraint_register(lua_State* L) {
    luaL_newmetatable(L, MT_CONSTRAINT);
    lua_pushcfunction(L, LuaObj_eq);           lua_setfield(L, -2, "__eq");
    lua_pushcfunction(L, Constraint_tostring); lua_setfield(L, -2, "__tostring");
    lua_pushcfunction(L, Constraint_newindex); lua_setfield(L, -2, "__newindex");
    lua_pushcfunction(L, Constraint_index);    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);
}

//---------------------------------------------------------

int JacobianNZ_index(lua_State* L) {
    auto jnz = check_luaobj<JacobianNZ>(L, MT_JACOBIAN_NZ, 1);
    string key = luaL_checkstring(L, 2);
    if      (key == "v" || key == "value" || key == "bv") lua_pushnumber(L, jnz->value);
    else if (key == "con" || key == "eq")                 push_luaobj<Constraint>(L, jnz->con, MT_CONSTRAINT);
    else if (key == "var")                                push_luaobj<Quantity, Variable>(L, jnz->var, MT_QUANTITY);
    else                                                  lua_pushnil(L);

    return 1;
}

int JacobianNZ_newindex(lua_State* L) {
    auto jnz = check_luaobj<JacobianNZ>(L, MT_JACOBIAN_NZ, 1);
    string key = luaL_checkstring(L, 2);
    if (key == "v" || key == "value" || key == "bv") jnz->value = luaL_checknumber(L, 3);
    else
        luaL_error(L, format("no property named \"{}\"", key).c_str());

    return 0;
}

void JacobianNZ_register(lua_State* L) {
    luaL_newmetatable(L, MT_JACOBIAN_NZ);
    lua_pushcfunction(L, LuaObj_eq);           lua_setfield(L, -2, "__eq");
    lua_pushcfunction(L, JacobianNZ_newindex); lua_setfield(L, -2, "__newindex");
    lua_pushcfunction(L, JacobianNZ_index);    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);
}

//---------------------------------------------------------

int HessianNZ_index(lua_State* L) {
    auto hnz = check_luaobj<HessianNZ>(L, MT_HESSIAN_NZ, 1);
    string key = luaL_checkstring(L, 2);
    if      (key == "v" || key == "value" || key == "bv") lua_pushnumber(L, hnz->value);
    else if (key == "con" || key == "eq")                 push_luaobj<Constraint>(L, hnz->con, MT_CONSTRAINT);
    else if (key == "var1")                               push_luaobj<Quantity, Variable>(L, hnz->var1, MT_QUANTITY);
    else if (key == "var2")                               push_luaobj<Quantity, Variable>(L, hnz->var2, MT_QUANTITY);
    else                                                  lua_pushnil(L);

    return 1;
}

int HessianNZ_newindex(lua_State* L) {
    auto hnz = check_luaobj<HessianNZ>(L, MT_HESSIAN_NZ, 1);
    string key = luaL_checkstring(L, 2);
    if (key == "v" || key == "value" || key == "bv") hnz->value = luaL_checknumber(L, 3);
    else
        luaL_error(L, format("no property named \"{}\"", key).c_str());

    return 0;
}

void HessianNZ_register(lua_State* L) {
    luaL_newmetatable(L, MT_HESSIAN_NZ);
    lua_pushcfunction(L, LuaObj_eq);          lua_setfield(L, -2, "__eq");
    lua_pushcfunction(L, HessianNZ_newindex); lua_setfield(L, -2, "__newindex");
    lua_pushcfunction(L, HessianNZ_index);    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);
}

//---------------------------------------------------------

static vector<Ipopt::SmartPtr<Model>> models {};

int Model_eval_expr(lua_State* L) {
    auto M = check_luaobj<Model>(L, MT_MODEL, 1);
    string exprs = luaL_checkstring(L, 2);
    std::istringstream expr_stream {exprs};

    const std::regex re_binop(R"((\S+)(=|<|>)([^\s_]+)(?:_(\S+))?)");
    const std::regex re_spec(R"((fix|free)\s+(\S+))");

    bool ok = true;
    string expr_raw;
    while(std::getline(expr_stream, expr_raw)) {
        string expr_ta = strip_space(expr_raw);
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
                lua_warning(L, format("the Variable \"{}\" used in expression \"{}\" is not in the Model", lhs, expr_ts).c_str(), 0);
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
                    auto rhs_unit = (M->unitset->units.contains(rhs_unit_str) ? M->unitset->units[rhs_unit_str].get() : nullptr);
                    if (rhs_unit == nullptr) {
                        ok = false;
                        lua_warning(L, format("the Unit \"{}\" used in expression \"{}\" is not in the UnitSet",
                            rhs_unit_str, expr_ts).c_str(), 0);
                        continue;
                    }
                    rhs_value = lhs_var->convert(rhs_value, rhs_unit);
                }
            }
            else {
                ok = false;
                lua_warning(L, format("syntax error on the right-hand side of expression \"{}\"", expr_ts).c_str(), 0);
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
                lua_warning(L, format("the Variable \"{}\" used in expression \"{}\" is not in the Model", rhs, expr_ts).c_str(), 0);
                continue;
            }
            
            if (lhs == "free")
                rhs_var->free();
            else 
                rhs_var->fix();
        }
        else {
            ok = false;
            lua_warning(L, format("syntax error in the expression \"{}\"", expr_ts).c_str(), 0);
        }
    }
    
    lua_pushboolean(L, ok);
    return 1;
}

int Model_eval_objective(lua_State* L) {
    auto m = check_luaobj<Model>(L, MT_MODEL, 1);
    if (lua_gettop(L) == 1) {
        lua_pushnumber(L, m->eval_objective());
        return 1;
    }
    auto obj = check_luaobj<Objective>(L, MT_QUANTITY, 2);
    lua_pushnumber(L, obj->eval());
    return 1;
}

int Model_eval_obj_grad(lua_State* L) {
    auto m = check_luaobj<Model>(L, MT_MODEL, 1);
    m->eval_obj_grad();
    return 0;
}

int Model_show_connections(lua_State* L) {
    auto m = check_luaobj<Model>(L, MT_MODEL, 1);
    m->show_connections(*OUT);
    return 0;
}

int Model_show_prices(lua_State* L) {
    auto m = check_luaobj<Model>(L, MT_MODEL, 1);
    m->show_prices(*OUT);
    return 0;
}

int Model_show_objective(lua_State* L) {
    auto m = check_luaobj<Model>(L, MT_MODEL, 1);
    if (lua_gettop(L) == 1) {
        m->show_objective(m->obj, *OUT);
        return 0;
    }
    auto obj = check_luaobj<Objective>(L, MT_QUANTITY, 2);
    m->show_objective(obj, *OUT);
    return 0;
}

int Model_show_obj_grad(lua_State* L) {
    auto m = check_luaobj<Model>(L, MT_MODEL, 1);
    m->show_obj_grad(*OUT);
    return 0;
}

int Model_connect_vars(lua_State* L) {
    auto m = check_luaobj<Model>(L, MT_MODEL, 1);
    auto v1 = check_luaobj<Variable>(L, MT_QUANTITY, 2);
    auto v2 = check_luaobj<Variable>(L, MT_QUANTITY, 3);
    auto conn = m->add_connection(v1, v2);
    if (conn != nullptr) push_luaobj<Connection>(L, conn, MT_CONNECTION);
    else                 lua_pushnil(L);
    return 1;
}

int Model_add_prices(lua_State* L) {
    auto m = check_luaobj<Model>(L, MT_MODEL, 1);
    auto n_args = lua_gettop(L);
    bool ok {true};
    lua_newtable(L);    // push table of prices
    for (int i = 2; i <= n_args; i++) {
        luaL_argcheck(L, lua_istable(L, i), i, "expected a table");
        auto n_elem = lua_rawlen(L, i);    // number of elements in the ith arg, where arg = {price_name, value, unit}
        luaL_argcheck(L, n_elem == 3, i, "expected a table that looks like {string, number, Unit}");
        string price_name = get_string_elem(L, i, 1);
        luaL_argcheck(L, !price_name.empty(), i, "Price name cannot be empty");
        auto value = get_double_elem(L, i, 2, ok);
        luaL_argcheck(L, ok, i, "expecting Price value to be a number");
        Unit* unit {nullptr};
        if (lua_rawgeti(L, i, 3) == LUA_TSTRING) {  // push elem 3
            string unit_str = lua_tostring(L, -1);
            luaL_argcheck(L, m->unitset->units.contains(unit_str), i, format("Unit \"{}\" is not in the UnitSet", unit_str).c_str());
            unit = m->unitset->units[unit_str].get();
        }
        else
            unit = get_luaobj_elem<Unit>(L, i, 3);
        lua_pop(L, 1);                              // pop elem 3
        luaL_argcheck(L, unit != nullptr, i, "expecting element 3 to be a Unit");

        auto p = m->add_price(price_name, value, unit);
        push_luaobj<Quantity, Price>(L, p, MT_QUANTITY);    // push Price
        lua_setfield(L, -2, price_name.c_str());            // set {name = Price} and pop Price
    }

    return 1;
}

int Model_add_new_objective(lua_State* L) {
    auto m = check_luaobj<Model>(L, MT_MODEL, 1);
    auto n_args = lua_gettop(L);
    int n_start = 2;

    // arg 2 is a new objective name.
    string obj_name = luaL_checkstring(L, 2);
    n_start++;

    // arg 3 is a Unit or a unit string.
    Unit* obj_unit;
    luaL_argcheck(L, lua_isstring(L, 3) || lua_isuserdata(L, 3), 3, "expected a unit string or a Unit");
    if (lua_isstring(L, 3)) {
        string unit_str = lua_tostring(L, 3);
        luaL_argcheck(L, !unit_str.empty(), 3, "unit string cannot be empty");
        luaL_argcheck(L, m->unitset->units.contains(unit_str), 3, format("Unit \"{}\" is not in the UnitSet", unit_str).c_str());
        obj_unit = m->unitset->units[unit_str].get();
    }
    else 
        obj_unit = check_luaobj<Unit>(L, MT_UNIT, 3);
    n_start++;

    // If arg 4 is a number, it's the objective scale factor.
    double obj_scale {1.0};
    if (lua_isnumber(L, 4)) {
        obj_scale = lua_tonumber(L, 4);
        n_start++;
    }

    // Create a new objective function.
    auto obj = m->add_objective(obj_name, obj_unit, obj_scale);

    // Remaining args are either ObjTerms or existing Objectives or names of existing Objectives.
    int n_terms = n_args - n_start + 1;
    return do_add_obj(L, obj, n_args, n_start, n_terms, true);

}

int Model_get(lua_State* L) {
    auto m = check_luaobj<Model>(L, MT_MODEL, 1);
    string name = luaL_checkstring(L, 2);
    if (m->x_map.contains(name))
        push_luaobj<Quantity, Variable>(L, m->x_map[name], MT_QUANTITY);
    else if (m->g_map.contains(name))
        push_luaobj<Constraint>(L, m->g_map[name], MT_CONSTRAINT);
    else if (m->prices.contains(name))
        push_luaobj<Quantity, Price>(L, m->prices[name].get(), MT_QUANTITY);
    else if (m->objectives.contains(name))
        push_luaobj<Quantity, Objective>(L, m->objectives[name].get(), MT_QUANTITY);
    else
        lua_pushnil(L);

    return 1;
}

int Model_new(lua_State* L) {
    string name = luaL_checkstring(L, 1);
    string index_fs_name = luaL_checkstring(L, 2);
    auto us = check_luaobj<UnitSet>(L, MT_UNITSET, 3);

    Ipopt::SmartPtr<Model> m = new Model{name, index_fs_name, us};
    Model* m_p = GetRawPtr(m);
    models.push_back(std::move(m));
    push_luaobj<Model>(L, m_p, MT_MODEL);

    return 1;
}

int Model_tostring(lua_State* L) {
    std::ostringstream oss;
    auto m = check_luaobj<Model>(L, MT_MODEL, 1);
    m->show_model(oss);
    lua_pushstring(L, oss.str().c_str());
    return 1;
}

int Model_newindex(lua_State* L) {
    auto m = check_luaobj<Model>(L, MT_MODEL, 1);
    string key = luaL_checkstring(L, 2);
    if (key == "obj" || key == "objective") m->obj = check_luaobj<Objective>(L, MT_QUANTITY, 3);
    else
        luaL_error(L, format("no property named \"{}\"", key).c_str());

    return 0;
}

int Model_index(lua_State* L) {
    auto m = check_luaobj<Model>(L, MT_MODEL, 1);
    string key = luaL_checkstring(L, 2);
    if      (key == "name")                        lua_pushstring(L, m->name.c_str());
    else if (key == "index_fs")                    push_luaobj<Flowsheet>(L, m->index_fs.get(), MT_FLOWSHEET);
    else if (key == "unitset")                     push_luaobj<UnitSet>(L, m->unitset, MT_UNITSET);
    else if (key == "obj" || key == "objective")   push_luaobj<Quantity, Objective>(L, m->obj, MT_QUANTITY);
    else if (key == "eval")                        lua_pushcfunction(L, Model_eval_expr);
    else if (key == "get")                         lua_pushcfunction(L, Model_get);
    else if (key == "eval_constraints")            lua_pushcfunction(L, eval_constraints);
    else if (key == "eval_jacobian")               lua_pushcfunction(L, eval_jacobian);
    else if (key == "eval_hessian")                lua_pushcfunction(L, eval_hessian);
    else if (key == "eval_objective")              lua_pushcfunction(L, Model_eval_objective);
    else if (key == "eval_objgrad")                lua_pushcfunction(L, Model_eval_obj_grad);
    else if (key == "show_constraints")            lua_pushcfunction(L, show_constraints);
    else if (key == "show_variables" || key == "show_vars") lua_pushcfunction(L, show_variables);
    else if (key == "show_fixed")                  lua_pushcfunction(L, show_fixed);
    else if (key == "show_active")                 lua_pushcfunction(L, show_active);
    else if (key == "show_jacobian")               lua_pushcfunction(L, show_jacobian);
    else if (key == "show_hessian")                lua_pushcfunction(L, show_hessian);
    else if (key == "show_connections")            lua_pushcfunction(L, Model_show_connections);
    else if (key == "show_prices")                 lua_pushcfunction(L, Model_show_prices);
    else if (key == "show_objective")              lua_pushcfunction(L, Model_show_objective);
    else if (key == "show_objgrad")                lua_pushcfunction(L, Model_show_obj_grad);
    else if (key == "init" || key == "initialize") lua_pushcfunction(L, initialize);
    else if (key == "Prices")                      lua_pushcfunction(L, Model_add_prices);
    else if (key == "prices") {
        lua_newtable(L);
        for (const auto& [name, price] : m->prices) {
            push_luaobj<Price>(L, price.get(), MT_QUANTITY);
            lua_setfield(L, -2, name.c_str());
        }            
    }
    else if (key == "add_objective")               lua_pushcfunction(L, Model_add_new_objective);
    else if (key == "connect")                     lua_pushcfunction(L, Model_connect_vars);
    else if (key == "write_vars" || key == "write_variables") lua_pushcfunction(L, write_variables);
    else                                           lua_pushnil(L);
    return 1;
}

void Model_register(lua_State* L) {
    luaL_newmetatable(L, MT_MODEL);
    lua_pushcfunction(L, LuaObj_eq);      lua_setfield(L, -2, "__eq");
    lua_pushcfunction(L, Model_tostring); lua_setfield(L, -2, "__tostring");
    lua_pushcfunction(L, Model_index);    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, Model_newindex); lua_setfield(L, -2, "__newindex");
    lua_pop(L, 1);
}

//---------------------------------------------------------

int Stream_connect(lua_State* L) {
    auto strm1 = check_luaobj<Stream>(L, MT_STREAM, 1);
    Stream* strm2 {};
    if (lua_gettop(L) > 1) strm2 = check_luaobj<Stream>(L, MT_STREAM, 2);
    const auto conn = strm1->connect(strm2);
    const int n_conn = static_cast<int>(conn.size());
    if (lua_checkstack(L, n_conn)) {
        for (const auto& conn_p : conn)
            push_luaobj<Connection>(L, conn_p, MT_CONNECTION);
        return n_conn;
    }
    else {
        lua_pushnil(L);
        return 1;
    }
}

int Stream_tostring(lua_State* L) {
    auto strm = check_luaobj<Stream>(L, MT_STREAM, 1);
    lua_pushstring(L, strm->to_str().c_str());
    return 1;
}

int Stream_index(lua_State* L) {
    auto strm = check_luaobj<Stream>(L, MT_STREAM, 1);
    string key = luaL_checkstring(L, 2);
    if      (key == "name")       lua_pushstring(L, strm->name.c_str());
    else if (key == "connect")    lua_pushcfunction(L, Stream_connect);
    else if (key == "components") {
        lua_newtable(L);
        for (int i = 1; const auto& c : strm->comps) {
            lua_pushstring(L, c.c_str());
            lua_seti(L, -2, i++);
        }            
    }
    else lua_pushnil(L);
    return 1;
}

void Stream_register(lua_State* L) {
    luaL_newmetatable(L, MT_STREAM);
    lua_pushcfunction(L, LuaObj_eq);       lua_setfield(L, -2, "__eq");
    lua_pushcfunction(L, Stream_tostring); lua_setfield(L, -2, "__tostring");
    lua_pushcfunction(L, Stream_index);    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);
}

//---------------------------------------------------------

int Flowsheet_add_streams(lua_State* L) {
    auto fs = check_luaobj<Flowsheet>(L, MT_FLOWSHEET, 1);
    int n_strms = lua_gettop(L) - 1;
    if (n_strms == 0) {
        lua_pushnil(L);
        return 1;
    }

    vector<Stream*> strms(n_strms);
    const char* msg {"expected a table that looks like {name, {c1, c2, ...}}"};
    for (int i = 2; i <= n_strms + 1; i++) {
        // ith arg looks like {"Name", {"Comp1", "Comp2", etc}}
        luaL_checktype(L, i, LUA_TTABLE);
        luaL_argcheck(L, lua_rawlen(L, i) == 2, i, msg);
        string strm_name = get_string_elem(L, i, 1); 
        luaL_argcheck(L, !strm_name.empty(), i, "Stream name cannot be empty");
        luaL_argcheck(L, lua_rawgeti(L, i, 2) == LUA_TTABLE, i, msg);   // Push elem 2 of ith arg, e.g., {"Comp1", "Comp2"}
        int n_comps = lua_rawlen(L, -1);
        luaL_argcheck(L, n_comps > 0, i, "expected at least one component");
        vector<string> comps(n_comps);
        for (int j = 1; j <= n_comps; j++) {
            string cname = get_string_elem(L, -1, j);
            luaL_argcheck(L, !cname.empty(), i, "component name cannot be empty");
            comps[j - 1] = cname;
        }
        lua_pop(L, 1);                                                  // Pop elem 2 of ith arg
        strms[i - 2] = fs->add_stream(strm_name, std::move(comps));
    }

    lua_checkstack(L, n_strms);
    for (int i = 1; i <= n_strms; i++)
        push_luaobj<Stream>(L, strms[i - 1], MT_STREAM);

    return n_strms;
}

int Flowsheet_add_flowsheet(lua_State* L) {
    auto fs = check_luaobj<Flowsheet>(L, MT_FLOWSHEET, 1);
    string fs_name = luaL_checkstring(L, 2);
    luaL_argcheck(L, !fs_name.empty(), 2, "Flowsheet name cannot be empty");
    auto fs_new = fs->add_flowsheet(fs_name);
    push_luaobj<Flowsheet>(L, fs_new, MT_FLOWSHEET);
    return 1;
}

int Flowsheet_connect_streams(lua_State* L) {
    auto fs = check_luaobj<Flowsheet>(L, MT_FLOWSHEET, 1);
    bool ok = fs->connect_streams();
    lua_pushboolean(L, ok);
    return 1;
}

int Flowsheet_get(lua_State* L) {
    auto fs = check_luaobj<Flowsheet>(L, MT_FLOWSHEET, 1);
    string name = luaL_checkstring(L, 2);
    if (name.empty()) {
        lua_pushnil(L);
        return 1;
    }
    if (name == fs->name)
        push_luaobj<Flowsheet>(L, fs, MT_FLOWSHEET);
    else if (fs->child_map.contains(name))
        push_luaobj<Flowsheet>(L, fs->child_map[name], MT_FLOWSHEET);
    else if (fs->blocks_map.contains(name)) {
        auto blk = fs->blocks_map[name];
        switch (blk->blk_type) {
            case BlockType::Mixer:
                push_luaobj<Block, Mixer>(L, blk, MT_BLOCK);
                break;
            case BlockType::Splitter:
                push_luaobj<Block, Splitter>(L, blk, MT_BLOCK);
                break;
            case BlockType::Separator:
                push_luaobj<Block, Separator>(L, blk, MT_BLOCK);
                break;
            case BlockType::YieldReactor:
                push_luaobj<Block, YieldReactor>(L, blk, MT_BLOCK);
                break;
            case BlockType::MultiYieldReactor:
                push_luaobj<Block, MultiYieldReactor>(L, blk, MT_BLOCK);
                break;
            case BlockType::StoicReactor:
                push_luaobj<Block, StoicReactor>(L, blk, MT_BLOCK);
                break;
            default:
                lua_pushnil(L);
        }
    }
    else if (fs->calcs_map.contains(name))
        push_luaobj<Calc>(L, fs->calcs_map[name], MT_CALC);
    else if (fs->streams.contains(name))
        push_luaobj<Stream>(L, fs->streams[name].get(), MT_STREAM);
    else
        lua_pushnil(L);
    
    return 1;
}

int Flowsheet_render(lua_State* L) {
    auto fs = check_luaobj<Flowsheet>(L, MT_FLOWSHEET, 1);
    //render_flowsheet(*fs, std::cout);
    return 0;
}

int Flowsheet_tostring(lua_State* L) {
    auto fs = check_luaobj<Flowsheet>(L, MT_FLOWSHEET, 1);
    std::ostringstream oss;
    fs->show_flowsheet(oss);
    lua_pushstring(L, oss.str().c_str());
    return 1;
}

int Flowsheet_add_Mixer(lua_State* L);
int Flowsheet_add_Splitter(lua_State* L);
int Flowsheet_add_Separator(lua_State* L);
int Flowsheet_add_YieldReactor(lua_State* L);
int Flowsheet_add_MultiYieldReactor(lua_State* L);
int Flowsheet_add_StoicReactor(lua_State* L);
int Flowsheet_add_Calc(lua_State* L);

int Flowsheet_index(lua_State* L) {
    auto fs = check_luaobj<Flowsheet>(L, MT_FLOWSHEET, 1);
    string key = luaL_checkstring(L, 2);
    if      (key == "name")              lua_pushstring(L, fs->name.c_str());
    else if (key == "Streams")           lua_pushcfunction(L, Flowsheet_add_streams);
    else if (key == "Mixer")             lua_pushcfunction(L, Flowsheet_add_Mixer);
    else if (key == "Splitter")          lua_pushcfunction(L, Flowsheet_add_Splitter);
    else if (key == "Separator")         lua_pushcfunction(L, Flowsheet_add_Separator);
    else if (key == "YieldReactor")      lua_pushcfunction(L, Flowsheet_add_YieldReactor);
    else if (key == "MultiYieldReactor") lua_pushcfunction(L, Flowsheet_add_MultiYieldReactor);
    else if (key == "StoicReactor")      lua_pushcfunction(L, Flowsheet_add_StoicReactor);
    else if (key == "Calc")              lua_pushcfunction(L, Flowsheet_add_Calc);
    else if (key == "Flowsheet")         lua_pushcfunction(L, Flowsheet_add_flowsheet);
    else if (key == "get")               lua_pushcfunction(L, Flowsheet_get);
    else if (key == "render")            lua_pushcfunction(L, Flowsheet_render);
    else if (key == "connect_streams")   lua_pushcfunction(L, Flowsheet_connect_streams);
    else if (key == "streams") {
        lua_newtable(L);
        for (const auto& [name, strm] : fs->streams) {
            push_luaobj<Stream>(L, strm.get(), MT_STREAM);
            lua_setfield(L, -2, name.c_str());
        }            
    }
    else lua_pushnil(L);
    return 1;
}

void Flowsheet_register(lua_State* L) {
    luaL_newmetatable(L, MT_FLOWSHEET);
    lua_pushcfunction(L, LuaObj_eq);          lua_setfield(L, -2, "__eq");
    lua_pushcfunction(L, Flowsheet_tostring); lua_setfield(L, -2, "__tostring");
    lua_pushcfunction(L, Flowsheet_index);    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);
}

//---------------------------------------------------------

void start_Block(lua_State* L, string& blk_name, vector<Stream*>& inlets, vector<Stream*>& outlets) {
    blk_name = luaL_checkstring(L, 2);               

    // Table of inlet streams.
    luaL_checktype(L, 3, LUA_TTABLE);
    auto n_inlets = lua_rawlen(L, 3);
    luaL_argcheck(L, n_inlets > 0, 3, "expected at least one inlet Stream");
    inlets.resize(n_inlets);
    for (int i = 1; i <= n_inlets; i++) {
        inlets[i - 1] = get_luaobj_elem<Stream>(L, 3, i);
        luaL_argcheck(L, inlets[i - 1] != nullptr, 3, format("element {} to be a Stream", i).c_str());
    }

    // Table of outlet streams.
    luaL_checktype(L, 4, LUA_TTABLE);
    auto n_outlets = lua_rawlen(L, 4);
    luaL_argcheck(L, n_outlets > 0, 4, "expected at least one outlet Stream");
    outlets.resize(n_outlets);
    for (int i = 1; i <= n_outlets; i++) {
        outlets[i - 1] = get_luaobj_elem<Stream>(L, 4, i);
        luaL_argcheck(L, outlets[i - 1] != nullptr, 4, format("element {} to be a Stream", i).c_str());
    }

}

template <typename T, typename ...blk_params_T>
int finish_Block(lua_State* L,
                 string& blk_name,
                 Flowsheet* fs, 
                 vector<Stream*>& inlets, 
                 vector<Stream*>& outlets, 
                 blk_params_T& ...blk_params) {
    try {
        auto blk_p = fs->add_block<T>(blk_name, std::move(inlets), std::move(outlets), blk_params...);
        push_luaobj<Block, T>(L, blk_p, MT_BLOCK);
    }
    catch (const std::exception& e) {
        luaL_error(L, format("can't create {}: {}", blk_name, e.what()).c_str());
    }
    return 1;
}

template <typename T>
int add_Block(lua_State* L, Flowsheet* fs) {
    string blk_name;
    vector<Stream*> inlets, outlets;
    start_Block(L, blk_name, inlets, outlets);
    return finish_Block<T>(L, blk_name, fs, inlets, outlets);
}

int Flowsheet_add_Mixer(lua_State* L) {
    auto fs = check_luaobj<Flowsheet>(L, MT_FLOWSHEET, 1);
    return add_Block<Mixer>(L, fs);
}

int Flowsheet_add_Splitter(lua_State* L) {
    auto fs = check_luaobj<Flowsheet>(L, MT_FLOWSHEET, 1);
    return add_Block<Splitter>(L, fs);
}

int Flowsheet_add_Separator(lua_State* L) {
    auto fs = check_luaobj<Flowsheet>(L, MT_FLOWSHEET, 1);
    return add_Block<Separator>(L, fs);
}

int Flowsheet_add_YieldReactor(lua_State* L) {
    const auto fs = check_luaobj<Flowsheet>(L, MT_FLOWSHEET, 1);
    return add_Block<YieldReactor>(L, fs);
}

int Flowsheet_add_MultiYieldReactor(lua_State* L) {
    const auto fs = check_luaobj<Flowsheet>(L, MT_FLOWSHEET, 1);
    string blk_name;
    vector<Stream*> inlets, outlets;
    start_Block(L, blk_name, inlets, outlets);
    const auto n_feeds = inlets.size();

    // Reactor name,
    string reactor_name = luaL_checkstring(L, 5);
    luaL_argcheck(L, !reactor_name.empty(), 5, "reactor name cannot be empty");

    // Feed names.
    vector<string> feed_names(n_feeds);
    for (int i = 0, j = 6; i < n_feeds; i++, j++) {
        feed_names[i] = luaL_checkstring(L, j);
        luaL_argcheck(L, !feed_names[i].empty(), j, "feed name cannot be empty");
    }

    return finish_Block<MultiYieldReactor, const string&, const vector<string>&>(L, blk_name, fs, inlets, outlets, reactor_name, feed_names);
}

int Flowsheet_add_StoicReactor(lua_State* L) {
    auto fs = check_luaobj<Flowsheet>(L, MT_FLOWSHEET, 1);
    string blk_name;
    vector<Stream*> inlets, outlets;
    start_Block(L, blk_name, inlets, outlets);

    auto u_mw_def = fs->m->unitset->get_default_unit("molewt");
    bool ok {true};

    // Arg 5 is a table of molecular weights.
    luaL_checktype(L, 5, LUA_TTABLE);
    unordered_map<string, Quantity> mw {};
    lua_pushnil(L);                             // push a nil key to start
    while (lua_next(L, 5) != 0) {               // pops the key, then pushes next key-value pair
        Unit* u_mw;
        double mw_val;
        string comp_name = lua_tostring(L, -2); // key is comp_name
        if (lua_istable(L, -1)) {               // value is a number or a table the looks like {number, unit}
            luaL_argcheck(L, lua_rawlen(L, -1) == 2, 5, "expected molecular weight specification to look like {number, Unit}");
            mw_val = get_double_elem(L, -1, 1, ok);
            luaL_argcheck(L, ok, 5, "expected molecular weight to be a number");
            auto type = lua_rawgeti(L, -1, 2);  // push element 2 of the table
            if (type == LUA_TSTRING) {
                string u_str = lua_tostring(L, -1);
                luaL_argcheck(L, fs->m->unitset->units.contains(u_str), 5, format("Unit \"{}\" not in the UnitSet", u_str).c_str());
                u_mw = fs->m->unitset->units[u_str].get();
            } else {
                u_mw = get_luaobj_elem<Unit>(L, -1, 2);
                luaL_argcheck(L, u_mw != nullptr, 5, "expected molecular weight specification to look like {number, Unit}");
            }
            lua_pop(L, 1);                      // pop element 2
        }
        else if (lua_isnumber(L, -1)) {
            mw_val = lua_tonumber(L, -1);
            u_mw = u_mw_def;
        }
        else
            luaL_argerror(L, 5, "expected molecular weight to be a number or a {number, Unit} table");
            
        lua_pop(L, 1);                          // pop the value of the current key-value pair
        mw[comp_name] = {mw_val, u_mw};
    }

    // Arg 6 is a table of stoichiometric coefficients.
    luaL_checktype(L, 6, LUA_TTABLE);
    auto n_rx = lua_rawlen(L, 6);
    vector<unordered_map<string, double>> stoic_coef(n_rx);
    for (int i = 1; i <= n_rx; i++) {
        luaL_argcheck(L, lua_rawgeti(L, 6, i) == LUA_TTABLE, 6,    // Push reaction i, e.g., { H2 = -1.0, C2H2 = -1.0, C2H4 = 1.0 }
            format("expected element {} to be a table", i).c_str());
        lua_pushnil(L);                             // push a nil key to start
        while (lua_next(L, -2) != 0) {              // pops the key, then pushes next key-value pair
            string comp_name = lua_tostring(L, -2);
            double coef = lua_tonumber(L, -1);
            lua_pop(L, 1);                          // Pop value
            stoic_coef[i - 1][comp_name] = coef;
        }
        lua_pop(L, 1);                                             // Pop reaction i
    }
    
    // Arg 7 is a table of conversion specs.
    luaL_argcheck(L, lua_istable(L, 7) && lua_rawlen(L, 7) > 0, 7,
        "expected a table of one or more conversion specifications");
    auto n_keys = lua_rawlen(L, 7);
    luaL_argcheck(L, n_keys == n_rx, 7,
        format("expected {} conversion specifications, got {}", n_rx, n_keys).c_str());
    vector<string> conversion_keys(n_keys);
    for (int i = 1; i <= n_keys; i++) {
        string ckey = get_string_elem(L, 7, i);
        luaL_argcheck(L, !ckey.empty(), 7, "expected a component name");
        conversion_keys[i - 1] = ckey;
    }

    return finish_Block<StoicReactor,
                        const unordered_map<string, Quantity>&,
                        const vector<unordered_map<string, double>>&,
                        const vector<string>&>(L, blk_name, fs, inlets, outlets, mw, stoic_coef, conversion_keys);
}

int Flowsheet_add_Calc(lua_State* L) {
    auto fs = check_luaobj<Flowsheet>(L, MT_FLOWSHEET, 1);
    string calc_name = luaL_checkstring(L, 2);               
    auto calc_p = fs->add_calc(calc_name);
    push_luaobj<Calc>(L, calc_p, MT_CALC);
    return 1;
}

int Block_connect(lua_State* L) {
    auto blk = check_luaobj<Block>(L, MT_BLOCK, 1);
    const auto conn = blk->connect();
    const int n_conn = static_cast<int>(conn.size());
    if (lua_checkstack(L, n_conn)) {
        for (const auto& conn_p : conn)
            push_luaobj<Connection>(L, conn_p, MT_CONNECTION);
        return n_conn;
    }
    else {
        lua_pushnil(L);
        return 1;
    }
}

int Block_get(lua_State* L) {
    auto blk = check_luaobj<Block>(L, MT_BLOCK, 1);
    string name = luaL_checkstring(L, 2);
    auto M = blk->fs->m;
    if (M->x_map.contains(name))
        push_luaobj<Quantity, Variable>(L, M->x_map[name], MT_QUANTITY);
    else if (M->g_map.contains(name))
        push_luaobj<Constraint>(L, M->g_map[name], MT_CONSTRAINT);
    else
        lua_pushnil(L);

    return 1;
}

int Block_tostring(lua_State* L) {
    auto blk = check_luaobj<Block>(L, MT_BLOCK, 1);
    lua_pushstring(L, blk->to_str_2().c_str());
    return 1;
}

int Block_index(lua_State* L) {
    auto blk = check_luaobj<Block>(L, MT_BLOCK, 1);
    string key = luaL_checkstring(L, 2);
    if      (key == "name")                        lua_pushstring(L, blk->name.c_str());
    else if (key == "init" || key == "initialize") lua_pushcfunction(L, initialize);
    else if (key == "get")                         lua_pushcfunction(L, Block_get);
    else if (key == "eval_constraints")            lua_pushcfunction(L, eval_constraints);
    else if (key == "eval_jacobian")               lua_pushcfunction(L, eval_jacobian);
    else if (key == "eval_hessian")                lua_pushcfunction(L, eval_hessian);
    else if (key == "show_variables" || key == "show_vars") lua_pushcfunction(L, show_variables);
    else if (key == "show_fixed")                  lua_pushcfunction(L, show_fixed);
    else if (key == "show_active")                 lua_pushcfunction(L, show_active);
    else if (key == "show_constraints")            lua_pushcfunction(L, show_constraints);
    else if (key == "show_jacobian")               lua_pushcfunction(L, show_jacobian);
    else if (key == "show_hessian")                lua_pushcfunction(L, show_hessian);
    else if (key == "connect")                     lua_pushcfunction(L, Block_connect);
    else if (key == "write_vars" || key == "write_variables") lua_pushcfunction(L, write_variables);
    else lua_pushnil(L);
    return 1;
}

void Block_register(lua_State* L) {
    luaL_newmetatable(L, MT_BLOCK);
    lua_pushcfunction(L, LuaObj_eq);      lua_setfield(L, -2, "__eq");
    lua_pushcfunction(L, Block_tostring); lua_setfield(L, -2, "__tostring");
    lua_pushcfunction(L, Block_index);    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);
}

//---------------------------------------------------------

int Calc_add_variables(lua_State* L) {
    auto calc_p = check_luaobj<Calc>(L, MT_CALC, 1);
    auto M = calc_p->fs->m;
    auto n_args = lua_gettop(L);
    for (int i = 2; i <= n_args; i++) {
        luaL_checktype(L, i, LUA_TTABLE);
        auto n_elem = lua_rawlen(L, i);    // number of elements in the ith argument table
        luaL_argcheck(L, n_elem == 2, i, "expected a table that looks like {string, Unit}");
        string var_name = get_string_elem(L, i, 1);  // element 1 is variable name
        luaL_argcheck(L, !var_name.empty(), i, "Variable name cannot be empty");
        Unit* unit {nullptr};
        if (lua_rawgeti(L, i, 2) == LUA_TSTRING) {  // push elem 2
            string unit_str = lua_tostring(L, -1);
            luaL_argcheck(L, M->unitset->units.contains(unit_str), i, format("Unit \"{}\" is not in the UnitSet", unit_str).c_str());
            unit = M->unitset->units[unit_str].get();
        }
        else
            unit = get_luaobj_elem<Unit>(L, i, 2);  // element 2 is a Unit
        lua_pop(L, 1);                              // pop elem 2
        luaL_argcheck(L, unit != nullptr, i, "expected element 2 to be a Unit");
        auto v = M->add_var(calc_p->prefix + var_name, unit);
        calc_p->x.push_back(v);
        push_luaobj<Quantity, Variable>(L, v, MT_QUANTITY);
    }

    lua_checkstack(L, n_args - 1);
    return n_args - 1;
}

int Calc_add_constraints(lua_State* L) {
    auto calc_p = check_luaobj<Calc>(L, MT_CALC, 1);
    auto M = calc_p->fs->m;
    auto n_args = lua_gettop(L);
    for (int i = 2; i <= n_args; i++) {
        string eq_name = luaL_checkstring(L, i);
        luaL_argcheck(L, !eq_name.empty(), i, "Constraint name cannot be empty");
        auto eq = M->add_constraint(calc_p->prefix + eq_name);
        calc_p->g.push_back(eq);
        push_luaobj<Constraint>(L, eq, MT_CONSTRAINT);
    }

    lua_checkstack(L, n_args - 1);
    return n_args - 1;
}

int Calc_add_jacobian_nzs(lua_State* L) {
    auto calc_p = check_luaobj<Calc>(L, MT_CALC, 1);
    auto M = calc_p->fs->m;
    auto n_args = lua_gettop(L);
    for (int i = 2; i <= n_args; i++) {
        luaL_checktype(L, i, LUA_TTABLE);
        auto n_elem = lua_rawlen(L, i);    // number of elements in the ith JNZ list
        luaL_argcheck(L, n_elem == 2, i, "expected a table that looks like {Constraint, Variable}");
        auto con = get_luaobj_elem<Constraint>(L, i, 1); // element 1 is a Constraint
        luaL_argcheck(L, con != nullptr, i, "expected element 1 to be a Constraint");
        auto var = get_luaobj_elem<Variable>(L, i, 2);   // element 2 is a Variable
        luaL_argcheck(L, var != nullptr, i, "expected element 2 to be a Variable");
        auto jnz = M->add_J_NZ(con, var);
        calc_p->J.push_back(jnz);
        push_luaobj<JacobianNZ>(L, jnz, MT_JACOBIAN_NZ);
    }

    lua_checkstack(L, n_args - 1);
    return n_args - 1;
}

int Calc_add_hessian_nzs(lua_State* L) {
    auto calc_p = check_luaobj<Calc>(L, MT_CALC, 1);
    auto M = calc_p->fs->m;
    auto n_args = lua_gettop(L);
    for (int i = 2; i <= n_args; i++) {
        luaL_checktype(L, i, LUA_TTABLE);
        auto n_elem = lua_rawlen(L, i);    // number of elements in the ith HNZ list
        luaL_argcheck(L, n_elem == 3, i, "expected a table that looks like {Constraint, Variable, Variable}");
        auto con = get_luaobj_elem<Constraint>(L, i, 1); // element 1 is a Constraint
        luaL_argcheck(L, con != nullptr, i, "expected element 1 to be a Constraint");
        auto var1 = get_luaobj_elem<Variable>(L, i, 2);   // element 2 is a Variable
        luaL_argcheck(L, var1 != nullptr, i, "expected element 2 to be a Variable");
        auto var2 = get_luaobj_elem<Variable>(L, i, 3);   // element 3 is a Variable
        luaL_argcheck(L, var2 != nullptr, i, "expected element 3 to be a Variable");
        auto hnz = M->add_H_NZ(con, var1, var2);
        calc_p->H.push_back(hnz);
        push_luaobj<HessianNZ>(L, hnz, MT_HESSIAN_NZ);
    }

    lua_checkstack(L, n_args - 1);
    return n_args - 1;
}

int Calc_get(lua_State* L) {
    auto calc = check_luaobj<Calc>(L, MT_CALC, 1);
    string name = luaL_checkstring(L, 2);
    auto M = calc->fs->m;
    if (M->x_map.contains(name))
        push_luaobj<Quantity, Variable>(L, M->x_map[name], MT_QUANTITY);
    else if (M->g_map.contains(name))
        push_luaobj<Constraint>(L, M->g_map[name], MT_CONSTRAINT);
    else
        lua_pushnil(L);

    return 1;
}

int Calc_tostring(lua_State* L) {
    auto calc = check_luaobj<Calc>(L, MT_CALC, 1);
    std::ostringstream oss;
    calc->show_calc(oss);
    lua_pushstring(L, oss.str().c_str());
    return 1;
}

int Calc_index(lua_State* L) {
    auto calc = check_luaobj<Calc>(L, MT_CALC, 1);
    string key = luaL_checkstring(L, 2);
    if      (key == "name")                                  lua_pushstring(L, calc->name.c_str());
    else if (key == "init" || key == "initialize")           lua_pushcfunction(L, initialize);
    else if (key == "eval_constraints")                      lua_pushcfunction(L, eval_constraints);
    else if (key == "show_constraints")                      lua_pushcfunction(L, show_constraints);
    else if (key == "show_variables" || key == "show_vars")  lua_pushcfunction(L, show_variables);
    else if (key == "show_fixed")                            lua_pushcfunction(L, show_fixed);
    else if (key == "show_active")                           lua_pushcfunction(L, show_active);
    else if (key == "show_jacobian")                         lua_pushcfunction(L, show_jacobian);
    else if (key == "show_hessian")                          lua_pushcfunction(L, show_hessian);
    else if (key == "get")                                   lua_pushcfunction(L, Calc_get);
    else if (key == "add_vars" || key == "add_variables")    lua_pushcfunction(L, Calc_add_variables);
    else if (key == "add_cons" || key == "add_constraints")  lua_pushcfunction(L, Calc_add_constraints);
    else if (key == "add_jnzs" || key == "add_jacobian_nzs") lua_pushcfunction(L, Calc_add_jacobian_nzs);
    else if (key == "add_hnzs" || key == "add_hessian_nzs")  lua_pushcfunction(L, Calc_add_hessian_nzs);
    else if (key == "write_vars" || key == "write_variables") lua_pushcfunction(L, write_variables);
    else                                                     lua_pushnil(L);
    return 1;
}

void Calc_register(lua_State* L) {
    luaL_newmetatable(L, MT_CALC);
    lua_pushcfunction(L, LuaObj_eq);     lua_setfield(L, -2, "__eq");
    lua_pushcfunction(L, Calc_tostring); lua_setfield(L, -2, "__tostring");
    lua_pushcfunction(L, Calc_index);    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);
}

//---------------------------------------------------------

int Connection_tostring(lua_State* L) {
    auto cnx = check_luaobj<Connection>(L, MT_CONNECTION, 1);
    lua_pushstring(L, cnx->to_str_2().c_str());
    return 1;
}

int Connection_index(lua_State* L) {
    auto cnx = check_luaobj<Connection>(L, MT_CONNECTION, 1);
    string key = luaL_checkstring(L, 2);
    if      (key == "eq" || key == "con") push_luaobj<Constraint>(L, cnx->eq, MT_CONSTRAINT);
    else if (key == "var1")               push_luaobj<Quantity, Variable>(L, cnx->var1, MT_QUANTITY);
    else if (key == "var2")               push_luaobj<Quantity, Variable>(L, cnx->var2, MT_QUANTITY);
    else                                  lua_pushnil(L);
    return 1;
}

void Connection_register(lua_State* L) {
    luaL_newmetatable(L, MT_CONNECTION);
    lua_pushcfunction(L, LuaObj_eq);           lua_setfield(L, -2, "__eq");
    lua_pushcfunction(L, Connection_tostring); lua_setfield(L, -2, "__tostring");
    lua_pushcfunction(L, Connection_index);    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);
}

//---------------------------------------------------------

void call_lua_function(const string& func_name) {
    if (scripter_lua_state == nullptr) throw std::runtime_error("call_lua_function: Lua state is nil");
    auto L = scripter_lua_state;
    auto type = lua_getglobal(L, func_name.c_str());
    if (type != LUA_TFUNCTION)
        luaL_error(L, (func_name + ": not a function").c_str());
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

static constexpr luaL_Reg function_table[] {
    { "UnitSet",           UnitSet_new           },
    { "Quantity",          Quantity_new          },
    { "Q",                 Quantity_new          },
    { "Model",             Model_new             },
    { "Solver",            Solver_new            },
    { "Output",            set_output            },
    { "Reactions",         eval_reactions        },
    { "read_vars",         read_variables        },
    { "read_variables",    read_variables        },
    { "show_LuaObj_type",  show_LuaObj_type      },
    { nullptr,             nullptr               }
};

void register_objs(lua_State* L) {
    Unit_register(L); UnitKind_register(L); UnitSet_register(L);
    Quantity_register(L);
    Constraint_register(L); JacobianNZ_register(L); HessianNZ_register(L); Connection_register(L);
    Model_register(L); Flowsheet_register(L); Stream_register(L); Block_register(L); Calc_register(L);
    Solver_register(L);
}

// Make type() call a custom function to show C++ name of userdata
static const char* override_lua_type{R"(
        local _builtin_type = type
        type = function(var)
            if _builtin_type(var) == "userdata" then
                return show_LuaObj_type(var)
            else
                return _builtin_type(var)
            end
        end
    )"};

lua_State* start_lua() {
    auto L = luaL_newstate();
    if (!L) return nullptr;
    luaL_openlibs(L);
    scripter_lua_state = L;

    register_objs(L);
    lua_pushglobaltable(L);
    luaL_setfuncs(L, function_table, 0);

    luaL_dostring(L, override_lua_type);

    return L;
}

#ifdef _WIN32
#define EXPORT_LUAOPEN __declspec(dllexport)
#include <windows.h>
#else
#define EXPORT_LUAOPEN
#endif

// Needed to make print() work in Jupyter notebooks
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

extern "C" EXPORT_LUAOPEN int luaopen_mboptlib(lua_State* L)
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);    // Make Windows render box-drawing characters
#endif

    scripter_lua_state = L;
    register_objs(L);
    lua_pushglobaltable(L);
    luaL_setfuncs(L, function_table, 0);
    lua_pop(L, 1);

    luaL_dostring(L, override_lua_type);

    static LuaStreambuf lua_buf(L);
    if (std::getenv("JPY_PARENT_PID") != nullptr) {
        cout.rdbuf(&lua_buf);

        // In a Jupyter notebook xeus-lua's print() does not call tostring() on each argument. 
        // Override print() to make it do that.
        luaL_dostring(L, R"(
            local _builtin_print = print
            print = function(...)
                local args = {}
                for i = 1, select('#', ...) do
                    args[i] = tostring(select(i, ...))
                end
                _builtin_print(table.unpack(args))
            end
        )");
    }

    return 0;                     
}