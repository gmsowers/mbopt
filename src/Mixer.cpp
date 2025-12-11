#include <cassert>
#include <iostream>
#include "Mixer.hpp"

Mixer::Mixer(const std::string&            name_,
             FlowsheetPtr                  fs_,
             const std::vector<StreamPtr>& inlets_,
             const std::vector<StreamPtr>& outlets_): Block(name_, fs_, inlets_, outlets_)
{
    const auto& sout = outlets[0];
    auto m = fs->m;

    Comps inlet_comps_union {};
    for (const auto& sin : inlets)
        inlet_comps_union += sin->comps;
    assert(inlet_comps_union == sout->comps);

    // Component mass balances, \sum_{Ni in inlets}(mix1.Ni.mass_Cj) - mix1.OUT.mass_Cj == 0 for Cj in outlet_comps.
    for (const auto& compID : sout->comps) {
        auto eq = m->add_constraint(prefix + compID + "_mass_balance");
        g.push_back(eq);
        for (const auto& sin : inlets)
            if (x_strm[sin].mass.contains(compID))
                J.push_back(m->add_jacobian_element(eq, x_strm[sin].mass.at(compID)));
        J.push_back(m->add_jacobian_element(eq, x_strm[sout].mass.at(compID)));
    }

    // Total mass flow definitions, \sum_{Cj in comps}(mix1.Si.mass_Cj) - mix1.Si.mass == 0 for Si in streams.
    //  Inlet streams:
    for (const auto& sin : inlets) {
        auto eq = m->add_constraint(prefix + sin->name + "_total_mass_def");
        g.push_back(eq);
        for (const auto& compID : sin->comps) {
            J.push_back(m->add_jacobian_element(eq, x_strm[sin].mass.at(compID)));
        }
        J.push_back(m->add_jacobian_element(eq, x_strm[sin].total_mass));
    }
    //  Outlet stream:
    {
        auto eq = m->add_constraint(prefix + sout->name + "_total_mass_def");
        g.push_back(eq);
        for (const auto& compID : sout->comps) {
            J.push_back(m->add_jacobian_element(eq, x_strm[sout].mass.at(compID)));
        }
        J.push_back(m->add_jacobian_element(eq, x_strm[sout].total_mass));
    }

    // Mass fraction definitions, (mix1.Si.mass * mix1.Si.massfrac_Cj) - mix1.Si.mass_Cj == 0
    //    for Si in streams, Cj in comps.
    for (const auto& sin : inlets)
        for (const auto& compID : sin->comps) {
            auto eq = m->add_constraint(prefix + sin->name + "." + compID + "_massfrac_def");
            g.push_back(eq);
            J.push_back(m->add_jacobian_element(eq, x_strm[sin].total_mass));
            J.push_back(m->add_jacobian_element(eq, x_strm[sin].massfrac.at(compID)));
            J.push_back(m->add_jacobian_element(eq, x_strm[sin].mass.at(compID)));
            H.push_back(m->add_hessian_element(eq, x_strm[sin].total_mass, x_strm[sin].massfrac.at(compID)));
        }
    for (const auto& compID : sout->comps) {
        auto eq = m->add_constraint(prefix + sout->name + "." + compID + "_massfrac_def");
        g.push_back(eq);
        J.push_back(m->add_jacobian_element(eq, x_strm[sout].total_mass));
        J.push_back(m->add_jacobian_element(eq, x_strm[sout].massfrac.at(compID)));
        J.push_back(m->add_jacobian_element(eq, x_strm[sout].mass.at(compID)));
        H.push_back(m->add_hessian_element(eq, x_strm[sout].total_mass, x_strm[sout].massfrac.at(compID)));
    }

}

//---------------------------------------------------------

