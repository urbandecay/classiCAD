#pragma once

#include "object_id.h"

#include <QVector>

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
    const QVector<ObjectId> &objectIds() const;
    QVector<ObjectId> &objectIds();

    ObjectId primaryObjectId() const;
    ObjectId &primaryObjectId();

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
    int &activeControlPointIndex();
    void setActiveControlPoint(ObjectId objectId, int index);
    void clearActiveControlPoint();

private:
    QVector<ObjectId> objectIds_;
    ObjectId primaryObjectId_ = ObjectId::invalid();
    ControlPointReference activeControlPoint_;
};

} // namespace classiCAD
