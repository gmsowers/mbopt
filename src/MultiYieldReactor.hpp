#pragma once
#include "Model.hpp"

class MultiYieldReactor final : public Block
{
public:
    MultiYieldReactor() = default;
    MultiYieldReactor(string_view           name_,
                      Flowsheet*            fs_,
                      vector<Stream*>&&     inlets_ ,
                      vector<Stream*>&&     outlets_,
                      const string&         reactor_name,
                      const vector<string>& feed_names_);

    void initialize()       override;
    void eval_constraints() override;
    void eval_jacobian()    override;
    void eval_hessian()     override;

private:
    vector<string>                             feed_names      {};
    unordered_map<string,
        unordered_map<string,
            unordered_map<string, Variable*>>> yields          {};
    Variable*                                  total_feed_mass {};
    vector<Variable*>                          n_rx            {};
    vector<Variable*>                          feed_rates      {};
    Variable*                                  n_total_rx      {};
};