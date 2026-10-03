#pragma once

#include "tool.h"

namespace classiCAD {

class EllipseTool final : public InteractionTool {
public:
    explicit EllipseTool(ToolId tool);

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
        MajorDiameter,
        MinorRadius,
        FocusSpacing,
    };

    EllipseMode mode() const;
    int constructionStage() const;
    int requiredPointCount() const;
    bool isRadiusMode() const;
    bool isEndpointsMode() const;
    bool isCornersMode() const;
    bool isFociMode() const;

    Point3D inputWorldPoint(const ToolInput &input,
                            const WorkPlaneFrame &frame,
                            const ToolContext &context) const;
    Point3D projectedCursorPoint(const ToolInput &input,
                                 const WorkPlaneFrame &frame,
                                 const ToolContext &context) const;
    Point3D resolveMajorPoint(const ToolInput &input,
                              const ToolContext &context) const;
    void updateFromInput(const ToolInput &input, ToolContext &context);
    void updateEllipseFrame(const Point3D &majorPoint,
                            ToolContext &context);
    WorkPlaneFrame makePerpendicularFrame(const Point3D &majorDirection,
                                          const ToolContext &context) const;
    void setFrame(const WorkPlaneFrame &frame, ToolContext &context);
    Point3D pointOnEllipse(const Point3D &point) const;
    bool ellipseDefinition(QVector<QPointF> *definition,
                           QPointF *center = nullptr) const;
    Shape makeEllipseShape(const QVector<QPointF> &definition,
                           const QPointF &center) const;
    void applyNumericInput(ToolContext &context);
    void beginNumericInput(NumericInput mode, const QString &initialText = {});
    void updateStatus(const ToolContext &context);
    void publish(ToolContext &context);
    void finish(ToolContext &context);
    void reset();

    ToolId tool_ = ToolId::Ellipse;
    QVector<Point3D> points_;
    WorkPlaneFrame referenceFrame_;
    WorkPlaneFrame drawingFrame_;
    WorkPlaneFrame frameBeforeLock_;
    ToolInput lastInput_;
    Point3D cursorWorld_;
    Point3D majorAxisWorld_;
    Point3D initialXAxis_;
    Point3D initialYAxis_;
    qreal majorRadius_ = 0.0;
    qreal minorRadius_ = 0.0;
    QString numericText_;
    NumericInput numericInput_ = NumericInput::None;
    int verticalOverrideAxis_ = 0;
    int axisConstraintKey_ = 0;
    bool hasCursorPoint_ = false;
    bool frameCaptured_ = false;
    bool planeLocked_ = false;
    bool perpendicularMode_ = false;
    bool keepFoci_ = false;
    bool wasVertical_ = false;
    bool numericPointLocked_ = false;
    ToolStatus status_;
    QString hudDimensionsLine_;
    QString hudInstructionsLine_;
};

} // namespace classiCAD
