#include "two_curve_line_tool.h"

#include "core/geometry/curve_evaluator.h"
#include "core/model.h"
#include "tool_context.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace classiCAD {
namespace {

qreal cross2D(const QPointF &first, const QPointF &second)
{
    return first.x() * second.y() - first.y() * second.x();
}

qreal length2D(const QPointF &vector)
{
    return std::hypot(vector.x(), vector.y());
}

QVector<Shape::NurbsCurve2D> shapeCurves(const Shape &shape)
{
    QVector<Shape::NurbsCurve2D> result;
    if (shape.geometryType == GeometryType::PolyCurve) {
        result = shape.components;
    } else if (validateNurbsCurve(shape.nurbs)) {
        result.append(shape.nurbs);
    }
    result.erase(std::remove_if(result.begin(), result.end(),
                                [](const Shape::NurbsCurve2D &curve) {
                                    return !validateNurbsCurve(curve);
                                }),
                 result.end());
    return result;
}

bool curveInFrame(const Shape &shape,
                  const Shape::NurbsCurve2D &curve,
                  const WorkPlaneFrame &frame,
                  Shape::NurbsCurve2D *mapped)
{
    if (mapped == nullptr) {
        return false;
    }
    const WorkPlaneFrame sourceFrame = shapeWorkPlaneFrame(shape);
    if (!isValidWorkPlaneFrame(sourceFrame) || !isValidWorkPlaneFrame(frame) ||
        std::abs(std::abs(sourceFrame.normal.x * frame.normal.x +
                          sourceFrame.normal.y * frame.normal.y +
                          sourceFrame.normal.z * frame.normal.z) - 1.0) > 1.0e-6) {
        return false;
    }
    const qreal planeOffset = signedDistanceFromWorkPlaneFrame(sourceFrame.origin,
                                                                frame);
    const qreal planeScale = std::max<qreal>(1.0,
        std::hypot(sourceFrame.origin.x, sourceFrame.origin.y));
    if (std::abs(planeOffset) > 1.0e-7 * planeScale) {
        return false;
    }
    *mapped = curve;
    for (int index = 0; index < curve.controlPoints.size(); ++index) {
        const Point3D world = workPlaneFramePointToWorld(curve.controlPoints[index],
                                                         sourceFrame);
        if (std::abs(signedDistanceFromWorkPlaneFrame(world, frame)) >
            1.0e-7 * planeScale) {
            return false;
        }
        mapped->controlPoints[index] = worldPointToWorkPlaneFrame(world, frame);
    }
    return true;
}

struct CurveSample {
    QPointF point;
    QPointF tangent;
    qreal parameter = 0.0;
};

QVector<CurveSample> sampleCurve(const Shape::NurbsCurve2D &curve)
{
    constexpr int divisions = 96;
    QVector<CurveSample> result;
    qreal start = 0.0;
    qreal end = 0.0;
    if (!nurbsParameterDomain(curve, &start, &end)) {
        return result;
    }
    result.reserve(divisions + 1);
    for (int index = 0; index <= divisions; ++index) {
        const qreal fraction = static_cast<qreal>(index) / divisions;
        const qreal parameter = start + (end - start) * fraction;
        CurveSample sample;
        sample.parameter = parameter;
        if (!evaluateNurbsPoint(curve, parameter, &sample.point) ||
            !evaluateNurbsDerivative(curve, parameter, &sample.tangent)) {
            return {};
        }
        const qreal magnitude = length2D(sample.tangent);
        if (magnitude <= 1.0e-10) {
            sample.tangent = {};
            if (index > 0 && index < divisions) {
                QPointF next;
                if (evaluateNurbsPoint(curve,
                                       start + (end - start) * (index + 1) / divisions,
                                       &next)) {
                    sample.tangent = next - result.last().point;
                }
            }
        }
        const qreal tangentLength = length2D(sample.tangent);
        if (tangentLength > 1.0e-10) {
            sample.tangent /= tangentLength;
        }
        result.append(sample);
    }
    return result;
}

qreal relationError(ToolId tool,
                    const QPointF &first,
                    const QPointF &firstTangent,
                    const QPointF &second,
                    const QPointF &secondTangent)
{
    const QPointF chord = second - first;
    const qreal chordLength = length2D(chord);
    if (chordLength <= 1.0e-9 || length2D(firstTangent) <= 1.0e-9 ||
        length2D(secondTangent) <= 1.0e-9) {
        return std::numeric_limits<qreal>::infinity();
    }
    const QPointF direction = chord / chordLength;
    if (tool == ToolId::TangentToTwoCurves) {
        return std::abs(cross2D(direction, firstTangent)) +
               std::abs(cross2D(direction, secondTangent));
    }
    return std::abs(QPointF::dotProduct(direction, firstTangent)) +
           std::abs(QPointF::dotProduct(direction, secondTangent));
}

bool relationResidual(ToolId tool,
                      const Shape::NurbsCurve2D &firstCurve,
                      const Shape::NurbsCurve2D &secondCurve,
                      qreal firstFraction,
                      qreal secondFraction,
                      std::array<qreal, 2> *residual,
                      QPointF *firstPoint = nullptr,
                      QPointF *secondPoint = nullptr)
{
    if (residual == nullptr) {
        return false;
    }
    qreal firstStart = 0.0;
    qreal firstEnd = 0.0;
    qreal secondStart = 0.0;
    qreal secondEnd = 0.0;
    if (!nurbsParameterDomain(firstCurve, &firstStart, &firstEnd) ||
        !nurbsParameterDomain(secondCurve, &secondStart, &secondEnd)) {
        return false;
    }
    QPointF first;
    QPointF second;
    QPointF firstTangent;
    QPointF secondTangent;
    if (!evaluateNurbsPoint(firstCurve,
                            firstStart + (firstEnd - firstStart) * firstFraction,
                            &first) ||
        !evaluateNurbsPoint(secondCurve,
                            secondStart + (secondEnd - secondStart) * secondFraction,
                            &second) ||
        !evaluateNurbsDerivative(
            firstCurve,
            firstStart + (firstEnd - firstStart) * firstFraction,
            &firstTangent) ||
        !evaluateNurbsDerivative(
            secondCurve,
            secondStart + (secondEnd - secondStart) * secondFraction,
            &secondTangent)) {
        return false;
    }
    const qreal chordLength = length2D(second - first);
    const qreal firstTangentLength = length2D(firstTangent);
    const qreal secondTangentLength = length2D(secondTangent);
    if (chordLength <= 1.0e-10 || firstTangentLength <= 1.0e-10 ||
        secondTangentLength <= 1.0e-10) {
        return false;
    }
    const QPointF direction = (second - first) / chordLength;
    firstTangent /= firstTangentLength;
    secondTangent /= secondTangentLength;
    if (tool == ToolId::TangentToTwoCurves) {
        (*residual)[0] = cross2D(direction, firstTangent);
        (*residual)[1] = cross2D(direction, secondTangent);
    } else {
        (*residual)[0] = QPointF::dotProduct(direction, firstTangent);
        (*residual)[1] = QPointF::dotProduct(direction, secondTangent);
    }
    if (firstPoint != nullptr) {
        *firstPoint = first;
    }
    if (secondPoint != nullptr) {
        *secondPoint = second;
    }
    return std::isfinite((*residual)[0]) && std::isfinite((*residual)[1]);
}

bool refineRelation(ToolId tool,
                    const Shape::NurbsCurve2D &firstCurve,
                    const Shape::NurbsCurve2D &secondCurve,
                    qreal *firstFraction,
                    qreal *secondFraction,
                    QPointF *firstPoint,
                    QPointF *secondPoint)
{
    if (firstFraction == nullptr || secondFraction == nullptr ||
        firstPoint == nullptr || secondPoint == nullptr) {
        return false;
    }
    constexpr qreal derivativeStep = 1.0e-4;
    std::array<qreal, 2> residual{};
    if (!relationResidual(tool, firstCurve, secondCurve,
                          *firstFraction, *secondFraction, &residual)) {
        return false;
    }
    qreal error = std::hypot(residual[0], residual[1]);
    for (int iteration = 0; iteration < 24 && error > 1.0e-8; ++iteration) {
        const qreal firstMinus = std::max<qreal>(0.0, *firstFraction - derivativeStep);
        const qreal firstPlus = std::min<qreal>(1.0, *firstFraction + derivativeStep);
        const qreal secondMinus = std::max<qreal>(0.0, *secondFraction - derivativeStep);
        const qreal secondPlus = std::min<qreal>(1.0, *secondFraction + derivativeStep);
        std::array<qreal, 2> firstLow{};
        std::array<qreal, 2> firstHigh{};
        std::array<qreal, 2> secondLow{};
        std::array<qreal, 2> secondHigh{};
        if (firstPlus - firstMinus <= 1.0e-12 ||
            secondPlus - secondMinus <= 1.0e-12 ||
            !relationResidual(tool, firstCurve, secondCurve,
                              firstMinus, *secondFraction, &firstLow) ||
            !relationResidual(tool, firstCurve, secondCurve,
                              firstPlus, *secondFraction, &firstHigh) ||
            !relationResidual(tool, firstCurve, secondCurve,
                              *firstFraction, secondMinus, &secondLow) ||
            !relationResidual(tool, firstCurve, secondCurve,
                              *firstFraction, secondPlus, &secondHigh)) {
            break;
        }
        const qreal j00 = (firstHigh[0] - firstLow[0]) /
                          (firstPlus - firstMinus);
        const qreal j10 = (firstHigh[1] - firstLow[1]) /
                          (firstPlus - firstMinus);
        const qreal j01 = (secondHigh[0] - secondLow[0]) /
                          (secondPlus - secondMinus);
        const qreal j11 = (secondHigh[1] - secondLow[1]) /
                          (secondPlus - secondMinus);
        const qreal determinant = j00 * j11 - j01 * j10;
        if (std::abs(determinant) <= 1.0e-10) {
            break;
        }
        qreal firstStep = (-residual[0] * j11 + j01 * residual[1]) /
                          determinant;
        qreal secondStep = (-j00 * residual[1] + j10 * residual[0]) /
                           determinant;
        const qreal largestStep = std::max(std::abs(firstStep),
                                           std::abs(secondStep));
        if (largestStep > 0.1) {
            const qreal scale = 0.1 / largestStep;
            firstStep *= scale;
            secondStep *= scale;
        }

        bool improved = false;
        for (qreal fraction = 1.0; fraction >= 0.125; fraction *= 0.5) {
            const qreal candidateFirst = std::clamp(
                *firstFraction + firstStep * fraction, 0.0, 1.0);
            const qreal candidateSecond = std::clamp(
                *secondFraction + secondStep * fraction, 0.0, 1.0);
            std::array<qreal, 2> candidateResidual{};
            if (!relationResidual(tool, firstCurve, secondCurve,
                                  candidateFirst, candidateSecond,
                                  &candidateResidual)) {
                continue;
            }
            const qreal candidateError = std::hypot(candidateResidual[0],
                                                    candidateResidual[1]);
            if (candidateError < error) {
                *firstFraction = candidateFirst;
                *secondFraction = candidateSecond;
                residual = candidateResidual;
                error = candidateError;
                improved = true;
                break;
            }
        }
        if (!improved) {
            break;
        }
    }
    const qreal tolerance = tool == ToolId::PerpendicularToTwoCurves
                                ? 0.001
                                : 0.005;
    if (error > tolerance ||
        !relationResidual(tool, firstCurve, secondCurve,
                          *firstFraction, *secondFraction, &residual,
                          firstPoint, secondPoint)) {
        return false;
    }
    return true;
}

bool findClosestRelation(ToolId tool,
                         const QVector<Shape::NurbsCurve2D> &firstCurves,
                         const QVector<Shape::NurbsCurve2D> &secondCurves,
                         const QPointF &cursor,
                         QPointF *firstPoint,
                         QPointF *secondPoint)
{
    if (firstPoint == nullptr || secondPoint == nullptr) {
        return false;
    }
    qreal bestCursorDistance = std::numeric_limits<qreal>::infinity();
    qreal bestError = std::numeric_limits<qreal>::infinity();
    qreal bestFirstFraction = 0.0;
    qreal bestSecondFraction = 0.0;
    const Shape::NurbsCurve2D *bestFirstCurve = nullptr;
    const Shape::NurbsCurve2D *bestSecondCurve = nullptr;
    for (const Shape::NurbsCurve2D &firstCurve : firstCurves) {
        const QVector<CurveSample> firstSamples = sampleCurve(firstCurve);
        for (const Shape::NurbsCurve2D &secondCurve : secondCurves) {
            const QVector<CurveSample> secondSamples = sampleCurve(secondCurve);
            qreal firstStart = 0.0;
            qreal firstEnd = 0.0;
            qreal secondStart = 0.0;
            qreal secondEnd = 0.0;
            if (!nurbsParameterDomain(firstCurve, &firstStart, &firstEnd) ||
                !nurbsParameterDomain(secondCurve, &secondStart, &secondEnd) ||
                firstEnd <= firstStart || secondEnd <= secondStart) {
                continue;
            }
            for (const CurveSample &first : firstSamples) {
                for (const CurveSample &second : secondSamples) {
                    const qreal error = relationError(tool,
                                                      first.point,
                                                      first.tangent,
                                                      second.point,
                                                      second.tangent);
                    if (!std::isfinite(error) || error > 0.12) {
                        continue;
                    }
                    const QPointF midpoint = (first.point + second.point) * 0.5;
                    const QPointF offset = midpoint - cursor;
                    const qreal cursorDistance = QPointF::dotProduct(offset, offset);
                    if (cursorDistance < bestCursorDistance - 1.0e-9 ||
                        (std::abs(cursorDistance - bestCursorDistance) <= 1.0e-9 &&
                         error < bestError)) {
                        bestError = error;
                        bestCursorDistance = cursorDistance;
                        bestFirstFraction = (first.parameter - firstStart) /
                                            (firstEnd - firstStart);
                        bestSecondFraction = (second.parameter - secondStart) /
                                             (secondEnd - secondStart);
                        bestFirstCurve = &firstCurve;
                        bestSecondCurve = &secondCurve;
                    }
                }
            }
        }
    }
    if (!std::isfinite(bestError) || bestFirstCurve == nullptr ||
        bestSecondCurve == nullptr) {
        return false;
    }
    return refineRelation(tool, *bestFirstCurve, *bestSecondCurve,
                          &bestFirstFraction, &bestSecondFraction,
                          firstPoint, secondPoint);
}

Shape lineShape(const QPointF &first,
                const QPointF &second,
                const WorkPlaneFrame &frame)
{
    Shape result;
    result.geometryType = GeometryType::Line;
    result.points = {first, second};
    result.nurbs = makeDegreeOneNurbs(result.points);
    result.workPlaneFrame = frame;
    result.workPlane = WorkPlane::XY;
    result.workPlaneOffset = 0.0;
    if (std::abs(frame.normal.z) >= 1.0 - 1.0e-8) {
        result.workPlaneOffset = frame.origin.z;
    } else if (std::abs(frame.normal.y) >= 1.0 - 1.0e-8) {
        result.workPlane = WorkPlane::XZ;
        result.workPlaneOffset = frame.origin.y;
    } else if (std::abs(frame.normal.x) >= 1.0 - 1.0e-8) {
        result.workPlane = WorkPlane::YZ;
        result.workPlaneOffset = frame.origin.x;
    }
    return result;
}

} // namespace

