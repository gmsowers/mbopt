#include <sol/sol.hpp>
#include "Model.hpp"

void lua_start();
void lua_run_script(string script_file_name);

ModelPtr lua_new_model(string model_name, sol::table lua_unit_set);
void lua_test_model(ModelPtr m);


