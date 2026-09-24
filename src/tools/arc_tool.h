#pragma once

#include "shape_creation_tool.h"

namespace classiCAD {

class ArcTool final : public ShapeCreationTool {
public:
    ArcTool();

protected:
    bool buildShape(const ToolContext &context, Shape *shape) const override;
};

} // namespace classiCAD
