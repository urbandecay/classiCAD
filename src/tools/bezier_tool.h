#pragma once

#include "shape_creation_tool.h"

namespace classiCAD {

class BezierTool final : public ShapeCreationTool {
public:
    BezierTool();

protected:
    bool buildShape(const ToolContext &context, Shape *shape) const override;
};

} // namespace classiCAD
