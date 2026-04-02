#include <stdexcept>
#include "MultiYieldReactor.hpp"

MultiYieldReactor::MultiYieldReactor(string_view           name_,
                                     Flowsheet*            fs_,
                                     vector<Stream*>&&     inlets_,
                                     vector<Stream*>&&     outlets_,
                                     const string&         reactor_name,
                                     const vector<string>& feed_names_
                                    ) :
                                Block(name_,
                                      fs_,
                                      BlockType::MultiYieldReactor,
                                      std::move(inlets_),
                                      std::move(outlets_)),
                                feed_names {feed_names_}
{
    if (inlets.size() != outlets.size())
        throw std::invalid_argument("number of inlet streams must equal the number of outlet streams");

    if (feed_names.size() != inlets.size())
        throw std::invalid_argument("number of inlet streams must equal the number of feeds");

    const auto m = fs->m;
    const auto n_feeds = feed_names.size();

    // Total mass flow definition for inlet and outlet streams, \sum_{Cj in comps}(rx1.Si.mass_Cj) - rx1.Si.mass == 0,
    //     for Si in inlet and outlet streams, Cj in comps.
    for (const auto& sin : inlets) {
        auto eq = m->add_constraint(prefix + sin->name + "_total_mass_def");
        g.push_back(eq);
        for (const auto& c : sin->comps)
            J.push_back(m->add_J_NZ(eq, x_strm[sin].mass.at(c)));
        J.push_back(m->add_J_NZ(eq, x_strm[sin].total_mass));
    }
    for (const auto& sout : outlets) {
        auto eq = m->add_constraint(prefix + sout->name + "_total_mass_def");
        g.push_back(eq);
        for (const auto& c : sout->comps)
            J.push_back(m->add_J_NZ(eq, x_strm[sout].mass.at(c)));
        J.push_back(m->add_J_NZ(eq, x_strm[sout].total_mass));
    }

    // Mass fraction definitions, (rx1.Si.mass * rx1.Si.massfrac_Cj) - rx1.Si.mass_Cj == 0,
    //    for Si in inlet and outlet streams, Cj in comps.
    for (const auto& sin : inlets)
        for (const auto& c : sin->comps) {
            auto eq = m->add_constraint(prefix + sin->name + "." + c + "_massfrac_def");
            g.push_back(eq);
            J.push_back(m->add_J_NZ(eq, x_strm[sin].total_mass));
            J.push_back(m->add_J_NZ(eq, x_strm[sin].massfrac.at(c)));
            J.push_back(m->add_J_NZ(eq, x_strm[sin].mass.at(c)));
            H.push_back(m->add_H_NZ(eq, x_strm[sin].total_mass, x_strm[sin].massfrac.at(c)));
        }

    for (const auto& sout : outlets)
        for (const auto& c : sout->comps) {
            auto eq = m->add_constraint(prefix + sout->name + "." + c + "_massfrac_def");
            g.push_back(eq);
            J.push_back(m->add_J_NZ(eq, x_strm[sout].total_mass));
            J.push_back(m->add_J_NZ(eq, x_strm[sout].massfrac.at(c)));
            J.push_back(m->add_J_NZ(eq, x_strm[sout].mass.at(c)));
            H.push_back(m->add_H_NZ(eq, x_strm[sout].total_mass, x_strm[sout].massfrac.at(c)));
        }

    // Make the yield variables, e.g.,
    //    rx1.feed1_y_cOut_from_cIn for cIn in inlet.comps, cOut in outlet.comps
    // yields[feed_name][cIn][cOut] is the yield to outlet component cOut from inlet component cIn in feed feed_name.
    // For each feed, all the yields are fixed except one. The free yield is yields[feed_name][cIn][cIn] if cIn is in the
    //    outlet component list, otherwise the yield to the last outlet component is free.
    for (size_t i = 0; i < n_feeds; i++) {
        auto& yf = yields[feed_names[i]];
        for (const auto& c_in : inlets[i]->comps) {
            auto& y = yf[c_in];
            for (const auto& c_out : outlets[i]->comps) {
                auto v = m->add_var(prefix + feed_names[i] + "_y_" + c_out + "_from_" + c_in, m->unitset->get_default_unit("frac"));
                x.push_back(v);
                y[c_out] = v;
                v->fix();
            }
            y.contains(c_in) ? y[c_in]->free() : y[outlets[i]->comps.back()]->free();
        }
    }

    // Outlet component generation equations,
    //   e.g., sum(rx1.feed1.mass_cIn * rx1.feed1_y_cOut_from_cIn for cIn in inlet.comps) - rx1.out1.mass_cOut == 0
    for (size_t i = 0; i < n_feeds; i++) {
        for (const auto& c_out : outlets[i]->comps) {
            auto eq = m->add_constraint(prefix + feed_names[i] + "_" + c_out + "_generation");
            g.push_back(eq);
            for (const auto& c_in : inlets[i]->comps) {
                J.push_back(m->add_J_NZ(eq, x_strm[inlets[i]].mass.at(c_in)));
                J.push_back(m->add_J_NZ(eq, yields[feed_names[i]][c_in][c_out]));
                H.push_back(m->add_H_NZ(eq, x_strm[inlets[i]].mass.at(c_in), yields[feed_names[i]][c_in][c_out]));
            }
            J.push_back(m->add_J_NZ(eq, x_strm[outlets[i]].mass.at(c_out)));
        }
    }

    // Equations to sum the yields to one for each inlet component,
    //   e.g., sum(rx1.feed1_y_cOut_from_cIn for cIn in inlet1.comps) - 1 == 0
    for (size_t i = 0; i < n_feeds; i++) {
        for (const auto& c_in : inlets[i]->comps) {
            auto eq = m->add_constraint(prefix + feed_names[i] + "_" + c_in + "_yield_sum");
            g.push_back(eq);
            for (const auto& c_out : outlets[i]->comps)
                J.push_back(m->add_J_NZ(eq, yields[feed_names[i]][c_in][c_out]));
        }
    }
    
    // Total feed mass flow rate and an equation to calculate it.
    //    e.g., sum(rx1.Si.mass for Si in inlet streams) - rx1.total_feed_mass == 0
    auto u_massflow = m->unitset->get_default_unit("massflow");
    x.push_back(total_feed_mass = m->add_var(prefix + "total_feed_mass", u_massflow));
    auto eq = m->add_constraint(prefix + "total_feed_mass_calc");
    g.push_back(eq);
    for (size_t i = 0; i < n_feeds; i++)
        J.push_back(m->add_J_NZ(eq, x_strm[inlets[i]].total_mass));
    J.push_back(m->add_J_NZ(eq, total_feed_mass));

    // Make the n_*_rx and feed_rate variables and the equations relating them.
    //   e.g., rx1.n_feed1_rx * rx1.feed1_rate - rx1.in1.mass == 0
    auto u_count = m->unitset->get_default_unit("count");
    n_rx.resize(n_feeds);
    feed_rates.resize(n_feeds);
    for (size_t i = 0; i < n_feeds; i++) {
        x.push_back(n_rx[i] = m->add_var(prefix + feed_names[i] + "_n_" + reactor_name, u_count));
        x.push_back(feed_rates[i] = m->add_var(prefix + feed_names[i] + "_feed_rate", u_massflow));
        feed_rates[i]->fix();
        eq = m->add_constraint(prefix + feed_names[i] + "_total_mass_calc");
        g.push_back(eq);
        J.push_back(m->add_J_NZ(eq, n_rx[i]));
        J.push_back(m->add_J_NZ(eq, feed_rates[i]));
        J.push_back(m->add_J_NZ(eq, x_strm[inlets[i]].total_mass));
        H.push_back(m->add_H_NZ(eq, n_rx[i], feed_rates[i]));
    }

    // Make a variable and equation for totaling n_feedi_reactors,
    //   e.g., sum(rx1.n_feedi_reactor) - rx1.n_reactor == 0
    n_total_rx = m->add_var(prefix + "n_" + reactor_name, u_count);
    eq = m->add_constraint(prefix + "total_" + reactor_name + "_calc");
    g.push_back(eq);
    for (size_t i = 0; i < n_feeds; i++)
        J.push_back(m->add_J_NZ(eq, n_rx[i]));
    J.push_back(m->add_J_NZ(eq, n_total_rx));

}

