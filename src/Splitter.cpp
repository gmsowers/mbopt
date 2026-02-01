#include <cassert>
#include "Splitter.hpp"

Splitter::Splitter(string_view       name_,
                   Flowsheet*        fs_,
                   vector<Stream*>&& inlets_,
                   vector<Stream*>&& outlets_) : 
                        Block(name_, fs_, std::move(inlets_), std::move(outlets_))
{
    const auto& sin = inlets[0];
    const auto m = fs->m;

    for (const auto& sout : outlets)
        assert(sin->comps == sout->comps);

    x_split.resize(outlets.size());
    
    // Total mass flow definition for inlet stream, \sum_{Cj in comps}(spl1.Sin.mass_Cj) - spl1.Sin.mass == 0.
    auto eq = m->add_constraint(prefix + sin->name + "_total_mass_def");
    g.push_back(eq);
    for (const auto& compID : sin->comps)
        J.push_back(m->add_J_NZ(eq, x_strm[sin].mass.at(compID)));
    J.push_back(m->add_J_NZ(eq, x_strm[sin].total_mass));

    // Mass fraction definitions, (spl1.Si.mass * spl1.Si.massfrac_Cj) - spl1.Si.mass_Cj == 0
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

    // Split fraction definitions, (spl1.Sin.mass * spl1.Si.splitfrac) - spl1.Si.mass == 0
    //    for Si in outlet streams.
    for (int i = 0; const auto& sout : outlets) {
        auto v = m->add_var(prefix + sout->name + ".splitfrac", m->unit_set.get_default_unit("frac"));
        x.push_back(v);
        x_split[i++] = v;
        eq = m->add_constraint(prefix + sout->name + "_splitfrac_def");
        g.push_back(eq);
        J.push_back(m->add_J_NZ(eq, x_strm[sin].total_mass));
        J.push_back(m->add_J_NZ(eq, v));
        J.push_back(m->add_J_NZ(eq, x_strm[sout].total_mass));
        H.push_back(m->add_H_NZ(eq, x_strm[sin].total_mass, v));
    }

    // Split fractions sum to 1, Sum(spl1.Si.splitfrac) - 1 ==0, for Si in outlet streams.
    eq = m->add_constraint(prefix + "splitfrac_sum");
    g.push_back(eq);
    for (int i = 0; i < x_split.size(); i++)
        J.push_back(m->add_J_NZ(eq, x_split[i]));

    // Composition copy, spl1.Sin.massfrac_Cj - spl1.Si.massfrac_Cj == 0, for Si in outlet streams, Cj in Sin.comps.
    for (const auto& sout : outlets)
        for (const auto& compID : sout->comps) {
            eq = m->add_constraint(prefix + sout->name + "." + compID + "_massfrac_copy");
            g.push_back(eq);
            J.push_back(m->add_J_NZ(eq, x_strm[sin].massfrac.at(compID)));
            J.push_back(m->add_J_NZ(eq, x_strm[sout].massfrac.at(compID)));
        }

    // Fix all the splitfracs except the last one.
    for (int i = 0; i < x_split.size() - 1; i++)
        x_split[i]->fix();
}

//---------------------------------------------------------

void Splitter::initialize() {
    const auto& sin = inlets[0];

    // Calculate the values of free split fracs.
    auto n_fixed = 0;
    auto sum_fixed = 0.0;
    for (const auto& frac : x_split)
        if (frac->is_fixed()) {
            n_fixed++;
            sum_fixed += *frac;
        }
    auto n_free = x_split.size() - n_fixed;
    if (n_free > 0) {
        double free_frac = sum_fixed / n_free;
        for (const auto& frac : x_split)
            if (frac->is_free())
                *frac = free_frac;
    }
    
    // Calculate total mass flow rate of inlet stream.
    double base_val {0.0};
    for (const auto& compID : sin->comps)
        base_val += *x_strm[sin].mass[compID];
    x_strm[sin].total_mass->convert_and_set(base_val);

    // Calculate outlet stream total mass flow rates.
    for (int i = 0; const auto& sout : outlets)
        x_strm[sout].total_mass->convert_and_set(*x_strm[sin].total_mass * *x_split[i++]);

    // Calculate mass fractions in inlet stream.
    for (const auto& compID : sin->comps)
        x_strm[sin].massfrac[compID]->convert_and_set(*x_strm[sin].mass[compID] / *x_strm[sin].total_mass);

    // Set mass fractions of outlet streams, and calculate outlet stream component mass flow rates.
    for (const auto& sout : outlets)
        for (const auto& compID : sout->comps) {
            auto mf = *x_strm[sin].massfrac[compID];
            x_strm[sout].massfrac[compID]->convert_and_set(mf);
            x_strm[sout].mass[compID]->convert_and_set(mf * *x_strm[sout].total_mass);
        }

}

