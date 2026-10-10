#pragma once

#include "core/document/shape.h"
#include "core/document/object_id.h"
#include "core/tool_id.h"
#include "tool_input.h"

#include <QColor>
#include <QLineF>
#include <QString>
#include <QVector>

namespace classiCAD {

class ToolContext;

enum class ToolLifecycleState {
    Inactive,
    Active,
    Completed,
    Cancelled,
};

struct ToolStatus {
    ToolLifecycleState state = ToolLifecycleState::Inactive;
    QString text;
    bool canCommit = false;
};

struct ToolPreviewGuide {
    ToolPreviewGuide() = default;
    ToolPreviewGuide(const QLineF &guideLine,
                     const QColor &guideColor,
                     bool isDashed = false,
                     const WorkPlaneFrame &guideFrame = {},
                     bool hasGuideFrame = false)
        : line(guideLine),
          color(guideColor),
          dashed(isDashed),
          workPlaneFrame(guideFrame),
          hasWorkPlaneFrame(hasGuideFrame)
    {
    }

    QLineF line;
    QColor color;
    bool dashed = false;
    WorkPlaneFrame workPlaneFrame;
    bool hasWorkPlaneFrame = false;
};

struct ToolPreview {
    QVector<QPointF> points;
    QVector<ToolPreviewGuide> guides;
    WorkPlaneFrame workPlaneFrame;
    bool hasWorkPlaneFrame = false;
    QPointF cursorPoint;
    QVector<Point3D> worldPoints;
    Point3D worldCursorPoint;
    QVector<Shape> shapes;
    QVector<ObjectId> hiddenObjectIds;
    QVector<ObjectId> completedExtrusionObjectIds;
    QVector<ObjectId> completedFaceExtrusionObjectIds;
    bool planeLocked = false;
    SnapResult snap;
    bool overridesSnap = false;
    Shape shape;
    bool hasShape = false;
    bool cursorVisible = false;
    bool hasCursorPoint = false;
    QString statusText;
    QString hudDimensionsLine;
    QString hudInstructionsLine;
    int activeStage = -1;
};

class InteractionTool {
public:
    enum class EventResult {
        Unhandled,
        Handled,
    };

    virtual ~InteractionTool() = default;

    virtual ToolId id() const = 0;
    virtual void begin(ToolContext &context);
    virtual bool handleMousePress(const ToolInput &input, ToolContext &context);
    virtual bool handleMouseMove(const ToolInput &input, ToolContext &context);
    virtual bool handleMouseRelease(const ToolInput &input, ToolContext &context);
    virtual bool handleWheel(const ToolInput &input, ToolContext &context);
    virtual bool handleKey(const ToolInput &input, ToolContext &context);

    // Explicit routing boundary for Qt adapters. The bool-based handlers stay
    // as the incremental migration contract for existing tools.
    virtual EventResult dispatchMousePress(const ToolInput &input,
                                           ToolContext &context);
    EventResult dispatchMouseMove(const ToolInput &input, ToolContext &context);
    EventResult dispatchWheel(const ToolInput &input, ToolContext &context);
    virtual EventResult dispatchKey(const ToolInput &input,
                                    ToolContext &context);

    virtual void cancel(ToolContext &context);
    virtual void commit(ToolContext &context);
    virtual ToolPreview preview() const;
    virtual ToolStatus status() const;
};

} // namespace classiCAD