//---------------------------------------------------------

void MultiYieldReactor::initialize() {
    const auto n_feeds = inlets.size();

    // Calculate total mass flow rates of the inlet streams.
    double mf_total_base = 0.0;
    for (const auto& sin : inlets) {
        double mf {0.0};
        for (const auto& c : sin->comps)
            mf += *x_strm[sin].mass[c];
        mf_total_base += mf;
        x_strm[sin].total_mass->convert_and_set(mf);
    }
    total_feed_mass->convert_and_set(mf_total_base);

    // Calculate mass fractions in inlet streams.
    for (const auto& sin : inlets)
        for (const auto& c : sin->comps)
            x_strm[sin].massfrac[c]->convert_and_set(*x_strm[sin].mass[c] / *x_strm[sin].total_mass);
    
    // Calculate values of free yields.
    for (size_t i = 0; i < n_feeds; i++) {
        auto& yf = yields[feed_names[i]];
        for (const auto& c_in : inlets[i]->comps) {
            auto& y = yf[c_in];
            double fixed_yields_sum {0.0};
            string free_yield_comp {};
            for (const auto& c_out : outlets[i]->comps) {
                if (y[c_out]->spec == VariableSpec::Fixed)
                    fixed_yields_sum += *y[c_out];
                else
                    free_yield_comp = c_out;
            }
            y[free_yield_comp]->convert_and_set(1.0 - fixed_yields_sum);
        }
    }

    // Calculate outlet stream component mass flow rates.
    for (size_t i = 0; i < n_feeds; i++) {
        auto& yf = yields[feed_names[i]];
        for (const auto& c_out : outlets[i]->comps) {
            double comp_sum {0.0};
            for (const auto& c_in : inlets[i]->comps)
                comp_sum += *x_strm[inlets[i]].mass.at(c_in) * *yf[c_in][c_out];
            x_strm[outlets[i]].mass.at(c_out)->convert_and_set(comp_sum);
        }
    }

    // Calculate total mass flow rates of the outlet streams.
    for (const auto& sout : outlets) {
        double out_mass {0.0};
        for (const auto& c_out : sout->comps)
            out_mass += *x_strm[sout].mass[c_out];
        x_strm[sout].total_mass->convert_and_set(out_mass);
    }

    // Calculate mass fractions in the outlet streams.
    for (const auto& sout : outlets)
        for (const auto& c_out : sout->comps)
            x_strm[sout].massfrac[c_out]->convert_and_set(*x_strm[sout].mass[c_out] / *x_strm[sout].total_mass);

    // Calculate the n_feedi_reactor variables.
    double n_total_rx_base = 0.0;
    for (size_t i = 0; i < n_feeds; i++) {
        double n_rx_i_base = *x_strm[inlets[i]].total_mass / *feed_rates[i];
        n_total_rx_base += n_rx_i_base;
        n_rx[i]->convert_and_set(n_rx_i_base);
    }
    n_total_rx->convert_and_set(n_total_rx_base);

}

