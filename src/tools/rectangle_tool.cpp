#include "rectangle_tool.h"

#include "tool_context.h"
#include "core/document/document_settings.h"

#include <algorithm>
#include <cmath>

namespace classiCAD {

namespace {
constexpr qreal epsilon = 1.0e-9;
Point3D add(Point3D a, Point3D b) { return {a.x+b.x, a.y+b.y, a.z+b.z}; }
Point3D sub(Point3D a, Point3D b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
Point3D scale(Point3D a, qreal s) { return {a.x*s, a.y*s, a.z*s}; }
qreal dot(Point3D a, Point3D b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
Point3D cross(Point3D a, Point3D b)
{ return {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x}; }
qreal magnitude(Point3D a) { return std::sqrt(dot(a,a)); }
Point3D unit(Point3D a)
{ const qreal n=magnitude(a); return n>epsilon ? scale(a,1.0/n) : Point3D{}; }
qreal sign(qreal value) { return value < 0.0 ? -1.0 : 1.0; }
WorkPlaneFrame basis(Point3D origin, Point3D normal)
{
    const Point3D z=unit(normal);
    const Point3D axis=std::abs(z.x)<0.99 ? Point3D{1,0,0} : Point3D{0,1,0};
    const Point3D y=unit(cross(z,axis));
    WorkPlaneFrame frame;
    frame.origin=origin; frame.xAxis=unit(cross(y,z)); frame.yAxis=y;
    frame.normal=z; frame.valid=true;
    return frame;
}
bool numericStart(const QString &text)
{
    return !text.isEmpty() && (text.front().isDigit() ||
        QStringLiteral(".,+-").contains(text.front()));
}
QString lengthSuffix(DocumentLengthUnit unit)
{
    switch (unit) {
    case DocumentLengthUnit::Millimeter: return QStringLiteral("mm");
    case DocumentLengthUnit::Centimeter: return QStringLiteral("cm");
    case DocumentLengthUnit::Meter: return QStringLiteral("m");
    case DocumentLengthUnit::Inch: return QStringLiteral("in");
    case DocumentLengthUnit::Foot: return QStringLiteral("ft");
    }
    return QStringLiteral("mm");
}
}

RectangleTool::RectangleTool(ToolId tool) : tool_(tool) {}
ToolId RectangleTool::id() const { return tool_; }
bool RectangleTool::threePoint() const { return tool_==ToolId::RectangleThreePoint; }
bool RectangleTool::edgeBased() const
{ return tool_==ToolId::Rectangle || threePoint(); }
bool RectangleTool::fromCenter() const { return tool_==ToolId::RectangleFromCenter; }

void RectangleTool::begin(ToolContext &context)
{
    const ToolId tool=tool_;
    *this=RectangleTool(tool);
    frame_=context.viewportTransform().workPlaneFrame();
    referenceFrame_=frame_;
    status_.state=ToolLifecycleState::Active;
    publish(context);
}

Point3D RectangleTool::eventPoint(const ToolInput &input) const
{
    return workPlaneFramePointToWorld(input.worldPosition,
        isValidWorkPlaneFrame(input.workPlaneFrame) ? input.workPlaneFrame : frame_);
}

Point3D RectangleTool::planePoint(const ToolInput &input, ToolContext &context,
                                 bool allowSnap) const
{
    const Point3D target=eventPoint(input);
    if (allowSnap && input.snapType!=SnapType::None &&
        magnitude(sub(target,anchor_))>1.0e-6) {
        return sub(target,scale(frame_.normal,dot(sub(target,frame_.origin),frame_.normal)));
    }
    QPointF local;
    if (context.viewportTransform().screenToWorkPlane(
            input.screenPosition,input.viewportSize,frame_,&local)) {
        return workPlaneFramePointToWorld(local,frame_);
    }
    return sub(target,scale(frame_.normal,dot(sub(target,frame_.origin),frame_.normal)));
}

Point3D RectangleTool::inferAxis(const ToolInput &input, ToolContext &context,
                                const Point3D &anchor, const Point3D &target) const
{
    if (input.modifiers.testFlag(Qt::AltModifier)) return target;
    QPointF origin;
    if (!context.viewportTransform().worldPointToScreenUnclipped(
            anchor,input.viewportSize,&origin)) return target;
    const QPointF delta=input.screenPosition-origin;
    const qreal distance=std::hypot(delta.x(),delta.y());
    if (distance<=epsilon) return target;
    qreal best=std::cos(6.0*3.14159265358979323846/180.0);
    Point3D axis;
    for (Point3D candidate : {Point3D{1,0,0},Point3D{0,1,0},Point3D{0,0,1}}) {
        if (std::abs(dot(candidate,referenceFrame_.normal))>=0.9) continue;
        QPointF endpoint;
        if (!context.viewportTransform().worldPointToScreenUnclipped(
                add(anchor,candidate),input.viewportSize,&endpoint)) continue;
        const QPointF projected=endpoint-origin;
        const qreal length=std::hypot(projected.x(),projected.y());
        if (length<=epsilon) continue;
        const qreal alignment=std::abs(QPointF::dotProduct(delta,projected)/(distance*length));
        if (alignment>best) { best=alignment; axis=candidate; }
    }
    Point3D inferred;
    if (magnitude(axis)>epsilon && context.viewportTransform().screenToWorldAxis(
            input.screenPosition,input.viewportSize,anchor,axis,&inferred)) return inferred;
    return target;
}

void RectangleTool::updateGeometry(const ToolInput &input, ToolContext &context)
{
    lastInput_=input;
    if (stage_==0) {
        if (!planeLocked_ && isValidWorkPlaneFrame(input.workPlaneFrame)) frame_=input.workPlaneFrame;
        cursor_=planeLocked_ ? planePoint(input,context,true) : eventPoint(input);
        cursorValid_=true;
        return;
    }
    if (!edgeBased()) {
        frame_=basis(anchor_,referenceFrame_.normal);
        if (perpendicular_) {
            const Point3D view=context.viewportTransform().viewDirection();
            Point3D normal=std::abs(dot(view,frame_.xAxis))>std::abs(dot(view,frame_.yAxis))
                ? frame_.xAxis : frame_.yAxis;
            if (dot(normal,view)>0.0) normal=scale(normal,-1);
            frame_.yAxis=unit(referenceFrame_.normal);
            frame_.xAxis=unit(cross(frame_.yAxis,normal)); frame_.normal=normal;
        }
        cursor_=planePoint(input,context,!perpendicular_);
        const Point3D delta=sub(cursor_,anchor_);
        dx_=dot(delta,frame_.xAxis); dy_=dot(delta,frame_.yAxis);
        const qreal factor=fromCenter() ? 0.5 : 1.0;
        if (square_) {
            const qreal side=(xLocked_ || yLocked_)
                ? std::max(xLocked_ ? lockedX_*factor : 0.0,yLocked_ ? lockedY_*factor : 0.0)
                : std::max(std::abs(dx_),std::abs(dy_));
            dx_=(xLocked_ ? signX_ : sign(dx_))*side;
            dy_=(yLocked_ ? signY_ : sign(dy_))*side;
        } else {
            if (xLocked_) dx_=signX_*lockedX_*factor;
            if (yLocked_) dy_=signY_*lockedY_*factor;
        }
        cursor_=add(anchor_,add(scale(frame_.xAxis,dx_),scale(frame_.yAxis,dy_)));
        if (fromCenter()) {
            vertices_={{dx_,dy_},{-dx_,dy_},{-dx_,-dy_},{dx_,-dy_}};
        } else {
            vertices_={{0,0},{dx_,0},{dx_,dy_},{0,dy_}};
        }
    } else if (stage_==1) {
        frame_=referenceFrame_;
        frame_.origin=anchor_;
        cursor_=inferAxis(input,context,anchor_,planePoint(input,context,true));
        cursor_=sub(cursor_,scale(referenceFrame_.normal,
                                 dot(sub(cursor_,anchor_),referenceFrame_.normal)));
        const Point3D edge=sub(cursor_,anchor_);
        if (xLocked_ && magnitude(edge)>epsilon) cursor_=add(anchor_,scale(unit(edge),lockedX_));
        dx_=magnitude(sub(cursor_,anchor_)); dy_=0;
        vertices_.clear();
    } else {
        const Point3D edge=unit(sub(edgeEnd_,anchor_));
        frame_.origin=anchor_; frame_.xAxis=edge;
        frame_.yAxis=perpendicular_ ? unit(referenceFrame_.normal)
            : unit(cross(referenceFrame_.normal,edge));
        frame_.normal=unit(cross(frame_.xAxis,frame_.yAxis)); frame_.valid=true;
        if (xLocked_) edgeEnd_=add(anchor_,scale(edge,lockedX_));
        Point3D heightPoint;
        if (!context.viewportTransform().screenToWorldAxis(input.screenPosition,
                input.viewportSize,edgeEnd_,frame_.yAxis,&heightPoint)) {
            heightPoint=planePoint(input,context,false);
        }
        if (!perpendicular_) heightPoint=inferAxis(input,context,edgeEnd_,heightPoint);
        dy_=dot(sub(heightPoint,edgeEnd_),frame_.yAxis);
        if (yLocked_) dy_=signY_*lockedY_;
        dx_=magnitude(sub(edgeEnd_,anchor_));
        if (square_) {
            const qreal side = (xLocked_ || yLocked_)
                ? std::max(xLocked_ ? lockedX_ : 0.0, yLocked_ ? lockedY_ : 0.0)
                : dx_;
            dx_=side;
            dy_=(yLocked_ ? signY_ : sign(dy_))*side;
        }
        vertices_={{0,0},{dx_,0},{dx_,dy_},{0,dy_}};
        cursor_=add(anchor_,add(scale(frame_.xAxis,dx_),scale(frame_.yAxis,dy_)));
    }
    cursorValid_=true;
    context.viewportTransform().setWorkPlaneFrame(frame_);
}

bool RectangleTool::handleMouseMove(const ToolInput &input, ToolContext &context)
{
    updateGeometry(input,context); publish(context); return true;
}

bool RectangleTool::handleMousePress(const ToolInput &input, ToolContext &context)
{
    if (input.button==Qt::RightButton) {
        updateGeometry(input,context);
        finish(context);
        return true;
    }
    if (input.button!=Qt::LeftButton) return false;
    if (numeric_!=NumericInput::None) return true;
    updateGeometry(input,context);
    if (stage_==0) {
        anchor_=cursor_; referenceFrame_=frame_; referenceFrame_.origin=anchor_;
        frame_=referenceFrame_; stage_=1; planeLocked_=true;
        context.viewportTransform().setWorkPlaneFrame(frame_);
    } else if (edgeBased() && stage_==1) {
        if (dx_<=epsilon) return true;
        edgeEnd_=cursor_; stage_=2;
        updateGeometry(input,context);
    } else {
        if (std::abs(dx_)<=epsilon || std::abs(dy_)<=epsilon) return true;
        finish(context);
        return true;
    }
    publish(context); return true;
}

bool RectangleTool::handleKey(const ToolInput &input, ToolContext &context)
{
    if (input.key==Qt::Key_Escape) { cancel(context); context.finishTool(ToolId::Select); return true; }
    if (numeric_!=NumericInput::None) {
        if (input.key==Qt::Key_Return || input.key==Qt::Key_Enter) {
            qreal value=0;
            if (parseDocumentLengthInput(text_,context.document().settings().lengthUnit,&value) &&
                std::isfinite(value) && std::abs(value)>epsilon) {
                if (numeric_==NumericInput::X || numeric_==NumericInput::Square) {
                    lockedX_=std::abs(value); signX_=sign(dx_); xLocked_=true;
                }
                if (numeric_==NumericInput::Y || numeric_==NumericInput::Square) {
                    lockedY_=std::abs(value); signY_=sign(dy_); yLocked_=true;
                }
                numeric_=NumericInput::None; text_.clear();
                updateGeometry(lastInput_,context);
            }
        } else if (input.key==Qt::Key_Backspace || input.key==Qt::Key_Delete) {
            text_.chop(1);
        } else if (!input.text.isEmpty()) {
            for (QChar character : input.text) {
                if (character.isLetterOrNumber() || QStringLiteral(".,+-/'\" ").contains(character))
                    text_.append(character);
            }
        }
        publish(context); return true;
    }
    if (input.key==Qt::Key_Return || input.key==Qt::Key_Enter || input.key==Qt::Key_Space) {
        finish(context);
        return true;
    }
    if (input.key==Qt::Key_L) {
        planeLocked_=!planeLocked_;
        if (stage_==0 && planeLocked_) {
            frame_.origin=cursor_;
            referenceFrame_=frame_;
        }
        if (stage_>0) {
            referenceFrame_=basis(anchor_,frame_.normal);
            updateGeometry(lastInput_,context);
        }
        publish(context); return true;
    }
    if (stage_==0) return false;
    if (input.key==Qt::Key_P) perpendicular_=!perpendicular_;
    else if (input.key==Qt::Key_Shift && !threePoint()) square_=!square_;
    else if (input.key==Qt::Key_X || input.key==Qt::Key_Y) {
        numeric_=input.key==Qt::Key_X ? NumericInput::X : NumericInput::Y;
        text_.clear();
    } else if (square_ && !threePoint() && numericStart(input.text)) {
        numeric_=NumericInput::Square; text_=input.text;
    } else return false;
    if (numeric_==NumericInput::None) updateGeometry(lastInput_,context);
    publish(context); return true;
}

Shape RectangleTool::rectangleShape() const
{
    Shape shape; shape.geometryType=GeometryType::Rectangle;
    shape.points=vertices_; shape.workPlaneFrame=frame_;
    QVector<QPointF> closed=vertices_;
    if (!closed.isEmpty()) closed.append(closed.first());
    shape.nurbs=makeDegreeOneNurbs(closed);
    return shape;
}

ToolPreview RectangleTool::preview() const
{
    ToolPreview result;
    result.workPlaneFrame=frame_; result.hasWorkPlaneFrame=isValidWorkPlaneFrame(frame_);
    result.planeLocked=planeLocked_ || stage_>0;
    if (stage_>0) result.points.append(worldPointToWorkPlaneFrame(anchor_,frame_));
    if (edgeBased() && stage_==2) result.points.append(
        worldPointToWorkPlaneFrame(add(anchor_,scale(frame_.xAxis,dx_)),frame_));
    result.hasCursorPoint=cursorValid_; result.cursorVisible=cursorValid_;
    result.cursorPoint=worldPointToWorkPlaneFrame(cursor_,frame_);
    if (cursorValid_ && stage_>0) {
        const Point3D guideOrigin = anchor_;
        const Point3D direction=unit(sub(cursor_,guideOrigin));
        QColor axisColor;
        if (std::abs(direction.x)>0.9999) axisColor=QColor(255,26,26);
        else if (std::abs(direction.y)>0.9999) axisColor=QColor(26,179,26);
        else if (std::abs(direction.z)>0.9999) axisColor=QColor(51,128,255);
        if (axisColor.isValid()) {
            result.guides.append({QLineF(worldPointToWorkPlaneFrame(guideOrigin,frame_),
                                         result.cursorPoint),axisColor,false});
        }
    }
    if (vertices_.size()==4 && std::abs(dx_)>epsilon && std::abs(dy_)>epsilon) {
        result.shape=rectangleShape(); result.hasShape=true;
    } else if (edgeBased() && stage_==1 && dx_>epsilon) {
        result.shape.geometryType=GeometryType::Line;
        result.shape.points={worldPointToWorkPlaneFrame(anchor_,frame_),result.cursorPoint};
        result.shape.nurbs=makeDegreeOneNurbs(result.shape.points);
        result.shape.workPlaneFrame=frame_; result.hasShape=true;
    }
    result.hudDimensionsLine=dimensions_; result.hudInstructionsLine=instructions_;
    result.statusText=status_.text;
    return result;
}

void RectangleTool::updateStatus(const ToolContext &context)
{
    const auto unit=context.document().settings().lengthUnit;
    const qreal factor=fromCenter() ? 2.0 : 1.0;
    const auto value=[&](qreal size) {
        return QStringLiteral("%1 %2").arg(std::abs(size)/millimetersPerDocumentUnit(unit),0,'f',3)
            .arg(lengthSuffix(unit));
    };
    const QString x=numeric_==NumericInput::X ? text_+QStringLiteral("|")
        : value(xLocked_ ? lockedX_ : dx_*factor);
    const QString y=numeric_==NumericInput::Y ? text_+QStringLiteral("|")
        : value(yLocked_ ? lockedY_ : dy_*factor);
    dimensions_=numeric_==NumericInput::Square ? QStringLiteral("Square: %1|").arg(text_)
        : QStringLiteral("X: %1    Y: %2").arg(x,y);
    if (stage_==0) {
        dimensions_=toolName(tool_);
        instructions_=QStringLiteral("Click %1  •  L plane lock  •  Esc exits")
            .arg(fromCenter() ? QStringLiteral("center") : QStringLiteral("first corner"));
    } else {
        instructions_=numeric_!=NumericInput::None
            ? QStringLiteral("Type dimension  •  Enter applies  •  Esc exits")
            : QStringLiteral("%1  •  X width  •  Y height  •  P perp%2  •  Esc exits")
                .arg(edgeBased() && stage_==1 ? QStringLiteral("Click edge endpoint") : QStringLiteral("Click to finish"),
                     threePoint() ? QStringLiteral("  •  Alt bypass") : QStringLiteral("  •  Shift square"));
    }
    status_.state=ToolLifecycleState::Active; status_.text=instructions_;
    status_.canCommit=vertices_.size()==4 && std::abs(dx_)>epsilon && std::abs(dy_)>epsilon;
}

void RectangleTool::publish(ToolContext &context)
{ updateStatus(context); context.publishPreview(preview()); context.publishStatus(status_); }
ToolStatus RectangleTool::status() const { return status_; }
void RectangleTool::finish(ToolContext &context)
{
    if (vertices_.size()!=4 || std::abs(dx_)<=epsilon || std::abs(dy_)<=epsilon) return;
    const ToolPreview current=preview();
    if (current.hasShape && (!validateNurbsCurve(current.shape.nurbs) ||
        !context.commitShape(tool_,current.shape))) return;
    stage_=0; vertices_.clear(); cursorValid_=false;
    numeric_=NumericInput::None; dimensions_.clear(); instructions_.clear();
    planeLocked_=false; status_.state=ToolLifecycleState::Completed;
    context.publishPreview(preview()); context.publishStatus(status_);
    context.finishTool(ToolId::Select);
}
void RectangleTool::cancel(ToolContext &context)
{
    stage_=0; vertices_.clear(); cursorValid_=false; numeric_=NumericInput::None;
    dimensions_.clear(); instructions_.clear(); planeLocked_=false;
    status_.state=ToolLifecycleState::Cancelled;
    context.publishPreview(preview()); context.publishStatus(status_);
}

} // namespace classiCAD
