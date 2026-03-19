#include <cassert>
#include "Mixer.hpp"

Mixer::Mixer(string_view       name_,
             Flowsheet*        fs_,
             vector<Stream*>&& inlets_,
             vector<Stream*>&& outlets_):
        Block(name_,
              fs_,
              BlockType::Mixer,
              std::move(inlets_),
              std::move(outlets_))
{
    const auto& sout = outlets[0];
    auto m = fs->m;

    vector<string> inlet_comps_union {};
    for (const auto& sin : inlets)
        inlet_comps_union += sin->comps;
    assert(inlet_comps_union == sout->comps);

    // Component mass balances, \sum_{Ni in inlets}(mix1.Ni.mass_Cj) - mix1.OUT.mass_Cj == 0 for Cj in outlet_comps.
    for (const auto& c : sout->comps) {
        auto eq = m->add_constraint(prefix + c + "_mass_balance");
        g.push_back(eq);
        for (const auto& sin : inlets)
            if (x_strm[sin].mass.contains(c))
                J.push_back(m->add_J_NZ(eq, x_strm[sin].mass.at(c)));
        J.push_back(m->add_J_NZ(eq, x_strm[sout].mass.at(c)));
    }

    // Total mass flow definitions, \sum_{Cj in comps}(mix1.Si.mass_Cj) - mix1.Si.mass == 0 for Si in streams.
    //  Inlet streams:
    for (const auto& sin : inlets) {
        auto eq = m->add_constraint(prefix + sin->name + "_total_mass_def");
        g.push_back(eq);
        for (const auto& c : sin->comps) {
            J.push_back(m->add_J_NZ(eq, x_strm[sin].mass.at(c)));
        }
        J.push_back(m->add_J_NZ(eq, x_strm[sin].total_mass));
    }
    //  Outlet stream:
    {
        auto eq = m->add_constraint(prefix + sout->name + "_total_mass_def");
        g.push_back(eq);
        for (const auto& c : sout->comps) {
            J.push_back(m->add_J_NZ(eq, x_strm[sout].mass.at(c)));
        }
        J.push_back(m->add_J_NZ(eq, x_strm[sout].total_mass));
    }

    // Mass fraction definitions, (mix1.Si.mass * mix1.Si.massfrac_Cj) - mix1.Si.mass_Cj == 0
    //    for Si in streams, Cj in comps.
    for (const auto& sin : inlets)
        for (const auto& c : sin->comps) {
            auto eq = m->add_constraint(prefix + sin->name + "." + c + "_massfrac_def");
            g.push_back(eq);
            J.push_back(m->add_J_NZ(eq, x_strm[sin].total_mass));
            J.push_back(m->add_J_NZ(eq, x_strm[sin].massfrac.at(c)));
            J.push_back(m->add_J_NZ(eq, x_strm[sin].mass.at(c)));
            H.push_back(m->add_H_NZ(eq, x_strm[sin].total_mass, x_strm[sin].massfrac.at(c)));
        }
    for (const auto& c : sout->comps) {
        auto eq = m->add_constraint(prefix + sout->name + "." + c + "_massfrac_def");
        g.push_back(eq);
        J.push_back(m->add_J_NZ(eq, x_strm[sout].total_mass));
        J.push_back(m->add_J_NZ(eq, x_strm[sout].massfrac.at(c)));
        J.push_back(m->add_J_NZ(eq, x_strm[sout].mass.at(c)));
        H.push_back(m->add_H_NZ(eq, x_strm[sout].total_mass, x_strm[sout].massfrac.at(c)));
    }

}

//---------------------------------------------------------

void Mixer::initialize() {
    const auto& sout = outlets[0];

    // Calculate outlet stream component mass flows, mix1.OUT.mass_Cj == \sum_{Ni in inlets}(mix1.Ni.mass_Cj) for Cj in outlet comps.
    for (const auto& c : sout->comps) {
        double base_val = 0.0;
        for (const auto& sin : inlets)
            if (x_strm[sin].mass.contains(c))
                base_val += *x_strm[sin].mass[c];
        x_strm[sout].mass[c]->convert_and_set(base_val);
    }

    // Calculate total mass flow rates, mix1.Si.mass == \sum_{Cj in comps}(mix1.Si.mass_Cj) for Si in streams.
    for (const auto& sin : inlets) {
        double base_val = 0.0;
        for (const auto& c : sin->comps)
            base_val += *x_strm[sin].mass[c];
        x_strm[sin].total_mass->convert_and_set(base_val);
    }
    {
        double base_val = 0.0;
        for (const auto& c : sout->comps)
            base_val += *x_strm[sout].mass[c];
        x_strm[sout].total_mass->convert_and_set(base_val);
    }

    // Calculate mass fractions, mix1.Si.massfrac_Cj = mix1.Si.mass_Cj / mix1.Si.mass
    //    for Si in streams, Cj in comps.
    for (const auto& sin : inlets)
        for (const auto& c : sin->comps)
            x_strm[sin].massfrac[c]->convert_and_set(*x_strm[sin].mass[c] / *x_strm[sin].total_mass);
    for (const auto& c : sout->comps)
        x_strm[sout].massfrac[c]->convert_and_set(*x_strm[sout].mass[c] / *x_strm[sout].total_mass);

}

