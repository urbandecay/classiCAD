#pragma once

#include "core/tool_id.h"
#include "services/sampling/curve_sample_data.h"

#include <QPainter>

namespace classiCAD {

class ViewportEraseOverlayRenderer final {
public:
    void drawCandidatePreview(QPainter &painter,
                              const QVector<EraseCurveSampleCache> &targetCurves,
                              int shapeIndex,
                              bool eraseStrokeHasPoints) const;
    void drawCursorPreview(QPainter &painter,
                           ToolId activeTool,
                           const QPointF &eraseCursorScreen,
                           bool cursorValid,
                           bool eraseCursorPressed,
                           int candidateCount) const;

private:
    void drawSampledIntervals(QPainter &painter,
                              const SampledNurbsCurve2D &sampled,
                              const QVector<ParameterInterval> &intervals) const;
};

} // namespace classiCAD
