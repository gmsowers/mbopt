#include <iostream>
#include "lua_interface.hpp"

int main()
{
    lua_init();
    auto [ok, err_str] = lua_run_script("model.lua");
    if (!ok)
        std::cout << err_str << '\n';
        
    return 0;
}