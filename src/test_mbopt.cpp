#include <iostream>
#include "lua_interface.hpp"

int main(int argc, const char *argv[])
{
    lua_init();
    for (int i = 1; i < argc; i++) {
        //std::cout << "Running Lua script: " << argv[i] << std::endl;
        auto [ok, err_str] = lua_run_script(std::string(argv[i]));
        if (!ok)
            std::cout << "Error running Lua script: " << err_str << '\n';
    }
    return 0;
}