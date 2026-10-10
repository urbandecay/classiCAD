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
    ArcTool directRadiusTool;
    directRadiusTool.setMode(ArcMode::OnePoint);
    directRadiusTool.setInputPoints({{0.0, 0.0}});
    const ArcKeyResult directNumpadStart = directRadiusTool.handleKeyInput(
        Qt::Key_End, {}, Qt::KeypadModifier, false);
    ArcTool directAngleTool;
    directAngleTool.setMode(ArcMode::OnePoint);
    directAngleTool.setInputPoints({{0.0, 0.0}, {2.0, 0.0}});
    const ArcKeyResult directAngleStart = directAngleTool.handleKeyInput(
        Qt::Key_7, QStringLiteral("7"), Qt::NoModifier, false);
    const bool directAngleEntryCorrect =
        directAngleStart.handled &&
        directAngleStart.action == ArcKeyAction::BeginTextInput &&
        directAngleTool.textInputMode() == ArcTextInputMode::Angle &&
        directAngleTool.textInput() == QStringLiteral("7");
    directAngleTool.clearTextInput();
    const ArcKeyResult keypadFinish = directAngleTool.handleKeyInput(
        Qt::Key_Enter, {}, Qt::KeypadModifier, false);

    ArcTool radiusTool;
    radiusTool.setMode(ArcMode::OnePoint);
    radiusTool.setInputPoints({{0.0, 0.0}});
    const bool beganRadiusInput = radiusTool.beginTextInput(
        ArcTextInputMode::Radius, {}, true);
    const ArcKeyResult keypadDigit = radiusTool.handleKeyInput(
        Qt::Key_4, {}, Qt::KeypadModifier, false);
    const ArcKeyResult keypadDecimal = radiusTool.handleKeyInput(
        Qt::Key_Period, {}, Qt::KeypadModifier, false);
    const ArcKeyResult keypadSecondDigit = radiusTool.handleKeyInput(
        Qt::Key_5, {}, Qt::KeypadModifier, false);
    const ArcKeyResult numLockOffDigit = radiusTool.handleKeyInput(
        Qt::Key_End, {}, Qt::KeypadModifier, false);
    const ArcKeyResult numLockOffDecimal = radiusTool.handleKeyInput(
        Qt::Key_Delete, {}, Qt::KeypadModifier, false);
    const ArcKeyResult numpadEnter = radiusTool.handleKeyInput(
        Qt::Key_Enter, {}, Qt::KeypadModifier, false);

    ArcTool arcTool;
    arcTool.setMode(ArcMode::TwoPoint);
    arcTool.setInputPoints({{0.0, 0.0}, {8.0, 0.0}});
    const bool beganChordInput = arcTool.beginTextInput(
        ArcTextInputMode::ChordLength, QStringLiteral("12"), true);
    const ArcKeyResult result = arcTool.handleKeyInput(
        Qt::Key_Escape, {}, Qt::AltModifier, true);

    const bool passed = check(directNumpadStart.handled &&
                                  directNumpadStart.action == ArcKeyAction::BeginTextInput &&
                                  directRadiusTool.textInputMode() == ArcTextInputMode::Radius &&
                                  directRadiusTool.textInput() == QStringLiteral("1"),
                              "A numpad digit must start One Point Arc radius input without pressing R") &&
                       check(directAngleEntryCorrect,
                              "A number key must start One Point Arc angle input without pressing A") &&
                       check(keypadFinish.handled &&
                                  keypadFinish.action == ArcKeyAction::FinishArc,
                              "Numpad Enter must finish a One Point Arc after applying its numeric angle") &&
                       check(beganRadiusInput && keypadDigit.handled &&
                                  keypadDecimal.handled &&
                                  keypadSecondDigit.handled &&
                                  numLockOffDigit.handled &&
                                  numLockOffDecimal.handled &&
                                  numpadEnter.handled &&
                                  numpadEnter.action == ArcKeyAction::ApplyTextInput &&
                                  radiusTool.textInput() == QStringLiteral("4.5" "1."),
                              "One Point Arc radius input must accept numpad digits and Enter with Num Lock on or off") &&
                       check(beganChordInput &&
                                  result.handled &&
                                  result.action == ArcKeyAction::CancelArc &&
                                  arcTool.textInputMode() == ArcTextInputMode::None &&
                                  arcTool.textInput().isEmpty() &&
                                  arcTool.inputPoints().isEmpty(),
                              "ArcTool must own Escape cancellation, including active numeric input and modifiers");
    return passed ? 0 : 1;
}
