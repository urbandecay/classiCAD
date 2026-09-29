# Blender viewport parity plan

This is the staged implementation plan for matching Blender's 3D viewport grid,
camera projection, and navigation in classiCAD. The scope is the visible 3D
view and its input behavior.

Each stage should be built and validated before the next begins. Blender's
GPL-2.0-or-later grid implementation is the behavioral reference; classiCAD
adapts the relevant algorithms to its Qt/OpenGL architecture.

## Stages

| Stage | Responsibility | Status |
|---|---|---|
| 1. Grid scale and LOD selection | Port Blender's decimal step ladder, view-dependent focus-distance selection, fixed-axis orthographic minimum spacing, and linear blend between grid levels. Keep the shader's three adjacent levels aligned to Blender's selected level. | Complete |
| 2. Camera-relative grid placement and axis/view mapping | Match Blender's grid origin tracking while panning/orbiting; distinguish fixed-axis views from user-defined orthographic views; render the appropriate work plane and global X/Y/Z axes in each view. | Complete |
| 3. Depth, occlusion, and z-bias | Integrate the grid with scene depth so curves/objects occlude it correctly; reproduce Blender's grid-line occlusion and perspective additive-pass depth-bias behavior. This may require moving grid composition into a shared viewport GL pass instead of rendering it to a standalone image. | Complete |
| 4. Theme, units, and appearance | Feed grid spacing from explicit document units/grid settings; match axis/grid theme colors, opacity, stipple, and perspective/orthographic fades without hard-coded renderer-only colors. | Complete |
| 5. Parity regression and interaction validation | Compare a fixed matrix of Blender and classiCAD views (six axis views, isometric ortho, perspective, close/far zoom, pan, and orbit), add durable render/scale regressions, and verify the existing mouse navigation and CPU fallback remain intact. | In progress |
| 6. Blender camera and navigation parity | Port Blender's viewport projection multiplier, wheel distance step, zoom-to-cursor depth rule, smooth-view timing/easing, and zoom distance range. Compare orbit/pan and saved navigation settings with Blender. | In progress |
| 7. Native GPU viewport composition | Draw the grid and scene in the widget's GPU presentation path so camera motion does not synchronously read the framebuffer back into a Qt image on every repaint. Retain a tested CPU fallback. | Pending |

## Stage 1 implementation notes

- Blender's `overlay_grid.hh` derives perspective focus distance from the
  camera-to-floor height and view direction, and uses a zoom-derived distance
  for fixed-axis orthographic views. The renderer now uses those same rules.
- Blender's default grid steps are powers of ten. Fixed-axis orthographic views
  expose three additional subdivisions below the base step; perspective and
  user-defined views use the base step ladder.
- The selected LOD fraction is interpolated linearly between adjacent world
  step sizes, as Blender does, instead of using the fractional part of
  `log10(distance)`.
- The vertex shader selects each of the three drawn levels from the same
  bounded step ladder, including at its lower/upper limits.
- User-defined orthographic view distance, exact camera-relative offsets,
  complete global-axis behavior, scene-depth integration, and configurable
  theme/unit inputs remain future stages. Those are explicitly not claimed as
  complete by Stage 1.
- Validation passed: `cmake --build build -j2`, all three CTest suites,
  `git diff --check`, offscreen application startup/fallback, and a brief
  XCB/software-OpenGL application run with no shader/context initialization
  errors.

## Stage 2 implementation notes

- Added `BlenderGridFrame` as the shared resolver for the visual grid plane,
  camera-relative origin, LOD focus distance, and visible global axes. Both the
  GPU renderer and Qt fallback consume this frame.
- Fixed-axis views select Blender's planes: Top/Bottom use XY, Front/Back use
  XZ, and Right/Left use YZ. Free-angle orthographic and perspective views use
  the XY floor plane. This is display-only; the user's active CAD construction
  plane and offset remain unchanged for drawing and picking.
