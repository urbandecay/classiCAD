#include "history.h"

namespace classiCAD {

History::History(Document &document)
    : document_(document)
{
}

bool History::canUndo() const
{
    return !undoStack_.isEmpty();
}

bool History::canRedo() const
{
    return !redoStack_.isEmpty();
}

int History::undoCount() const
{
    return undoStack_.size();
}

int History::redoCount() const
{
    return redoStack_.size();
}

void History::record()
{
    undoStack_.append(document_.snapshot());
    redoStack_.clear();
}

bool History::undo()
{
    if (!canUndo()) {
        return false;
    }

    redoStack_.append(document_.snapshot());
    document_.restoreSnapshot(undoStack_.takeLast());
    return true;
}

bool History::redo()
{
    if (!canRedo()) {
        return false;
    }

    undoStack_.append(document_.snapshot());
    document_.restoreSnapshot(redoStack_.takeLast());
    return true;
}

void History::clear()
{
    undoStack_.clear();
    redoStack_.clear();
}

} // namespace classiCAD