//---------------------------------------------------------

void MultiYieldReactor::eval_constraints()
{
    auto ic = 0;
    const auto n_feeds = inlets.size();

    // Total mass flow definition for inlet and outlet streams, \sum_{Cj in comps}(rx1.Si.mass_Cj) - rx1.Si.mass == 0,
    //     for Si in streams, Cj in comps.
    for (const auto& sin : inlets) {
        *g[ic] = 0.0;
        for (const auto& c : sin->comps)
            *g[ic] += *x_strm[sin].mass.at(c);
        *g[ic++] -= *x_strm[sin].total_mass;
    }
    for (const auto& sout : outlets) {
        *g[ic] = 0.0;
        for (const auto& c : sout->comps)
            *g[ic] += *x_strm[sout].mass.at(c);
        *g[ic++] -= *x_strm[sout].total_mass;
    }

    // Mass fraction definitions, (rx1.Si.mass * rx1.Si.massfrac_Cj) - rx1.Si.mass_Cj == 0
    //    for Si in inlet and outlet streams, Cj in comps.
    for (const auto& sin : inlets)
        for (const auto& c : sin->comps)
            *g[ic++] = *x_strm[sin].total_mass * *x_strm[sin].massfrac[c] - *x_strm[sin].mass[c];
    for (const auto& sout : outlets)
        for (const auto& c : sout->comps)
            *g[ic++] = *x_strm[sout].total_mass * *x_strm[sout].massfrac[c] - *x_strm[sout].mass[c];

    // Outlet component generation equations,
    //   e.g., sum(rx1.feed1.mass_cIn * rx1.feed1_y_cOut_from_cIn for cIn in inlet.comps) - rx1.out1.mass_cOut == 0
    for (size_t i = 0; i < n_feeds; i++) {
        for (const auto& c_out : outlets[i]->comps) {
            *g[ic] = 0.0;
            for (const auto& c_in : inlets[i]->comps)
                *g[ic] += *x_strm[inlets[i]].mass.at(c_in) * *yields[feed_names[i]][c_in][c_out];
            *g[ic++] -= *x_strm[outlets[i]].mass.at(c_out);
        }
    }

    // Equations to sum the yields to one for each inlet component,
    //   e.g., sum(rx1.feed1_y_cOut_from_cIn for cIn in inlet1.comps) - 1 == 0
    for (size_t i = 0; i < n_feeds; i++) {
        for (const auto& c_in : inlets[i]->comps) {
            *g[ic] = 0.0;
            for (const auto& c_out : outlets[i]->comps)
                *g[ic] += *yields[feed_names[i]][c_in][c_out];
            *g[ic++] -= 1.0;
        }
    }

    // Equation to calculate the total feed mass flow rate,
    //    e.g., sum(rx1.Si.mass for Si in inlet streams) - rx1.total_feed_mass == 0
    *g[ic] = 0.0;
    for (size_t i = 0; i < n_feeds; i++)
        *g[ic] += *x_strm[inlets[i]].total_mass;
    *g[ic++] -= *total_feed_mass;

    // Equations relating the n_*_rx and feed_rate variables,
    //   e.g., rx1.n_feed1_rx * rx1.feed1_rate - rx1.in1.mass == 0
    for (size_t i = 0; i < n_feeds; i++)
        *g[ic++] = *n_rx[i] * *feed_rates[i] - *x_strm[inlets[i]].total_mass;

    // Equation for totaling n_feedi_reactors,
    //   e.g., sum(rx1.n_feedi_reactor) - rx1.n_reactor == 0
    *g[ic] = 0.0;
    for (size_t i = 0; i < n_feeds; i++)
        *g[ic] += *n_rx[i];
    *g[ic++] -= *n_total_rx;

}