- Perspective grid placement uses Blender's camera-height/view-angle blend;
  orthographic placement follows the center view ray onto the grid plane, with
  the projected view target as the parallel-ray fallback. Grid lines snap
  around that camera-relative origin while panning.
- Axis lines are emitted along world X/Y/Z, colored by their global axis, and
  limited to axes in the displayed plane. Blender's default visibility (X/Y
  shown, Z hidden) is retained, which avoids a projected Z-axis dot in aligned
  views.
- Added a distinct `gridViewDistance` camera-state value for free-angle
  orthographic grid LOD. Orthographic zoom updates it, while fixed-axis views
  continue using Blender's projection-derived distance. The application's
  orthographic camera projection remains zoom-based.
- Validation passed: `cmake --build build -j2`, all three CTest suites,
  `git diff --check`, and software-OpenGL shader startup validation. The CPU
  fallback shares the plane/origin/axis mapping; Stage 4 addresses configurable
  theme and unit inputs.

## Stage 3 implementation notes

- Added a depth-only scene pass to the offscreen grid framebuffer. It builds
  sampled 3D curve segments from each visible object's own workplane and offset,
  includes point markers and picture-frame surfaces, and omits screen-space
  dimension annotations.
- The viewport paints committed scene geometry to a transparent Qt image,
  then composites the transparent GPU grid over it. Grid fragments use
  `GL_LEQUAL` against scene depth, so nearer scene strokes suppress the grid
  while the grid remains visible over scene strokes behind its plane.
- Matched Blender's draw-pass rule: the first grid iteration writes depth and
  later perspective additive iterations do not. The vertex shader applies
  Blender's progressive perspective clip-space z offsets. Transparent output
  accumulates premultiplied RGB and coverage for correct Qt composition.
- Picture depth currently treats its frame as a solid planar surface; alpha
  cutouts inside RGBA reference images do not yet punch holes in the depth
  pass. This limitation is isolated to image-backed picture objects.
- Validation passed: `cmake --build build -j2`, all three CTest suites,
  `git diff --check`, and a five-second XCB/Mesa software-OpenGL launch. The
  application initialized without shader/context setup errors. The fixed-view
  image comparison and depth-occlusion matrix remain part of Stage 5.

## Stage 4 implementation notes

- Added persistent `DocumentSettings` for display units and base grid spacing.
  The document geometry remains in millimeters; unit selection converts only
  the visible grid interval before Blender's LOD step selection. Document JSON
  version 4 stores these settings, while versions 1–3 retain the previous
  millimeter/1-unit defaults.
- Added View > Grid Units and Spacing, plus Edit > Preferences > Viewport
  controls for grid/emphasis/X/Y/Z colors, overall opacity, and low-alpha
  stipple. A shared `BlenderGridAppearance` feeds the GPU shader uniforms and
  Qt fallback, including configurable grazing/edge fade parameters; renderers
  no longer own separate fixed theme colors.
- The color and fade inputs follow Blender's overlay grid shader's theme and
  fade structure, while document-unit scaling follows Blender's scene/view
  grid-step selection. This is an adaptation to classiCAD's mm-based document
  model, not a wholesale port of Blender's draw engine.
- Validation passed: `cmake --build build -j4`, all three CTest suites,
  `git diff --check`, document-settings save/load/backward-compatibility and
  unit-scaled LOD contracts. The offscreen application starts and falls back
  when Qt cannot create an OpenGL context.

## Stage 5 progress

- Added durable CPU-fallback image checks across all six cardinal views,
  isometric orthographic, and perspective; verifies X/Y axis colors, smooth
  horizon fading, and visible changes under close/far zoom, pan, and orbit.
- Added document grid-settings undo/redo coverage. Existing tests already cover
  scale transitions, grid-plane/view mapping, picking, navigation gizmo hits,
  and scene-depth geometry construction.
