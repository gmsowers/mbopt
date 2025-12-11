#include <iostream>
#include "Model.hpp"
#include "Mixer.hpp"

using namespace std;

int main()
{
    unordered_map<string, Unit> unit_set {};

    unit_set["massflow"] = {"kg/hr", "dg/hr", 0.1};
    unit_set["massfrac"] = {"frac", "frac", 1.0};

    Model m {"m", unit_set};
    auto fs = m.index_fs;

    Comps c {"H2", "O2"};
    auto sin1 = fs->add_stream("N1", c);

    Comps c2 {"H2", "O2", "CO"};
    auto sin2 = fs->add_stream("N2", c2);

    Comps cmix {c + c2};
    auto sout = fs->add_stream("OUT", cmix);

    auto mix1 = fs->add_block<Mixer>("mix1", {sin1, sin2}, {sout});

    *m.x_map["mix1.N1.mass_H2"] = 100.0;
    *m.x_map["mix1.N1.mass_O2"] = 50.0;
    *m.x_map["mix1.N2.mass_H2"] = 40.0;
    *m.x_map["mix1.N2.mass_O2"] = 10.0;
    *m.x_map["mix1.N2.mass_CO"] = 20.0;
    mix1->initialize();
    mix1->eval_constraints();
    for (auto& eq : m.g)
        cout << eq->name << " = " << eq->value << '\n';
    mix1->eval_jacobian();
    for (auto& elem : mix1->J)
        cout << elem->con->name << " wrt " << elem->var->name << " = " << elem->value << '\n';
    mix1->eval_hessian();
    for (auto& elem : mix1->H)
        cout << elem->var1->name << " , " << elem->var2->name << " = " << elem->value << '\n';
}