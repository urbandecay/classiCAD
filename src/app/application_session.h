#pragma once

#include "core/document/document.h"
#include "core/document/selection_model.h"
#include "core/history/document_transaction.h"
#include "core/history/history.h"
#include "tools/tool_registry.h"

#include <functional>

namespace classiCAD {

enum class ApplicationSessionChange {
    History,
    Layers,
    Selection,
    DocumentWillBeReplaced,
    DocumentReplaced,
};

// Application-wide mutable model state. The viewport borrows these models;
// project and update controllers can work against the same session without
// reaching through the widget.
class ApplicationSession final {
public:
    using ChangeObserver = std::function<void(ApplicationSessionChange)>;

    ApplicationSession();

    Document &document();
    const Document &document() const;
    SelectionModel &selection();
    const SelectionModel &selection() const;
    History &history();
    const History &history() const;
    ToolRegistry &toolRegistry();
    const ToolRegistry &toolRegistry() const;

    void setChangeObserver(ChangeObserver observer);
    void notifyHistoryChanged();
    void notifyLayersChanged();
    void notifySelectionChanged();
    DocumentTransaction beginTransaction();
    bool commitTransaction(DocumentTransaction &transaction);
    void replaceDocument(Document document);

private:
    void notify(ApplicationSessionChange change);

    Document document_;
    SelectionModel selection_;
    History history_;
    ToolRegistry toolRegistry_;
    ChangeObserver changeObserver_;
};

} // namespace classiCAD
