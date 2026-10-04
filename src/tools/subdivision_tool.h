#pragma once

#include "core/document/object_id.h"
#include "core/document/shape.h"
#include "tool_input.h"

#include <QString>
#include <QVector>

namespace classiCAD {

class ToolContext;

struct SubdivisionCommitResult {
    bool committed = false;
    bool changed = false;
};

struct SubdivisionWheelResult {
    bool handled = false;
    bool sectionsChanged = false;
    int logicalSteps = 0;
};

enum class SubdivisionInputAction {
    Unhandled,
    Apply,
    Cancel,
};

class SubdivisionTool final {
public:
    void begin(ObjectId targetObjectId,
               int initialSections,
               int maximumSections);
    void finish();
    void resetWheelTracking();
    int wheelStepsFromEvent(int angleDelta, int pixelDelta);
    SubdivisionWheelResult handleWheel(const ToolInput &input,
                                       int maximumSections,
                                       ToolContext &context);
    SubdivisionInputAction handleMousePress(const ToolInput &input) const;
    SubdivisionInputAction handleKey(int key) const;
    bool adjustSections(int steps, int maximumSections);
    void refreshPreview(const Shape &shape);
    const QVector<double> &previewParameters() const;
    SubdivisionCommitResult commit(ObjectId targetObjectId,
                                   int sections,
                                   const QVector<double> &parameters,
                                   ToolContext &context);
    QString prompt() const;
    bool isActive() const;
    ObjectId targetObjectId() const;
    int sections() const;
    int wheelAngleAccumulator() const;
    qreal wheelPixelAccumulator() const;

private:
    bool active_ = false;
    ObjectId targetObjectId_ = ObjectId::invalid();
    int sections_ = 2;
    int wheelAngleAccumulator_ = 0;
    qreal wheelPixelAccumulator_ = 0.0;
    QVector<double> previewParameters_;
};

} // namespace classiCAD