//---------------------------------------------------------

void MultiYieldReactor::eval_jacobian() {
    auto ic = 0;
    const auto n_feeds = feed_names.size();

    // Total mass flow definition for inlet and outlet streams, \sum_{Cj in comps}(rx1.Si.mass_Cj) - rx1.Si.mass == 0,
    //     for Si in streams, Cj in comps.
    for (size_t j = 0; j < n_feeds; j++) {
        for (size_t i = 0; i < inlets[j]->comps.size(); i++)
            *J[ic++] = 1.0;
        *J[ic++] = -1.0;
    }
    for (size_t j = 0; j < n_feeds; j++) {
        for (size_t i = 0; i < outlets[j]->comps.size(); i++)
            *J[ic++] = 1.0;
        *J[ic++] = -1.0;
    }

    // Mass fraction definitions, (rx1.Si.mass * rx1.Si.massfrac_Cj) - rx1.Si.mass_Cj == 0
    //    for Si in inlet and outlet streams, Cj in comps.
    for (size_t j = 0; j < n_feeds; j++) {
        const auto sin = inlets[j];
        for (const auto& c : sin->comps) {
            *J[ic++] = *x_strm[sin].massfrac.at(c);
            *J[ic++] = *x_strm[sin].total_mass;
            *J[ic++] = -1.0;
        }
    }
    for (size_t j = 0; j < n_feeds; j++) {
        const auto sout = outlets[j];
        for (const auto& c : sout->comps) {
            *J[ic++] = *x_strm[sout].massfrac.at(c);
            *J[ic++] = *x_strm[sout].total_mass;
            *J[ic++] = -1.0;
        }
    }
    
    // Outlet component generation equations,
    //   e.g., sum(rx1.feed1.mass_cIn * rx1.feed1_y_cOut_from_cIn for cIn in inlet.comps) - rx1.out1.mass_cOut == 0
    for (size_t i = 0; i < n_feeds; i++) {
        for (const auto& c_out : outlets[i]->comps) {
            for (const auto& c_in : inlets[i]->comps) {
                *J[ic++] = *yields[feed_names[i]][c_in][c_out];
                *J[ic++] = *x_strm[inlets[i]].mass.at(c_in);
            }
            *J[ic++] = -1.0;
        }
    }

    // Equations to sum the yields to one for each inlet component,
    //   e.g., sum(rx1.feed1_y_cOut_from_cIn for cIn in inlet1.comps) - 1 == 0
    for (size_t i = 0; i < n_feeds; i++)
        for (size_t j = 0; j < inlets[i]->comps.size(); j++)
            for (size_t k = 0; k < outlets[i]->comps.size(); k++)
                *J[ic++] = 1.0;

    // Equation to calculate the total feed mass flow rate,
    //    e.g., sum(rx1.Si.mass for Si in inlet streams) - rx1.total_feed_mass == 0
    for (size_t i = 0; i < n_feeds; i++)
        *J[ic++] = 1.0;
    *J[ic++] = -1.0;

    // Equations relating the n_*_rx and feed_rate variables,
    //   e.g., rx1.n_feed1_rx * rx1.feed1_rate - rx1.in1.mass == 0
    for (size_t i = 0; i < n_feeds; i++) {
        *J[ic++] = *feed_rates[i];
        *J[ic++] = *n_rx[i];
        *J[ic++] = -1.0;
    }

    // Equation for totaling n_feedi_reactors,
    //   e.g., sum(n_feedi_reactor) - n_reactor == 0
    for (size_t i = 0; i < n_feeds; i++)
        *J[ic++] = 1.0;
    *J[ic++] = -1.0;
    
}

