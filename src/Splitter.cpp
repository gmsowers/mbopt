#include <cassert>
#include "Splitter.hpp"

Splitter::Splitter(string_view       name_,
                   Flowsheet*        fs_,
                   vector<Stream*>&& inlets_,
                   vector<Stream*>&& outlets_) : 
                Block(name_,
                      fs_,
                      BlockType::Splitter,
                      std::move(inlets_),
                      std::move(outlets_))
{
    const auto& sin = inlets[0];
    const auto m = fs->m;

    for (const auto& sout : outlets)
        assert(sin->comps == sout->comps);

    x_split.resize(outlets.size());
    
    // Total mass flow definition for inlet stream, \sum_{Cj in comps}(spl1.Sin.mass_Cj) - spl1.Sin.mass == 0.
    auto eq = m->add_constraint(prefix + sin->name + "_total_mass_def");
    g.push_back(eq);
    for (const auto& c : sin->comps)
        J.push_back(m->add_J_NZ(eq, x_strm[sin].mass.at(c)));
    J.push_back(m->add_J_NZ(eq, x_strm[sin].total_mass));

    // Mass fraction definitions, (spl1.Si.mass * spl1.Si.massfrac_Cj) - spl1.Si.mass_Cj == 0
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

    // Split fraction definitions, (spl1.Sin.mass * spl1.Si.splitfrac) - spl1.Si.mass == 0
    //    for Si in outlet streams.
    for (int i = 0; const auto& sout : outlets) {
        auto v = m->add_var(prefix + sout->name + ".splitfrac", m->unitset->get_default_unit("frac"));
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
    for (const auto& split : x_split)
        J.push_back(m->add_J_NZ(eq, split));

    // Composition copy, spl1.Sin.massfrac_Cj - spl1.Si.massfrac_Cj == 0, for Si in outlet streams, Cj in Sin.comps.
    for (const auto& sout : outlets)
        for (const auto& c : sout->comps) {
            eq = m->add_constraint(prefix + sout->name + "." + c + "_massfrac_copy");
            g.push_back(eq);
            J.push_back(m->add_J_NZ(eq, x_strm[sin].massfrac.at(c)));
            J.push_back(m->add_J_NZ(eq, x_strm[sout].massfrac.at(c)));
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
        double free_frac = sum_fixed / static_cast<double>(n_free);
        for (const auto& frac : x_split)
            if (frac->is_free())
                frac->convert_and_set(free_frac);
    }
    
    // Calculate total mass flow rate of inlet stream.
    double base_val {0.0};
    for (const auto& c : sin->comps)
        base_val += *x_strm[sin].mass[c];
    x_strm[sin].total_mass->convert_and_set(base_val);

    // Calculate outlet stream total mass flow rates.
    for (int i = 0; const auto& sout : outlets)
        x_strm[sout].total_mass->convert_and_set(*x_strm[sin].total_mass * *x_split[i++]);

    // Calculate mass fractions in inlet stream.
    for (const auto& c : sin->comps)
        x_strm[sin].massfrac[c]->convert_and_set(*x_strm[sin].mass[c] / *x_strm[sin].total_mass);

    // Set mass fractions of outlet streams, and calculate outlet stream component mass flow rates.
    for (const auto& sout : outlets)
        for (const auto& c : sout->comps) {
            auto mf = *x_strm[sin].massfrac[c];
            x_strm[sout].massfrac[c]->convert_and_set(mf);
            x_strm[sout].mass[c]->convert_and_set(mf * *x_strm[sout].total_mass);
        }

}

//---------------------------------------------------------

void Splitter::eval_constraints()
{
    const auto& sin = inlets[0];
    auto ic = 0;

    // Total mass flow definition for inlet stream, \sum_{Cj in comps}(spl1.Sin.mass_Cj) - spl1.Sin.mass == 0.
    *g[ic] = 0.0;
    for (const auto& c : sin->comps)
        *g[ic] += *x_strm[sin].mass[c];
    *g[ic++] -= *x_strm[sin].total_mass;

    // Mass fraction definitions, (spl1.Si.mass * spl1.Si.massfrac_Cj) - spl1.Si.mass_Cj == 0
    //    for Si in streams, Cj in comps.
    for (const auto& c : sin->comps)
        *g[ic++] = *x_strm[sin].total_mass * *x_strm[sin].massfrac[c] - *x_strm[sin].mass[c];
    for (const auto& sout : outlets)
        for (const auto& c : sout->comps)
            *g[ic++] = *x_strm[sout].total_mass * *x_strm[sout].massfrac[c] - *x_strm[sout].mass[c];

    // Split fraction definitions, (spl1.Sin.mass * spl1.Si.splitfrac) - spl1.Si.mass == 0
    //    for Si in outlet streams.
    for (int i = 0; const auto& sout : outlets)
        *g[ic++] = *x_strm[sin].total_mass * *x_split[i++] - *x_strm[sout].total_mass;

    // Split fractions sum to 1, Sum(spl1.Si.splitfrac) - 1 ==0, for Si in outlet streams.
    *g[ic] = 0.0;
    for (const auto& split : x_split)
        *g[ic] += *split;
    *g[ic++] -= 1.0;

    // Composition copy, spl1.Sin.massfrac_Cj - spl1.Si.massfrac_Cj == 0, for Si in outlet streams, Cj in Sin.comps.
    for (const auto& sout : outlets)
        for (const auto& c : sout->comps)
            *g[ic++] = *x_strm[sin].massfrac[c] - *x_strm[sout].massfrac[c];
        
}

//---------------------------------------------------------

void Splitter::eval_jacobian() {
    const auto& sin = inlets[0];
    auto ic = 0;

    // Total mass flow definition for inlet stream, \sum_{Cj in comps}(spl1.Sin.mass_Cj) - spl1.Sin.mass == 0.
    for (size_t i = 0; i < sin->comps.size(); ++i)
        *J[ic++] = 1.0;
    *J[ic++] = -1.0;

    // Mass fraction definitions, (spl1.Si.mass * spl1.Si.massfrac_Cj) - spl1.Si.mass_Cj == 0
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

    // Split fraction definitions, (spl1.Sin.mass * spl1.Si.splitfrac) - spl1.Si.mass == 0
    //    for Si in outlet streams.
    for (int i = 0; i < outlets.size(); i++) {
        *J[ic++] = *x_split[i];
        *J[ic++] = *x_strm[sin].total_mass;
        *J[ic++] = -1.0;
    }

    // Split fractions sum to 1, Sum(spl1.Si.splitfrac) - 1 ==0, for Si in outlet streams.
    for (int i = 0; i < x_split.size(); i++)
        *J[ic++] = 1.0;

    // Composition copy, spl1.Sin.massfrac_Cj - spl1.Si.massfrac_Cj == 0, for Si in outlet streams, Cj in Sin.comps.
    for (const auto& sout : outlets)
        for (size_t i = 0; i < sout->comps.size(); ++i) {
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
