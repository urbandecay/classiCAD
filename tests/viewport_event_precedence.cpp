#include "app/application_session.h"
#include "core/geometry/curve_construction.h"
#include "ui/viewport_widget_api.h"

#include <QApplication>
#include <QKeyEvent>
#include <QDebug>

#include <memory>

using namespace classiCAD;

namespace {

void sendKey(QWidget *widget, int key)
{
    QKeyEvent event(QEvent::KeyPress, key, Qt::NoModifier);
    QApplication::sendEvent(widget, &event);
}

Shape lineShape(qreal y)
{
    Shape shape;
    shape.geometryType = GeometryType::Line;
    shape.workPlaneFrame = makeWorkPlaneFrame(WorkPlane::XY);
    shape.points = {{0.0, y}, {2.0, y}};
    shape.nurbs = makeDegreeOneNurbs(shape.points);
    return shape;
}

Shape rectangleShape()
{
    Shape shape;
    shape.geometryType = GeometryType::Rectangle;
    shape.workPlaneFrame = makeWorkPlaneFrame(WorkPlane::XY);
    shape.points = {{0.0, 0.0}, {4.0, 3.0}};
    shape.nurbs = makeDegreeOneNurbs(
        {{0.0, 0.0}, {4.0, 0.0}, {4.0, 3.0}, {0.0, 3.0}, {0.0, 0.0}});
    return shape;
}

bool check(bool condition, const char *message)
{
    if (!condition) {
        qCritical() << message;
    }
    return condition;
}

} // namespace

int main(int argc, char **argv)
{
    QApplication application(argc, argv);
    ApplicationSession session;
    const ObjectId rectangleId = session.document().append(rectangleShape());
    session.document().append(lineShape(5.0));
    std::unique_ptr<ViewportWidgetApi> viewport(createViewportWidget(session));
    viewport->resize(480, 360);
    viewport->show();
    application.processEvents();

    bool passed = true;
    session.selection().clear();
    const ViewportCommandResult joinStarted =
        viewport->executeCommand(ViewportCommand::BeginJoin);
    passed &= check(joinStarted.accepted,
                    "Join must enter its modal selection state");
    sendKey(viewport.get(), Qt::Key_A);
    passed &= check(session.selection().objectIds().isEmpty(),
                    "Select All must not override active Join input");
    sendKey(viewport.get(), Qt::Key_Escape);

    session.selection().setObjectIds({rectangleId}, rectangleId);
    const ViewportCommandResult duplicateStarted =
        viewport->executeCommand(ViewportCommand::BeginDuplicate);
    passed &= check(duplicateStarted.accepted,
                    "Duplicate must enter its modal placement state");
    sendKey(viewport.get(), Qt::Key_F);
    passed &= check(session.document().shape(rectangleId)->geometryType ==
                        GeometryType::Rectangle &&
                        session.history().undoCount() == 0,
                    "Fill must not edit selected geometry during Duplicate placement");
    sendKey(viewport.get(), Qt::Key_A);
    passed &= check(session.selection().objectIds() ==
                        QVector<ObjectId>{rectangleId},
                    "Select All must not replace the Duplicate source selection");
    sendKey(viewport.get(), Qt::Key_Delete);
    passed &= check(session.document().size() == 2 &&
                        session.document().shape(rectangleId) != nullptr &&
                        session.history().undoCount() == 0,
                    "Delete must not remove the Duplicate source during placement");
    sendKey(viewport.get(), Qt::Key_Escape);

    sendKey(viewport.get(), Qt::Key_A);
    passed &= check(session.selection().objectIds().size() == 2,
                    "Select All must remain available in idle Select mode");

    ToolId finishedTool = ToolId::Arc;
    ViewportUiCallbacks callbacks;
    callbacks.commandFinished = [&finishedTool](ToolId tool) {
        finishedTool = tool;
    };
    viewport->setUiCallbacks(callbacks);
    viewport->setArcMode(ArcMode::TwoPoint);
    viewport->setTool(ToolId::Arc);
    sendKey(viewport.get(), Qt::Key_Escape);
    passed &= check(finishedTool == ToolId::Select,
                    "ArcTool Escape must finish the active command through the viewport boundary");
    sendKey(viewport.get(), Qt::Key_A);
    passed &= check(session.selection().objectIds().size() == 2,
                    "Arc Escape must return keyboard routing to Select mode");

    qInfo() << "Viewport event precedence:" << (passed ? "passed" : "failed");
    return passed ? 0 : 1;
}
