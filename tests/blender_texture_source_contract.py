"""Focused Blender 5.2.2 contract for Workbench's active texture node."""

import importlib.util
import pathlib
import sys

import bpy


root = pathlib.Path(__file__).resolve().parents[1]
extractor_path = root / "src/core/serialization/blender_scene_extract.py"
spec = importlib.util.spec_from_file_location("classicad_scene_extract", extractor_path)
extractor = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = extractor
spec.loader.exec_module(extractor)

material = bpy.data.materials.new("ActiveTextureContract")
material.use_nodes = True
nodes = material.node_tree.nodes
nodes.clear()
image_node = nodes.new("ShaderNodeTexImage")
image_node.image = bpy.data.images.new("WorkbenchActiveImage", width=2, height=2)
image_node.show_texture = True
ordinary_active_node = nodes.new("ShaderNodeValue")
nodes.active = ordinary_active_node

references = extractor.image_references(material)
assert len(references) == 1, references
assert references[0]["image_name"] == "WorkbenchActiveImage", references
assert nodes.active == ordinary_active_node

print("active Workbench texture node contract passed")
