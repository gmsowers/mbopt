#pragma once
#include <climits>  // Necessary because luaconf.h uses LLONG_MAX, which isn't defined by gcc on MacOS
#include <lua.hpp>
#include <string>

struct LuaResult {
    bool ok {true};
    std::string err_str {};
};

lua_State* start_lua();
LuaResult run_lua_script(lua_State* L, const char* script_file_name);
