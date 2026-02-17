#include <ranges>
#include <cassert>
#include "StoicReactor.hpp"

StoicReactor::StoicReactor(string_view                                  name_,
                           Flowsheet*                                   fs_,
                           vector<Stream*>&&                            inlets_,
                           vector<Stream*>&&                            outlets_,
                           const unordered_map<string, Quantity>&       mw_,
                           const vector<unordered_map<string, double>>& stoic_coef_,
                           const vector<string>&                        conversion_keys_
                          ) : 
                        Block(name_,
                              fs_,
                              std::move(inlets_),
                              std::move(outlets_)),
                        mw              {mw_},
                        stoic_coef      {stoic_coef_},
                        conversion_keys {conversion_keys_}
{
    const auto& sin = inlets[0];
    const auto& sout = outlets[0];
    const auto m = fs->m;

    n_rx = stoic_coef.size();
    assert(conversion_keys.size() == n_rx);
    extents.resize(n_rx);
    conversions.resize(n_rx);

    // Total mass flow definition for inlet and outlet streams, \sum_{Cj in comps}(arx.Si.mass_Cj) - arx.Si.mass == 0,
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

    // Mass fraction definitions, (arx.Si.mass * arx.Si.massfrac_Cj) - arx.Si.mass_Cj == 0,
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

    // Make sets containing the names of reacting and inert components, respectively.
    for (const auto& sc : stoic_coef)
        for (const auto& c : std::views::keys(sc))
            if (std::ranges::find(rx_comps, c) == rx_comps.end())
                rx_comps.push_back(c);
    for (const auto& c : sout->comps)
        if (std::ranges::find(rx_comps, c) == rx_comps.end())
            inert_comps.push_back(c);

    // Equations and variables to calculate the molar flow rates of the reacting components, in both
    //    inlet and outlet streams.
    //    e.g., arx.in.mass_c2h2 - arx.in.moles_c2h2 * mw["c2h2"] == 0
    auto u_moleflow = m->unit_set.get_default_unit("moleflow");
    for (const auto& c : sin->comps) {
        if (std::ranges::find(rx_comps, c) != rx_comps.end()) {
            x.push_back(inlet_moles[c] = m->add_var(prefix + sin->name + ".moles_" + c, u_moleflow));
            eq = m->add_constraint(prefix + sin->name + "." + c + "_moles_calc");
            g.push_back(eq);
            J.push_back(m->add_J_NZ(eq, x_strm[sin].mass.at(c)));
            J.push_back(m->add_J_NZ(eq, inlet_moles[c]));
        }
    }
    for (const auto& c : sout->comps) {
        if (std::ranges::find(rx_comps, c) != rx_comps.end()) {
            x.push_back(outlet_moles[c] = m->add_var(prefix + sout->name + ".moles_" + c, u_moleflow));
            eq = m->add_constraint(prefix + sout->name + "." + c + "_moles_calc");
            g.push_back(eq);
            J.push_back(m->add_J_NZ(eq, x_strm[sout].mass.at(c)));
            J.push_back(m->add_J_NZ(eq, outlet_moles[c]));
        }
    }
    
    // Equations and variables relating the extents of reaction and the reacting component molar flow rates.
    //    e.g., \sum_i(stoic_coef[i][Cj] * arx.extent_i) + arx.in.moles_Cj - arx.out.moles_Cj == 0 for i = 0, n_rx - 1,
    //              Cj in rx_comps.
    for (int i = 0; i < n_rx; i++)
        x.push_back(extents[i] = m->add_var(prefix + "extent_rx_" + std::to_string(i + 1), u_moleflow));
    for (const auto& c : rx_comps) {
        eq = m->add_constraint(prefix + c + "_balance");
        g.push_back(eq);
        for (int i = 0; i < n_rx; i++) {
            if (stoic_coef[i].contains(c))
                J.push_back(m->add_J_NZ(eq, extents[i]));
        }
        if (inlet_moles.contains(c)) J.push_back(m->add_J_NZ(eq, inlet_moles[c]));
        if (outlet_moles.contains(c)) J.push_back(m->add_J_NZ(eq, outlet_moles[c]));
    }

    // Equations defining a per-reaction conversion of some component in terms of the extent of that reaction,
    //    e.g., arx.in.moles_c2h2 * arx.conv_c2h2_rx_1 + stoic_coef[1]["c2h2"] * arx.extent_rx_1 == 0
    auto u_frac = m->unit_set.get_default_unit("frac");
    for (int i = 0; i < n_rx; i++) {
        auto c = conversion_keys[i];
        string prefix_ = prefix + "conv_" + c + "_rx_" + std::to_string(i + 1);
        x.push_back(conversions[i] = m->add_var(prefix_, u_frac));
        conversions[i]->fix();
        eq = m->add_constraint(prefix_ + "_def");
        g.push_back(eq);
        J.push_back(m->add_J_NZ(eq, inlet_moles[c]));
        J.push_back(m->add_J_NZ(eq, conversions[i]));
        J.push_back(m->add_J_NZ(eq, extents[i]));
        H.push_back(m->add_H_NZ(eq, inlet_moles[c], conversions[i]));
    }

    // For the inert components, copy the inlet component mass flow to the outlet,
    //    e.g., arx.out.mass_co2 - arx.in.mass_co2 == 0
    for (const auto& c : inert_comps) {
        eq = m->add_constraint(prefix + c + "_balance");
        g.push_back(eq);
        J.push_back(m->add_J_NZ(eq, x_strm[sout].mass[c]));
        J.push_back(m->add_J_NZ(eq, x_strm[sin].mass[c]));
    }

}

