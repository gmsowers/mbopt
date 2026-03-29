#include <cassert>
#include "Separator.hpp"

Separator::Separator(string_view       name_,
                     Flowsheet*        fs_,
                     vector<Stream*>&& inlets_,
                     vector<Stream*>&& outlets_) : 
                        Block(name_,
                              fs_,
                              BlockType::Separator,
                              std::move(inlets_),
                              std::move(outlets_))
{
    const auto& sin = inlets[0];
    auto m = fs->m;

    vector<string> outlet_comps_union {};
    for (const auto& sout : outlets)
        outlet_comps_union += sout->comps;
    assert(outlet_comps_union == sin->comps);

    // Total mass flow definition for all streams, \sum_{Cj in comps}(sep1.Si.mass_Cj) - sep1.Si.mass == 0,
    //     for Si in streams, Cj in comps.
    auto eq = m->add_constraint(prefix + sin->name + "_total_mass_def");
    g.push_back(eq);
    for (const auto& c : sin->comps)
        J.push_back(m->add_J_NZ(eq, x_strm[sin].mass.at(c)));
    J.push_back(m->add_J_NZ(eq, x_strm[sin].total_mass));
    for (const auto& sout : outlets) {
        eq = m->add_constraint(prefix + sout->name + "_total_mass_def");
        g.push_back(eq);
        for (const auto& c : sout->comps) {
            J.push_back(m->add_J_NZ(eq, x_strm[sout].mass.at(c)));
        }
        J.push_back(m->add_J_NZ(eq, x_strm[sout].total_mass));
    }

    // Mass fraction definitions, (sep1.Si.mass * sep1.Si.massfrac_Cj) - sep1.Si.mass_Cj == 0,
    //    for Si in streams, Cj in comps.
    for (const auto& c : sin->comps) {
        eq = m->add_constraint(prefix + sin->name + "." + c + "_massfrac_def");
        g.push_back(eq);
        J.push_back(m->add_J_NZ(eq, x_strm[sin].total_mass));
        J.push_back(m->add_J_NZ(eq, x_strm[sin].massfrac.at(c)));
        J.push_back(m->add_J_NZ(eq, x_strm[sin].mass.at(c)));
        H.push_back(m->add_H_NZ(eq, x_strm[sin].total_mass, x_strm[sin].massfrac.at(c)));
    }
    for (const auto& sout : outlets)
        for (const auto& c : sout->comps) {
            eq = m->add_constraint(prefix + sout->name + "." + c + "_massfrac_def");
            g.push_back(eq);
            J.push_back(m->add_J_NZ(eq, x_strm[sout].total_mass));
            J.push_back(m->add_J_NZ(eq, x_strm[sout].massfrac.at(c)));
            J.push_back(m->add_J_NZ(eq, x_strm[sout].mass.at(c)));
            H.push_back(m->add_H_NZ(eq, x_strm[sout].total_mass, x_strm[sout].massfrac.at(c)));
        }

    // Component split definitions, (sep1.Sin.mass_Cj * sep1.Si.split_Cj) - sep1.Si.mass_Cj == 0
    //    for Si in outlet streams, Cj in outlet stream comps.
    for (const auto& c : sin->comps)
        for (const auto& sout : outlets)
            if (sout->has_comp(c)) {
                auto v = m->add_var(prefix + sout->name + ".split_" + c, m->unitset->get_default_unit("frac"));
                x.push_back(v);
                x_split.push_back(v);
                eq = m->add_constraint(prefix + sout->name + "_split_" + c + "_def");
                g.push_back(eq);
                J.push_back(m->add_J_NZ(eq, x_strm[sin].mass.at(c)));
                J.push_back(m->add_J_NZ(eq, v));
                J.push_back(m->add_J_NZ(eq, x_strm[sout].mass.at(c)));
                H.push_back(m->add_H_NZ(eq, x_strm[sin].mass.at(c), v));
            }

    // Splits of each component sum to 1, Sum(sep1.Si.split_Cj) - 1 == 0, for Si in outlet streams,
    //     Cj in inlet stream components. Cj included in Sum only if Cj is present in outlet stream Si.
    // Fix the splits for all but one component in each outlet stream.
    for (int i = 0; const auto& c : sin->comps) {
        eq = m->add_constraint(prefix + "split_" + c + "_sum");
        g.push_back(eq);
        for (const auto& sout : outlets)
            if (sout->has_comp(c)) {
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
    for (const auto& c : sin->comps)
        mf += *x_strm[sin].mass[c];
    x_strm[sin].total_mass->convert_and_set(mf);

    // Calculate mass fractions in inlet stream.
    for (const auto& c : sin->comps)
        x_strm[sin].massfrac[c]->convert_and_set(*x_strm[sin].mass[c] / *x_strm[sin].total_mass);

    // Calculate free split variables.
    for (int i = 0; const auto& c : sin->comps) {
        double split_sum = 0.0;
        for (const auto& sout : outlets)
            if (sout->has_comp(c))
                split_sum += *x_split[i++];
        x_split[i - 1]->convert_and_set(1.0 - (split_sum - *x_split[i - 1]));
    }

    // Calculate outlet stream component mass flow rates.
    for (int i = 0; const auto& c : sin->comps) {
        double inlet_comp_mass = *x_strm[sin].mass.at(c);
        for (const auto& sout : outlets)
            if (sout->has_comp(c))
                x_strm[sout].mass[c]->convert_and_set(*x_split[i++] * inlet_comp_mass);
    }

    // Calculate total mass flow rates of the outlet streams.
    for (const auto& sout : outlets) {
        double base_val {0.0};
        for (const auto& c : sout->comps)
            base_val += *x_strm[sout].mass[c];
        x_strm[sout].total_mass->convert_and_set(base_val);
    }

    // Calculate mass fractions in the outlet streams
    for (const auto& sout : outlets)
        for (const auto& c : sout->comps)
            x_strm[sout].massfrac[c]->convert_and_set(*x_strm[sout].mass[c] / *x_strm[sout].total_mass);

}

//---------------------------------------------------------

void Separator::eval_constraints()
{
    const auto& sin = inlets[0];
    auto ic = 0;

    // Total mass flow definition for all streams, \sum_{Cj in comps}(sep1.Si.mass_Cj) - sep1.Si.mass == 0,
    //     for Si in streams, Cj in comps.
    *g[ic] = 0.0;
    for (const auto& c : sin->comps)
        *g[ic] += *x_strm[sin].mass.at(c);
    *g[ic++] -= *x_strm[sin].total_mass;

    for (const auto& sout : outlets) {
        *g[ic] = 0.0;
        for (const auto& c : sout->comps)
            *g[ic] += *x_strm[sout].mass.at(c);
        *g[ic++] -= *x_strm[sout].total_mass;
    }

    // Mass fraction definitions, (sep1.Si.mass * sep1.Si.massfrac_Cj) - sep1.Si.mass_Cj == 0
    //    for Si in streams, Cj in comps.
    for (const auto& c : sin->comps)
        *g[ic++] = *x_strm[sin].total_mass * *x_strm[sin].massfrac[c] - *x_strm[sin].mass[c];
    for (const auto& sout : outlets)
        for (const auto& c : sout->comps)
            *g[ic++] = *x_strm[sout].total_mass * *x_strm[sout].massfrac[c] - *x_strm[sout].mass[c];

    // Component split definitions, (sep1.Sin.mass_Cj * sep1.Si.split_Cj) - sep1.Si.mass_Cj == 0
    //    for Si in outlet streams, Cj in outlet stream comps.
    for (int i = 0; const auto& c : sin->comps)
        for (const auto& sout : outlets)
            if (sout->has_comp(c))
                *g[ic++] = *x_strm[sin].mass.at(c) * *x_split[i++] - *x_strm[sout].mass.at(c);

    // Splits of each component sum to 1, Sum(sep1.Si.split_Cj) - 1 == 0, for Si in outlet streams,
    //     Cj in inlet stream components. Cj included in Sum only if Cj is present in outlet stream Si.
    for (int i = 0; const auto& c : sin->comps) {
        *g[ic] = 0.0;
        for (const auto& sout : outlets)
            if (sout->has_comp(c))
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
    for (size_t i = 0; i < sin->comps.size(); ++i)
        *J[ic++] = 1.0;
    *J[ic++] = -1.0;
    for (const auto& sout : outlets) {
        for (size_t i = 0; i < sout->comps.size(); ++i)
            *J[ic++] = 1.0;
        *J[ic++] = -1.0;
    }

    // Mass fraction definitions, (sep1.Si.mass * sep1.Si.massfrac_Cj) - sep1.Si.mass_Cj == 0
    //    for Si in streams, Cj in comps.
    for (const auto& c : sin->comps) {
        *J[ic++] = *x_strm[sin].massfrac.at(c);
        *J[ic++] = *x_strm[sin].total_mass;
        *J[ic++] = -1.0;
    }
    for (const auto& sout : outlets)
        for (const auto& c : sout->comps) {
            *J[ic++] = *x_strm[sout].massfrac.at(c);
            *J[ic++] = *x_strm[sout].total_mass;
            *J[ic++] = -1.0;
        }

    // Component split definitions, (sep1.Sin.mass_Cj * sep1.Si.split_Cj) - sep1.Si.mass_Cj == 0
    //    for Si in outlet streams, Cj in outlet stream comps.
    for (int i = 0; const auto& c : sin->comps)
        for (const auto& sout : outlets)
            if (sout->has_comp(c)) {
                *J[ic++] = *x_split[i++];
                *J[ic++] = *x_strm[sin].mass.at(c);
                *J[ic++] = -1.0;
            }

    // Splits of each component sum to 1, Sum(sep1.Si.split_Cj) - 1 == 0, for Si in outlet streams,
    //     Cj in inlet stream components. Cj included in Sum only if Cj is present in outlet stream Si.
    for (const auto& c : sin->comps)
        for (const auto& sout : outlets)
            if (sout->has_comp(c))
                *J[ic++] = 1.0;

}

//---------------------------------------------------------

void Separator::eval_hessian() {
    const auto& sin = inlets[0];
    auto ic = 0;

    // Mass fraction definitions, (sep1.Si.mass * sep1.Si.massfrac_Cj) - sep1.Si.mass_Cj == 0,
    //    for Si in streams, Cj in comps.
    for (size_t i = 0; i < sin->comps.size(); ++i)
        *H[ic++] = 1.0;
    for (const auto& sout : outlets)
        for (size_t i = 0; i < sout->comps.size(); ++i)
            *H[ic++] = 1.0;
    
    // Component split definitions, (sep1.Sin.mass_Cj * sep1.Si.split_Cj) - sep1.Si.mass_Cj == 0
    //    for Si in outlet streams, Cj in outlet stream comps.
    for (const auto& c : sin->comps)
        for (const auto& sout : outlets)
            if (sout->has_comp(c))
                *H[ic++] = 1.0;

}