//---------------------------------------------------------

void Splitter::eval_constraints()
{
    const auto& sin = inlets[0];
    auto ic = 0;

    // Total mass flow definition for inlet stream, \sum_{Cj in comps}(spl1.Sin.mass_Cj) - spl1.Sin.mass == 0.
    *g[ic] = 0.0;
    for (const auto& compID : sin->comps)
        *g[ic] += *x_strm[sin].mass[compID];
    *g[ic++] -= *x_strm[sin].total_mass;

    // Mass fraction definitions, (spl1.Si.mass * spl1.Si.massfrac_Cj) - spl1.Si.mass_Cj == 0
    //    for Si in streams, Cj in comps.
    for (const auto& compID : sin->comps)
        *g[ic++] = *x_strm[sin].total_mass * *x_strm[sin].massfrac[compID] - *x_strm[sin].mass[compID];
    for (const auto& sout : outlets)
        for (const auto& compID : sout->comps)
            *g[ic++] = *x_strm[sout].total_mass * *x_strm[sout].massfrac[compID] - *x_strm[sout].mass[compID];

    // Split fraction definitions, (spl1.Sin.mass * spl1.Si.splitfrac) - spl1.Si.mass == 0
    //    for Si in outlet streams.
    for (int i = 0; const auto& sout : outlets)
        *g[ic++] = *x_strm[sin].total_mass * *x_split[i++] - *x_strm[sout].total_mass;

    // Split fractions sum to 1, Sum(spl1.Si.splitfrac) - 1 ==0, for Si in outlet streams.
    *g[ic] = 0.0;
    for (int i = 0; i < x_split.size(); i++)
        *g[ic] += *x_split[i];
    *g[ic++] -= 1.0;

    // Composition copy, spl1.Sin.massfrac_Cj - spl1.Si.massfrac_Cj == 0, for Si in outlet streams, Cj in Sin.comps.
    for (const auto& sout : outlets)
        for (const auto& compID : sout->comps)
            *g[ic++] = *x_strm[sin].massfrac[compID] - *x_strm[sout].massfrac[compID];
        
}

//---------------------------------------------------------

void Splitter::eval_jacobian() {
    const auto& sin = inlets[0];
    auto ic = 0;

    // Total mass flow definition for inlet stream, \sum_{Cj in comps}(spl1.Sin.mass_Cj) - spl1.Sin.mass == 0.
    for (const auto& compID : sin->comps)
        *J[ic++] = 1.0;
    *J[ic++] = -1.0;

    // Mass fraction definitions, (spl1.Si.mass * spl1.Si.massfrac_Cj) - spl1.Si.mass_Cj == 0
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

    // Split fraction definitions, (spl1.Sin.mass * spl1.Si.splitfrac) - spl1.Si.mass == 0
    //    for Si in outlet streams.
    for (int i = 0; const auto& sout : outlets) {
        *J[ic++] = *x_split[i++];
        *J[ic++] = *x_strm[sin].total_mass;
        *J[ic++] = -1.0;
    }

    // Split fractions sum to 1, Sum(spl1.Si.splitfrac) - 1 ==0, for Si in outlet streams.
    for (int i = 0; i < x_split.size(); i++)
        *J[ic++] = 1.0;

    // Composition copy, spl1.Sin.massfrac_Cj - spl1.Si.massfrac_Cj == 0, for Si in outlet streams, Cj in Sin.comps.
    for (const auto& sout : outlets)
        for (const auto& compID : sout->comps) {
            *J[ic++] = 1.0;
            *J[ic++] = -1.0;
        }

}

//---------------------------------------------------------

void Splitter::eval_hessian() {
    const auto& sin = inlets[0];
    auto ic = 0;

    // Mass fraction definitions, (spl1.Si.mass * spl1.Si.massfrac_Cj) - spl1.Si.mass_Cj == 0
    //    for Si in streams, Cj in comps.
    for (int i = 0; i < sin->comps.size(); i++)
        *H[ic++] = 1.0;
    for (int i = 0; i < outlets.size(); i++)
        for (int j = 0; j < sin->comps.size(); j++) // sin->comps == sout-> comps for all outlet streams
            *H[ic++] = 1.0;

    // Split fraction definitions, (spl1.Sin.mass * spl1.Si.splitfrac) - spl1.Si.mass == 0
    //    for Si in outlet streams.
    for (int i = 0; i < outlets.size(); i++)
        *H[ic++] = 1.0;
    

}
