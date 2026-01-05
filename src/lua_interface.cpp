#include <cassert>
#include <sstream>
#include <regex>
#include <charconv>
#include "lua_interface.hpp"
#include "Model.hpp"
#include "Mixer.hpp"

using sol::lua_nil;

static sol::state lua;

string trim(string_view s) {
    string res {};
    for (const auto& c : s) {
        if (c == ' ' || c == '\t' || c == '\n')
            continue;
        res += c;
    }
    return res;
}

bool is_number(string_view s, double& value) {
    const char *cb = s.data();
    const char *ce = cb + s.size();
    auto [p, e] = std::from_chars(cb, ce, value);
    return e == std::errc() && p == ce;
}

LuaResult lua_run_script(string script_file_name) {
    auto res = lua.script_file(script_file_name, sol::script_pass_on_error);
    if (!res.valid()) {
        sol::error err = res;
        return {false, err.what()};
    }
    return {};
}

UnitSetPtr lua_unit_set(sol::table lua_unit_set) {
    auto u = make_shared<UnitSet>();

    if (!lua_unit_set["kinds"].valid()) {
        cerr << "UnitSet: No \"kinds\" key found in table\n";
        return nullptr;
    }

    for (const auto& [k, v] : lua_unit_set.get<sol::table>("kinds")) {
        auto s = v.as<vector<string>>();
        u->add_kind(k.as<string>(), s[0], s.size() > 1 ? s[1] : "");
    }

    if (!lua_unit_set["units"].valid()) {
        cerr << "UnitSet: No \"units\" key found in table\n";
        return nullptr;
    }

    for (const auto& [k, v] : lua_unit_set.get<sol::table>("units")) {
        string kind_str = k.as<string>();
        if (!u->kinds.contains(kind_str)) {
            cerr << "UnitSet: No kind \"" << kind_str << "\" found in table\n";
            return nullptr;
        }
        auto kind = u->kinds[kind_str];
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
            u->add_unit(unit_str, kind, unit_ratio, unit_offset);
        }
    }
    return u;
}

std::pair<ModelPtr, FlowsheetPtr> lua_new_model(string name, string index_fs_name, UnitSetPtr lua_unit_set_ptr) {
    {
        ModelPtr M = lua["M"];
        if (M != nullptr) {
            delete M;
            lua["M"] = lua_nil;
        }
    }
    ModelPtr M = new Model {name, index_fs_name, *lua_unit_set_ptr};
    return {M, M->index_fs};
}

void lua_delete_model() {
    ModelPtr M = lua["M"];
    if (M != nullptr) {
        delete M;
        lua["M"] = lua_nil;
    }
}

FlowsheetPtr lua_get_index_fs() {
    ModelPtr M = lua["M1"];
    return M != nullptr ? M->index_fs : nullptr;
}

auto lua_add_streams(sol::variadic_args lua_stream_specs) {
    FlowsheetPtr fs = lua["FS"];
    vector<StreamPtr> streams {};
    for (const auto& arg : lua_stream_specs) {
        sol::table stream_spec = arg.as<sol::table>();
        string name = stream_spec[1];
        vector<string> comps = stream_spec[2].get<vector<string>>();
        streams.push_back(fs->add_stream(name, comps));
    }
    return sol::as_returns(std::move(streams));
}

BlockPtr lua_add_Mixer(string name, vector<StreamPtr> inlets, StreamPtr outlet) {
    FlowsheetPtr fs = lua["FS"];
    if (fs == nullptr) return nullptr;
    auto outlets = vector<StreamPtr> {outlet};
    auto blk_ptr = fs->add_block<Mixer>(name, inlets, outlets);
    return blk_ptr;
}

std::regex re_binop(R"((\S+)(=|<|>)([^\s_]+)(?:_(\S+))?)");

