#pragma once

#include <functional>

namespace classiCAD {

// Commands exposed to the application shell. The application maps these
// stable IDs onto document commands or active-tool operations through handlers.
enum class ApplicationCommand {
    Undo,
    Redo,
    BeginSubdivision,
    CancelSubdivision,
    ApplySubdivision,
    BeginJoin,
    Explode,
    Fill,
    BeginRotate,
    BeginScale,
    BeginMirror,
    BeginDuplicate,
    DuplicateInPlace,
    BeginPointExtrude,
};

struct ApplicationCommandResult {
    bool accepted = false;
    int count = 0;
};

class CommandRouter final {
public:
    using Handler = std::function<ApplicationCommandResult(int)>;

    struct Handlers {
        Handler undo;
        Handler redo;
        Handler beginSubdivision;
        Handler cancelSubdivision;
        Handler applySubdivision;
        Handler beginJoin;
        Handler explode;
        Handler fill;
        Handler beginRotate;
        Handler beginScale;
        Handler beginMirror;
        Handler beginDuplicate;
        Handler duplicateInPlace;
        Handler beginPointExtrude;
    };

    explicit CommandRouter(Handlers handlers = {});

    void setHandlers(Handlers handlers);
    ApplicationCommandResult execute(ApplicationCommand command,
                                     int argument = 0) const;

private:
    Handlers handlers_;
};

} // namespace classiCAD
