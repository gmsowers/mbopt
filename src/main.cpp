#include <iostream>
#ifdef _WIN32
#include <windows.h>
#endif
#include "scripter.hpp"

int main(int argc, const char *argv[])
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    lua_State* L = start_lua();
    if (!L) {
        std::cerr << "Failed to start Lua\n";
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        auto [ok, err_str] = run_lua_script(L, argv[i]);
        if (!ok) {
            std::cerr << "Error running Lua script: " << err_str << '\n';
            return 1;
        }
    }
    return 0;
}