"""Exercise stable per-face assignment transport on grouped Blender proxies.

Run with Blender 5.2.2: blender --background --python
tests/blender_project_proxy_contract.py
"""

import importlib.util
import copy
import pathlib
import sys

import bpy


root = pathlib.Path(__file__).resolve().parents[1]
adapter_path = root / "src/core/serialization/blender_project_adapter.py"
spec = importlib.util.spec_from_file_location("classiCAD_project_adapter", adapter_path)
adapter = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = adapter
spec.loader.exec_module(adapter)

material_a = bpy.data.materials.new("ProxyContract.A")
material_a["classiCAD_material_id"] = "material-a"
material_b = bpy.data.materials.new("ProxyContract.B")
material_b["classiCAD_material_id"] = "material-b"
document = {
    "materials": [
        {"id": "material-a", "name": "A", "baseColorSceneLinear": [1, 0, 0, 1]},
        {"id": "material-b", "name": "B", "baseColorSceneLinear": [0, 0, 1, 1]},
    ],
    "objects": [
        {
            "id": 41,
            "geometry": {"geometryType": 15, "nurbsSolid": {
                "baseCapFaceId": "base-41", "displacedCapFaceId": "top-41",
                "wallFaceIds": ["wall-41"]}},
            "nurbsSolidFaceMaterialIds": {"base-41": "material-a"},
        },
        {
            "id": 42,
            "geometry": {"geometryType": 15, "nurbsSolid": {
                "baseCapFaceId": "base-42", "displacedCapFaceId": "top-42",
                "wallFaceIds": ["wall-42"]}},
            "nurbsSolidFaceMaterialIds": {},
        },
    ],
}
exact_geometry_before = {
    str(record["id"]): copy.deepcopy(record["geometry"])
    for record in document["objects"]
}

# One grouped proxy contains two seed solids; its stable references do not
# depend on polygon order or triangle count.
mesh = bpy.data.meshes.new("ProxyContract.Group")
mesh.from_pydata(
    [(0, 0, 0), (1, 0, 0), (0, 1, 0), (0, 0, 1), (1, 1, 1)], [],
    [(0, 1, 2), (0, 2, 3), (1, 2, 4), (0, 3, 4)],
)
mesh.update()
references = [
    ("41", "base-41"), ("41", "base-41"),
    ("41", "wall-41"), ("42", "base-42"),
]
adapter.attach_solid_face_metadata(mesh, references, document)
obj = bpy.data.objects.new("classiCAD.surface-proxy.contract", mesh)
obj["classiCAD_proxy_geometry"] = "NurbsSurface and NurbsSolid"
bpy.context.scene.collection.objects.link(obj)

slots = {
    str(slot.material.get("classiCAD_material_id", "")): index
    for index, slot in enumerate(obj.material_slots)
}
mesh.polygons[0].material_index = slots["material-b"]
mesh.polygons[1].material_index = slots["material-b"]
mesh.polygons[2].material_index = slots["material-a"]
mesh.polygons[3].material_index = slots["material-b"]
adapter.merge_solid_proxy_materials(document)
records = {str(record["id"]): record for record in document["objects"]}
assert records["41"]["nurbsSolidFaceMaterialIds"] == {
    "base-41": "material-b", "wall-41": "material-a"
}
assert records["42"]["nurbsSolidFaceMaterialIds"] == {
    "base-42": "material-b"
}

# Mixed polygons in one semantic face are ignored as a whole, preserving the
# embedded exact document's previous assignment.
records["41"]["nurbsSolidFaceMaterialIds"]["base-41"] = "material-a"
mesh.polygons[1].material_index = slots["material-a"]
adapter.merge_solid_proxy_materials(document)
assert records["41"]["nurbsSolidFaceMaterialIds"]["base-41"] == "material-a"

# A uniformly selected classiCAD.Unassigned slot explicitly clears the
# assignment, while a missing/unknown slot remains unresolved.
unassigned_slot = next(
    index for index, slot in enumerate(obj.material_slots)
    if slot.material and slot.material.get("classiCAD_unassigned_material_slot")
)
mesh.polygons[0].material_index = unassigned_slot
mesh.polygons[1].material_index = unassigned_slot
adapter.merge_solid_proxy_materials(document)
assert "base-41" not in records["41"]["nurbsSolidFaceMaterialIds"]
assert all(record["geometry"] == exact_geometry_before[str(record["id"])]
           for record in document["objects"])

print("Blender project solid proxy face-material contract passed")
