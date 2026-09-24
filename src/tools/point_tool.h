#pragma once

#include "shape_creation_tool.h"

namespace classiCAD {

class PointTool final : public ShapeCreationTool {
public:
    PointTool();

protected:
    bool buildShape(const ToolContext &context, Shape *shape) const override;
};

} // namespace classiCAD
