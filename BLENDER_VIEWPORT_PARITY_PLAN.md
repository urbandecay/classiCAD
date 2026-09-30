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
| 5. Parity regression and interaction validation | Compare a fixed matrix of Blender and classiCAD views (six axis views, isometric ortho, perspective, close/far zoom, pan, and orbit), add durable render/scale regressions, and verify the existing mouse navigation and CPU fallback remain intact. | Complete |
| 6. Blender camera and navigation parity | Port Blender's viewport projection multiplier, wheel distance step, zoom-to-cursor depth rule, smooth-view timing/easing, and zoom distance range. Compare orbit/pan and saved navigation settings with Blender. | Complete |
| 7. Native GPU viewport composition | Draw the grid and scene in the widget's GPU presentation path so camera motion does not synchronously read the framebuffer back into a Qt image on every repaint. Retain a tested CPU fallback. | Complete |

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
  preferences were not changed. The classiCAD comparison harness applies the
  clip range in memory; saved classiCAD preferences remain unchanged.
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
- The framing-matched top capture measures about 7 px for the fine grid and
  68 px for the prominent grid. A later visible-window comparison confirms
  that the coarse pitch matches Blender at baseline, while fine-level LOD
  visibility still differs: classiCAD shows the 7 px subdivision where
  Blender's baseline view suppresses it. Earlier crop MAEs came from the
  screenshot-area capture path and are superseded by the controlled visible-
  window measurements below.
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
  native OpenGL interaction/capture test. The later hardware spot check is
  recorded under Stage 7.
- Controlled visible-window parity matrix (2026-09-30; supersedes the earlier
  `screenshot_area` comparisons, which returned transparent/black output in
  the nested Mesa display): Blender and classiCAD captures are both 591x511,
  rendered by Mesa llvmpipe (LLVM 20.1.2, Mesa 25.2.8), with a 50 mm lens,
  clip range 0.01/1000, and centered target. The Blender script explicitly
  sets that clip range; the classiCAD harness applies the same values in
  memory, leaving saved user preferences untouched. Both runs use the same
  1-unit grid and matching orthographic/perspective poses. The isolated
  Blender capture's background gradient (48/61) and grid gray (84, alpha 128)
  also match the saved Blender theme. The six cardinal views, isometric
  orthographic and isometric perspective captures are under
  `/tmp/classicad-stage5-llvmpipe-r3` and
  `/tmp/classicad-stage5-blender-llvmpipe`.
- The fixed-view matrix aligns the colored-axis origins and isometric axis
  directions; the red and green isometric line slopes are +0.5773 and -0.5773
  in both applications. Direct top-view scale checks use classiCAD zoom
  0.6809 versus Blender distance 60 at baseline, zoom 1.3613 versus distance
  30 for close, and zoom 0.3405 versus distance 120 for far. The dominant
  top-grid pitches agree at approximately 68, 136, and 34 px, respectively.
  The orthographic matrix uses the same baseline match, and perspective remains
  at classiCAD zoom 1.0 versus Blender distance 60.
- Appearance differences persist with viewport size, clip range, and renderer
  held constant. In neutral background pixels, radial-bin medians (center,
  middle, outer) are classiCAD (60, 58, 56) versus Blender (63, 63, 63) in top
  view and (62, 59, 56) versus (67, 66, 63) in isometric perspective. The
  median green-axis pixel in top view is classiCAD (102, 160, 28) versus
  Blender (107, 170, 22); the palette inputs are close, so the remaining
  difference is in final line coverage/compositing rather than a renderer
  mismatch. Fine grid LOD lines are more visible in classiCAD at baseline and
  far zoom. Neutral-pixel ROI MAE is 6.42 for top and 9.14 for perspective;
  those values include the LOD visibility difference and viewport-specific UI,
  and are not pure background-color measurements. Exact background, fine-grid,
  and axis-pixel parity remain open, but the Stage 5 view/scale comparisons and
  clip-controlled investigation are complete.
- The native XCB interaction/capture harness passed after switching its
  comparison camera to 0.01/1000. The extreme-distance grid-clipping regression
  renders retain their separate 0.01/10000 preferences so their existing
  far-plane coverage remains intact. A matched native perspective pan/orbit
  comparison is recorded in Stage 6.
