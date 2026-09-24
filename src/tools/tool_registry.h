#pragma once

#include "tool.h"

#include <memory>
#include <vector>

namespace classiCAD {

class ToolRegistry {
public:
    ToolRegistry();

    void add(std::unique_ptr<InteractionTool> tool);
    InteractionTool *find(ToolId id) const;

private:
    std::vector<std::unique_ptr<InteractionTool>> tools_;
};

} // namespace classiCAD
