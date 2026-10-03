#pragma once

#include "core/model.h"
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
    QLineF line;
    QColor color;
    bool dashed = false;
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
};

class InteractionTool {
public:
    virtual ~InteractionTool() = default;

    virtual ToolId id() const = 0;
    virtual void begin(ToolContext &context);
    virtual bool handleMousePress(const ToolInput &input, ToolContext &context);
    virtual bool handleMouseMove(const ToolInput &input, ToolContext &context);
    virtual bool handleMouseRelease(const ToolInput &input, ToolContext &context);
    virtual bool handleWheel(const ToolInput &input, ToolContext &context);
    virtual bool handleKey(const ToolInput &input, ToolContext &context);
    virtual void cancel(ToolContext &context);
    virtual void commit(ToolContext &context);
    virtual ToolPreview preview() const;
    virtual ToolStatus status() const;
};

} // namespace classiCAD