void Mixer::initialize() {
    const auto& sout = outlets[0];

    // Calculate outlet stream component mass flows, mix1.OUT.mass_Cj == \sum_{Ni in inlets}(mix1.Ni.mass_Cj) for Cj in outlet comps.
    for (const auto& compID : sout->comps) {
        double base_val = 0.0;
        for (const auto& sin : inlets)
            if (x_strm[sin].mass.contains(compID))
                base_val += *x_strm[sin].mass[compID];
        x_strm[sout].mass[compID]->from_base(base_val);
    }

    // Calculate total mass flow rates, mix1.Si.mass == \sum_{Cj in comps}(mix1.Si.mass_Cj) for Si in streams.
    for (const auto& sin : inlets) {
        double base_val = 0.0;
        for (const auto& compID : sin->comps)
            base_val += *x_strm[sin].mass[compID];
        x_strm[sin].total_mass->from_base(base_val);
    }
    {
        double base_val = 0.0;
        for (const auto& compID : sout->comps)
            base_val += *x_strm[sout].mass[compID];
        x_strm[sout].total_mass->from_base(base_val);
    }

    // Calculate mass fractions, mix1.Si.massfrac_Cj = mix1.Si.mass_Cj / mix1.Si.mass
    //    for Si in streams, Cj in comps.
    for (const auto& sin : inlets)
        for (const auto& compID : sin->comps)
            x_strm[sin].massfrac[compID]->from_base(*x_strm[sin].mass[compID] / *x_strm[sin].total_mass);
    for (const auto& compID : sout->comps)
        x_strm[sout].massfrac[compID]->from_base(*x_strm[sout].mass[compID] / *x_strm[sout].total_mass);

}

//---------------------------------------------------------

void Mixer::eval_constraints()
{
    const auto& sout = outlets[0];
    auto ic = 0;

    // Component mass balances, \sum_{Ni in inlets}(mix1.Ni.mass_Cj) - mix1.OUT.mass_Cj == 0 for Cj in outlet comps.
    *g[ic] = 0.0;
    for (const auto& compID : sout->comps) {
        for (const auto& sin : inlets)
            if (x_strm[sin].mass.contains(compID))
                *g[ic] += *x_strm[sin].mass.at(compID);
        *g[ic++] -= *x_strm[sout].mass.at(compID);
    }

    // Total mass flow definitions, \sum_{Cj in comps}(mix1.Si.mass_Cj) - mix1.Si.mass == 0 for Si in streams.
    //  Inlet streams:
    for (const auto& sin : inlets) {
        *g[ic] = 0.0;
        for (const auto& compID : sin->comps) {
            *g[ic] += *x_strm[sin].mass[compID];
        }
        *g[ic++] -= *x_strm[sin].total_mass;
    }
    //  Outlet stream:
    *g[ic] = 0.0;
    for (const auto& compID : sout->comps)
        *g[ic] += *x_strm[sout].mass[compID];
    *g[ic++] -= *x_strm[sout].total_mass;

    // Mass fraction definitions, (mix1.Si.mass * mix1.Si.massfrac_Cj) - mix1.Si.mass_Cj == 0
    //    for Si in streams, Cj in comps.
    for (const auto& sin : inlets)
        for (const auto& compID : sin->comps)
            *g[ic++] = *x_strm[sin].total_mass * *x_strm[sin].massfrac[compID] - *x_strm[sin].mass[compID];
    for (const auto& compID : sout->comps)
        *g[ic++] = *x_strm[sout].total_mass * *x_strm[sout].massfrac[compID] - *x_strm[sout].mass[compID];
}

//---------------------------------------------------------

void Mixer::eval_jacobian() {
    const auto& sout = outlets[0];
    auto ic = 0;

    // Component mass balances, \sum_{Ni in inlets}(mix1.Ni.mass_Cj) - mix1.OUT.mass_Cj == 0 for Cj in outlet comps.
    for (const auto& compID : sout->comps) {
        for (const auto& sin : inlets)
            if (x_strm[sin].mass.contains(compID))
                *J[ic++] = 1.0;
        *J[ic++] = -1.0;
    }

    // Total mass flow definitions, \sum_{Cj in comps}(mix1.Si.mass_Cj) - mix1.Si.mass == 0 for Si in streams.
    //  Inlet streams:
    for (const auto& sin : inlets) {
        for (const auto& compID : sin->comps)
            *J[ic++] = 1.0;
        *J[ic++] = -1.0;
    }
    //  Outlet stream:
    for (const auto& compID : sout->comps)
        *J[ic++] = 1.0;
    *J[ic++] = -1.0;

    // Mass fraction definitions, (mix1.Si.mass * mix1.Si.massfrac_Cj) - mix1.Si.mass_Cj == 0
    //    for Si in streams, Cj in comps.
    for (const auto& sin : inlets)
        for (const auto& compID : sin->comps) {
            *J[ic++] = *x_strm[sin].massfrac.at(compID);
            *J[ic++] = *x_strm[sin].total_mass;
            *J[ic++] = -1.0;
        }
    for (const auto& compID : sout->comps) {
        *J[ic++] = *x_strm[sout].massfrac.at(compID);
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
        for (const auto& compID : sin->comps)
            *H[ic++] = 1.0;
            
    for (const auto& compID : sout->comps)
        *H[ic++] = 1.0;

}
