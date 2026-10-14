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
png_rows_udim = (
    b"\x00" + bytes((10, 90, 240, 255, 210, 30, 70, 255)),
    b"\x00" + bytes((70, 200, 120, 255, 15, 25, 35, 128)),
)
png_data_udim = (
    b"\x89PNG\r\n\x1a\n" +
    png_chunk(b"IHDR", struct.pack(">IIBBBBB", 2, 2, 8, 6, 0, 0, 0)) +
    png_chunk(b"IDAT", zlib.compress(b"".join(png_rows_udim))) +
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

    # File-backed float images can use Blender's own loader/upload path. The
    # scene-linear payload is present too, but the packed EXR remains the
    # authoritative proxy source so Blender applies its original alpha mode.
    float_exr_path = pathlib.Path(directory) / "float-source.exr"
    float_pixels = [
        -0.25, 0.75, 2.5, 0.0,
        0.125, -1.0, 4.0, 0.4,
        10.0, 0.5, -2.0, 1.0,
    ]
    float_source = bpy.data.images.new(
        "ProxyContract.FloatSource", width=3, height=1,
        float_buffer=True, alpha=True,
    )
    float_source.colorspace_settings.name = "Non-Color"
    float_source.alpha_mode = "STRAIGHT"
    float_source.pixels.foreach_set(float_pixels)
    float_source.file_format = "OPEN_EXR"
    previous_color_mode = bpy.context.scene.render.image_settings.color_mode
    bpy.context.scene.render.image_settings.color_mode = "RGBA"
    float_source.filepath_raw = str(float_exr_path)
    float_source.save()
    bpy.context.scene.render.image_settings.color_mode = previous_color_mode
    float_exr_data = float_exr_path.read_bytes()
    float_material_record = {
        "id": "material-float-proxy", "name": "Float Proxy",
        "baseColorSceneLinear": [1, 1, 1, 1], "roughness": 0.5,
        "metallic": 0.0,
        "imageTextures": [{
            "nodeName": "ProxyContract.FloatTexture",
            "nodeType": "TEX_IMAGE",
            "imageName": "ProxyContract.FloatImage",
            "filepath": "",
            "colorSpace": "Non-Color", "alphaMode": "STRAIGHT",
            "interpolation": "Linear", "extension": "REPEAT",
            "sourceTransformId": "blender-float-buffer",
            "sceneLinearFloatDataBase64": base64.b64encode(
                b"".join(struct.pack("<f", value) for value in float_pixels)
            ).decode("ascii"),
            "packedDataBase64": base64.b64encode(float_exr_data).decode("ascii"),
            "packed": True, "sourceFileExists": False, "tiles": [],
        }],
    }
    document["materials"].append(float_material_record)
    float_material = adapter.classiCAD_materials(document)["material-float-proxy"]
    obj.data.materials.append(float_material)
    float_texture_node = next(
        node for node in float_material.node_tree.nodes
        if node.get("classiCAD_proxy_image_node")
    )
    assert float_texture_node.show_texture
    assert float_texture_node.image.is_float
    assert float_texture_node.image.alpha_mode == "STRAIGHT"
    assert float_texture_node.image.colorspace_settings.name == "Non-Color"
    assert all(math.isclose(actual, expected, abs_tol=1.0e-6)
               for actual, expected in zip(float_texture_node.image.pixels[:], float_pixels))
    assert bytes(float_texture_node.image.packed_file.data) == float_exr_data

    # Source-less float payloads become packed scene-linear EXRs. PREMUL data
    # is already multiplied by alpha in the transport, so the proxy preserves
    # its values and restores the original alpha mode without multiplying RGB
    # a second time.
    payload_modes = {
        "STRAIGHT": [0.8, 0.2, -0.4, 0.25, 1.5, -2.0, 0.75, 0.5],
        "PREMUL": [0.2, 0.05, -0.1, 0.25, 0.75, -1.0, 0.375, 0.5],
        "NONE": [0.8, 0.2, -0.4, 0.25, 1.5, -2.0, 0.75, 0.5],
        "CHANNEL_PACKED": [0.8, 0.2, -0.4, 0.25, 1.5, -2.0, 0.75, 0.5],
    }
    float_payload_materials = []
    for alpha_mode, expected_pixels in payload_modes.items():
        material_id = "material-float-payload-" + alpha_mode.lower()
        source_color_space = "ACEScg" if alpha_mode == "PREMUL" else "sRGB"
        transform_id = "a" * 64 if alpha_mode == "PREMUL" else "blender-float-buffer"
        document["materials"].append({
            "id": material_id, "name": "Float Payload " + alpha_mode,
            "baseColorSceneLinear": [1, 1, 1, 1], "roughness": 0.5,
            "metallic": 0.0,
            "imageTextures": [{
                "nodeName": "ProxyContract.Payload." + alpha_mode,
                "nodeType": "TEX_IMAGE",
                "imageName": "ProxyContract.PayloadImage." + alpha_mode,
                "filepath": "", "colorSpace": source_color_space,
                "alphaMode": alpha_mode, "interpolation": "Linear",
                "extension": "REPEAT", "sourceTransformId": transform_id,
                "sceneLinearFloatWidth": 2, "sceneLinearFloatHeight": 1,
                "sceneLinearFloatDataBase64": base64.b64encode(
                    b"".join(struct.pack("<f", value)
                             for value in expected_pixels)
                ).decode("ascii"),
                "packedDataBase64": "", "packed": False,
                "sourceFileExists": False, "tiles": [],
            }],
        })
        float_payload_materials.append((material_id, alpha_mode, source_color_space,
                                        expected_pixels))
    assert adapter._create_classicad_float_payload_image({
        "sceneLinearFloatWidth": 3, "sceneLinearFloatHeight": 1,
        "sceneLinearFloatDataBase64": base64.b64encode(
            struct.pack("<8f", *payload_modes["STRAIGHT"])
        ).decode("ascii"),
    }) is None
    assert adapter._create_classicad_float_payload_image({
        "sceneLinearFloatWidth": 1, "sceneLinearFloatHeight": 1,
        "sceneLinearFloatDataBase64": base64.b64encode(
            struct.pack("<4f", float("nan"), 0.0, 0.0, 1.0)
        ).decode("ascii"),
    }) is None
    payload_material_map = adapter.classiCAD_materials(document)
    for material_id, alpha_mode, source_color_space, expected_pixels in float_payload_materials:
        payload_material = payload_material_map[material_id]
        obj.data.materials.append(payload_material)
        payload_node = next(
            node for node in payload_material.node_tree.nodes
            if node.get("classiCAD_proxy_image_node")
        )
        payload_image = payload_node.image
        assert payload_node.show_texture and payload_node.interpolation == "Linear"
        assert payload_image.is_float and payload_image.alpha_mode == alpha_mode
        assert payload_image.colorspace_settings.name == "Non-Color"
        assert payload_image.get("classiCAD_source_alpha_mode") == alpha_mode
        assert payload_image.get("classiCAD_source_color_space") == source_color_space
        assert all(math.isclose(actual, expected, abs_tol=1.0e-6)
                   for actual, expected in zip(payload_image.pixels[:], expected_pixels))
        assert payload_image.packed_file is not None
    image_count_after_float_sync = sum(
        1 for image in bpy.data.images if image.get("classiCAD_proxy_image")
    )
    adapter.classiCAD_materials(document)
    assert sum(1 for image in bpy.data.images
               if image.get("classiCAD_proxy_image")) == image_count_after_float_sync

    # Rebuild packed and external UDIM sources as packed Blender tiled images.
    packed_udim_template = str(pathlib.Path(directory) / "packed.<UDIM>.png")
    external_udim_template = str(pathlib.Path(directory) / "external.<UDIM>.png")
    external_udim_paths = []
    for number, payload in ((1001, png_data), (1002, png_data_udim)):
        external_tile_path = pathlib.Path(
            external_udim_template.replace("<UDIM>", str(number))
        )
        external_tile_path.write_bytes(payload)
        external_udim_paths.append((number, payload, external_tile_path))

    udim_records = [
        {
            "id": "material-udim-packed", "name": "UDIM Packed",
            "baseColorSceneLinear": [1, 1, 1, 1], "roughness": 0.5,
            "metallic": 0.0,
            "imageTextures": [{
                "nodeName": "ProxyContract.PackedUDIM",
                "nodeType": "TEX_IMAGE",
                "imageName": "ProxyContract.PackedUDIMImage",
                "filepath": packed_udim_template,
                "colorSpace": "sRGB", "alphaMode": "STRAIGHT",
                "interpolation": "Closest", "extension": "MIRROR",
                "sourceTransformId": "identity", "packedDataBase64": "",
                "packed": True, "sourceFileExists": True,
                "sceneLinearFloatDataBase64": "",
                "tiles": [
                    {"number": 1001,
                     "filepath": packed_udim_template.replace("<UDIM>", "1001"),
                     "sourceFileExists": False,
                     "packedDataBase64": base64.b64encode(png_data).decode("ascii"),
                     "sceneLinearFloatDataBase64": ""},
                    {"number": 1002,
                     "filepath": packed_udim_template.replace("<UDIM>", "1002"),
                     "sourceFileExists": False,
                     "packedDataBase64": base64.b64encode(png_data_udim).decode("ascii"),
                     "sceneLinearFloatDataBase64": ""},
                ],
            }],
        },
        {
            "id": "material-udim-external", "name": "UDIM External",
            "baseColorSceneLinear": [1, 1, 1, 1], "roughness": 0.5,
            "metallic": 0.0,
            "imageTextures": [{
                "nodeName": "ProxyContract.ExternalUDIM",
                "nodeType": "TEX_IMAGE",
                "imageName": "ProxyContract.ExternalUDIMImage",
                "filepath": external_udim_template,
                "colorSpace": "sRGB", "alphaMode": "STRAIGHT",
                "interpolation": "Linear", "extension": "REPEAT",
                "sourceTransformId": "identity", "packedDataBase64": "",
                "packed": False, "sourceFileExists": True,
                "sceneLinearFloatDataBase64": "",
                "tiles": [
                    {"number": number, "filepath": str(path),
                     "sourceFileExists": True, "packedDataBase64": "",
                     "sceneLinearFloatDataBase64": ""}
                    for number, _, path in external_udim_paths
                ],
            }],
        },
    ]
    document["materials"].extend(udim_records)
    udim_materials = adapter.classiCAD_materials(document)
    obj.data.materials.append(udim_materials["material-udim-packed"])
    obj.data.materials.append(udim_materials["material-udim-external"])
    for material_id, expected_node, expected_interpolation, expected_extension in (
            ("material-udim-packed", "ProxyContract.PackedUDIM", "Closest", "MIRROR"),
            ("material-udim-external", "ProxyContract.ExternalUDIM", "Linear", "REPEAT")):
        udim_material = next(
            material for material in bpy.data.materials
            if material.get("classiCAD_material_id") == material_id
        )
        udim_node = next(
            node for node in udim_material.node_tree.nodes
            if node.get("classiCAD_proxy_image_node")
        )
        assert udim_node.name == expected_node
        assert udim_node.type == "TEX_IMAGE" and udim_node.show_texture
        assert udim_material.node_tree.nodes.active == udim_node
        assert udim_node.interpolation == expected_interpolation
        assert udim_node.extension == expected_extension
        assert udim_node.image.source == "TILED"
        assert sorted(tile.number for tile in udim_node.image.tiles) == [1001, 1002]
        packed_tiles = {
            packed.tile_number: bytes(packed.packed_file.data)
            for packed in udim_node.image.packed_files
        }
        assert packed_tiles == {1001: png_data, 1002: png_data_udim}

    image_count_after_udim_sync = sum(
        1 for image in bpy.data.images if image.get("classiCAD_proxy_image")
    )
    adapter.classiCAD_materials(document)
    assert sum(1 for image in bpy.data.images
               if image.get("classiCAD_proxy_image")) == image_count_after_udim_sync

    udim_temporary_templates = []
    for material_id in ("material-udim-packed", "material-udim-external"):
        udim_material = next(
            material for material in bpy.data.materials
            if material.get("classiCAD_material_id") == material_id
        )
        udim_node = next(
            node for node in udim_material.node_tree.nodes
            if node.get("classiCAD_proxy_image_node")
        )
        udim_temporary_templates.append(
            (pathlib.Path(bpy.path.abspath(udim_node.image.filepath)),
             tuple(tile.number for tile in udim_node.image.tiles))
        )

    blend_path = pathlib.Path(directory) / "proxy.blend"
    bpy.ops.wm.save_as_mainfile(filepath=str(blend_path))
    # The proxy has embedded bytes; the adapter's temporary source tiles can
    # disappear before the project is reopened.
    for template_path, numbers in udim_temporary_templates:
        for number in numbers:
            pathlib.Path(str(template_path).replace(
                "<UDIM>", str(number)
            )).unlink(missing_ok=True)
        template_path.parent.rmdir()
    float_exr_path.unlink()
    for temporary_float_path in adapter.TEMPORARY_PROXY_IMAGE_PATHS:
        pathlib.Path(temporary_float_path).unlink(missing_ok=True)
    adapter.TEMPORARY_PROXY_IMAGE_PATHS.clear()
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
    float_material = next(
        material for material in bpy.data.materials
        if material.get("classiCAD_material_id") == "material-float-proxy"
    )
    float_texture_node = next(
        node for node in float_material.node_tree.nodes
        if node.get("classiCAD_proxy_image_node")
    )
    assert float_texture_node.show_texture
    assert float_texture_node.image.is_float
    assert float_texture_node.image.alpha_mode == "STRAIGHT"
    assert float_texture_node.image.colorspace_settings.name == "Non-Color"
    assert all(math.isclose(actual, expected, abs_tol=1.0e-6)
               for actual, expected in zip(float_texture_node.image.pixels[:], float_pixels))
    assert bytes(float_texture_node.image.packed_file.data) == float_exr_data
    for material_id, alpha_mode, source_color_space, expected_pixels in float_payload_materials:
        payload_material = next(
            material for material in bpy.data.materials
            if material.get("classiCAD_material_id") == material_id
        )
        payload_node = next(
            node for node in payload_material.node_tree.nodes
            if node.get("classiCAD_proxy_image_node")
        )
        payload_image = payload_node.image
        assert payload_node.show_texture and payload_node.interpolation == "Linear"
        assert payload_image.is_float and payload_image.alpha_mode == alpha_mode
        assert payload_image.colorspace_settings.name == "Non-Color"
        assert payload_image.get("classiCAD_source_alpha_mode") == alpha_mode
        assert payload_image.get("classiCAD_source_color_space") == source_color_space
        assert all(math.isclose(actual, expected, abs_tol=1.0e-6)
                   for actual, expected in zip(payload_image.pixels[:], expected_pixels))
        assert payload_image.packed_file is not None
    for material_id, expected_node, expected_tiles in (
            ("material-udim-packed", "ProxyContract.PackedUDIM",
             {1001: png_data, 1002: png_data_udim}),
            ("material-udim-external", "ProxyContract.ExternalUDIM",
             {1001: png_data, 1002: png_data_udim})):
        udim_material = next(
            material for material in bpy.data.materials
            if material.get("classiCAD_material_id") == material_id
        )
        udim_node = next(
            node for node in udim_material.node_tree.nodes
            if node.get("classiCAD_proxy_image_node")
        )
        assert udim_node.name == expected_node
        assert udim_node.image.source == "TILED"
        packed_tiles = {
            packed.tile_number: bytes(packed.packed_file.data)
            for packed in udim_node.image.packed_files
        }
        assert packed_tiles == expected_tiles

print("Blender project proxy UV, UDIM, and face-material contract passed")
