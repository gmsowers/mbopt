#pragma once
#include <lua.hpp>
#include <string>

struct LuaResult {
    bool ok {true};
    std::string err_str {};
};

lua_State* start_lua();
LuaResult run_lua_script(lua_State* L, const char* script_file_name);
