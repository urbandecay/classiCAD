#pragma once

#include "core/model.h"
#include "tool_input.h"

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

struct ToolPreview {
    QVector<QPointF> points;
    Shape shape;
    bool hasShape = false;
    bool cursorVisible = false;
    QString statusText;
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
