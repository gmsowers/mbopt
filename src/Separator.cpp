#include <cassert>
#include <iostream>
#include "Separator.hpp"

Separator::Separator(string_view       name_,
                   Flowsheet*        fs_,
                   vector<Stream*>&& inlets_,
                   vector<Stream*>&& outlets_) : 
                        Block(name_, fs_, std::move(inlets_), std::move(outlets_))
{
    const auto& sin = inlets[0];
    auto m = fs->m;

    vector<string> outlet_comps_union {};
    for (const auto& sout : outlets)
        outlet_comps_union += sout->comps;
    assert(outlet_comps_union == sin->comps);

    Constraint* eq;
    
    // Total mass flow definition for all streams, \sum_{Cj in comps}(sep1.Si.mass_Cj) - sep1.Si.mass == 0,
    //     for Si in streams, Cj in comps.
    eq = m->add_constraint(prefix + sin->name + "_total_mass_def");
    g.push_back(eq);
    for (const auto& compID : sin->comps)
        J.push_back(m->add_J_NZ(eq, x_strm[sin].mass.at(compID)));
    J.push_back(m->add_J_NZ(eq, x_strm[sin].total_mass));
    for (const auto& sout : outlets) {
        auto eq = m->add_constraint(prefix + sout->name + "_total_mass_def");
        g.push_back(eq);
        for (const auto& compID : sout->comps) {
            J.push_back(m->add_J_NZ(eq, x_strm[sout].mass.at(compID)));
        }
        J.push_back(m->add_J_NZ(eq, x_strm[sout].total_mass));
    }

    // Mass fraction definitions, (sep1.Si.mass * sep1.Si.massfrac_Cj) - sep1.Si.mass_Cj == 0,
    //    for Si in streams, Cj in comps.
    for (const auto& compID : sin->comps) {
        eq = m->add_constraint(prefix + sin->name + "." + compID + "_massfrac_def");
        g.push_back(eq);
        J.push_back(m->add_J_NZ(eq, x_strm[sin].total_mass));
        J.push_back(m->add_J_NZ(eq, x_strm[sin].massfrac.at(compID)));
        J.push_back(m->add_J_NZ(eq, x_strm[sin].mass.at(compID)));
        H.push_back(m->add_H_NZ(eq, x_strm[sin].total_mass, x_strm[sin].massfrac.at(compID)));
    }
    for (const auto& sout : outlets)
        for (const auto& compID : sout->comps) {
            eq = m->add_constraint(prefix + sout->name + "." + compID + "_massfrac_def");
            g.push_back(eq);
            J.push_back(m->add_J_NZ(eq, x_strm[sout].total_mass));
            J.push_back(m->add_J_NZ(eq, x_strm[sout].massfrac.at(compID)));
            J.push_back(m->add_J_NZ(eq, x_strm[sout].mass.at(compID)));
            H.push_back(m->add_H_NZ(eq, x_strm[sout].total_mass, x_strm[sout].massfrac.at(compID)));
        }

    // Component split definitions, (sep1.Sin.mass_Cj * sep1.Si.split_Cj) - sep1.Si.mass_Cj == 0
    //    for Si in outlet streams, Cj in outlet stream comps.
    for (const auto& compID : sin->comps)
        for (const auto& sout : outlets)
            if (sout->has_comp(compID)) {
                auto v = m->add_var(prefix + sout->name + ".split_" + compID, m->unit_set.get_default_unit("frac"));
                x.push_back(v);
                x_split.push_back(v);
                eq = m->add_constraint(prefix + sout->name + "_split_" + compID + "_def");
                g.push_back(eq);
                J.push_back(m->add_J_NZ(eq, x_strm[sin].mass.at(compID)));
                J.push_back(m->add_J_NZ(eq, v));
                J.push_back(m->add_J_NZ(eq, x_strm[sout].mass.at(compID)));
                H.push_back(m->add_H_NZ(eq, x_strm[sin].mass.at(compID), v));
            }

    // Splits of each component sum to 1, Sum(sep1.Si.split_Cj) - 1 == 0, for Si in outlet streams,
    //     Cj in inlet stream components. Cj included in Sum only if Cj is present in outlet stream Si.
    // Fix the splits for all but one component in each outlet stream.
    for (int i = 0; const auto& compID : sin->comps) {
        eq = m->add_constraint(prefix + "split_" + compID + "_sum");
        g.push_back(eq);
        for (const auto& sout : outlets)
            if (sout->has_comp(compID)) {
                J.push_back(m->add_J_NZ(eq, x_split[i]));
                x_split[i++]->fix();
            }
        x_split[i - 1]->free();
    }
}

