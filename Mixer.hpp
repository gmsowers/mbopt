#pragma once
#include "Model.hpp"

class Mixer final : public Block
{
public:
    Mixer() = default;
    Mixer(const std::string&            name_,
          FlowsheetPtr                  fs_,
          const std::vector<StreamPtr>& inlets_ = {},
          const std::vector<StreamPtr>& outlets_ = {});

    void eval_constraints() override;
};