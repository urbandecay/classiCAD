#pragma once

#include "layer_id.h"
#include "object_id.h"
#include "../model.h"

namespace classiCAD {

struct SceneObject {
    ObjectId id = ObjectId::invalid();
    LayerId layerId = LayerId::invalid();
    Shape geometry;
};

} // namespace classiCAD
