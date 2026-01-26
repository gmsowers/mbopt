#pragma once
#include "Model.hpp"

class YieldReactor final : public Block
{
public:
    YieldReactor() = default;
    YieldReactor(string_view       name_,
             Flowsheet*        fs_,
             vector<Stream*>&& inlets_ ,
             vector<Stream*>&& outlets_);

    void initialize()       override;
    void eval_constraints() override;
    void eval_jacobian()    override;
    void eval_hessian()     override;

private:
    unordered_map<string, unordered_map<string, Variable*>> yields {};

};