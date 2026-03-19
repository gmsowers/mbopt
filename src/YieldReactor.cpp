#include "YieldReactor.hpp"

YieldReactor::YieldReactor(string_view       name_,
                           Flowsheet*        fs_,
                           vector<Stream*>&& inlets_,
                           vector<Stream*>&& outlets_) : 
                        Block(name_,
                              fs_,
                              BlockType::YieldReactor,
                              std::move(inlets_),
                              std::move(outlets_))
{
    const auto& sin = inlets[0];
    const auto& sout = outlets[0];
    const auto m = fs->m;

    // Total mass flow definition for inlet and outlet streams, \sum_{Cj in comps}(rx1.Si.mass_Cj) - rx1.Si.mass == 0,
    //     for Si in inlet and outlet streams, Cj in comps.
    auto eq = m->add_constraint(prefix + sin->name + "_total_mass_def");
    g.push_back(eq);
    for (const auto& c : sin->comps)
        J.push_back(m->add_J_NZ(eq, x_strm[sin].mass.at(c)));
    J.push_back(m->add_J_NZ(eq, x_strm[sin].total_mass));
    eq = m->add_constraint(prefix + sout->name + "_total_mass_def");
    g.push_back(eq);
    for (const auto& c : sout->comps)
        J.push_back(m->add_J_NZ(eq, x_strm[sout].mass.at(c)));
    J.push_back(m->add_J_NZ(eq, x_strm[sout].total_mass));

    // Mass fraction definitions, (rx1.Si.mass * rx1.Si.massfrac_Cj) - rx1.Si.mass_Cj == 0,
    //    for Si in inlet and outlet streams, Cj in comps.
    for (const auto& c : sin->comps) {
        eq = m->add_constraint(prefix + sin->name + "." + c + "_massfrac_def");
        g.push_back(eq);
        J.push_back(m->add_J_NZ(eq, x_strm[sin].total_mass));
        J.push_back(m->add_J_NZ(eq, x_strm[sin].massfrac.at(c)));
        J.push_back(m->add_J_NZ(eq, x_strm[sin].mass.at(c)));
        H.push_back(m->add_H_NZ(eq, x_strm[sin].total_mass, x_strm[sin].massfrac.at(c)));
    }
    for (const auto& c : sout->comps) {
        eq = m->add_constraint(prefix + sout->name + "." + c + "_massfrac_def");
        g.push_back(eq);
        J.push_back(m->add_J_NZ(eq, x_strm[sout].total_mass));
        J.push_back(m->add_J_NZ(eq, x_strm[sout].massfrac.at(c)));
        J.push_back(m->add_J_NZ(eq, x_strm[sout].mass.at(c)));
        H.push_back(m->add_H_NZ(eq, x_strm[sout].total_mass, x_strm[sout].massfrac.at(c)));
    }

    // Make the yield variables, e.g.,
    //    rx1.y_cOut_from_cIn for cIn in inlet.comps, cOut in outlet.comps
    // yields[cIn][cOut] is the yield to outlet component cOut from inlet component cIn.
    // All the yields are fixed at except one. The free yield is yields[cIn][cIn] if cIn is in the
    //    outlet component list, otherwise the yield to the last outlet component is free.
    for (const auto& c_in : sin->comps) {
        auto& y = yields[c_in];
        for (const auto& c_out : sout->comps) {
            auto v = m->add_var(prefix + "y_" + c_out + "_from_" + c_in, m->unit_set.get_default_unit("frac"));
            x.push_back(v);
            y[c_out] = v;
            v->fix();
        }
        y.contains(c_in) ? y[c_in]->free() : y[sout->comps.back()]->free();
    }

    // Outlet component generation equations,
    //   e.g., sum(rx1.in.mass_cIn * rx1.y_cOut_from_cIn for cIn in inlet.comps) - rx1.out.mass_cOut == 0
    for (const auto& c_out : sout->comps) {
        eq = m->add_constraint(prefix + c_out + "_generation");
        g.push_back(eq);
        for (const auto& c_in : sin->comps) {
            J.push_back(m->add_J_NZ(eq, x_strm[sin].mass.at(c_in)));
            J.push_back(m->add_J_NZ(eq, yields[c_in][c_out]));
            H.push_back(m->add_H_NZ(eq, x_strm[sin].mass.at(c_in), yields[c_in][c_out]));
        }
        J.push_back(m->add_J_NZ(eq, x_strm[sout].mass.at(c_out)));
    }

    // Equations to sum the yields to one for each inlet component,
    //   e.g., sum(rx1.y_cOut_from_cIn for cIn in inlet.comps) - 1 == 0
    for (const auto& c_in : sin->comps) {
        eq = m->add_constraint(prefix + c_in + "_yield_sum");
        g.push_back(eq);
        for (const auto& c_out : sout->comps)
            J.push_back(m->add_J_NZ(eq, yields[c_in][c_out]));
    }

}

