#pragma once

#include "../document/document.h"

#include <QVector>

namespace classiCAD {

class History final {
public:
    explicit History(Document &document);

    bool canUndo() const;
    bool canRedo() const;
    int undoCount() const;
    int redoCount() const;

    void record();
    void record(const Document::Snapshot &snapshot);
    bool undo();
    bool redo();
    void clear();

private:
    Document &document_;
    QVector<Document::Snapshot> undoStack_;
    QVector<Document::Snapshot> redoStack_;
};

} // namespace classiCAD
