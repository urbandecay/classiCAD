#include "command_router.h"

#include <utility>

namespace classiCAD {

CommandRouter::CommandRouter(Handlers handlers)
    : handlers_(std::move(handlers))
{
}

void CommandRouter::setHandlers(Handlers handlers)
{
    handlers_ = std::move(handlers);
}

ApplicationCommandResult CommandRouter::execute(ApplicationCommand command,
                                                int argument) const
{
    const Handler *handler = nullptr;
    switch (command) {
    case ApplicationCommand::Undo:
        handler = &handlers_.undo;
        break;
    case ApplicationCommand::Redo:
        handler = &handlers_.redo;
        break;
    case ApplicationCommand::BeginSubdivision:
        handler = &handlers_.beginSubdivision;
        break;
    case ApplicationCommand::CancelSubdivision:
        handler = &handlers_.cancelSubdivision;
        break;
    case ApplicationCommand::ApplySubdivision:
        handler = &handlers_.applySubdivision;
        break;
    case ApplicationCommand::BeginJoin:
        handler = &handlers_.beginJoin;
        break;
    case ApplicationCommand::Explode:
        handler = &handlers_.explode;
        break;
    case ApplicationCommand::Fill:
        handler = &handlers_.fill;
        break;
    case ApplicationCommand::BeginRotate:
        handler = &handlers_.beginRotate;
        break;
    case ApplicationCommand::BeginScale:
        handler = &handlers_.beginScale;
        break;
    case ApplicationCommand::BeginMirror:
        handler = &handlers_.beginMirror;
        break;
    case ApplicationCommand::BeginDuplicate:
        handler = &handlers_.beginDuplicate;
        break;
    case ApplicationCommand::DuplicateInPlace:
        handler = &handlers_.duplicateInPlace;
        break;
    case ApplicationCommand::BeginPointExtrude:
        handler = &handlers_.beginPointExtrude;
        break;
    case ApplicationCommand::Weld:
        handler = &handlers_.weld;
        break;
    }

    return handler != nullptr && *handler ? (*handler)(argument)
                                          : ApplicationCommandResult{};
}

} // namespace classiCAD
