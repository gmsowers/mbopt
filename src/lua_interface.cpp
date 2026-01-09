#include <cassert>
#include <sstream>
#include <regex>
#include <charconv>
#include "lua_interface.hpp"
#include "Model.hpp"
#include "Mixer.hpp"

using sol::lua_nil;

static sol::state lua;
static Model* current_model;

string remove_ws(string_view s) {
    string res {};
    for (const auto& c : s) {
        if (c == ' ' || c == '\t' || c == '\n')
            continue;
        res += c;
    }
    return res;
}

string trim(string_view s) {
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

LuaResult lua_run_script(const string& script_file_name) {
    auto res = lua.script_file(script_file_name, sol::script_pass_on_error);
    if (!res.valid()) {
        sol::error err = res;
        return {false, err.what()};
    }
    return {};
}

sol::optional<UnitSet> lua_unit_set(const sol::table& lua_unit_set) {
    UnitSet u {};

    if (!lua_unit_set["kinds"].valid()) {
        cerr << "Error in UnitSet: No \"kinds\" key found in table.\n";
        return sol::nullopt;
    }

    for (const auto& [k, v] : lua_unit_set.get<sol::table>("kinds")) {
        auto s = v.as<vector<string>>();
        u.add_kind(k.as<string>(), s[0], s.size() > 1 ? s[1] : "");
    }

    if (!lua_unit_set["units"].valid()) {
        cerr << "Error in UnitSet: No \"units\" key found in table.\n";
        return sol::nullopt;
    }

    for (const auto& [k, v] : lua_unit_set.get<sol::table>("units")) {
        string kind_str = k.as<string>();
        if (!u.kinds.contains(kind_str)) {
            cerr << "Error in UnitSet: No kind \"" << kind_str << "\" found in table.\n";
            return sol::nullopt;
        }
        auto kind = u.kinds[kind_str].get();
        for (const auto& [i, units] : v.as<sol::table>()) {
            string unit_str {};
            double unit_ratio {1.0}, unit_offset {0.0};
            for (const auto& [j, unit_field] : units.as<sol::table>()) {
                if (unit_field.is<string>())
                    unit_str = unit_field.as<string>();
                else {
                    auto ix = j.as<int>();
                    if (ix == 2)
                        unit_ratio = unit_field.as<double>();
                    if (ix == 3)
                        unit_offset = unit_field.as<double>();
                }
            }
            u.add_unit(unit_str, kind, unit_ratio, unit_offset);
        }
    }
    return u;
}

std::pair<Model*, Flowsheet*> lua_new_model(string name, string index_fs_name, UnitSet& unit_set) {
    if (current_model != nullptr) {
        delete current_model;
        lua["M"] = lua_nil;
    }
    current_model = new Model {name, index_fs_name, std::move(unit_set)};
    return {current_model, current_model->index_fs.get()};
}

void lua_delete_model() {
    if (current_model != nullptr) {
        delete current_model;
        lua["M"] = lua_nil;
    }
}

Flowsheet* lua_get_index_fs() {
    Model* M = lua["M"];
    return M != nullptr ? M->index_fs.get() : nullptr;
}

auto lua_add_streams(sol::variadic_args lua_stream_specs) {
    Flowsheet* fs = lua["FS"];
    vector<Stream*> streams {};
    for (const auto& arg : lua_stream_specs) {
        sol::table stream_spec = arg.as<sol::table>();
        string name = stream_spec[1];
        vector<string> comps = stream_spec[2].get<vector<string>>();
        streams.push_back(fs->add_stream(name, comps));
    }
    return sol::as_returns(streams);
}

Block* lua_add_Mixer(string name, vector<Stream*> inlets, Stream* outlet) {
    Flowsheet* fs = lua["FS"];
    if (fs == nullptr) return nullptr;
    auto outlets = vector<Stream*> {outlet};
    auto blk_p = fs->add_block<Mixer>(name, inlets, outlets);
    return blk_p;
}

const std::regex re_binop(R"((\S+)(=|<|>)([^\s_]+)(?:_(\S+))?)");

bool lua_set(string expressions) {
    std::istringstream expr_stream {expressions};
    Model* M = lua["M"];
    if (M == nullptr) return false;

    bool ok = true;
    string expr_raw;
    while(std::getline(expr_stream, expr_raw)) {
        string expr = remove_ws(expr_raw);
        if (expr.empty()) continue;
        std::smatch m;
        if (std::regex_match(expr, m, re_binop)) {
            string lhs = m[1].str(),
                   op  = m[2].str(),
                   rhs = m[3].str();

            auto lhs_var = M->var(lhs);
            if (lhs_var == nullptr) {
                ok = false;
                cerr << "Error in Set: variable \"" << lhs << "\" not found\n";
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
                        cerr << "Error in Set: right-hand side unit \"" << rhs_unit_str << "\" not found.\n";
                        continue;
                    }
                    rhs_value = lhs_var->convert(rhs_value, rhs_unit);
                }
            }
            else {
                ok = false;
                cerr << "Error in Set: invalid right-hand side \"" << rhs << "\".\n";
                continue;
            }

            if (op == "=")
                lhs_var->value = rhs_value;
            else if (op == "<")
                lhs_var->upper = rhs_value;
            else if (op == ">")
                lhs_var->lower = rhs_value;
        }
        else {
            ok = false;
            cerr << "Error in Set: invalid expression \"" << trim(expr_raw) << "\".\n";
        }
    }
    return ok;
}

