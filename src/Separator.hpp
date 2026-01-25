#pragma once
#include "Model.hpp"

class Separator final : public Block
{
public:
    Separator() = default;
    Separator(string_view       name_,
             Flowsheet*        fs_,
             vector<Stream*>&& inlets_ ,
             vector<Stream*>&& outlets_);

    void initialize()       override;
    void eval_constraints() override;
    void eval_jacobian()    override;
    void eval_hessian()     override;

private:
    vector<Variable*> x_split {};

};