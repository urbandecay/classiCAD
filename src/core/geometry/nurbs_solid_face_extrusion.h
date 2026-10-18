#pragma once

#include "nurbs_solid.h"
#include <QString>

namespace classiCAD {

enum class SolidFaceExtrusionError { None, InvalidOperation, UnsupportedBoundary };

// A complete, immutable candidate. Callers use the same candidate for preview
// and commit; a rejected operation never partially edits the source shell.
struct SolidFaceExtrusionResult {
    NurbsExtrusionSolid3D solid;
    int faceIndex = -1;
    QString error;
    SolidFaceExtrusionError errorCode = SolidFaceExtrusionError::None;
    bool succeeded() const { return error.isEmpty() && faceIndex >= 0; }
};

SolidFaceExtrusionResult planNurbsSolidFaceExtrusion(
    const NurbsExtrusionSolid3D &solid, int faceIndex,
    const Point3D &displacement, bool manifold);

} // namespace classiCAD
