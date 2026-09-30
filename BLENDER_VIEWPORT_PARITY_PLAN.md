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
| 7. Native GPU viewport composition | Draw the grid and scene in the widget's GPU presentation path so camera motion does not synchronously read the framebuffer back into a Qt image on every repaint. Retain a tested CPU fallback. | In progress |

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
- Added opt-in screenshot capture to the XCB test with
  `CLASSICAD_VIEWPORT_CAPTURE_DIR`; it records the native widget's initial and
  perspective frames plus GPU grid renders for six axis views, orthographic
  isometric, perspective, close/far zoom, and pan.
- The first screenshot comparison was not apples-to-apples: the classiCAD
  renderer capture was 640x480 while Blender's captured viewport was 591x511.
  The saved classiCAD preferences also had clip start/end 0.1/100000, while the
  current Blender 5.2.2 View3D values are 0.01/1000. Do not treat the earlier
  pixel-pitch estimate as a controlled parity result.
- Re-ran the capture with both viewport images at 591x511 and explicit,
  in-memory comparison preferences in the classiCAD test: 50 mm lens,
  clip start/end 0.01/1000, 8x viewport AA, and 1-unit grid spacing. The test
  view used classiCAD's default camera state. The Blender capture script
  explicitly sets `region.view_distance=60`, zero view location, and the same
  top/isometric orientations for its captures; the earlier caveat comparing
  Blender's saved startup distance (~18) with classiCAD's 60 was incorrect for
  these screenshots because the script overwrites that startup state. Blender
  preferences were not changed. The saved classiCAD clip preferences were
  aligned to 0.01/1000.
- The matched captures are in `/tmp/classicad-blender-matched-captures` and
  `/tmp/classicad-blender-user-reference-captures`. The viewport dimensions and
  listed camera/grid preferences match. Inspection of Blender's saved 5.2
  theme found its radial 3D View background colors (`gradient` 48/255,
  `high_gradient` 61/255) and grid colors (RGB 84/255, alpha 0.502 for minor
  and 1.0 for major lines).
- Replaced classiCAD's flat viewport background with a full-screen OpenGL
  radial-gradient pass using those theme colors; the non-GL fallback uses the
  same gradient. Grid lines now use Blender's saved theme grays, and the
  renderer composites overlapping LOD/iteration lines with alpha-over instead
  of additively brightening coincident lines. Previously shipped gray defaults
  are migrated only when the full stored palette is unchanged; custom colors
  are preserved.
- Rechecked the 591x511 Blender and native classiCAD captures at the scripted
  pose. The perspective view uses the same nominal 50 mm lens, 60-unit camera
  distance, zero target/pan, and 45-degree yaw / 35.264-degree pitch. The
  orthographic views share the same orientation and nominal distance, but the
  projection scales are not equivalent: Blender derives ortho scale from its
  view matrix, while classiCAD uses its independent screen-space `zoom` (1.0).
  In the top-view screenshots, the prominent grid interval measures about
  68 px in Blender versus 100 px in classiCAD; the fine interval is about 7 px
  versus 10 px. Added an opt-in native-widget recapture that applies a centered
  smooth-wheel delta of -253 (zoom 1.0 to 0.6809) for top orthographic only,
  then applies the inverse before taking the perspective screenshot. This is
  a screenshot-test adjustment; the app's initial zoom/default is unchanged.
  The new screenshots are in `/tmp/classicad-blender-controlled-captures`.
- The framing-matched top capture now measures about 7 px for the fine grid
  and 68 px for the prominent grid, matching Blender's intervals. Neutral
  grayscale MAE in the central viewport crop fell from 5.23 to 3.80 intensity
  levels. The perspective capture remains at zoom 1.0 / distance 60; its same
  crop MAE is 2.82, with band MAEs of 2.80 near the top, 3.45 through the
  center, and 2.34 near the bottom. This confirms the earlier top-view
  discrepancy was mainly orthographic framing, while small rendering/color
  differences remain.
- Center-axis samples are close for red (Blender 200/42/63, classiCAD
  198/37/62); green differs in its blue component (Blender 108/172/21,
  classiCAD 106/170/5). The grid remains visible through the perspective
  horizon fade in both captures, but the classiCAD neutral grid/background is
  about 2-3 intensity levels darker across the sampled horizontal bands.
  These are diagnostics, not a parity pass: green-axis hue, subtle grid/fade
  contrast, and interaction feel still need follow-up.
- Added regression checks for the Blender theme grid colors and the radial
  background. The native XCB test uses Mesa software OpenGL; this confirms the
  GL path but not hardware-GPU performance.
- Latest appearance follow-up (2026-09-29): aligned the background shader with
  Blender 5.2's [overlay background shader](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/overlay/shaders/overlay_background_frag.glsl)
  (normalized radial distance, gamma-space interpolation, and 4x4 Bayer
  dither); the Qt fallback now uses the same radial/gamma ramp. The Y-axis
  palette was calibrated against the blended native screenshot, rather than
  treating its source RGB as the final pixel color.
