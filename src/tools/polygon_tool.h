#pragma once

#include "shape_creation_tool.h"

namespace classiCAD {

class PolygonTool final : public ShapeCreationTool {
public:
    explicit PolygonTool(ToolId tool);

protected:
    bool buildShape(const ToolContext &context, Shape *shape) const override;
};

} // namespace classiCAD
