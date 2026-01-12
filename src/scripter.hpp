#pragma once
#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>
#include <string>

struct LuaResult {
    bool ok {true};
    std::string err_str {};
};

void start_lua();
LuaResult run_lua_script(const std::string& script_file_name);