void lua_set(string expressions) {
    std::istringstream expr_stream {expressions};
    ModelPtr M = lua["M"];
    if (M == nullptr) return;

    string expr_raw;
    while(std::getline(expr_stream, expr_raw)) {
        string expr = trim(expr_raw);
        std::smatch m;
        if (std::regex_match(expr, m, re_binop)) {
            string lhs = m[1].str(),
                   op  = m[2].str(),
                   rhs = m[3].str();

            auto lhs_var = M->var(lhs);
            double rhs_value {};

            if (rhs == "Inf") {
                rhs_value = NO_BOUND;
            }
            else if (rhs == "-Inf") {
                rhs_value = -NO_BOUND;
            }
            else if (is_number(rhs, rhs_value)) {
                if (m[4].matched) {
                    auto rhs_unit = M->unit_set.units[m[4].str()];
                    rhs_value = lhs_var->convert(rhs_value, rhs_unit);
                }
            }

            if (op == "=")
                lhs_var->value = rhs_value;
            else if (op == "<")
                lhs_var->upper = rhs_value;
            else if (op == ">")
                lhs_var->lower = rhs_value;

        }
    }
}

void lua_initialize_model() {
    ModelPtr M = lua["M"];
    if (M == nullptr) return;
    M->initialize();
}

void lua_eval_constraints() {
    ModelPtr M = lua["M"];
    if (M == nullptr) return;
    M->eval_constraints();
}

void lua_set_string_solver_option(string option, string val) {
    ModelPtr M = lua["M"];
    if (M == nullptr) return;
    M->solver->Options()->SetStringValue(option, val);
}

void lua_set_integer_solver_option(string option, int val) {
    ModelPtr M = lua["M"];
    if (M == nullptr) return;
    M->solver->Options()->SetIntegerValue(option, val);
}

void lua_set_numeric_solver_option(string option, double val) {
    ModelPtr M = lua["M"];
    if (M == nullptr) return;
    M->solver->Options()->SetNumericValue(option, val);
}

int lua_initialize_solver() {
    ModelPtr M = lua["M"];
    if (M == nullptr) return -1;
    return M->solver->Initialize();
}

int lua_solve() {
    ModelPtr M = lua["M"];
    if (M == nullptr) return -1;
    return M->solver->OptimizeTNLP(M);
}

void lua_show_variables() {
    ModelPtr M = lua["M"];
    if (M == nullptr) return;
    M->show_variables();
}

void lua_show_model_variables(ModelPtr m) {
    if (m == nullptr) return;
    m->show_variables();
}

void lua_show_block_variables(BlockPtr blk) {
    if (blk == nullptr) return;
    blk->show_variables();
}

std::pair<Ndouble, sol::optional<string>> lua_get_value(string var_name) {
    ModelPtr M = lua["M"];
    if (M == nullptr) return {sol::nullopt, sol::nullopt};
    if (M->x_map.contains(var_name))
        return {M->x_map[var_name]->value,
                M->x_map[var_name]->unit->str};
    return {sol::nullopt, sol::nullopt};
}

std::pair<Ndouble, sol::optional<string>> lua_get_lower(string var_name) {
    ModelPtr M = lua["M"];
    if (M == nullptr) return {sol::nullopt, sol::nullopt};
    if (M->x_map.contains(var_name))
        return {M->x_map[var_name]->lower.value(),
                M->x_map[var_name]->unit->str};
    return {sol::nullopt, sol::nullopt};
}

std::pair<Ndouble, sol::optional<string>> lua_get_upper(string var_name) {
    ModelPtr M = lua["M"];
    if (M == nullptr) return {sol::nullopt, sol::nullopt};
    if (M->x_map.contains(var_name))
        return {M->x_map[var_name]->upper.value(),
                M->x_map[var_name]->unit->str};
    return {sol::nullopt, sol::nullopt};
}

Ndouble lua_change_unit(string var_name, string new_unit_str) {
    ModelPtr M = lua["M"];
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
    lua.set_function("ShowVariables", sol::overload(lua_show_variables,
                                                    lua_show_model_variables,
                                                    lua_show_block_variables));
    lua.set_function("InitializeModel", lua_initialize_model);
    lua.set_function("EvalConstraints", lua_eval_constraints);
    lua.set_function("Val", lua_get_value);
    lua.set_function("LB", lua_get_lower);
    lua.set_function("UB", lua_get_upper);
    lua.set_function("ChangeUnit", lua_change_unit);
    lua.set_function("SolverOption", sol::overload(lua_set_string_solver_option,
                                                   lua_set_integer_solver_option,
                                                   lua_set_numeric_solver_option));
    lua.set_function("InitializeSolver", lua_initialize_solver);
    lua.set_function("Solve", lua_solve);

}
