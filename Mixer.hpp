#pragma once
#include "Model.hpp"

class Mixer final : public Block
{
public:
    Mixer() = default;
    Mixer(const std::string&            name_,
          ModelPtr                      m_,
          FlowsheetPtr                  fs_,
          const std::vector<StreamPtr>& inlets_,
          StreamPtr                     outlet);

    void eval_constraints() override;
};