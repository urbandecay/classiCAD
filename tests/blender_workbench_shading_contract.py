"""Verify Workbench import ignores Eevee-only Studio background controls."""

import importlib.util
import pathlib
import sys

import bpy


assert bpy.app.version[:3] == (5, 2, 2), bpy.app.version_string
root = pathlib.Path(__file__).resolve().parents[1]
extractor_path = root / "src/core/serialization/blender_scene_extract.py"
spec = importlib.util.spec_from_file_location(
    "classicad_scene_extract", extractor_path
)
extractor = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = extractor
spec.loader.exec_module(extractor)

scene = bpy.context.scene
shading = scene.display.shading
shading.light = "STUDIO"
shading.studiolight_background_alpha = 0.37
shading.studiolight_background_blur = 0.19

record = extractor.workbench_shading_record(scene, shading)
assert abs(record["studiolight_background_alpha"] - 0.37) < 1.0e-6, record
assert abs(record["studiolight_background_blur"] - 0.19) < 1.0e-6, record
assert "studiolight_background_alpha" not in record["unsupported"], record
assert "studiolight_background_blur" not in record["unsupported"], record

print("Workbench shading ignores Eevee-only Studio background controls")