const std::regex re_spec(R"(\s*(fix|free)\s+(\S+)\s*)");

bool lua_specs(string expressions) {
    std::istringstream expr_stream {expressions};
    Model* M = lua["M"];
    if (M == nullptr) return false;

    bool ok = true;
    string expr;
    while(std::getline(expr_stream, expr)) {
        string line = trim(expr);
        if (line.empty()) continue;
        std::smatch m;
        if (std::regex_match(expr, m, re_spec)) {
            string lhs = m[1].str(),
                   rhs = m[2].str();

            auto rhs_var = M->var(rhs);
            if (rhs_var == nullptr) {
                ok = false;
                cerr << "Error in Specs: variable \"" << rhs << "\" not found\n";
                continue;
            }
            
            rhs_var->spec = (lhs == "free" ? VariableSpec::Free : VariableSpec::Fixed);
        }
        else {
            ok = false;
            cerr << "Error in Specs: invalid spec \"" << trim(expr) << "\". Spec must be \"fix varname\" or \"free varname\".\n";
        }
    }
    return ok;
}

void lua_initialize_model() {
    Model* M = lua["M"];
    if (M == nullptr) return;
    M->initialize();
}

void lua_eval_constraints() {
    Model* M = lua["M"];
    if (M == nullptr) return;
    M->eval_constraints();
}

void lua_set_string_solver_option(string option, string val) {
    Model* M = lua["M"];
    if (M == nullptr) return;
    solver->Options()->SetStringValue(option, val);
}

void lua_set_integer_solver_option(string option, int val) {
    Model* M = lua["M"];
    if (M == nullptr) return;
    solver->Options()->SetIntegerValue(option, val);
}

void lua_set_numeric_solver_option(string option, double val) {
    Model* M = lua["M"];
    if (M == nullptr) return;
    solver->Options()->SetNumericValue(option, val);
}

int lua_initialize_solver() {
    Model* M = lua["M"];
    if (M == nullptr) return -1;
    return solver->Initialize();
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

std::pair<Ndouble, sol::optional<string>> lua_get_value(string var_name) {
    Model* M = lua["M"];
    if (M == nullptr) return {sol::nullopt, sol::nullopt};
    if (M->x_map.contains(var_name))
        return {M->x_map[var_name]->value,
                M->x_map[var_name]->unit->str};
    return {sol::nullopt, sol::nullopt};
}

std::pair<Ndouble, sol::optional<string>> lua_get_lower(string var_name) {
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

std::pair<Ndouble, sol::optional<string>> lua_get_upper(string var_name) {
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

sol::optional<string> lua_get_spec(string var_name) {
    using namespace std::literals;
    Model* M = lua["M"];
    if (M == nullptr) return sol::nullopt;
    if (M->x_map.contains(var_name))
        return (M->x_map[var_name]->spec == VariableSpec::Fixed ? "fixed"s : "free"s);
    return sol::nullopt;
}

Ndouble lua_change_unit(string var_name, string new_unit_str) {
    Model* M = lua["M"];
    if (M == nullptr) return sol::nullopt;
    if (M->x_map.contains(var_name))
        return M->x_map[var_name]->change_unit(M, new_unit_str);
    return sol::nullopt;
}

void lua_init() {
	lua.open_libraries(sol::lib::base,
                       sol::lib::string,
                       sol::lib::os,
                       sol::lib::math,
                       sol::lib::table);

    lua.set_function("UnitSet", lua_unit_set);
    lua.set_function("Model", lua_new_model);
    lua.set_function("DeleteModel", lua_delete_model);
    lua.set_function("IndexFS", lua_get_index_fs);
    lua.set_function("Streams", lua_add_streams);
    lua.set_function("Mixer", lua_add_Mixer);
    lua.set_function("Set", lua_set);
    lua.set_function("Specs", lua_specs);
    lua.set_function("ShowVariables", sol::overload(lua_show_variables,
                                                    lua_show_model_variables,
                                                    lua_show_block_variables));
    lua.set_function("InitializeModel", lua_initialize_model);
    lua.set_function("EvalConstraints", lua_eval_constraints);
    lua.set_function("Val", lua_get_value);
    lua.set_function("LB", lua_get_lower);
    lua.set_function("UB", lua_get_upper);
    lua.set_function("Spec", lua_get_spec);
    lua.set_function("ChangeUnit", lua_change_unit);
    lua.set_function("SolverOption", sol::overload(lua_set_string_solver_option,
                                                   lua_set_integer_solver_option,
                                                   lua_set_numeric_solver_option));
    lua.set_function("InitializeSolver", lua_initialize_solver);
    lua.set_function("Solve", lua_solve);

}
