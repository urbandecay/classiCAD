#pragma once

#include "shape_creation_tool.h"

namespace classiCAD {

class EllipseTool final : public ShapeCreationTool {
public:
    explicit EllipseTool(ToolId tool);

protected:
    bool buildShape(const ToolContext &context, Shape *shape) const override;
};

} // namespace classiCAD
