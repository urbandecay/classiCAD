#pragma once

#include "layer_id.h"
#include "object_id.h"
#include "shape.h"

#include <QVector3D>
#include <QVector4D>

namespace classiCAD {

// Workbench material inputs are stored in scene-linear RGB, matching Blender's
// material data. These values affect viewport display only; they do not alter
// the exact NURBS geometry stored in SceneObject::geometry.
struct WorkbenchMaterialData {
    QVector3D baseColorSceneLinear{0.8f, 0.8f, 0.8f};
    qreal roughness = 0.4;
    qreal metallic = 0.0;
    qreal alpha = 1.0;
};

struct SceneObject {
    ObjectId id = ObjectId::invalid();
    LayerId layerId = LayerId::invalid();
    Shape geometry;
    // World-space translation applied after evaluating spatial NURBS surface
    // or solid geometry. Planar curves keep their existing workplane mapping.
    Point3D placementTranslation;
    // Blender Object.color default: opaque white in scene-linear RGBA.
    QVector4D displayColorSceneLinear{1.0f, 1.0f, 1.0f, 1.0f};
    WorkbenchMaterialData workbenchMaterial;
};

} // namespace classiCAD
