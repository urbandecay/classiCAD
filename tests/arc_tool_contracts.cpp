#include "tools/arc_tool.h"

#include <QDebug>

using namespace classiCAD;

namespace {

bool check(bool condition, const char *message)
{
    if (!condition) {
        qCritical("%s", message);
    }
    return condition;
}

} // namespace

int main()
{
    ArcTool arcTool;
    arcTool.setMode(ArcMode::TwoPoint);
    arcTool.setInputPoints({{0.0, 0.0}, {8.0, 0.0}});
    const bool beganChordInput = arcTool.beginTextInput(
        ArcTextInputMode::ChordLength, QStringLiteral("12"), true);
    const ArcKeyResult result = arcTool.handleKeyInput(
        Qt::Key_Escape, {}, Qt::AltModifier, true);

    const bool passed = check(beganChordInput &&
                                  result.handled &&
                                  result.action == ArcKeyAction::CancelArc &&
                                  arcTool.textInputMode() == ArcTextInputMode::None &&
                                  arcTool.textInput().isEmpty() &&
                                  arcTool.inputPoints().isEmpty(),
                              "ArcTool must own Escape cancellation, including active numeric input and modifiers");
    return passed ? 0 : 1;
}
