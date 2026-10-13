"""Check resolved Blender render filepath transport extraction."""

import importlib.util
import os
import pathlib
import sys
import tempfile

import bpy


root = pathlib.Path(__file__).resolve().parents[1]
extractor_path = root / "src/core/serialization/blender_scene_extract.py"
spec = importlib.util.spec_from_file_location("classiCAD_scene_extract", extractor_path)
extractor = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = extractor
spec.loader.exec_module(extractor)

scene = bpy.context.scene
with tempfile.TemporaryDirectory(prefix="classicad-render-path-") as temp_dir:
    project_path = os.path.join(temp_dir, "project.blend")
    bpy.ops.wm.save_as_mainfile(filepath=project_path)
    scene.render.filepath = "//renders/final_"
    expected = os.path.join(temp_dir, "renders", "final_")
    assert extractor.resolved_render_filepath(scene.render) == expected

    scene.render.filepath = "//renders/frame_####"
    expected_tokens = os.path.join(temp_dir, "renders", "frame_####")
    assert extractor.resolved_render_filepath(scene.render) == expected_tokens

scene.render.filepath = ""
assert extractor.resolved_render_filepath(scene.render) == ""
