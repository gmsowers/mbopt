#pragma once
#include <sol/sol.hpp>
#include <string>

void lua_start();
void lua_run_script(std::string script_file_name);