//---------------------------------------------------------

void YieldReactor::initialize() {
    const auto& sin = inlets[0];
    const auto& sout = outlets[0];

    // Calculate total mass flow rate of inlet stream.
    double mf {0.0};
    for (const auto& c : sin->comps)
        mf += *x_strm[sin].mass[c];
    x_strm[sin].total_mass->convert_and_set(mf);

    // Calculate mass fractions in inlet stream.
    for (const auto& c : sin->comps)
        x_strm[sin].massfrac[c]->convert_and_set(*x_strm[sin].mass[c] / *x_strm[sin].total_mass);

    // Calculate values of free yields.
    for (const auto& c_in : sin->comps) {
        auto& y = yields[c_in];
        double fixed_yields_sum {0.0};
        string free_yield_comp {};
        for (const auto& c_out : sout->comps) {
            if (y[c_out]->spec == VariableSpec::Fixed)
                fixed_yields_sum += *y[c_out];
            else
                free_yield_comp = c_out;
        }
        y[free_yield_comp]->convert_and_set(1.0 - fixed_yields_sum);
    }

    // Calculate outlet stream component mass flow rates.
    for (const auto& c_out : sout->comps) {
        double comp_sum {0.0};
        for (const auto& c_in : sin->comps)
            comp_sum += *x_strm[sin].mass.at(c_in) * *yields[c_in][c_out];
        x_strm[sout].mass.at(c_out)->convert_and_set(comp_sum);
    }

    // Calculate total mass flow rate of the outlet stream.
    double out_mass {0.0};
    for (const auto& c_out : sout->comps)
        out_mass += *x_strm[sout].mass[c_out];
    x_strm[sout].total_mass->convert_and_set(out_mass);

    // Calculate mass fractions in the outlet stream.
    for (const auto& c_out : sout->comps)
        x_strm[sout].massfrac[c_out]->convert_and_set(*x_strm[sout].mass[c_out] / *x_strm[sout].total_mass);

}

//---------------------------------------------------------

