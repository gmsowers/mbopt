#pragma once
#include <lua.hpp>
#include <string>

struct LuaResult {
    bool ok {true};
    std::string err_str {};
};

bool start_lua();
LuaResult run_lua_script(const char* script_file_name);