//---------------------------------------------------------

void Separator::initialize() {
    const auto& sin = inlets[0];

    // Calculate total mass flow rate of inlet stream.
    double mf {0.0};
    for (const auto& compID : sin->comps)
        mf += *x_strm[sin].mass[compID];
    x_strm[sin].total_mass->convert_and_set(mf);

    // Calculate mass fractions in inlet stream.
    for (const auto& compID : sin->comps)
        x_strm[sin].massfrac[compID]->convert_and_set(*x_strm[sin].mass[compID] / *x_strm[sin].total_mass);

    // Calculate free split variables.
    for (int i = 0; const auto& compID : sin->comps) {
        double split_sum = 0.0;
        for (const auto& sout : outlets)
            if (sout->has_comp(compID))
                split_sum += *x_split[i++];
        x_split[i - 1]->convert_and_set(1.0 - (split_sum - *x_split[i - 1]));
    }

    // Calculate outlet stream component mass flow rates.
    for (int i = 0; const auto& compID : sin->comps) {
        double inlet_comp_mass = *x_strm[sin].mass.at(compID);
        for (const auto& sout : outlets)
            if (sout->has_comp(compID))
                x_strm[sout].mass[compID]->convert_and_set(*x_split[i++] * inlet_comp_mass);
    }

    // Calculate total mass flow rates of the outlet streams.
    for (const auto& sout : outlets) {
        double base_val {0.0};
        for (const auto& compID : sout->comps)
            base_val += *x_strm[sout].mass[compID];
        x_strm[sout].total_mass->convert_and_set(base_val);
    }

    // Calculate mass fractions in the outlet streams
    for (const auto& sout : outlets)
        for (const auto& compID : sout->comps)
            x_strm[sout].massfrac[compID]->convert_and_set(*x_strm[sout].mass[compID] / *x_strm[sout].total_mass);

}

//---------------------------------------------------------

void Separator::eval_constraints()
{
    const auto& sin = inlets[0];
    auto ic = 0;

    // Total mass flow definition for all streams, \sum_{Cj in comps}(sep1.Si.mass_Cj) - sep1.Si.mass == 0,
    //     for Si in streams, Cj in comps.
    *g[ic] = 0.0;
    for (const auto& compID : sin->comps)
        *g[ic] += *x_strm[sin].mass.at(compID);
    *g[ic++] -= *x_strm[sin].total_mass;

    for (const auto& sout : outlets) {
        *g[ic] = 0.0;
        for (const auto& compID : sout->comps)
            *g[ic] += *x_strm[sout].mass.at(compID);
        *g[ic++] -= *x_strm[sout].total_mass;
    }

    // Mass fraction definitions, (sep1.Si.mass * sep1.Si.massfrac_Cj) - sep1.Si.mass_Cj == 0
    //    for Si in streams, Cj in comps.
    for (const auto& compID : sin->comps)
        *g[ic++] = *x_strm[sin].total_mass * *x_strm[sin].massfrac[compID] - *x_strm[sin].mass[compID];
    for (const auto& sout : outlets)
        for (const auto& compID : sout->comps)
            *g[ic++] = *x_strm[sout].total_mass * *x_strm[sout].massfrac[compID] - *x_strm[sout].mass[compID];

    // Component split definitions, (sep1.Sin.mass_Cj * sep1.Si.split_Cj) - sep1.Si.mass_Cj == 0
    //    for Si in outlet streams, Cj in outlet stream comps.
    for (int i = 0; const auto& compID : sin->comps)
        for (const auto& sout : outlets)
            if (sout->has_comp(compID))
                *g[ic++] = *x_strm[sin].mass.at(compID) * *x_split[i++] - *x_strm[sout].mass.at(compID);

    // Splits of each component sum to 1, Sum(sep1.Si.split_Cj) - 1 == 0, for Si in outlet streams,
    //     Cj in inlet stream components. Cj included in Sum only if Cj is present in outlet stream Si.
    for (int i = 0; const auto& compID : sin->comps) {
        *g[ic] = 0.0;
        for (const auto& sout : outlets)
            if (sout->has_comp(compID))
                *g[ic] += *x_split[i++];
        *g[ic++] -= 1.0;
    }
                
}

