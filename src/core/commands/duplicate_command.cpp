#include "duplicate_command.h"

#include "core/geometry/nurbs_surface.h"
#include "core/geometry/nurbs_solid.h"

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
    duplicateObjectIds->reserve(plan.duplicateObjects.size());
    for (const SceneObject &duplicate : plan.duplicateObjects) {
        const ObjectId duplicateId =
            transaction.insertObject(document.size(), duplicate);
        if (!duplicateId.isValid()) {
            duplicateObjectIds->clear();
            return false;
        }
        duplicateObjectIds->append(duplicateId);
    }

    for (int duplicateIndex = 0;
         duplicateIndex < duplicateObjectIds->size();
         ++duplicateIndex) {
        Shape *duplicateGeometry =
            transaction.editGeometry((*duplicateObjectIds)[duplicateIndex]);
        if (duplicateGeometry == nullptr ||
            !isDimensionGeometryType(duplicateGeometry->geometryType)) {
            continue;
        }
        for (DimensionAnchorReference &anchor : duplicateGeometry->dimensionAnchors) {
            bool remapped = false;
            for (int sourceIndex = 0;
                 sourceIndex < plan.duplicateObjects.size();
                 ++sourceIndex) {
                if (anchor.objectId == plan.sourceObjectIds[sourceIndex]) {
                    anchor.objectId = (*duplicateObjectIds)[sourceIndex];
                    remapped = true;
                    break;
                }
            }
            if (!remapped) {
                // Keep the annotation positioned while detaching references
                // that were not included in this duplicate operation.
                anchor = DimensionAnchorReference{};
            }
        }
    }
    return true;
}

} // namespace classiCAD
