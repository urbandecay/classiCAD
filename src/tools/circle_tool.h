#pragma once

#include "tool.h"

namespace classiCAD {

class CircleTool final : public InteractionTool {
public:
    explicit CircleTool(ToolId tool = ToolId::Circle);

    ToolId id() const override;
    void begin(ToolContext &context) override;
    bool handleMousePress(const ToolInput &input, ToolContext &context) override;
    bool handleMouseMove(const ToolInput &input, ToolContext &context) override;
    bool handleKey(const ToolInput &input, ToolContext &context) override;
    void cancel(ToolContext &context) override;
    ToolPreview preview() const override;
    ToolStatus status() const override;

private:
    enum class NumericInput {
        None,
        Radius,
        Diameter,
    };

    bool isOnePoint() const;
    bool isDiameter() const;
    bool isThreePoint() const;
    QPointF resolveCursor(const ToolInput &input) const;
    QPointF pointInDrawingFrame(const QPointF &point) const;
    QVector<QPointF> pointsInDrawingFrame(
        const QVector<QPointF> &points) const;
    void updatePerpendicularDrawingFrame(const QPointF &cursorPoint,
                                         const ToolContext &context);
    bool circleDefinition(const QVector<QPointF> &candidatePoints,
                          QVector<QPointF> *definition) const;
    Shape makeCircleShape(const QVector<QPointF> &definition) const;
    void applyNumericInput(ToolContext &context);
    void beginNumericInput(NumericInput mode, const QString &initialText = {});
    void updateStatus(const ToolContext &context);
    void publish(ToolContext &context);
    void finish(ToolContext &context);
    void reset();

    ToolId tool_ = ToolId::Circle;
    QVector<QPointF> points_;
    WorkPlaneFrame referenceFrame_;
    WorkPlaneFrame drawingFrame_;
    WorkPlaneFrame frameBeforeLock_;
    QPointF cursorPoint_;
    ToolInput lastInput_;
    QString numericText_;
    NumericInput numericInput_ = NumericInput::None;
    int axisConstraintKey_ = 0;
    int normalAxisLockKey_ = 0;
    bool hasCursorPoint_ = false;
    bool frameCaptured_ = false;
    bool planeLocked_ = false;
    bool perpendicularMode_ = false;
    bool threePointPointsInDrawingFrame_ = false;
    ToolStatus status_;
    QString hudDimensionsLine_;
    QString hudInstructionsLine_;
};

} // namespace classiCAD