- Fixed the native screenshot test's resize race: it now waits for the GL
  surface to settle and asserts the top-view capture is exactly 591x511 before
  comparing it with Blender. The background regression samples a neutral edge
  patch instead of a single rounded viewport-corner pixel.
- Re-measured the current matched captures over neutral grayscale pixels in
  ROI x=35..554, y=100..454: mean absolute error is 4.53 levels in top
  orthographic and 4.91 in isometric perspective. These supersede the earlier
  3.80/2.82 measurements, which were from the prior appearance/capture run.
  The mean Y-axis sample is now Blender (104.5, 166.3, 25.4) versus classiCAD
  (104.6, 166.6, 25.6). Neutral radial-bin medians remain about 1-3 levels
  darker in classiCAD, so exact background color-management parity remains
  open; this is not a claim of pixel-identical viewport output.
- Revalidation passed: full C++ build, all four CTest suites, and the XCB/Mesa
  native OpenGL interaction/capture test. Hardware-GPU performance is still
  untested.

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
  Blender's saved startup file opens near 18 units. Quaternion turntable orbit
  and quaternion smooth-view interpolation now cross the poles without a pitch
  clamp. Orbit-start depth selection tests visible shapes on their own offset
  workplanes, with sampled committed NURBS points supplying curve depth,
  rather than projecting onto the active construction plane, and
  pivot changes preserve the perspective eye position. Orbit start now tries an
  on-demand GPU depth-buffer pick of visible scene geometry and falls back to
  the sampled CPU hit test; the small depth readback happens only once at orbit
  start, not on every frame. A selectable Blender-style trackball method uses
  Blender 5.2's 1.1-radius, aspect-correct sphere/hyperbola mapping for drag
  start/current positions, deriving the axis from their cross product and
  scaling the angle by a separately saved sensitivity. Trackball is wired to
  gizmo and mouse orbit drags;
  turntable remains the default. Core math and overlapping-depth regressions
  pass, but live Blender gesture calibration is not validated yet. Stage 5 now
  has controlled native-GPU screenshots; their remaining grid/axis differences
  are recorded there. Stage 7 still needs remaining scene items and close-zoom
  curve-quality validation before claiming full parity.
- Validation passed: `cmake --build build -j2`, all four CTest suites,
  `git diff --check`, the XCB/Mesa software-OpenGL interaction test, and a
  five-second offscreen application startup. The offscreen platform cannot
  create an OpenGL context, so it exercised the CPU fallback as expected.

## Stage 7 progress

- A `QOpenGLWidget` now presents the viewport on desktop OpenGL systems. The
  procedural grid remains in a GPU framebuffer and is resolved/composited into
  the widget framebuffer without a `toImage()` readback during normal camera
  navigation. The offscreen/minimal Qt path still uses the previous raster
  composition and grid fallback.
- Common committed CAD curves (lines, rectangles, polygons, circles, ellipses,
  arcs, PolyCurves, Béziers, and NURBS) use an OpenGL stroke pass when their
  stored geometry and line style support it. The pass caches sampled geometry
  across camera moves and applies per-shape color and width. Bézier/NURBS
  control polygon guides use the same pass with a dashed pattern.
- Main curve/shape previews for drawing tools, tangent results, mirror previews,
  and duplicate previews now use a separate cached OpenGL stroke renderer over
  the grid. Qt still paints cursor/snap markers, construction guides, labels,
  pictures, dimensions, erase overlays, and noncontinuous layer line styles.
  Unsupported GL systems
  use the established Qt rendering path. The native scene stroke path uses
  cached fixed NURBS tessellation rather than the painter's adaptive display
  setting. Degree-one spans now emit only their exact endpoints; higher-degree
  spans use 128 samples (with an 8192-sample budget). A regression checks
  that an exact radius-100 circle stays below 0.1 logical
  viewport pixels of chord error at maximum orthographic zoom. This verifies
  tessellation quality in the model, but is not a substitute for a direct
  hardware-GPU visual/performance check.
- Desktop interaction checks cover wheel zoom, pan/orbit, an in-progress GPU
  rectangle preview and its committed stroke,
  a Bézier with visible control guides, and a point marker. Full build, all
  four CTest suites, XCB/Mesa interaction check, offscreen startup/fallback,
  and `git diff --check` passed. Stage 7 remains in progress until
  remaining scene items (including textured picture rendering and more layer
  line styles in the GPU stroke pass) are addressed. Close-zoom NURBS chord
  error is now covered by a geometry regression; live hardware-GPU visual
  quality and performance remain unverified.

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
- [Blender 5.2 viewport turntable and trackball rotation](https://github.com/blender/blender/blob/v5.2.0/source/blender/editors/space_view3d/view3d_navigate_view_rotate.cc)
- [Blender viewport camera parameters](https://github.com/blender/blender/blob/main/source/blender/blenkernel/intern/camera.cc)
