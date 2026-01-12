#include <iostream>
#include "scripter.hpp"

int main(int argc, const char *argv[])
{
    start_lua();
    for (int i = 1; i < argc; i++) {
        //std::cout << "Running Lua script: " << argv[i] << std::endl;
        auto [ok, err_str] = run_lua_script(std::string(argv[i]));
        if (!ok)
            std::cout << "Error running Lua script: " << err_str << '\n';
    }
    return 0;
}