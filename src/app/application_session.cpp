#include "application_session.h"

#include <utility>

namespace classiCAD {

ApplicationSession::ApplicationSession()
    : history_(document_)
{
}

Document &ApplicationSession::document()
{
    return document_;
}

const Document &ApplicationSession::document() const
{
    return document_;
}

SelectionModel &ApplicationSession::selection()
{
    return selection_;
}

const SelectionModel &ApplicationSession::selection() const
{
    return selection_;
}

History &ApplicationSession::history()
{
    return history_;
}

const History &ApplicationSession::history() const
{
    return history_;
}

ToolRegistry &ApplicationSession::toolRegistry()
{
    return toolRegistry_;
}

const ToolRegistry &ApplicationSession::toolRegistry() const
{
    return toolRegistry_;
}

void ApplicationSession::setChangeObserver(ChangeObserver observer)
{
    changeObserver_ = std::move(observer);
}

void ApplicationSession::notifyHistoryChanged()
{
    notify(ApplicationSessionChange::History);
}

void ApplicationSession::notifyLayersChanged()
{
    notify(ApplicationSessionChange::Layers);
}

void ApplicationSession::notifySelectionChanged()
{
    notify(ApplicationSessionChange::Selection);
}

DocumentTransaction ApplicationSession::beginTransaction()
{
    return DocumentTransaction(document_, history_);
}

bool ApplicationSession::commitTransaction(DocumentTransaction &transaction)
{
    if (!transaction.commit()) {
        return false;
    }
    const DocumentChangeSet &changes = transaction.changes();
    if (changes.affectsPersistentDocument()) {
        notifyHistoryChanged();
    }
    if (changes.affectsLayerPresentation()) {
        notifyLayersChanged();
    }
    if (changes.selectionChanged) {
        notify(ApplicationSessionChange::Selection);
    }
    return true;
}

void ApplicationSession::replaceDocument(Document document)
{
    notify(ApplicationSessionChange::DocumentWillBeReplaced);
    document_.replaceWith(document);
    selection_.clear();
    history_.clear();
    notify(ApplicationSessionChange::DocumentReplaced);
}

void ApplicationSession::notify(ApplicationSessionChange change)
{
    if (changeObserver_) {
        changeObserver_(change);
    }
}

} // namespace classiCAD
