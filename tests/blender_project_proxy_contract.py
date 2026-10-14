"""Exercise stable per-face assignment transport on grouped Blender proxies.

Run with Blender 5.2.2: blender --background --python
tests/blender_project_proxy_contract.py
"""

import importlib.util
import copy
import base64
import binascii
import math
import pathlib
import struct
import sys
import tempfile
import zlib

import bpy


root = pathlib.Path(__file__).resolve().parents[1]
adapter_path = root / "src/core/serialization/blender_project_adapter.py"
spec = importlib.util.spec_from_file_location("classiCAD_project_adapter", adapter_path)
adapter = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = adapter
spec.loader.exec_module(adapter)

material_a = bpy.data.materials.new("ProxyContract.A")
material_a["classiCAD_material_id"] = "material-a"
material_a.roughness = 0.92
material_a.metallic = 0.03
material_b = bpy.data.materials.new("ProxyContract.B")
material_b["classiCAD_material_id"] = "material-b"
material_b.roughness = 0.08
material_b.metallic = 0.97
document = {
    "materials": [
        {"id": "material-a", "name": "A", "baseColorSceneLinear": [1, 0, 0, 1],
         "roughness": 0.17, "metallic": 0.76},
        {"id": "material-b", "name": "B", "baseColorSceneLinear": [0, 0, 1, 1],
         "roughness": 0.83, "metallic": 0.04},
        {"id": "material-c", "name": "C", "baseColorSceneLinear": [0, 1, 0, 1],
         "roughness": 0.39, "metallic": 0.61},
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

# Per-object proxy construction retains an independent UV pair for each
# polygon corner, which allows NURBS seams without duplicating mesh vertices.
single_proxy = adapter.add_nurbs_surface_mesh(
    bpy.context.scene.collection,
    "classiCAD.surface-proxy.uv-contract",
    {},
    {
        "vertices": [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)],
        "faces": [(0, 1, 2), (0, 2, 3)],
        "cornerUvs": [
            ((0.0, 0.0), (0.5, 0.0), (0.5, 1.0)),
            ((0.8, 0.2), (0.5, 1.0), (0.0, 1.0)),
        ],
    },
)
assert single_proxy.data.uv_layers.active.name == "classiCAD.ParametricUV"
assert single_proxy.data.uv_layers.active_render.name == "classiCAD.ParametricUV"
single_uvs = [tuple(loop.uv) for loop in single_proxy.data.uv_layers.active.data]
expected_single_uvs = [
    (0.0, 0.0), (0.5, 0.0), (0.5, 1.0),
    (0.8, 0.2), (0.5, 1.0), (0.0, 1.0),
]
assert len(single_uvs) == len(expected_single_uvs)
assert all(math.isclose(actual, expected, abs_tol=1.0e-6)
           for actual_pair, expected_pair in zip(single_uvs, expected_single_uvs)
           for actual, expected in zip(actual_pair, expected_pair))

# Grouping surfaces from different NURBS parameter domains preserves their
# per-face UV islands and does not normalize the whole layer as one object.
uv_layer_id = "uv-contract-layer"
uv_layer_collection = bpy.data.collections.new("ProxyContract.UVLayer")
bpy.context.scene.collection.children.link(uv_layer_collection)
adapter.create_grouped_scene_objects(
    {uv_layer_id: uv_layer_collection},
    [
        {"id": "uv-a", "layerId": uv_layer_id,
         "geometry": {"geometryType": 14}},
        {"id": "uv-b", "layerId": uv_layer_id,
         "geometry": {"geometryType": 14}},
    ],
    {
        "uv-a": {
            "vertices": [(0, 0, 0), (1, 0, 0), (0, 1, 0)],
            "faces": [(0, 1, 2)],
            "cornerUvs": [((0.0, 0.0), (1.0, 0.0), (0.0, 1.0))],
        },
        "uv-b": {
            "vertices": [(2, 0, 0), (3, 0, 0), (2, 1, 0)],
            "faces": [(0, 1, 2)],
            "cornerUvs": [((0.25, 0.75), (0.75, 0.75), (0.25, 0.25))],
        },
    },
    {"materials": []},
)
grouped_uv_proxy = next(
    obj for obj in bpy.data.objects
    if obj.get("classiCAD_layer_id") == uv_layer_id
)
assert grouped_uv_proxy.data.uv_layers.active.name == "classiCAD.ParametricUV"
grouped_uvs = [tuple(loop.uv)
               for loop in grouped_uv_proxy.data.uv_layers.active.data]
expected_grouped_uvs = [
    (0.0, 0.0), (1.0, 0.0), (0.0, 1.0),
    (0.25, 0.75), (0.75, 0.75), (0.25, 0.25),
]
assert len(grouped_uvs) == len(expected_grouped_uvs)
assert all(math.isclose(actual, expected, abs_tol=1.0e-6)
           for actual_pair, expected_pair in zip(grouped_uvs, expected_grouped_uvs)
           for actual, expected in zip(actual_pair, expected_pair))

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
proxy_materials = adapter.classiCAD_materials(document)
assert math.isclose(proxy_materials["material-a"].roughness, 0.17, abs_tol=1.0e-6)
assert math.isclose(proxy_materials["material-a"].metallic, 0.76, abs_tol=1.0e-6)
assert tuple(proxy_materials["material-a"].diffuse_color) == (1.0, 0.0, 0.0, 1.0)
assert math.isclose(proxy_materials["material-b"].roughness, 0.83, abs_tol=1.0e-6)
assert math.isclose(proxy_materials["material-b"].metallic, 0.04, abs_tol=1.0e-6)
assert tuple(proxy_materials["material-b"].diffuse_color) == (0.0, 0.0, 1.0, 1.0)
assert math.isclose(proxy_materials["material-c"].roughness, 0.39, abs_tol=1.0e-6)
assert math.isclose(proxy_materials["material-c"].metallic, 0.61, abs_tol=1.0e-6)
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


def png_chunk(kind, payload):
    return (struct.pack(">I", len(payload)) + kind + payload +
            struct.pack(">I", binascii.crc32(kind + payload) & 0xFFFFFFFF))


png_rows = (
    b"\x00" + bytes((240, 20, 30, 255, 20, 230, 40, 255)),
    b"\x00" + bytes((30, 40, 220, 255, 200, 180, 10, 64)),
)
png_data = (
    b"\x89PNG\r\n\x1a\n" +
    png_chunk(b"IHDR", struct.pack(">IIBBBBB", 2, 2, 8, 6, 0, 0, 0)) +
    png_chunk(b"IDAT", zlib.compress(b"".join(png_rows))) +
    png_chunk(b"IEND", b"")
)
material_a_record = next(
    record for record in document["materials"] if record["id"] == "material-a"
)
material_a_record["imageTextures"] = [{
    "nodeName": "ProxyContract.ActiveTexture",
    "nodeType": "TEX_IMAGE",
    "imageName": "ProxyContract.Texture",
    "imageLibraryPath": "",
    "filepath": "ProxyContract.png",
    "colorSpace": "sRGB",
    "alphaMode": "STRAIGHT",
    "interpolation": "Closest",
    "extension": "MIRROR",
    "sourceTransformId": "identity",
    "sceneLinearFloatDataBase64": "",
    "packedDataBase64": base64.b64encode(png_data).decode("ascii"),
    "packed": True,
    "sourceFileExists": True,
    "tiles": [],
}]
material_c_record = next(
    record for record in document["materials"] if record["id"] == "material-c"
)
material_c_record["imageTextures"] = [{
    "nodeName": "ProxyContract.EnvironmentTexture",
    "nodeType": "TEX_ENVIRONMENT",
    "imageName": "ProxyContract.EnvironmentImage",
    "filepath": "ProxyContractEnvironment.png",
    "colorSpace": "sRGB",
    "alphaMode": "STRAIGHT",
    "interpolation": "Closest",
    "extension": "EXTEND",
    "sourceTransformId": "identity",
    "sceneLinearFloatDataBase64": "",
    "packedDataBase64": base64.b64encode(png_data).decode("ascii"),
    "packed": True,
    "sourceFileExists": True,
    "tiles": [],
}]
adapter.classiCAD_materials(document)
material_a = next(
    material for material in bpy.data.materials
    if material.get("classiCAD_material_id") == "material-a"
)
active_texture_node = next(
    node for node in material_a.node_tree.nodes
    if node.get("classiCAD_proxy_image_node")
)
assert active_texture_node.type == "TEX_IMAGE"
assert active_texture_node.show_texture
assert material_a.node_tree.nodes.active == active_texture_node
assert active_texture_node.interpolation == "Closest"
assert active_texture_node.extension == "MIRROR"
assert active_texture_node.image.alpha_mode == "STRAIGHT"
assert active_texture_node.image.colorspace_settings.name == "sRGB"
assert active_texture_node.image.packed_file is not None
assert bytes(active_texture_node.image.packed_file.data) == png_data
material_c = next(
    material for material in bpy.data.materials
    if material.get("classiCAD_material_id") == "material-c"
)
environment_texture_node = next(
    node for node in material_c.node_tree.nodes
    if node.get("classiCAD_proxy_image_node")
)
assert environment_texture_node.type == "TEX_ENVIRONMENT"
assert environment_texture_node.show_texture
assert material_c.node_tree.nodes.active == environment_texture_node
assert environment_texture_node.interpolation == "Closest"
assert not hasattr(environment_texture_node, "extension")
assert environment_texture_node.image.packed_file is not None
assert bytes(environment_texture_node.image.packed_file.data) == png_data
packed_proxy_image_count = sum(
    1 for image in bpy.data.images if image.get("classiCAD_proxy_image")
)
assert packed_proxy_image_count == 2

with tempfile.TemporaryDirectory(prefix="classiCAD_proxy_contract_") as directory:
    external_path = pathlib.Path(directory) / "external.png"
    external_path.write_bytes(png_data)
    material_b_record = next(
        record for record in document["materials"] if record["id"] == "material-b"
    )
    material_b_record["imageTextures"] = [{
        "nodeName": "ProxyContract.ExternalTexture",
        "nodeType": "TEX_IMAGE",
        "imageName": "ProxyContract.ExternalTextureImage",
        "filepath": str(external_path),
        "colorSpace": "sRGB",
        "alphaMode": "STRAIGHT",
        "interpolation": "Linear",
        "extension": "REPEAT",
        "sourceTransformId": "identity",
        "packedDataBase64": "",
        "packed": False,
        "sourceFileExists": True,
        "tiles": [],
    }]
    adapter.classiCAD_materials(document)
    image_count_after_external_sync = sum(
        1 for image in bpy.data.images if image.get("classiCAD_proxy_image")
    )
    assert image_count_after_external_sync == packed_proxy_image_count + 1
    adapter.classiCAD_materials(document)
    assert sum(1 for image in bpy.data.images
               if image.get("classiCAD_proxy_image")) == image_count_after_external_sync
    material_b = next(
        material for material in bpy.data.materials
        if material.get("classiCAD_material_id") == "material-b"
    )
    external_texture_node = next(
        node for node in material_b.node_tree.nodes
        if node.get("classiCAD_proxy_image_node")
    )
    assert external_texture_node.show_texture
    assert bpy.path.abspath(external_texture_node.image.filepath) == str(external_path)

    blend_path = pathlib.Path(directory) / "proxy.blend"
    bpy.ops.wm.save_as_mainfile(filepath=str(blend_path))
    bpy.ops.wm.open_mainfile(filepath=str(blend_path))
    material_a = next(
        material for material in bpy.data.materials
        if material.get("classiCAD_material_id") == "material-a"
    )
    active_texture_node = next(
        node for node in material_a.node_tree.nodes
        if node.get("classiCAD_proxy_image_node")
    )
    assert active_texture_node.show_texture
    assert active_texture_node.interpolation == "Closest"
    assert active_texture_node.extension == "MIRROR"
    assert active_texture_node.image.packed_file is not None
    assert bytes(active_texture_node.image.packed_file.data) == png_data
    material_c = next(
        material for material in bpy.data.materials
        if material.get("classiCAD_material_id") == "material-c"
    )
    environment_texture_node = next(
        node for node in material_c.node_tree.nodes
        if node.get("classiCAD_proxy_image_node")
    )
    assert environment_texture_node.type == "TEX_ENVIRONMENT"
    assert environment_texture_node.show_texture
    assert environment_texture_node.interpolation == "Closest"
    assert environment_texture_node.image.packed_file is not None
    assert bytes(environment_texture_node.image.packed_file.data) == png_data
    material_b = next(
        material for material in bpy.data.materials
        if material.get("classiCAD_material_id") == "material-b"
    )
    external_texture_node = next(
        node for node in material_b.node_tree.nodes
        if node.get("classiCAD_proxy_image_node")
    )
    assert external_texture_node.show_texture
    assert bpy.path.abspath(external_texture_node.image.filepath) == str(external_path)

print("Blender project proxy UV and solid face-material contract passed")