//---------------------------------------------------------

void MultiYieldReactor::eval_hessian() {
    auto ic = 0;
    const auto n_feeds = feed_names.size();

    // Mass fraction definitions, (rx1.Si.mass * rx1.Si.massfrac_Cj) - rx1.Si.mass_Cj == 0,
    //    for Si in inlet and outlet streams, Cj in comps.
    for (size_t i = 0; i < n_feeds; i++)
        for (size_t j = 0; j < inlets[i]->comps.size(); j++)
            *H[ic++] = 1.0;
    for (size_t i = 0; i < n_feeds; i++)
        for (size_t j = 0; j < outlets[i]->comps.size(); j++)
            *H[ic++] = 1.0;

     // Outlet component generation equations,
    //   e.g., sum(rx1.feed1.mass_cIn * rx1.feed1_y_cOut_from_cIn for cIn in inlet.comps) - rx1.out1.mass_cOut == 0
    for (size_t i = 0; i < n_feeds; i++)
        for (size_t j = 0; j < outlets[i]->comps.size(); j++)
            for (size_t k = 0; k < inlets[i]->comps.size(); k++)
                *H[ic++] = 1.0;

    // Equations relating the n_*_rx and feed_rate variables,
    //   e.g., rx1.n_feed1_rx * rx1.feed1_rate - rx1.in1.mass == 0
    for (size_t i = 0; i < n_feeds; i++)
        *H[ic++] = 1.0;

}
