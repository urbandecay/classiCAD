"""Create a paired linked-object fixture and Blender Workbench reference.

Run with Blender 5.2.2:
  blender --background --python tests/blender_linked_random_import_fixture.py -- OUTPUT_DIRECTORY

The output directory contains two source libraries, the linked host project,
and its paired Workbench reference image.
"""

import pathlib
import sys

import bpy
from bpy_extras.object_utils import world_to_camera_view
from mathutils import Vector


PINNED_BLENDER_VERSION = (5, 2, 2)
OBJECT_NAME = "LinkedFixture"
IMAGE_SIZE = (640, 480)


def output_path_from_args():
    if "--" not in sys.argv:
        raise RuntimeError("usage: blender --background --python "
                           "tests/blender_linked_random_import_fixture.py -- OUTPUT_DIRECTORY")
    args = sys.argv[sys.argv.index("--") + 1:]
    if len(args) != 1:
        raise RuntimeError("expected exactly one output directory")
    path = pathlib.Path(args[0]).expanduser().resolve()
    path.mkdir(parents=True, exist_ok=True)
    return path


def create_library(path, location):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.mesh.primitive_cube_add(size=0.8, location=(0.0, 0.0, 0.0))
    obj = bpy.context.object
    obj.name = OBJECT_NAME
    obj.data.name = OBJECT_NAME + "Mesh"
    obj.location = location
    bpy.ops.wm.save_as_mainfile(filepath=str(path), check_existing=False)


def load_linked_object(path, collection):
    with bpy.data.libraries.load(str(path), link=True) as (data_from, data_to):
        if OBJECT_NAME not in data_from.objects:
            raise RuntimeError("source library is missing " + OBJECT_NAME)
        data_to.objects = [OBJECT_NAME]
    obj = data_to.objects[0]
    if obj is None:
        raise RuntimeError("could not link " + OBJECT_NAME + " from " + str(path))
    collection.objects.link(obj)
    return obj


def camera_settings(scene, camera):
    direction = Vector((4.0, -6.0, 4.0)).normalized()
    camera.location = direction * 12.0
    target = Vector((0.0, 0.0, 0.0))
    camera.rotation_euler = (target - camera.location).to_track_quat("-Z", "Y").to_euler()
    camera.data.type = "ORTHO"
    camera.data.ortho_scale = 5.0
    scene.camera = camera
    scene.render.resolution_x, scene.render.resolution_y = IMAGE_SIZE
    scene.render.resolution_percentage = 100


def assert_center_pixels(scene, objects, render_result):
    width, height = IMAGE_SIZE
    pixels = list(render_result.pixels[:])
    channels = render_result.channels
    if channels not in (3, 4):
        raise RuntimeError("unexpected Workbench Render Result channel count: " +
                           str(channels))
    samples = []
    for obj in objects:
        projected = world_to_camera_view(scene, scene.camera, obj.matrix_world.translation)
        x = min(width - 1, max(0, int(projected.x * width)))
        y = min(height - 1, max(0, int(projected.y * height)))
        offset = (y * width + x) * channels
        rgba = tuple(pixels[offset:offset + channels])
        assert projected.z > 0.0 and 0.0 < projected.x < 1.0 and 0.0 < projected.y < 1.0, (
            "linked object center is outside the camera frame", obj.name, projected[:])
        assert max(rgba[:3]) - min(rgba[:3]) > 0.025, (
            "linked object center pixel does not contain a chromatic Random color",
            obj.name, rgba)
        samples.append(rgba)
    assert max(abs(samples[0][channel] - samples[1][channel])
               for channel in range(3)) > 0.08, (
        "same-named linked objects must have distinct interior Random pixels", samples)


def main():
    if tuple(bpy.app.version[:3]) != PINNED_BLENDER_VERSION:
        raise RuntimeError("this fixture requires Blender 5.2.2; found " +
                           bpy.app.version_string)
    output_directory = output_path_from_args()
    library_a = output_directory / "linked-random-a.blend"
    library_b = output_directory / "linked-random-b.blend"
    host_path = output_directory / "linked-random-host.blend"
    image_path = output_directory / "linked-random-reference.png"

    create_library(library_a, (-1.0, 0.0, 0.0))
    create_library(library_b, (1.0, 0.0, 0.0))

    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    objects_collection = bpy.data.collections.new("Linked Random Objects")
    scene.collection.children.link(objects_collection)
    object_a = load_linked_object(library_a, objects_collection)
    object_b = load_linked_object(library_b, objects_collection)
    # Blender's ID.name remains the Workbench hash input. name_full may add
    # library suffixes for linked IDs, which is why the import path preserves
    # the raw library filepath separately.
    assert object_a.name == object_b.name == OBJECT_NAME
    assert object_a.library is not None and object_b.library is not None
    assert object_a.library.filepath != object_b.library.filepath

    camera_data = bpy.data.cameras.new("LinkedRandomCamera")
    camera = bpy.data.objects.new("LinkedRandomCamera", camera_data)
    scene.collection.objects.link(camera)
    camera_settings(scene, camera)

    scene.render.engine = "BLENDER_WORKBENCH"
    shading = scene.display.shading
    shading.light = "FLAT"
    shading.color_type = "RANDOM"
    shading.show_shadows = False
    shading.show_cavity = False
    scene.display.render_aa = "OFF"
    scene.render.image_settings.file_format = "PNG"
    scene.render.film_transparent = False
    scene.render.dither_intensity = 0.0
    # Match Blender's interactive Solid viewport transform used by classiCAD's
    # default Workbench presentation path.
    scene.view_settings.view_transform = "AgX"
    scene.view_settings.look = "None"
    scene.view_settings.exposure = 0.0
    scene.view_settings.gamma = 1.0
    scene.render.filepath = str(image_path)

    bpy.ops.render.render(write_still=True)
    rendered = bpy.data.images.load(str(image_path), check_existing=False)
    if tuple(rendered.size) != IMAGE_SIZE:
        raise RuntimeError("Blender reference render has an unexpected size: " +
                           repr(tuple(rendered.size)))
    assert_center_pixels(scene, (object_a, object_b), rendered)
    bpy.ops.wm.save_as_mainfile(filepath=str(host_path), check_existing=False)
    print("Linked Random fixture:", host_path)
    print("Linked Random reference:", image_path)
    print("Linked libraries:", library_a, library_b)


main()
