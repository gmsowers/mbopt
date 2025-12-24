#include "lua_interface.hpp"
#include <tuple>

static sol::state lua;

void lua_start() {
	lua.open_libraries(sol::lib::base);
    lua.set_function("new_model", lua_new_model);
    lua.set_function("test_model", lua_test_model);
}

void lua_run_script(string script_file_name) {
    lua.script_file(script_file_name);
}

ModelPtr lua_new_model(string name, sol::table lua_unit_set) {
    UnitSet u {};
    for (const auto& [k, v] : lua_unit_set.get<sol::table>("kinds")) {
        auto s = v.as<vector<string>>();
        u.add_kind(k.as<string>(), s[0], s.size() > 1 ? s[1] : "");
    }
    for (const auto& [k, v] : lua_unit_set.get<sol::table>("units")) {
        auto kind = u.kinds[k.as<string>()];
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

    return new Model {name, u};
}

void lua_test_model(ModelPtr m) {
    cout << m->name << '\n';
}