//---------------------------------------------------------

void Mixer::eval_constraints()
{
    const auto& sout = outlets[0];
    auto ic = 0;

    // Component mass balances, \sum_{Ni in inlets}(mix1.Ni.mass_Cj) - mix1.OUT.mass_Cj == 0 for Cj in outlet comps.
    for (const auto& c : sout->comps) {
        *g[ic] = 0.0;
        for (const auto& sin : inlets)
            if (x_strm[sin].mass.contains(c))
                *g[ic] += *x_strm[sin].mass.at(c);
        *g[ic++] -= *x_strm[sout].mass.at(c);
    }

    // Total mass flow definitions, \sum_{Cj in comps}(mix1.Si.mass_Cj) - mix1.Si.mass == 0 for Si in streams.
    //  Inlet streams:
    for (const auto& sin : inlets) {
        *g[ic] = 0.0;
        for (const auto& c : sin->comps) {
            *g[ic] += *x_strm[sin].mass[c];
        }
        *g[ic++] -= *x_strm[sin].total_mass;
    }
    //  Outlet stream:
    *g[ic] = 0.0;
    for (const auto& c : sout->comps)
        *g[ic] += *x_strm[sout].mass[c];
    *g[ic++] -= *x_strm[sout].total_mass;

    // Mass fraction definitions, (mix1.Si.mass * mix1.Si.massfrac_Cj) - mix1.Si.mass_Cj == 0
    //    for Si in streams, Cj in comps.
    for (const auto& sin : inlets)
        for (const auto& c : sin->comps)
            *g[ic++] = *x_strm[sin].total_mass * *x_strm[sin].massfrac[c] - *x_strm[sin].mass[c];
    for (const auto& c : sout->comps)
        *g[ic++] = *x_strm[sout].total_mass * *x_strm[sout].massfrac[c] - *x_strm[sout].mass[c];
}

//---------------------------------------------------------

void Mixer::eval_jacobian() {
    const auto& sout = outlets[0];
    auto ic = 0;

    // Component mass balances, \sum_{Ni in inlets}(mix1.Ni.mass_Cj) - mix1.OUT.mass_Cj == 0 for Cj in outlet comps.
    for (const auto& c : sout->comps) {
        for (const auto& sin : inlets)
            if (x_strm[sin].mass.contains(c))
                *J[ic++] = 1.0;
        *J[ic++] = -1.0;
    }

    // Total mass flow definitions, \sum_{Cj in comps}(mix1.Si.mass_Cj) - mix1.Si.mass == 0 for Si in streams.
    //  Inlet streams:
    for (const auto& sin : inlets) {
        for (size_t i = 0; i < sin->comps.size(); ++i)
            *J[ic++] = 1.0;
        *J[ic++] = -1.0;
    }
    //  Outlet stream:
    for (size_t i = 0; i < sout->comps.size(); ++i)
        *J[ic++] = 1.0;
    *J[ic++] = -1.0;

    // Mass fraction definitions, (mix1.Si.mass * mix1.Si.massfrac_Cj) - mix1.Si.mass_Cj == 0
    //    for Si in streams, Cj in comps.
    for (const auto& sin : inlets)
        for (const auto& c : sin->comps) {
            *J[ic++] = *x_strm[sin].massfrac.at(c);
            *J[ic++] = *x_strm[sin].total_mass;
            *J[ic++] = -1.0;
        }
    for (const auto& c : sout->comps) {
        *J[ic++] = *x_strm[sout].massfrac.at(c);
        *J[ic++] = *x_strm[sout].total_mass;
        *J[ic++] = -1.0;
    }

}

//---------------------------------------------------------

void Mixer::eval_hessian() {
    const auto& sout = outlets[0];
    auto ic = 0;

    // Mass fraction definitions, (mix1.Si.mass * mix1.Si.massfrac_Cj) - mix1.Si.mass_Cj == 0
    //    for Si in streams, Cj in comps.
    for (const auto& sin : inlets)
        for (size_t i = 0; i < sin->comps.size(); ++i)
            *H[ic++] = 1.0;
            
    for (size_t i = 0; i < sout->comps.size(); ++i)
        *H[ic++] = 1.0;

}