//---------------------------------------------------------

void StoicReactor::initialize() {
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

    // Calculate inlet stream component molar flow rates of the reacting components.
    for (const auto& c : sin->comps)
        if (std::ranges::find(rx_comps, c) != rx_comps.end())
            inlet_moles[c]->convert_and_set(*x_strm[sin].mass[c] / mw[c]);

    // Calculate outlet stream component mass flow rates of the inert components.
    for (const auto& c : inert_comps)
        x_strm[sout].mass.at(c)->convert_and_set(*x_strm[sin].mass.at(c));

    // Calculate the extents of reaction from the conversions.
    for (int i = 0; i < n_rx; i++) {
        auto c = conversion_keys[i];
        extents[i]->convert_and_set( -( *inlet_moles[c] * *conversions[i] ) / stoic_coef[i][c] );
    }

    // Calculate the outlet reacting component molar flow rates from the extents of reaction.
    //    e.g., \sum_i(stoic_coef[i][Cj] * arx.extent_i) + arx.in.moles_Cj - arx.out.moles_Cj == 0 for i = 0, n_rx - 1,
    //              Cj in rx_comps.
    for (const auto& c : rx_comps) {
        double mf = (inlet_moles.contains(c) ? *inlet_moles[c] : 0.0);
        for (int i = 0; i < n_rx; i++)
            mf += (stoic_coef[i].contains(c) ? stoic_coef[i][c] : 0.0) * *extents[i];
        if (outlet_moles.contains(c)) outlet_moles[c]->convert_and_set(mf);
    }

    // Calculate the outlet mass flow rates of the reacting components from the molar flow rates.
    //    e.g., arx.in.mass_c2h2 - arx.in.moles_c2h2 * mw["c2h2"] == 0
    for (const auto& c : sout->comps)
        if (std::ranges::find(rx_comps, c) != rx_comps.end())
            x_strm[sout].mass[c]->convert_and_set(*(outlet_moles.at(c)) * mw[c]);

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

void StoicReactor::eval_constraints()
{
    const auto& sin = inlets[0];
    const auto& sout = outlets[0];
    auto ic = 0;

    // Total mass flow definition for inlet and outlet streams, \sum_{Cj in comps}(arx1.Si.mass_Cj) - arx1.Si.mass == 0,
    //     for Si in streams, Cj in comps.
    *g[ic] = 0.0;
    for (const auto& c : sin->comps)
        *g[ic] += *x_strm[sin].mass.at(c);
    *g[ic++] -= *x_strm[sin].total_mass;

    *g[ic] = 0.0;
    for (const auto& c : sout->comps)
        *g[ic] += *x_strm[sout].mass.at(c);
    *g[ic++] -= *x_strm[sout].total_mass;

    // Mass fraction definitions, (arx1.Si.mass * arx1.Si.massfrac_Cj) - arx1.Si.mass_Cj == 0
    //    for Si in inlet and outlet streams, Cj in comps.
    for (const auto& c : sin->comps)
        *g[ic++] = *x_strm[sin].total_mass * *x_strm[sin].massfrac[c] - *x_strm[sin].mass[c];
    for (const auto& c : sout->comps)
        *g[ic++] = *x_strm[sout].total_mass * *x_strm[sout].massfrac[c] - *x_strm[sout].mass[c];

    // Equations to calculate the molar flow rates of the reacting components, in both
    //    inlet and outlet streams.
    //    e.g., arx.in.mass_c2h2 - arx.in.moles_c2h2 * mw["c2h2"] == 0
    for (const auto& c : sin->comps)
        if (std::ranges::find(rx_comps, c) != rx_comps.end())
            *g[ic++] = *x_strm[sin].mass.at(c) - *inlet_moles[c] * mw[c];
    for (const auto& c : sout->comps)
        if (std::ranges::find(rx_comps, c) != rx_comps.end())
            *g[ic++] = *x_strm[sout].mass.at(c) - *(outlet_moles.at(c)) * mw[c];

    // Equations relating the extents of reaction and the reacting component molar flow rates.
    //    e.g., \sum_i(stoic_coef[i][Cj] * arx.extent_i) + arx.in.moles_Cj - arx.out.moles_Cj == 0 for i = 0, n_rx - 1,
    //              Cj in rx_comps.
    for (const auto& c : rx_comps) {
        *g[ic] = 0.0;
        for (int i = 0; i < n_rx; i++)
            if (stoic_coef[i].contains(c))
                *g[ic] += stoic_coef[i][c] * *extents[i];
        if (inlet_moles.contains(c)) *g[ic] += *inlet_moles[c];
        if (outlet_moles.contains(c)) *g[ic] -= *outlet_moles[c];
        ic++;
    }

    // Equations relating the per-reaction conversion of some component to the extent of that reaction,
    //    e.g., arx.in.moles_c2h2 * arx.conv_c2h2_rx_1 + stoic_coef[1]["c2h2"] * arx.extent_rx_1 == 0
    for (int i = 0; i < n_rx; i++) {
        auto c = conversion_keys[i];
        *g[ic++] = *inlet_moles[c] * *conversions[i] + stoic_coef[i][c] * *extents[i];
    }

    // Inert components copy equations,
    //    e.g., arx.out.mass_co2 - arx.in.mass_co2 == 0
    for (const auto& c : inert_comps)
        *g[ic++] = *x_strm[sout].mass[c] - *x_strm[sin].mass[c];

}

//---------------------------------------------------------

void StoicReactor::eval_jacobian() {
    const auto& sin = inlets[0];
    const auto& sout = outlets[0];
    auto ic = 0;

    // Total mass flow definition for inlet and outlet streams, \sum_{Cj in comps}(arx.Si.mass_Cj) - arx.Si.mass == 0,
    //     for Si in streams, Cj in comps.
    for (size_t i = 0; i < sin->comps.size(); i++)
        *J[ic++] = 1.0;
    *J[ic++] = -1.0;
    for (size_t i = 0; i < sout->comps.size(); i++)
        *J[ic++] = 1.0;
    *J[ic++] = -1.0;

    // Mass fraction definitions, (sep1.Si.mass * arx.Si.massfrac_Cj) - arx.Si.mass_Cj == 0
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

    // Equations to calculate the molar flow rates of the reacting components, in both
    //    inlet and outlet streams.
    //    e.g., arx.in.mass_c2h2 - arx.in.moles_c2h2 * mw["c2h2"] == 0
    for (const auto& c : sin->comps) {
        if (std::ranges::find(rx_comps, c) != rx_comps.end()) {
            *J[ic++] = 1.0;
            *J[ic++] = -mw[c];
        }
    }
    for (const auto& c : sout->comps) {
        if (std::ranges::find(rx_comps, c) != rx_comps.end()) {
            *J[ic++] = 1.0;
            *J[ic++] = -mw[c];
        }
    }

    // Equations relating the extents of reaction and the reacting component molar flow rates.
    //    e.g., \sum_i(stoic_coef[i][Cj] * arx.extent_i) + arx.in.moles_Cj - arx.out.moles_Cj == 0 for i = 0, n_rx - 1,
    //              Cj in rx_comps.
    for (const auto& c : rx_comps) {
        for (int i = 0; i < n_rx; i++) {
            if (stoic_coef[i].contains(c))
                *J[ic++] = stoic_coef[i][c];
        }
        if (inlet_moles.contains(c)) *J[ic++] = 1.0;
        if (outlet_moles.contains(c)) *J[ic++] = -1.0;
    }

    // Equations relating the per-reaction conversion of some component to the extent of that reaction,
    //    e.g., arx.in.moles_c2h2 * arx.conv_c2h2_rx_1 + stoic_coef[1]["c2h2"] * arx.extent_rx_1 == 0
    for (int i = 0; i < n_rx; i++) {
        auto c = conversion_keys[i];
        *J[ic++] = *conversions[i];
        *J[ic++] = *inlet_moles[c];
        *J[ic++] = stoic_coef[i][c];
    }

    // Inert components copy equations,
    //    e.g., arx.out.mass_co2 - arx.in.mass_co2 == 0
    for (const auto& c : inert_comps) {
        *J[ic++] = 1.0;
        *J[ic++] = -1.0;
    }

}

//---------------------------------------------------------

void StoicReactor::eval_hessian() {
    const auto& sin = inlets[0];
    const auto& sout = outlets[0];
    auto ic = 0;

    // Mass fraction definitions, (arx.Si.mass * arx.Si.massfrac_Cj) - arx.Si.mass_Cj == 0,
    //    for Si in inlet and outlet streams, Cj in comps.
    for (size_t i = 0; i < sin->comps.size(); i++)
        *H[ic++] = 1.0;
    for (size_t i = 0; i < sout->comps.size(); i++)
        *H[ic++] = 1.0;

    // Equations defining a per-reaction conversion of some component in terms of the extent of that reaction,
    //    e.g., arx.in.moles_c2h2 * arx.conv_c2h2_rx_1 + stoic_coef[1]["c2h2"] * arx.extent_rx_1 == 0
    for (int i = 0; i < n_rx; i++)
        *H[ic++] = 1.0;

}
