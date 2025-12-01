#include <cassert>
#include <iostream>
#include "Mixer.hpp"

Mixer::Mixer(const std::string&            name_,
             ModelPtr                      m_,
             FlowsheetPtr                  fs_,
             const std::vector<StreamPtr>& inlets_,
             const std::vector<StreamPtr>& outlets_): Block(name_, m_, fs_, inlets_, outlets_)
{
    const StreamPtr outlet = outlets[0];
    Comps inlet_comps_union {};
    for (const auto& sin : inlets)
        inlet_comps_union += sin->comps;
    assert(inlet_comps_union == outlet->comps);

    const auto& sout = outlets[0];
    const std::string blk_prefix = name + ".";
    make_stream_variables(inlets, outlets);
    for (const auto& sin : inlets)
        for (const auto& compID : sin->comps)
            x_strm[sin].mass[compID]->fix();

    // Component mass balances, e.g., mix1.N1.mass_H2 + mix1.N2.mass_H2 - mix1.OUT.mass_H2 == 0
    for (const auto& compID : sout->comps) {
        auto eq = m->add_constraint(blk_prefix + compID + "_mass_balance");
        g.push_back(eq);
        for (const auto& sin : inlets)
            J.push_back(m->add_jacobian_element(eq, x_strm[sin].mass[compID]));
        J.push_back(m->add_jacobian_element(eq, x_strm[sout].mass[compID]));
    }

    // Mass fraction sum-to-one constraints, e.g., mix1.N1.mass_H2 + mix1.N1.mass_O2 - mix1.N1.mass == 0
    for (const auto& sin : inlets) {
        auto eq = m->add_constraint(blk_prefix + sin->name + ".massfrac_sum");
        g.push_back(eq);
        for (const auto& compID : sin->comps) {
            J.push_back(m->add_jacobian_element(eq, x_strm[sin].mass[compID]));
        }
        J.push_back(m->add_jacobian_element(eq, x_strm[sin].total_mass));
    }
    {
        auto eq = m->add_constraint(blk_prefix + sout->name + ".massfrac_sum");
        g.push_back(eq);
        for (const auto& compID : sout->comps) {
            J.push_back(m->add_jacobian_element(eq, x_strm[sout].mass[compID]));
        }
        J.push_back(m->add_jacobian_element(eq, x_strm[sout].total_mass));
    }

    // Mass fraction definitions, e.g., mix1.N1.mass * mix1.N1.massfrac_H2 - mix1.N1.mass_H2 == 0
    for (const auto& sin : inlets)
        for (const auto& compID : sin->comps) {
            auto eq = m->add_constraint(blk_prefix + sin->name + "." + compID + "_massfrac_def");
            g.push_back(eq);
            J.push_back(m->add_jacobian_element(eq, x_strm[sin].total_mass));
            J.push_back(m->add_jacobian_element(eq, x_strm[sin].massfrac[compID]));
            J.push_back(m->add_jacobian_element(eq, x_strm[sin].mass[compID]));
            H.push_back(m->add_hessian_element(eq, x_strm[sin].total_mass, x_strm[sin].massfrac[compID]));
        }
    for (const auto& compID : sout->comps) {
        auto eq = m->add_constraint(blk_prefix + sout->name + "." + compID + "_massfrac_def");
        g.push_back(eq);
        J.push_back(m->add_jacobian_element(eq, x_strm[sout].massfrac[compID]));
        J.push_back(m->add_jacobian_element(eq, x_strm[sout].total_mass));
        J.push_back(m->add_jacobian_element(eq, x_strm[sout].mass[compID]));
        H.push_back(m->add_hessian_element(eq, x_strm[sout].total_mass, x_strm[sout].massfrac[compID]));
    }

}

void Mixer::eval_constraints()
{
    const auto& sout = outlets[0];
    auto ic = 0;

    // Component mass balances, e.g., mix1.N1.mass_H2 + mix1.N2.mass_H2 - mix1.OUT.mass_H2 == 0
    *g[ic] = 0.0;
    for (const auto& compID : sout->comps) {
        for (const auto& sin : inlets)
            if (sin->has_comp(compID))
                *g[ic] += *x_strm[sin].mass[compID];
        *g[ic++] -= *x_strm[sout].mass[compID];
    }

    // Mass fraction sum-to-one constraints, e.g., mix1.N1.mass_H2 + mix1.N1.mass_O2 - mix1.N1.mass == 0
    for (const auto& sin : inlets) {
        *g[ic] = 0.0;
        for (const auto& compID : sin->comps) {
            *g[ic] += *x_strm[sin].mass[compID];
        }
        *g[ic++] -= *x_strm[sin].total_mass;
    }
    *g[ic] = 0.0;
    for (const auto& compID : sout->comps)
        *g[ic] += *x_strm[sout].mass[compID];
    *g[ic++] -= *x_strm[sout].total_mass;

    // Mass fraction definitions, e.g., mix1.N1.mass * mix1.N1.massfrac_H2 - mix1.N1.mass_H2 == 0
    for (const auto& sin : inlets)
        for (const auto& compID : sin->comps)
            *g[ic++] = *x_strm[sin].total_mass * *x_strm[sin].massfrac[compID] - *x_strm[sin].mass[compID];
    for (const auto& compID : sout->comps)
        *g[ic++] = *x_strm[sout].total_mass * *x_strm[sout].massfrac[compID] - *x_strm[sout].mass[compID];
}