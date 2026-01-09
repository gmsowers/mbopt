#pragma once
#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>
#include <string>

struct LuaResult {
    bool ok {true};
    std::string err_str {};
};

void lua_init();
LuaResult lua_run_script(const std::string& script_file_name);
