#pragma once

#include "layer_id.h"
#include "object_id.h"
#include "shape.h"

namespace classiCAD {

struct SceneObject {
    ObjectId id = ObjectId::invalid();
    // Geometry objects can belong to a logical object container. A root
    // object's containerId equals its own id; legacy/uninitialized records
    // are normalized to that form by Document.
    ObjectId containerId = ObjectId::invalid();
    LayerId layerId = LayerId::invalid();
    Shape geometry;
    // World-space translation applied after evaluating spatial NURBS surface
    // or solid geometry. Planar curves keep their existing workplane mapping.
    Point3D placementTranslation;
};

} // namespace classiCAD
