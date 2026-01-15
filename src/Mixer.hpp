#pragma once
#include "Model.hpp"

class Mixer final : public Block
{
public:
    Mixer() = default;
    Mixer(string_view       name_,
          Flowsheet*        fs_,
          vector<Stream*>&& inlets_ ,
          vector<Stream*>&& outlets_);

    void initialize()       override;
    void eval_constraints() override;
    void eval_jacobian()    override;
    void eval_hessian()     override;

};