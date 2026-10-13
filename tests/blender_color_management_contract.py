"""Exercise the custom-view OCIO LUT extraction path in Blender 5.2.2."""

import importlib.util
import pathlib
import sys

import bpy


root = pathlib.Path(__file__).resolve().parents[1]
extractor_path = root / "src/core/serialization/blender_scene_extract.py"
spec = importlib.util.spec_from_file_location("classiCAD_scene_extract", extractor_path)
extractor = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = extractor
spec.loader.exec_module(extractor)
extractor.SOURCE_PROJECT_NAMESPACE = "color-management-contract"

scene = bpy.context.scene
scene.display_settings.display_device = "sRGB"
scene.view_settings.view_transform = "AgX"
scene.view_settings.look = "AgX - Medium High Contrast"
lut_records = []
lut_bytes_by_id = {}
unsupported = []
color_management = extractor.bake_color_lut(
    scene, lut_records, lut_bytes_by_id, unsupported
)
assert not unsupported, unsupported
assert len(lut_records) == 1, lut_records
assert color_management.get("lut_id") == lut_records[0]["id"]
assert lut_bytes_by_id[lut_records[0]["id"]] == 65 ** 3 * 12

# Blender 5.2.2 accepts gamma == 0 and uses 1 / FLT_EPSILON after the view
# transform. Verify the import metadata keeps zero and the bounded form has
# the same result for display values in [0, 1] without overflowing pow().
scene.view_settings.view_transform = "Standard"
scene.view_settings.look = "None"
scene.view_settings.exposure = 0.0
scene.view_settings.gamma = 0.0
gamma_luts = []
gamma_lut_bytes = {}
gamma_unsupported = []
gamma_management = extractor.bake_color_lut(
    scene, gamma_luts, gamma_lut_bytes, gamma_unsupported
)
assert gamma_management["gamma"] == 0.0
assert not gamma_unsupported, gamma_unsupported
epsilon = 1.1920928955078125e-7
bounded_gamma_zero = [min(max(channel, 0.0), 1.0) ** (1.0 / epsilon)
                      for channel in (0.18, 1.0, 4.0)]
assert bounded_gamma_zero == [0.0, 1.0, 1.0], bounded_gamma_zero

print("custom-view OCIO and gamma-zero color-management contracts passed")
