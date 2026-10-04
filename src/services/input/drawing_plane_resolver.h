#pragma once

#include "core/geometry/work_plane.h"
#include "core/tool_id.h"
#include "services/viewport/viewport_transform.h"

namespace classiCAD {

enum class DrawingPlaneResolutionKind {
    None,
    HoveredObject,
    EdgeOnSelectionDrag,
    PrincipalFallback,
    CameraFacingLineFallback,
};

struct DrawingPlaneResolutionRequest {
    ToolId activeTool = ToolId::Select;
    bool drawingShape = false;
    bool selectingObject = false;
    bool controlPointsVisible = false;
    bool hasHoveredPlane = false;
    bool hoveredPlaneUsable = true;
    WorkPlaneFrame hoveredPlane;
    bool perspectiveEnabled = false;
    ViewportViewPreset viewPreset = ViewportViewPreset::Perspective;
    Point3D viewDirection{0.0, 0.0, -1.0};
    Point3D viewUp{0.0, 1.0, 0.0};
    Point3D viewTarget;
};

struct DrawingPlaneResolution {
    DrawingPlaneResolutionKind kind = DrawingPlaneResolutionKind::None;
    WorkPlane plane = WorkPlane::XY;
    WorkPlaneFrame frame;

    bool hasFrame() const
    {
        return kind != DrawingPlaneResolutionKind::None;
    }

    bool usesPrincipalPlane() const
    {
        return kind == DrawingPlaneResolutionKind::PrincipalFallback;
    }
};

// Resolves the shared planar drawing contract independently of the widget's
// mouse events. The viewport supplies hover hits and applies the returned
// frame to ViewportTransform.
class DrawingPlaneResolver final {
public:
    static DrawingPlaneResolution resolve(
        const DrawingPlaneResolutionRequest &request);

private:
    static WorkPlaneFrame viewAlignedFrame(
        const DrawingPlaneResolutionRequest &request);
};

} // namespace classiCAD
