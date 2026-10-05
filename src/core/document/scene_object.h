#pragma once

#include "layer_id.h"
#include "object_id.h"
#include "shape.h"

namespace classiCAD {

struct SceneObject {
    ObjectId id = ObjectId::invalid();
    LayerId layerId = LayerId::invalid();
    Shape geometry;
    // World-space translation applied after evaluating spatial NURBS surface
    // or solid geometry. Planar curves keep their existing workplane mapping.
    Point3D placementTranslation;
};

} // namespace classiCAD
