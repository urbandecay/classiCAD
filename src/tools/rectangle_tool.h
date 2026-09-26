#pragma once

#include "shape_creation_tool.h"

namespace classiCAD {

class RectangleTool final : public ShapeCreationTool {
public:
    explicit RectangleTool(ToolId tool = ToolId::Rectangle);

protected:
    bool buildShape(const ToolContext &context, Shape *shape) const override;
};

} // namespace classiCAD
