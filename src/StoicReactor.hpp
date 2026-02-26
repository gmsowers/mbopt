#pragma once
#include "Model.hpp"

class StoicReactor final : public Block
{
public:
    StoicReactor() = default;
    StoicReactor(string_view       name_,
                 Flowsheet*        fs_,
                 vector<Stream*>&& inlets_ ,
                 vector<Stream*>&& outlets_,
                 const unordered_map<string, Quantity>& mw_,
                 const vector<unordered_map<string, double>>& stoic_coef_,
                 const vector<string>& conversion_keys_);

    void initialize()       override;
    void eval_constraints() override;
    void eval_jacobian()    override;
    void eval_hessian()     override;

private:
    unordered_map<string, Quantity>       mw               {};
    vector<unordered_map<string, double>> stoic_coef       {};
    size_t                                n_rx             {0};
    vector<string>                        rx_comps         {},
                                          inert_comps      {};
    unordered_map<string, Variable*>      inlet_moles      {},
                                          outlet_moles     {};
    vector<Variable*>                     extents          {};
    vector<Variable*>                     conversions      {};
    vector<string>                        conversion_keys  {};
};