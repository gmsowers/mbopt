#include <iostream>
#include "Model.hpp"
#include "Mixer.hpp"

using namespace std;

int main()
{
    unordered_map<string, Unit> unit_set {};

    unit_set["massflow"] = {"kg/hr", "kg/s", 1.0/3600.0};
    unit_set["massfrac"] = {"frac", "frac", 1.0};
    Model m {"m", unit_set};

    Comps c {"H2", "O2"};
    auto sin1 = m.add_stream("N1", m.index_fs, c);
    Comps c2 {"H2", "O2", "CO"};
    Comps cmix {c + c2};
    auto sin2 = m.add_stream("N2", m.index_fs, c2);
    auto sout = m.add_stream("OUT", m.index_fs, cmix);
    auto mix1 = m.index_fs->add_block<Mixer>("mix1", &m, {sin1, sin2}, {sout});
    for (auto& v : m.x)
        cout << v->name << '\n';
    for (auto& eq : m.g)
        cout << eq->name << '\n';

    m.x[0]->value = 100.0;
    m.x[1]->value = 100.0;
    double xval = *m.x[0] + *m.x[1];
    cout << xval << '\n';
}