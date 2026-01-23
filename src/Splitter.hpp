#pragma once
#include "Model.hpp"

class Splitter final : public Block
{
public:
    Splitter() = default;
    Splitter(string_view       name_,
             Flowsheet*        fs_,
             vector<Stream*>&& inlets_ ,
             vector<Stream*>&& outlets_);

    void initialize()       override;
    void eval_constraints() override;
    void eval_jacobian()    override;
    void eval_hessian()     override;

private:
    vector<Variable*> x_split;

};