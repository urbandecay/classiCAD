#pragma once

#include "shape_creation_tool.h"

namespace classiCAD {

class CircleTool final : public ShapeCreationTool {
public:
    explicit CircleTool(ToolId tool = ToolId::Circle);

protected:
    bool buildShape(const ToolContext &context, Shape *shape) const override;
};

} // namespace classiCAD