void YieldReactor::eval_constraints()
{
    const auto& sin = inlets[0];
    const auto& sout = outlets[0];
    auto ic = 0;

    // Total mass flow definition for inlet and outlet streams, \sum_{Cj in comps}(rx1.Si.mass_Cj) - rx1.Si.mass == 0,
    //     for Si in streams, Cj in comps.
    *g[ic] = 0.0;
    for (const auto& c : sin->comps)
        *g[ic] += *x_strm[sin].mass.at(c);
    *g[ic++] -= *x_strm[sin].total_mass;

    *g[ic] = 0.0;
    for (const auto& c : sout->comps)
        *g[ic] += *x_strm[sout].mass.at(c);
    *g[ic++] -= *x_strm[sout].total_mass;

    // Mass fraction definitions, (rx1.Si.mass * rx1.Si.massfrac_Cj) - rx1.Si.mass_Cj == 0
    //    for Si in inlet and outlet streams, Cj in comps.
    for (const auto& c : sin->comps)
        *g[ic++] = *x_strm[sin].total_mass * *x_strm[sin].massfrac[c] - *x_strm[sin].mass[c];
    for (const auto& c : sout->comps)
        *g[ic++] = *x_strm[sout].total_mass * *x_strm[sout].massfrac[c] - *x_strm[sout].mass[c];

    // Outlet component generation equations,
    //   e.g., sum(rx1.in.mass_cIn * rx1.y_cOut_from_cIn for cIn in inlet.comps) - rx1.out.mass_cOut == 0
    for (const auto& c_out : sout->comps) {
        *g[ic] = 0.0;
        for (const auto& c_in : sin->comps)
            *g[ic] += *x_strm[sin].mass.at(c_in) * *yields[c_in][c_out];
        *g[ic++] -= *x_strm[sout].mass.at(c_out);
    }

    // Equations to sum the yields to one for each inlet component,
    //   e.g., sum(rx1.y_cOut_from_cIn for cIn in inlet.comps) - 1 == 0
    for (const auto& c_in : sin->comps) {
        *g[ic] = 0.0;
        for (const auto& c_out : sout->comps)
            *g[ic] += *yields[c_in][c_out];
        *g[ic++] -= 1.0;
    }

}

//---------------------------------------------------------

void YieldReactor::eval_jacobian() {
    const auto& sin = inlets[0];
    const auto& sout = outlets[0];
    auto ic = 0;

    // Total mass flow definition for inlet and outlet streams, \sum_{Cj in comps}(rx1.Si.mass_Cj) - rx1.Si.mass == 0,
    //     for Si in streams, Cj in comps.
    for (size_t i = 0; i < sin->comps.size(); i++)
        *J[ic++] = 1.0;
    *J[ic++] = -1.0;
    for (size_t i = 0; i < sout->comps.size(); i++)
        *J[ic++] = 1.0;
    *J[ic++] = -1.0;

    // Mass fraction definitions, (rx1.Si.mass * rx1.Si.massfrac_Cj) - rx1.Si.mass_Cj == 0
    //    for Si in inlet and outlet streams, Cj in comps.
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

    // Outlet component generation equations,
    //   e.g., sum(rx1.in.mass_cIn * rx1.y_cOut_from_cIn for cIn in inlet.comps) - rx1.out.mass_cOut == 0
    for (const auto& c_out : sout->comps) {
        for (const auto& c_in : sin->comps) {
            *J[ic++] = *yields[c_in][c_out];
            *J[ic++] = *x_strm[sin].mass.at(c_in);
        }
        *J[ic++] = -1.0;
    }

    // Equations to sum the yields to one for each inlet component,
    //   e.g., sum(rx1.y_cOut_from_cIn for cIn in inlet.comps) - 1 == 0
    for (size_t i = 0; i < sin->comps.size(); i++)
        for (size_t j = 0; j < sout->comps.size(); j++)
            *J[ic++] = 1.0;

}

//---------------------------------------------------------

void YieldReactor::eval_hessian() {
    const auto& sin = inlets[0];
    const auto& sout = outlets[0];
    auto ic = 0;

    // Mass fraction definitions, (rx1.Si.mass * rx1.Si.massfrac_Cj) - rx1.Si.mass_Cj == 0,
    //    for Si in inlet and outlet streams, Cj in comps.
    for (size_t i = 0; i < sin->comps.size(); i++)
        *H[ic++] = 1.0;
    for (size_t i = 0; i < sout->comps.size(); i++)
        *H[ic++] = 1.0;
    
    // Outlet component generation equations,
    //   e.g., sum(rx1.in.mass_cIn * rx1.y_cOut_from_cIn for cIn in inlet.comps) - rx1.out.mass_cOut == 0
    for (size_t i = 0; i < sout->comps.size(); i++)
        for (size_t j = 0; j < sin->comps.size(); j++)
            *H[ic++] = 1.0;

}