- Projection-scale follow-up (2026-09-30; supersedes the earlier manual
  orthographic zoom correction above): Blender 5.2.2's projection toggle keeps
  the view distance and lens unchanged ([toggle source](https://raw.githubusercontent.com/blender/blender/v5.2.2/source/blender/editors/space_view3d/view3d_edit.cc),
  [camera projection source](https://raw.githubusercontent.com/blender/blender/v5.2.2/source/blender/blenkernel/intern/camera.cc)). classiCAD had used
  `zoom` directly as orthographic pixels per world unit while its perspective
  camera used `60 / zoom` as distance, so zoom 1.0 produced different screen
  scales when changing projection. Added a shared target-plane scale of
  `max(viewport width, height) * lens / (72 * 60) * zoom` and used it for
  orthographic drawing, picking, grid rendering, panning, zoom anchoring, and
  screen-space tolerances. At the 591x511 Blender comparison size with a
  50 mm lens, classiCAD's default Top view now matches Blender's distance-60
  baseline without a test-only zoom. The measured prominent top-grid pitch is
  68.5 px in both captures. The native screenshot now waits for the frame to
  draw and asserts that the grid is present. Close/far captures use 2.0/0.5
  zoom, corresponding to Blender distances 30/120. Validation passed: full
  `cmake --build build -j2`, `classicad_core_contracts_test`, the hardware
  XCB interaction test with picture/style and large-scene GPU checks, and
  `git diff --check`. The hardware run used an NVIDIA GeForce GTX 1050 Ti; its
  1,500-stroke scene averaged 16.57 ms/frame with display pacing enabled. New
  native captures are under `/tmp/classicad-projection-toggle-hardware-final`.
- Appearance follow-up (2026-09-30): read the user's saved Blender 5.2.2
  preferences. Their Default theme has a radial background (`48`–`61` gray),
  grid RGB 84 with alpha 0.502 for minor lines and 1.0 for major lines, axis
  colors X `#ff3352`, Y `#8bdc00`, Z `#2890ff`, and grid-axis brightness 0.46.
  Restored the matching radial GPU/CPU backgrounds and close fine-grid
  visibility; reverted the temporary Y-axis adjustment made against a
  different screenshot. The exact transient palette now migrates back to the
  prior default, while custom saved colors remain preserved. A post-change
  viewport capture is still needed to confirm rendered pixels.

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
  pass. A matched live perspective pan and combined orbit drag has now been
  visually compared with Blender (details below); it confirms the yaw sign,
  turntable response for this drag, and 1:1 pan displacement. The expanded
  gesture calibration across additional drag extents and saved navigation
  settings is recorded below. Stage 5 now
  has controlled native XCB screenshots; their remaining grid/axis differences
  are recorded there. Stage 7's committed-picture, layer-style, and hardware
  checks are recorded below. Close-zoom curve chord error has a geometry
  regression.
- Validation passed: `cmake --build build -j2`, all four CTest suites,
  `git diff --check`, the XCB/Mesa software-OpenGL interaction test, and a
  five-second offscreen application startup. The offscreen platform cannot
  create an OpenGL context, so it exercised the CPU fallback as expected.
- Gesture review (2026-09-30): the older captures in
  `/tmp/classicad-nav-test-eV57wr` remain invalid for parity because classiCAD
  used Trackball, Blender's after-drag image shows no clear rotation, viewport
  widths differ, and drag coordinates were not recorded. A controlled live
  yaw check then ran only on isolated Xephyr display `:99`, using fresh apps
  and temporary settings; the original unsaved Blender session was untouched.
  Both started Top/Orthographic. classiCAD used the copied saved settings
  `panButton=RMB`, Turntable, and sensitivity 0.006981317 rad/px; it received a
  Shift+RMB horizontal drag of +100 px. Blender 5.2.2 used its default
  Turntable MMB orbit with the same +100 px horizontal delta. In both after
  captures, positive X tilts up-right and positive Y up-left, confirming the
  horizontal yaw direction in `275873c`. Captures are under
  `/tmp/classicad-turntable-nested-*.png`. This validates direction only:
  classiCAD stayed orthographic while Blender changed to User Perspective, and
  their viewport areas differed (about 536x572 versus 1050x670), so it did not
  validate perspective orbit or pan parity. At that point Trackball had only
  been checked mathematically; the live comparison is recorded below.
- Matched perspective gesture comparison (2026-09-30): used fresh temporary
  classiCAD and Blender 5.2.2 instances on isolated Xephyr display `:100`; the
  original unsaved Blender session was left untouched. Both were set to Top
  Perspective, centered target, 60-unit view distance, and 50 mm lens. The
  captured viewport crops were both 802x710 px (classiCAD crop at 98,119;
  Blender crop at 0,54). classiCAD used the copied saved `panButton=RMB`,
  Turntable, and 0.006981317 rad/px sensitivity; Blender used default
  Turntable navigation.
  A center drag of (+100,+50) px used RMB in classiCAD and Shift+MMB in
  Blender. The grid origin moved by (+100,+50) px in both captures, and the
  inverse drags returned both views to their starting origins. Then a second
  (+100,+50) px center drag used Shift+RMB for classiCAD orbit and MMB for
  Blender orbit. The fitted screen-space red X-axis slopes were -0.7885 and
  -0.7883; green Y-axis slopes were +1.1200 and +1.1199, respectively. The
  resulting axis directions and tilt match within about 0.02 degrees, directly
  validating the reversed horizontal yaw in `275873c` for this perspective
  Turntable gesture. The baseline grid origins differ by about 4 px
  horizontally and 15 px vertically between the viewport crops; these captures
  validate gesture response, not pixel-identical background or grid rendering.
  Captures are in `/tmp/classicad-perspective100-xdg` and
  `/tmp/blender-perspective100-xdg`. At that point other drag extents,
  independent yaw/pitch gestures, wheel zoom, and Trackball feel remained
  unvalidated; the expanded review below covers them.

- Expanded live gesture review (2026-09-30): fresh classiCAD and Blender 5.2.2
  instances ran side by side in isolated Xephyr display `:103`; Blender used a
  copy of the saved user preferences, and classiCAD used the matching
  navigation settings. Both viewport crops were 1311x846 px. With Turntable
  at 0.4 degrees/pixel, separate Top-view horizontal drags of +50 and +150 px
  rotated the colored axes by -20 and -60 degrees in both applications. A
  separate +50 px vertical drag and an off-center Isometric drag of (-80,+40)
  px also matched visually; fitted Isometric red/green axis changes differ by
  less than 0.01 degrees. The Front-view +75 px horizontal drag was exercised,
  but its floor grid is edge-on in Blender and disappears when classiCAD leaves
  the fixed Front view, so it is not used as visual parity evidence.
  Trackball was compared from Isometric using the same off-center start
  (-90,-40) px and drag (+180,+80) px. Its rendered axis motion looks alike;
  screen-line fits differ by 0.06 degrees for red and 1.44 degrees for green,
  so this records a close visual match rather than exact numeric parity.
  For off-center wheel zoom at (981,328), Top-view grid spacing changed from
  15 to 18 px in Blender and approximately 15/16 to 18/19 px in classiCAD.
  The colored-axis intersection moved from (655,423) to about (590,442) in
  both, matching zoom-to-cursor anchoring. Perspective wheel zoom also used
  the same 1.2 distance ratio, and classiCAD's sampled world point under the
  cursor was unchanged before and after. The interaction harness now saves
  repeatable captures for these gestures; they are under
  `/tmp/classicad-stage6-blender-captures-r2` and
  `/tmp/classicad-stage6-classicad-captures-r3`. The live classiCAD run used
  XCB/Mesa software OpenGL; hardware-GPU checks remain in Stage 7.

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
  duplicate previews, and the geometric rotate/scale guides now use a separate
  cached OpenGL stroke renderer over the grid. Picture placement and duplicated
  picture previews use cached OpenGL textures with an OpenGL frame outline.
  Qt still paints cursor/snap markers, angle/status labels, control handles,
  dimensions, and erase overlays. Committed picture images use cached GPU
  textures, with Qt fallback when native rendering is unavailable. Unsupported
  GL systems use the established Qt rendering path. The native scene stroke path uses
  cached fixed NURBS tessellation rather than the painter's adaptive display
  setting. Degree-one spans now emit only their exact endpoints; higher-degree
  spans use 128 samples (with an 8192-sample budget). A regression checks
  that an exact radius-100 circle stays below 0.1 logical
  viewport pixels of chord error at maximum orthographic zoom. This verifies
  tessellation quality in the model, but is not a substitute for a direct
  hardware-GPU visual/performance check.
- Desktop interaction checks cover wheel zoom, pan/orbit, an in-progress GPU
  rectangle preview and its committed stroke, a GPU picture placement preview
  and the resulting committed picture, a Bézier with visible control guides,
  and a point marker. Full build, all
  four CTest suites, XCB/Mesa interaction check, offscreen startup/fallback,
  and `git diff --check` passed. Close-zoom NURBS chord error is covered by a
  geometry regression.
- GPU completion checks (2026-09-30): extended the native stroke shader to draw
  the full 25-style built-in layer linetype set, including the multi-part
  CENTER, DASHDOT, DIVIDE, BORDER, and PHANTOM patterns. Pattern phase now
  carries across connected segments. A first hardware image check exposed
  DOT2 dots that were too faint because their diameter shrank along with their
  spacing; dot diameter now follows stroke width while spacing keeps its
  selected scale. A GPU-only hardware capture shows the committed picture's
  red, blue, and yellow quadrants while its transparent quadrant reveals the
  background. The same capture shows every built-in style; see
  `/tmp/classicad-stage7-hardware-final/stage7-gpu-picture-and-layer-styles.png`.
- The expanded XCB run reported `GL_RENDERER` directly as NVIDIA GeForce GTX
  1050 Ti/PCIe/SSE2. Its synthetic dense 1,500-stroke scene at 1280x720
  averaged 16.57 ms/frame (median 16.65 ms, p95 18.19 ms; about 60 frames/s);
  the measurement includes display pacing, so it confirms paced rendering on
  this GPU but does not measure uncapped maximum throughput or other GPU
  vendors. The stress capture is `/tmp/classicad-stage7-hardware-final/stage7-large-scene-1500-strokes.png`.
  `cmake --build build -j2`, `classicad_core_contracts_test`, the hardware
  `classicad_viewport_interaction_test` with picture/style and large-scene GPU
  checks enabled, and `git diff --check` passed. Stage 7 is complete for the
  available hardware; other GPU models have not been checked.

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