//---------------------------------------------------------

void Separator::eval_jacobian() {
    const auto& sin = inlets[0];
    auto ic = 0;

    // Total mass flow definition for all streams, \sum_{Cj in comps}(sep1.Si.mass_Cj) - sep1.Si.mass == 0,
    //     for Si in streams, Cj in comps.
    for (const auto& compID : sin->comps)
        *J[ic++] = 1.0;
    *J[ic++] = -1.0;
    for (const auto& sout : outlets) {
        for (const auto& compID : sout->comps)
            *J[ic++] = 1.0;
        *J[ic++] = -1.0;
    }

    // Mass fraction definitions, (sep1.Si.mass * sep1.Si.massfrac_Cj) - sep1.Si.mass_Cj == 0
    //    for Si in streams, Cj in comps.
    for (const auto& compID : sin->comps) {
        *J[ic++] = *x_strm[sin].massfrac.at(compID);
        *J[ic++] = *x_strm[sin].total_mass;
        *J[ic++] = -1.0;
    }
    for (const auto& sout : outlets)
        for (const auto& compID : sout->comps) {
            *J[ic++] = *x_strm[sout].massfrac.at(compID);
            *J[ic++] = *x_strm[sout].total_mass;
            *J[ic++] = -1.0;
        }

    // Component split definitions, (sep1.Sin.mass_Cj * sep1.Si.split_Cj) - sep1.Si.mass_Cj == 0
    //    for Si in outlet streams, Cj in outlet stream comps.
    for (int i = 0; const auto& compID : sin->comps)
        for (const auto& sout : outlets)
            if (sout->has_comp(compID)) {
                *J[ic++] = *x_split[i++];
                *J[ic++] = *x_strm[sin].mass.at(compID);
                *J[ic++] = -1.0;
            }

    // Splits of each component sum to 1, Sum(sep1.Si.split_Cj) - 1 == 0, for Si in outlet streams,
    //     Cj in inlet stream components. Cj included in Sum only if Cj is present in outlet stream Si.
    for (int i = 0; const auto& compID : sin->comps)
        for (const auto& sout : outlets)
            if (sout->has_comp(compID))
                *J[ic++] = 1.0;

}

//---------------------------------------------------------

void Separator::eval_hessian() {
    const auto& sin = inlets[0];
    auto ic = 0;

    // Mass fraction definitions, (sep1.Si.mass * sep1.Si.massfrac_Cj) - sep1.Si.mass_Cj == 0,
    //    for Si in streams, Cj in comps.
    for (const auto& compID : sin->comps)
        *H[ic++] = 1.0;
    for (const auto& sout : outlets)
        for (const auto& compID : sout->comps)
            *H[ic++] = 1.0;
    
    // Component split definitions, (sep1.Sin.mass_Cj * sep1.Si.split_Cj) - sep1.Si.mass_Cj == 0
    //    for Si in outlet streams, Cj in outlet stream comps.
    for (const auto& compID : sin->comps)
        for (const auto& sout : outlets)
            if (sout->has_comp(compID))
                *H[ic++] = 1.0;

}
