"""Create a Blender 5.2.2 linked-object fixture for classiCAD import tests.

Run with Blender 5.2.2:
  blender --background --python tests/blender_linked_random_import_fixture.py -- output-directory

The directory contains two source libraries and one host .blend file. Each
library exports a mesh object with the same name so the import contract can
verify that Blender's linked-library identity reaches classiCAD's viewport.
"""

import pathlib
import sys

import bpy


if bpy.app.version[:3] != (5, 2, 2):
    raise RuntimeError("This fixture is pinned to Blender 5.2.2")

arguments = sys.argv[sys.argv.index("--") + 1:]
if len(arguments) != 1:
    raise RuntimeError("Expected one output directory after --")
output_directory = pathlib.Path(arguments[0]).resolve()
output_directory.mkdir(parents=True, exist_ok=True)


def clear_scene():
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)


def create_library(path):
    clear_scene()
    mesh = bpy.data.meshes.new("LinkedFixtureMesh")
    mesh.from_pydata(
        [(-1, -1, -1), (-1, -1, 1), (-1, 1, -1), (-1, 1, 1),
         (1, -1, -1), (1, -1, 1), (1, 1, -1), (1, 1, 1)],
        [],
        [(0, 2, 6, 4), (1, 5, 7, 3), (0, 4, 5, 1),
         (4, 6, 7, 5), (1, 3, 2, 0), (2, 3, 7, 6)],
    )
    mesh.update()
    obj = bpy.data.objects.new("LinkedFixture", mesh)
    bpy.context.scene.collection.objects.link(obj)
    bpy.ops.wm.save_as_mainfile(filepath=str(path), check_existing=False)


library_a = output_directory / "linked-random-a.blend"
library_b = output_directory / "linked-random-b.blend"
host = output_directory / "linked-random-host.blend"
create_library(library_a)
create_library(library_b)

clear_scene()
bpy.ops.wm.save_as_mainfile(filepath=str(host), check_existing=False)
for library_path in (library_a, library_b):
    relative_library_path = "//" + library_path.name
    with bpy.data.libraries.load(relative_library_path, link=True) as (data_from, data_to):
        if "LinkedFixture" not in data_from.objects:
            raise RuntimeError("Source library is missing LinkedFixture")
        data_to.objects = ["LinkedFixture"]
    linked_object = data_to.objects[0]
    if linked_object is None or linked_object.library is None:
        raise RuntimeError("Blender did not link the fixture object")
    bpy.context.scene.collection.objects.link(linked_object)

bpy.ops.wm.save_as_mainfile(filepath=str(host), check_existing=False)

linked = [obj for obj in bpy.context.scene.objects if obj.name == "LinkedFixture"]
if len(linked) != 2:
    raise RuntimeError("Expected two same-named linked objects: %r" % linked)
if len({obj.library.filepath for obj in linked}) != 2:
    raise RuntimeError("The two linked objects must retain distinct library paths")
print("Created Blender 5.2.2 linked Random fixture: %s" % host)
