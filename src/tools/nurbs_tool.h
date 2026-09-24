#pragma once

#include "shape_creation_tool.h"

namespace classiCAD {

class NurbsTool final : public ShapeCreationTool {
public:
    NurbsTool();

protected:
    bool buildShape(const ToolContext &context, Shape *shape) const override;
};

} // namespace classiCAD
