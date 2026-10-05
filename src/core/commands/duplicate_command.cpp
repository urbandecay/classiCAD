#include "duplicate_command.h"

#include "core/geometry/nurbs_surface.h"
#include "core/geometry/nurbs_solid.h"

#include <QHash>

#include <cmath>

namespace classiCAD {
namespace {

bool validDuplicateGeometry(const Shape &shape)
{
    if (shape.geometryType == GeometryType::NurbsSolid) {
        return validateNurbsSolid(shape.nurbsSolid);
    }
    if (shape.geometryType == GeometryType::NurbsSurface) {
        return validateNurbsSurface(shape.nurbsSurface);
    }
    if (shape.geometryType == GeometryType::Point) {
        return shape.points.size() == 1 &&
               std::isfinite(shape.points.first().x()) &&
               std::isfinite(shape.points.first().y());
    }
    if (shape.geometryType == GeometryType::Picture) {
        return true;
    }
    return validateNurbsCurve(shape.nurbs) &&
           isValidWorkPlaneFrame(shape.workPlaneFrame);
}

} // namespace

bool buildDuplicateCommandPlan(const Document &document,
                               const QVector<ObjectId> &sourceObjectIds,
                               const QVector<Shape> &duplicateGeometry,
                               DuplicateCommandPlan *plan)
{
    if (plan == nullptr) {
        return false;
    }
    *plan = DuplicateCommandPlan{};
    if (sourceObjectIds.isEmpty() ||
        sourceObjectIds.size() != duplicateGeometry.size()) {
        return false;
    }

    plan->duplicateObjects.reserve(sourceObjectIds.size());
    plan->sourceObjectIds.reserve(sourceObjectIds.size());
    for (int index = 0; index < sourceObjectIds.size(); ++index) {
        const ObjectId sourceId = sourceObjectIds[index];
        const SceneObject *sourceObject = document.object(sourceId);
        const Shape &geometry = duplicateGeometry[index];
        if (sourceObject == nullptr || !document.isObjectEditable(sourceId) ||
            !validDuplicateGeometry(geometry)) {
            *plan = DuplicateCommandPlan{};
            return false;
        }

        SceneObject duplicate = *sourceObject;
        duplicate.id = ObjectId::invalid();
        duplicate.geometry = geometry;
        plan->sourceObjectIds.append(sourceId);
        plan->duplicateObjects.append(std::move(duplicate));
    }
    return true;
}

bool applyDuplicateCommand(const Document &document,
                           DocumentTransaction &transaction,
                           const DuplicateCommandPlan &plan,
                           QVector<ObjectId> *duplicateObjectIds)
{
    if (duplicateObjectIds == nullptr || plan.duplicateObjects.isEmpty()) {
        return false;
    }
    duplicateObjectIds->clear();
    *duplicateObjectIds = transaction.insertObjects(document.size(),
                                                     plan.duplicateObjects);
    if (duplicateObjectIds->size() != plan.duplicateObjects.size()) {
        duplicateObjectIds->clear();
        return false;
    }

    QHash<quint64, ObjectId> duplicateIdBySource;
    duplicateIdBySource.reserve(plan.sourceObjectIds.size());
    for (int index = 0; index < plan.sourceObjectIds.size(); ++index) {
        const quint64 sourceValue = plan.sourceObjectIds[index].value();
        if (!duplicateIdBySource.contains(sourceValue)) {
            duplicateIdBySource.insert(sourceValue, (*duplicateObjectIds)[index]);
        }
    }
    for (int duplicateIndex = 0;
         duplicateIndex < duplicateObjectIds->size();
         ++duplicateIndex) {
        const Shape *currentDuplicate =
            document.shape((*duplicateObjectIds)[duplicateIndex]);
        if (currentDuplicate == nullptr ||
            !isDimensionGeometryType(currentDuplicate->geometryType)) {
            continue;
        }
        Shape *duplicateGeometry =
            transaction.editGeometry((*duplicateObjectIds)[duplicateIndex]);
        if (duplicateGeometry == nullptr) {
            continue;
        }
        for (DimensionAnchorReference &anchor : duplicateGeometry->dimensionAnchors) {
            const auto mapped = duplicateIdBySource.constFind(anchor.objectId.value());
            if (mapped != duplicateIdBySource.cend()) {
                anchor.objectId = mapped.value();
            } else {
                // Keep the annotation positioned while detaching references
                // that were not included in this duplicate operation.
                anchor = DimensionAnchorReference{};
            }
        }
    }
    return true;
}

} // namespace classiCAD
