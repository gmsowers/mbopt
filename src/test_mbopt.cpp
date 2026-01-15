#include <iostream>
#include "scripter.hpp"

int main(int argc, const char *argv[])
{
    if (!start_lua()) {
        std::cerr << "Failed to start Lua\n";
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        auto [ok, err_str] = run_lua_script(argv[i]);
        if (!ok) {
            std::cerr << "Error running Lua script: " << err_str << '\n';
            return 1;
        }
    }
    return 0;
}