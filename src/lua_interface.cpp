#include <cassert>
#include <algorithm>
#include <sstream>
#include <regex>
#include <charconv>
#include "lua_interface.hpp"
#include "Model.hpp"
#include "Mixer.hpp"

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

void lua_run_script(string script_file_name) {
    lua.script_file(script_file_name);
}

UnitSet get_unit_set(sol::table& lua_unit_set) {
    UnitSet u {};
    assert(lua_unit_set["kinds"].valid());
    for (const auto& [k, v] : lua_unit_set.get<sol::table>("kinds")) {
        auto s = v.as<vector<string>>();
        u.add_kind(k.as<string>(), s[0], s.size() > 1 ? s[1] : "");
    }
    assert(lua_unit_set["units"].valid());
    for (const auto& [k, v] : lua_unit_set.get<sol::table>("units")) {
        string kind_str = k.as<string>();
        assert(u.kinds.contains(kind_str));
        auto kind = u.kinds[kind_str];
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

std::pair<ModelPtr, FlowsheetPtr> lua_new_model(string name, string index_fs_name, sol::table lua_unit_set) {
    UnitSet u = get_unit_set(lua_unit_set);
    auto M = new Model {name, index_fs_name, u};
    return std::pair<ModelPtr, FlowsheetPtr>(M, M->index_fs);
}

FlowsheetPtr lua_get_index_fs() {
    ModelPtr M = lua["M"];
    return M->index_fs;
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

BlockPtr lua_add_block(string name, string type, vector<StreamPtr> inlets, vector<StreamPtr> outlets) {
    FlowsheetPtr fs = lua["FS"];
    if (type == "Mixer") {
        auto blk_ptr = fs->add_block<Mixer>(name, inlets, outlets);
        return blk_ptr;
    }

    return nullptr;
}

std::regex re_binop(R"((\S+)(=|<|>)([^\s_]+)(?:_(\S+))?)");

void lua_set(string expressions) {
    std::istringstream expr_stream {expressions};
    ModelPtr M = lua["M"];

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
    M->initialize();
}

void lua_eval_constraints() {
    ModelPtr M = lua["M"];
    M->eval_constraints();
}

void lua_set_string_solver_option(string option, string val) {
    ModelPtr M = lua["M"];
    M->solver->Options()->SetStringValue(option, val);
}

void lua_set_integer_solver_option(string option, int val) {
    ModelPtr M = lua["M"];
    M->solver->Options()->SetIntegerValue(option, val);
}

void lua_set_numeric_solver_option(string option, double val) {
    ModelPtr M = lua["M"];
    M->solver->Options()->SetNumericValue(option, val);
}

int lua_initialize_solver() {
    ModelPtr M = lua["M"];
    return M->solver->Initialize();
}

int lua_solve() {
    ModelPtr M = lua["M"];
    return M->solver->OptimizeTNLP(M);
}

void lua_show_variables() {
    ModelPtr M = lua["M"];
    M->show_variables();
}

void lua_show_model_variables(ModelPtr m) {
    m->show_variables();
}

void lua_show_block_variables(BlockPtr blk) {
    blk->show_variables();
}

void lua_start() {
	lua.open_libraries(sol::lib::base);
    lua.set_function("Model", lua_new_model);
    lua.set_function("IndexFS", lua_get_index_fs);
    lua.set_function("Streams", lua_add_streams);
    lua.set_function("Block", lua_add_block);
    lua.set_function("Set", lua_set);
    lua.set_function("ShowVariables", sol::overload(lua_show_variables,
                                                    lua_show_model_variables,
                                                    lua_show_block_variables));
    lua.set_function("InitializeModel", lua_initialize_model);
    lua.set_function("EvalConstraints", lua_eval_constraints);
    lua.set_function("SolverOption", sol::overload(lua_set_string_solver_option,
                                                   lua_set_integer_solver_option,
                                                   lua_set_numeric_solver_option));
    lua.set_function("InitializeSolver", lua_initialize_solver);
    lua.set_function("Solve", lua_solve);
    
    
}