TwoCurveLineTool::TwoCurveLineTool(ToolId tool)
    : tool_(tool)
{
}

ToolId TwoCurveLineTool::id() const
{
    return tool_;
}

void TwoCurveLineTool::begin(ToolContext &context)
{
    firstCurveId_ = ObjectId::invalid();
    secondCurveId_ = ObjectId::invalid();
    firstCurves_.clear();
    secondCurves_.clear();
    drawingFrame_ = context.viewportTransform().workPlaneFrame();
    firstPoint_ = {};
    secondPoint_ = {};
    solutionAvailable_ = false;
    status_.state = ToolLifecycleState::Active;
    status_.canCommit = false;
    status_.text = QStringLiteral("%1: click the first curve")
                       .arg(toolName(tool_));
    publish(context);
}

bool TwoCurveLineTool::handleMousePress(const ToolInput &input,
                                         ToolContext &context)
{
    if (input.button == Qt::RightButton) {
        finish(context);
        return true;
    }
    if (input.button != Qt::LeftButton) {
        return false;
    }
    if (secondCurveId_.isValid()) {
        updateSolution(input, context);
        if (solutionAvailable_) {
            finish(context);
        }
        return true;
    }

    const int shapeIndex = context.curveHitTester().hitTestShape(
        context.document(), input.screenPosition,
        context.viewportTransform(), input.viewportSize);
    if (shapeIndex < 0) {
        status_.text = QStringLiteral("Click a visible curve");
        publish(context);
        return true;
    }
    const Shape &shape = context.document()[shapeIndex];
    const QVector<Shape::NurbsCurve2D> curves = shapeCurves(shape);
    if (curves.isEmpty()) {
        status_.text = QStringLiteral("Choose a NURBS curve");
        publish(context);
        return true;
    }
    const ObjectId objectId = context.document().objectIdAt(shapeIndex);
    if (!firstCurveId_.isValid()) {
        firstCurveId_ = objectId;
        drawingFrame_ = shapeWorkPlaneFrame(shape);
        if (!collectCurves(shape, &firstCurves_)) {
            firstCurveId_ = ObjectId::invalid();
            status_.text = QStringLiteral("Could not map the curve into its drawing plane");
            publish(context);
            return true;
        }
        context.viewportTransform().setWorkPlaneFrame(drawingFrame_);
        context.selection().setObjectIds({objectId}, objectId);
        status_.text = QStringLiteral("%1: click a second coplanar curve")
                           .arg(toolName(tool_));
        publish(context);
        return true;
    }
    if (objectId == firstCurveId_) {
        status_.text = QStringLiteral("Click a different second curve");
        publish(context);
        return true;
    }
    secondCurveId_ = objectId;
    if (!collectCurves(shape, &secondCurves_)) {
        secondCurveId_ = ObjectId::invalid();
        secondCurves_.clear();
        status_.text = QStringLiteral("The second curve must lie in the first curve's plane");
        publish(context);
        return true;
    }
    context.selection().setObjectIds({firstCurveId_, secondCurveId_}, secondCurveId_);
    status_.text = QStringLiteral("Move to preview the line, then click to create it");
    updateSolution(input, context);
    publish(context);
    return true;
}