- Added a real-widget event test for wheel zoom, configured pan-button drag,
  Shift+MMB pan, and Shift+configured-button orbit. Under offscreen CTest, it
  verifies the CPU fallback; an XCB/Mesa run requires actual GL output and
  verifies covered, distinct orthographic and perspective grid images.
- Validation passed: `cmake --build build -j4`, all four CTest suites,
  `git diff --check`, and `QT_QPA_PLATFORM=xcb LIBGL_ALWAYS_SOFTWARE=1
  ./build/classicad_viewport_interaction_test` with both GPU-render checks.
- A side-by-side pixel/image comparison against Blender's fixed screenshot
  matrix remains unverified. The GL test proves shader-backed rendering works,
  not that its pixels are identical to Blender's in every camera state.

## Stage 6 progress

- Compared Blender's `view3d_navigate_view_zoom.cc`, `view3d_navigate_smoothview.cc`,
  `BKE_camera_params_from_view3d`, and `overlay_grid.hh` with this viewport.
  Blender uses a 1.2 distance ratio for each wheel step, a 2.0 viewport
  projection factor, a target-depth point for zoom-to-mouse, and a 200 ms
  smoothstep view transition in the user's saved preferences.
- Ported those rules to the C++ camera and widget. Removed the custom
  perspective grid focus-distance ceiling, and use Blender's 151 perspective
  versus 301 orthographic grid lines. Added contracts for projection scale,
  edge-on zoom anchoring, and zooming beyond the far clip then returning.
- Read the user's saved Blender 5.2 theme colors and derived the grid, major
  grid, and axis colors with Blender's own shade/blend formulas. Defaults now
  use those values; only the prior exact hard-coded palette is migrated on
  preference load, so edited colors remain untouched.
- The viewport still uses a 60-unit initial view distance for the CAD scene;
  Blender's saved startup file opens near 18 units. The current yaw/pitch orbit
  state and pointer-gesture scaling have not been proven equivalent to
  Blender's quaternion navigation. The screenshot comparison in Stage 5 and
  native GPU composition in Stage 7 remain required before claiming parity.
- Validation passed: `cmake --build build -j2`, all four CTest suites,
  `git diff --check`, the XCB/Mesa software-OpenGL interaction test, and a
  five-second offscreen application startup. The offscreen platform cannot
  create an OpenGL context, so it exercised the CPU fallback as expected.

## Upstream behavioral references

- [Blender overlay grid setup](https://github.com/blender/blender/blob/main/source/blender/draw/engines/overlay/overlay_grid.hh)
- [Blender overlay grid vertex shader](https://github.com/blender/blender/blob/main/source/blender/draw/engines/overlay/shaders/overlay_grid_vert.glsl)
- [Blender viewport grid scale selection](https://github.com/blender/blender/blob/main/source/blender/editors/space_view3d/view3d_draw.cc)
- [Blender grid depth/blending pass setup](https://github.com/blender/blender/blob/main/source/blender/draw/engines/overlay/overlay_grid.hh)
- [Blender perspective grid z-bias](https://github.com/blender/blender/blob/main/source/blender/draw/engines/overlay/shaders/overlay_grid_vert.glsl)
- [Blender grid theme colors and fade behavior](https://github.com/blender/blender/blob/main/source/blender/draw/engines/overlay/shaders/overlay_grid_frag.glsl)
- [Blender scene/view unit and grid-step selection](https://github.com/blender/blender/blob/main/source/blender/editors/space_view3d/view3d_draw.cc)
- [Blender view zoom](https://github.com/blender/blender/blob/main/source/blender/editors/space_view3d/view3d_navigate_view_zoom.cc)
- [Blender smooth view](https://github.com/blender/blender/blob/main/source/blender/editors/space_view3d/view3d_navigate_smoothview.cc)
- [Blender viewport camera parameters](https://github.com/blender/blender/blob/main/source/blender/blenkernel/intern/camera.cc)
