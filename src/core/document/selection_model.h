#pragma once

#include "object_id.h"

#include <QVector>

#include <functional>

namespace classiCAD {

class Document;

struct ControlPointReference {
    ObjectId objectId = ObjectId::invalid();
    int index = -1;

    bool isValid() const
    {
        return objectId.isValid() && index >= 0;
    }
};

class SelectionModel final {
public:
    using ChangeNotifier = std::function<void()>;

    void setChangeNotifier(ChangeNotifier notifier);
    const QVector<ObjectId> &objectIds() const;

    const ObjectId &primaryObjectId() const;

    bool contains(ObjectId objectId) const;
    void clear();
    void setObjectIds(const QVector<ObjectId> &objectIds,
                      ObjectId primaryObjectId = ObjectId::invalid());
    void setPrimaryObjectId(ObjectId objectId);
    void add(ObjectId objectId);
    void remove(ObjectId objectId);
    void toggle(ObjectId objectId);
    void prune(const Document &document);

    const ControlPointReference &activeControlPoint() const;
    void setActiveControlPoint(ObjectId objectId, int index);
    void clearActiveControlPoint();

private:
    void notifyChanged();

    QVector<ObjectId> objectIds_;
    ObjectId primaryObjectId_ = ObjectId::invalid();
    ControlPointReference activeControlPoint_;
    ChangeNotifier changeNotifier_;
};

} // namespace classiCAD