bool TwoCurveLineTool::handleMouseMove(const ToolInput &input,
                                       ToolContext &context)
{
    if (secondCurveId_.isValid()) {
        updateSolution(input, context);
        publish(context);
        return true;
    }
    return false;
}

bool TwoCurveLineTool::handleKey(const ToolInput &input,
                                  ToolContext &context)
{
    if (input.key != Qt::Key_Escape) {
        return false;
    }
    cancel(context);
    context.finishTool(ToolId::Select);
    return true;
}

void TwoCurveLineTool::cancel(ToolContext &context)
{
    firstCurveId_ = ObjectId::invalid();
    secondCurveId_ = ObjectId::invalid();
    firstCurves_.clear();
    secondCurves_.clear();
    solutionAvailable_ = false;
    status_.state = ToolLifecycleState::Cancelled;
    status_.canCommit = false;
    status_.text = QStringLiteral("%1 cancelled").arg(toolName(tool_));
    publish(context);
}

ToolPreview TwoCurveLineTool::preview() const
{
    ToolPreview result;
    result.workPlaneFrame = drawingFrame_;
    result.hasWorkPlaneFrame = isValidWorkPlaneFrame(drawingFrame_);
    result.planeLocked = firstCurveId_.isValid();
    if (solutionAvailable_) {
        result.shape = lineShape(firstPoint_, secondPoint_, drawingFrame_);
        result.hasShape = true;
    }
    result.statusText = status_.text;
    return result;
}

