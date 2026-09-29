# Third-party notices

## Blender viewport grid shader adaptation

The viewport grid in `src/ui/viewport/shaders/blender_grid.vert`
and `src/ui/viewport/shaders/blender_grid.frag`, together with its draw setup
in `src/ui/viewport/blender_grid_renderer.cpp`, adapts Blender's overlay grid
implementation. Blender's procedural line-ID decoding, three grid levels,
camera-relative offset, fade and stipple calculations, and additive pass
weights were ported to classiCAD's standalone OpenGL path. Blender's internal
draw-engine interfaces were replaced with Qt/OpenGL uniforms and the existing
classiCAD camera/workplane model.

Upstream sources:

- `source/blender/draw/engines/overlay/shaders/overlay_grid_vert.glsl`
- `source/blender/draw/engines/overlay/shaders/overlay_grid_frag.glsl`
- `source/blender/draw/engines/overlay/overlay_grid.hh`

Copyright Blender Authors. The adapted code is licensed under GPL-2.0-or-later.
See `COPYING` and the SPDX notices in the adapted source files.