ToolStatus TwoCurveLineTool::status() const
{
    return status_;
}

bool TwoCurveLineTool::collectCurves(
    const Shape &shape,
    QVector<Shape::NurbsCurve2D> *curves) const
{
    if (curves == nullptr || !isValidWorkPlaneFrame(drawingFrame_)) {
        return false;
    }
    const QVector<Shape::NurbsCurve2D> sourceCurves = shapeCurves(shape);
    if (sourceCurves.isEmpty()) {
        return false;
    }
    QVector<Shape::NurbsCurve2D> mapped;
    mapped.reserve(sourceCurves.size());
    for (const Shape::NurbsCurve2D &curve : sourceCurves) {
        Shape::NurbsCurve2D result;
        if (!curveInFrame(shape, curve, drawingFrame_, &result)) {
            return false;
        }
        mapped.append(std::move(result));
    }
    *curves = std::move(mapped);
    return true;
}

bool TwoCurveLineTool::updateSolution(const ToolInput &input,
                                      const ToolContext &context)
{
    solutionAvailable_ = findClosestRelation(tool_, firstCurves_, secondCurves_,
                                            input.worldPosition,
                                            &firstPoint_, &secondPoint_);
    status_.canCommit = solutionAvailable_;
    status_.text = solutionAvailable_
                       ? QStringLiteral("%1 ready • click to create the line")
                             .arg(toolName(tool_))
                       : QStringLiteral("No %1 solution found near the cursor")
                             .arg(tool_ == ToolId::TangentToTwoCurves
                                      ? QStringLiteral("common tangent")
                                      : QStringLiteral("common perpendicular"));
    Q_UNUSED(context)
    return solutionAvailable_;
}

void TwoCurveLineTool::finish(ToolContext &context)
{
    if (solutionAvailable_) {
        const Shape line = lineShape(firstPoint_, secondPoint_, drawingFrame_);
        if (validateNurbsCurve(line.nurbs) && context.commitShape(tool_, line)) {
            status_.state = ToolLifecycleState::Completed;
            status_.text = QStringLiteral("%1 created").arg(toolName(tool_));
        }
    } else {
        status_.text = QStringLiteral("%1 has no line to create")
                           .arg(toolName(tool_));
    }
    publish(context);
    context.finishTool(ToolId::Select);
}

void TwoCurveLineTool::publish(ToolContext &context)
{
    context.publishPreview(preview());
    context.publishStatus(status_);
}

} // namespace classiCAD
