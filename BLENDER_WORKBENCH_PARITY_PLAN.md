# Blender Workbench and Viewport Anti-Aliasing Port Plan

## Goal

Make classiCAD's Solid viewport behave and look like Blender's Workbench viewport for the same scene, settings, view, and output size. Use Blender 5.2.2 as the reference version. The bundled Workbench lights, MatCaps, and color-management data in this repository already come from Blender 5.2.2.

This plan covers Workbench shading and effects, viewport anti-aliasing, and the Smooth Wires preferences shown in Blender's Viewport > Quality panel. It complements [BLENDER_VIEWPORT_PARITY_PLAN.md](BLENDER_VIEWPORT_PARITY_PLAN.md), which covers the grid, camera projection, and navigation.

The target is behavioral and visual parity. Pixel-for-pixel output can still vary with GPU and driver, so comparisons must use the same display and render conditions and record any remaining differences.

## Porting rules

1. Follow Blender's v5.2.2 implementation, including the code that actually runs. Use the source links at the end of this document; do not infer behavior from a control's name or from a newer Blender version.
2. Keep the application UI and geometry model in C++/Qt. Adapt the behavior to classiCAD's renderer rather than introducing a second scene or geometry model.
3. Treat classiCAD's exact NURBS surfaces and solids as authoritative. Tessellated triangles are display data only. Preserve the surface and solid contracts in AGENTS.md.
4. Keep every setting functional. A preference or shading option must change the rendered result, or be hidden until it is implemented.
5. Keep the GPU viewport and QPainter fallback understandable and predictable. If an effect cannot be supported by the fallback, document and expose that limitation rather than silently displaying a different result.
6. Keep attribution and applicable Blender license notices with any Blender code or shader logic that is copied or adapted. Prefer a small, documented adaptation over importing Blender's drawing engine.
7. Update this plan as mismatches are discovered. Record intentional differences and their reason instead of describing an incomplete port as complete.

## Current classiCAD status

### GPU opaque depth comparison (2026-10-12)

- The Solid GPU pass now uses `GL_LEQUAL` for opaque surface fragments, so
  equal-depth coplanar fragments can update the color/depth buffer in draw
  order. Blender 5.2.2 configures its Workbench opaque G-buffer with
  `DRW_STATE_DEPTH_LESS_EQUAL` in
  [`workbench_mesh_passes.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_mesh_passes.cc#L108-L124).
  classiCAD previously used `GL_LESS` in the opaque Solid branch of
  `ViewportSurfaceRenderer::draw`, rejecting equal-depth fragments. The
  mixed-alpha route had a second opaque subset draw state that also used
  `GL_LESS`; it now uses `GL_LEQUAL` before the transparent blend pass.
  Blender keeps opaque G-buffer state at `DRW_STATE_DEPTH_LESS_EQUAL`
  independently of the following transparent pass, per the same pinned
  source. This is a state-level parity fix; matched visual comparison remains
  pending.

### GPU Workbench X-Ray backface culling (2026-10-12)

- Solid X-Ray now retains backface culling when the setting is enabled. Blender
  5.2.2 derives `cull_state` from the backface-culling shading flag without
  conditioning it on X-Ray, and includes that state in
  [`TransparentPass::sync`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_mesh_passes.cc#L231-L247).
  classiCAD previously disabled culling for all X-Ray fills, including its
  depth-only X-Ray surface pass. Its depth-only pass now follows the same
  culling state, so backfaces neither contribute color nor replace front
  surface depth when culling is requested. Matched visual comparison remains
  pending.

### GPU Workbench X-Ray transparency, depth, and effects (2026-10-12)

- Solid X-Ray now routes every non-preview surface range through transparent
  accumulation, including ranges whose source material alpha is one. The
  Workbench color shader multiplies interpolated source alpha by the resolved
  X-Ray alpha once; the existing classiCAD `workbenchEffectiveAlpha` supplies
  that same single multiplication. The accumulation pass disables depth
  testing in X-Ray so the separate X-Ray depth-only pass used for CAD strokes
  cannot hide rear surfaces. The transparent-depth pass then records nearest
  surface depth and object IDs for the shared viewport targets.
- Blender 5.2.2 sets `xray_mode` from the resolved alpha and routes each X-Ray
  object to both transparent accumulation and transparent-depth passes in
  [`workbench_engine.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_engine.cc#L238-L250).
  [`workbench_transparent_accum_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_transparent_accum_frag.glsl#L84-L92)
  applies the global X-Ray alpha to the source alpha before OIT accumulation.
  Blender disables shadow, cavity, and DOF flags in X-Ray mode in
  [`workbench_state.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_state.cc#L155-L165).
- classiCAD keeps cavity and shadows suppressed in X-Ray, disables interactive
  and offscreen DOF while X-Ray is active, and allows object outlines through
  the private object-ID path. That path avoids relying on an opaque-only
  shared prepass for X-Ray objects. If weighted-OIT target creation fails,
  color falls back to depth-disabled sorted blending; that fallback does not
  produce the shared transparent-depth/object-ID coverage. X-Ray color,
  outline, and fallback behavior still need matched visual comparison.

### GPU Workbench orthographic-view depth of field (2026-10-12)

- GPU depth of field now runs in orthographic views when a scene camera has
  depth of field enabled and Workbench's depth-of-field shading option is on.
  Blender 5.2.2 sets `draw_dof` from those two conditions without requiring a
  perspective viewport in [`workbench_state.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_state.cc#L251-L252),
  then resolves camera-space depth through `drw_depth_screen_to_view` in
  [`workbench_effect_dof_resolve_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_effect_dof_resolve_frag.glsl#L20-L32).
  classiCAD had gated DOF on perspective projection and always decoded depth
  with the perspective formula. Its DOF shader now uses the linear near/far
  mapping for orthographic depth and retains the perspective formula for
  perspective views. Pass order remains per sample: Workbench resolves DOF
  before anti-aliasing, and classiCAD does the same. Matched orthographic-view
  output comparison remains pending. This change covers the GPU renderer;
  QPainter still gates its approximate DOF on perspective projection.

**DOF gate correction (2026-10-12):** This supersedes older summary bullets
and the `Cavity and depth of field` matrix row below that describe GPU DOF as
perspective-gated or say QPainter shares the GPU gate. Blender's GPU path runs
whenever an active scene camera has DOF enabled and the shading option is on,
including orthographic and free-view projections. The QPainter approximation
remains perspective-only. Any later statement below that says GPU DOF is off
in ordinary free views or orthographic views is superseded by this correction.

### QPainter Workbench DOF camera-relative depth (2026-10-12)

- QPainter scene-camera DOF now runs in perspective and orthographic views.
  Perspective samples use positive eye-relative depth; orthographic samples
  use positive depth from the active view target. The previous paths mixed an
  absolute world projection with the camera's focus distance, so translating
  the camera/view target and geometry together could change blur despite
  preserving their optical relationship. The software CoC now includes
  Blender's aperture-radius factor for both projections.
- The production-path offscreen contract checks a sharp in-focus impulse and
  blurred out-of-focus surface in both projections, plus image invariance when
  orthographic target and geometry translate together. It does not claim
  matched image parity with Blender. QPainter still uses its 12-tap software
  gather, lacks temporal jitter and a depth-aware resolve, and composites
  transparent colors before the gather.

### Workbench Image Texture interpolation (2026-10-12)

- GPU and QPainter Workbench sampling now use nearest only for `Closest` and
  bilinear filtering for `Linear`, `Cubic`, and `Smart`. The prior mapping
  incorrectly selected a custom 4x4 cubic B-spline for Cubic and Smart. Blender
  5.2.2 Workbench sets `use_filter` for every mode except `SHD_INTERP_CLOSEST`,
  leaving the GPU filtered sampler to perform linear filtering. The CPU path
  now follows that same behavior.
- The custom cubic shader taps remain unreachable from Workbench interpolation
  selection. This is specific to Workbench; other Blender render paths may
  implement Image Texture Cubic/Smart differently. Pinned source:
  [workbench_materials.cc](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_materials.cc#L469-L476).
- The QPainter cavity/outline effect-alpha sampler had retained a separate
  cubic B-spline for Cubic and Smart even after color sampling was corrected.
  It now uses the same bilinear filtered sample for those modes, keeping alpha
  cutouts aligned with the Workbench color pass. This follows the same pinned
  `workbench_materials.cc` image-filter setup above; visual comparison remains
  pending.

### QPainter Image Texture alpha coverage (2026-10-12)

- QPainter mesh and NURBS color paths now use sampled image alpha only for
  Blender's cutout decision. Samples below the filtered 0.1 threshold are
  discarded when alpha is enabled; surviving fragments retain the material
  or object alpha and are not made translucent by multiplying by image alpha.
  `NONE`/`IGNORE` and `CHANNEL_PACKED` continue to bypass the cutout (mesh
  images normalize alpha to opaque when loaded; NURBS checks the mode directly).
- This follows Blender 5.2.2: `workbench_image.bsl.hh` discards on sampled
  image alpha and returns image RGB, while `workbench_prepass.bsl.hh` writes
  fragment alpha from the material and X-Ray alpha. The cutoff/mode setup is
  in `workbench_materials.cc`. Pinned sources:
  [workbench_image.bsl.hh](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_image.bsl.hh#L46-L72),
  [workbench_prepass.bsl.hh](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_prepass.bsl.hh#L346-L369),
  [workbench_materials.cc](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_materials.cc#L86-L96).
- QPainter color parity is source-corrected; visual comparison remains pending.

- The QPainter effect prepass also samples prepared UV texture alpha for
  `NurbsSolid` derived face triangles, in addition to `NurbsSurface`, so
  cavity/outline/DOF object IDs follow the same image cutout and alpha-mode
  bypass rules. The offscreen contract compares a transparent solid face with
  a receiver-only scene and checks that `NONE` bypasses the cutout. This is
  effect-mask coverage only; the QPainter fallback still has no shadow pass.

### GPU Workbench curvature sample spacing (2026-10-12)

- GPU curvature neighbor reads now use `uiScale / viewportSize` offsets in
  normalized texture coordinates, matching Blender 5.2.2's curvature shader.
  Previously classiCAD rounded the UI scale to an integer texel, which
  quantized fractional device-pixel ratios. Curvature controls and cavity
  sample scheduling are unchanged. The C++ build passes; GPU image comparison
  remains pending.

### Offscreen render precision improvement and remaining float gap (2026-10-12)

- Blender's Workbench image-render path allocates its color target as
  `SFLOAT_16_16_16_16` and reads the Combined pass as four float channels;
  see [`workbench_engine.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_engine.cc#L602-L651)
  and its float readback at [lines 654-671](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_engine.cc#L654-L671).
  classiCAD's offscreen presentation target is now normalized RGBA16, read back
  with `glReadPixels(GL_RGBA, GL_UNSIGNED_SHORT)` into a top-down
  `QImage::Format_RGBA64`. This preserves up to 16 bits per display-referred
  channel through the result image. The existing Qt writer path requests
  `Format_RGBA64` for 16-bit PNG/TIFF and falls back to an 8-bit QImage if that
  write fails; other supported output formats remain 8-bit. The sidecar image
  metadata records 16-bit render precision and explicitly identifies it as
  normalized display-referred data.
- The result now also has a separate top-down interleaved RGBA-float payload
  captured from the final post-DOF scene-linear texture before display transform
  or CAD annotations. When OpenEXR is found by CMake, OPEN_EXR writes those
  channels directly from the float payload without routing through QImage.
  Blender's 16-bit OpenEXR setting now writes HALF channels with finite-range
  clamping; 32-bit writes retain FLOAT channels. Reported output precision
  follows the selected OpenEXR depth. Pinned references are Blender's
  [`image_format.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/blenkernel/intern/image_format.cc#L799-L805)
  depth-to-HALF flag and [`openexr_api.cpp`](https://github.com/blender/blender/blob/v5.2.2/source/blender/imbuf/intern/openexr/openexr_api.cpp#L652-L695)
  channel writer. OpenEXR remains optional; without its development package,
  output falls back to PNG and the float payload is not captured or written.
  The local build lacks those development headers, so this conditional EXR
  writer path has source review but no local compile coverage.
  EXR headers also carry the imported pixel width/height ratio as
  `pixelAspectRatio`; Blender derives it from `ppm_y / ppm_x` in
  [`openexr_api.cpp`](https://github.com/blender/blender/blob/v5.2.2/source/blender/imbuf/intern/openexr/openexr_api.cpp#L3071-L3082),
  and classiCAD writes the equivalent `pixelAspectX / pixelAspectY` value.
  Imported `exr_codec` now selects the matching OpenEXR header compression
  (NONE, PXR24, ZIP, PIZ, RLE, ZIPS, B44, B44A, DWAA, or DWAB); absent codec
  metadata defaults to ZIP. DWAA/DWAB quality uses Blender's integer mapping
  from quality 100→level 0 and 90→level 45, and HALF input clamps to Blender's
  DWA-dependent finite range before conversion. HTJ2K maps to 256-line mode
  only with OpenEXR 3.4+, matching Blender's version-gated path; older
  libraries retain ZIP fallback. Blender stores codec/quality in
  [`image_format.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/blenkernel/intern/image_format.cc#L3571-L3583),
  maps DWA quality and HALF range in
  [`openexr_api.cpp`](https://github.com/blender/blender/blob/v5.2.2/source/blender/imbuf/intern/openexr/openexr_api.cpp#L2802-L2850),
  and configures the codec enum in
  [`openexr_api.cpp`](https://github.com/blender/blender/blob/v5.2.2/source/blender/imbuf/intern/openexr/openexr_api.cpp#L2854-L2943).
  The normal PNG/TIFF path remains normalized and display-referred. Blender's
  registered pass lifecycle, multi-pass result API, and matched visual output
  remain open.

- Radiance HDR output now captures the scene-linear Combined payload and writes
  RGBE scanlines directly, preserving values above 1.0 instead of passing the
  display-transformed QImage through Qt's HDR plugin or falling back to PNG
  when that plugin is unavailable. Blender 5.2.2's
  [`format_hdr.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/imbuf/intern/format_hdr.cc#L51-L63)
  configures a three-channel floating-point HDR image and sends it to its HDR
  encoder. classiCAD writes the equivalent RGB Radiance representation from
  scene-linear values; alpha is not represented by RGBE. The file writer builds
  and the RGBE/RLE path has source review, but no decoder round-trip comparison
  has been run.

- EXR output honors RGB and BW color modes for both HALF and FLOAT channel
  storage, writing only R/G/B and omitting A. RGB preserves the existing
  scene-linear values, which are already premultiplied against black. BW
  computes the dot product with the bundled default Rec.709 luma coefficients
  `(0.2126, 0.7152, 0.0722)` and repeats that grayscale value into R/G/B. Blender
  applies alpha-under-black before conversion for non-RGBA output, and its
  `IMB_color_to_bw` uses `IMB_colormanagement_get_luminance`; see pinned
  [`colormanagement.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/imbuf/intern/colormanagement.cc#L2351-L2380),
  [`conversion.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/imbuf/intern/conversion.cc#L387-L405),
  and [`colormanagement_inline.h`](https://github.com/blender/blender/blob/v5.2.2/source/blender/imbuf/intern/colormanagement_inline.h#L19-L27).
  The OpenEXR writer emits R/G/B and conditionally A as documented in pinned
  [`openexr_api.cpp`](https://github.com/blender/blender/blob/v5.2.2/source/blender/imbuf/intern/openexr/openexr_api.cpp#L652-L710).
  Custom OCIO configurations with non-default luma coefficients are not
  imported, so BW output currently uses the bundled default coefficients.

- TIFF compression currently does not preserve all Blender codec settings.
  Blender 5.2.2's `ImageFormatData` defaults to DEFLATE and maps DEFLATE,
  LZW, PACKBITS, and NONE to separate TIFF writer flags in
  [`image_format.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/blenkernel/intern/image_format.cc#L782-L798);
  its TIFF backend emits `zip`, `lzw`, `packbits`, or `none` in
  [`format_tiff.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/imbuf/intern/format_tiff.cc#L55-L82).
  classiCAD passes these through `QImageWriter::setCompression`, but the Qt
  6.4 TIFF plugin only selects NONE or LZW and clamps the option to a binary
  value ([`qtiffhandler.cpp`](https://github.com/qt/qtimageformats/blob/v6.4.2/src/plugins/imageformats/tiff/qtiffhandler.cpp#L160-L168),
  [lines 906-910](https://github.com/qt/qtimageformats/blob/v6.4.2/src/plugins/imageformats/tiff/qtiffhandler.cpp#L906-L910)).
  Therefore the current default DEFLATE writes uncompressed TIFF, and
  PACKBITS writes LZW. classiCAD metadata now keeps the requested `tiffCodec`
  and adds `writtenTiffCodec` for Qt's effective NONE/LZW output. CMake has no
  TIFF/OIIO writer dependency and this environment has no TIFF development
  headers, so exact codec parity requires adding a suitable encoder; until
  then, DEFLATE/PACKBITS are known output approximations.

- PNG compression now maps Blender's 0–100 setting to the PNG writer's
  0–9 compression level using `int(compression / 11.1111)`, clamped at both
  ends. This matches Blender 5.2.2's [`format_png.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/imbuf/intern/format_png.cc#L67-L69);
  for example, Blender's default value of 15 selects level 1. The mapping only
  applies to PNG and leaves the separate TIFF codec handling unchanged.

- JPEG quality now maps Blender's nonpositive setting to quality 75 and caps
  positive values at 100 before passing them to Qt. Blender 5.2.2 applies the
  same `quality <= 0 ? 75 : min(quality, 100)` behavior in
  [`format_jpeg.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/imbuf/intern/format_jpeg.cc#L742-L780).
  Qt 6.4.2's [`qjpeghandler.cpp`](https://github.com/qt/qtbase/blob/v6.4.2/src/plugins/imageformats/jpeg/qjpeghandler.cpp#L566-L567)
  treats explicit quality 0 as 0 rather than its default, so the import value
  now receives Blender's fallback before writing. Both use libjpeg defaults
  for subsampling without overriding sampling factors.

- Targa output now uses a small local 8-bit encoder instead of depending on the
  installed Qt TGA plugin. It writes grayscale for BW, BGR for RGB, and BGRA
  for RGBA, selects raw packets for `TARGA_RAW` and TGA RLE packets for
  `TARGA`, and marks the image origin as top-left. Blender 5.2.2's
  [`format_targa.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/imbuf/intern/format_targa.cc#L367-L382)
  uses the selected color-mode channel count, UINT8 samples, and the same raw
  versus RLE setting. classiCAD emits valid grayscale (types 3/11) and true
  color (types 2/10) TGA streams. TGA's 16-bit output option is not represented;
  Targa remains an 8-bit format in this writer. File decoding and visual
  comparison remain pending.

### Opaque-film scene World background (2026-10-12)

- Blender Workbench imports each scene's `World.color` as scene-linear RGB in
  render-output metadata. For opaque-film offscreen Combined results,
  classiCAD uses this color when Workbench `background_type` is not
  `VIEWPORT`; `VIEWPORT` keeps its imported shading color. If that background
  type is selected but the scene has no World, the offscreen target remains
  transparent as Blender's zero clear color does. Transparent film also
  clears without drawing a background. The World color reaches the scene-linear
  target directly without an 8-bit `QColor` conversion; the interactive
  `WORLD` background now uses the same imported scene-linear World color.
- Blender 5.2.2 chooses `shading.background_color` only for `VIEWPORT` and
  otherwise uses `scene->world` horizon RGB while rendering opaque film in
  [`workbench_state.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_state.cc#L175-L183).
  Workbench clears its render framebuffer from that state color in
  [`workbench_engine.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_engine.cc#L488-L494).
  Blender's [`rna_world.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/makesrna/intern/rna_world.cc#L235-L240)
  maps `World.color` to the horizon-color field. Matched image comparison
  remains pending.
- The same imported scene World color now drives the interactive viewport when
  `background_type` is `WORLD`. The active imported scene is resolved through
  its selected view-layer key; when a World is absent or old metadata lacks its
  color, the existing neutral gray fallback remains. GPU rendering receives
  the scene-linear RGB directly, while the QPainter fallback applies the
  configured Workbench display transform before its display-referred fill.
  `THEME`, `VIEWPORT`, and Custom
  backgrounds keep their prior paths. Blender's pinned state selection above
  provides the World-versus-viewport behavior.
- **QPainter HDR conversion update:** World RGB now passes through the shared
  Workbench exposure, view-transform LUT, and gamma conversion before it is
  placed in `QColor`, so QColor's bounded channels no longer discard HDR range
  before view transformation. A focused contract checks Standard transform
  plus exposure on an HDR input. Matched GPU/QPainter images and custom LUT
  comparison remain open.
- **Remaining offscreen validation:** add a no-geometry background contract
  matrix using distinct RGB values. With opaque film, `THEME` and `WORLD` must
  clear to the imported scene World color, while `VIEWPORT` must use the
  imported shading background color. With no World assigned, `THEME` and
  `WORLD` leave the target alpha transparent; with transparent film, every
  mode leaves the background transparent. Check scene-linear Combined values
  and alpha, then display-referred output to cover premultiplication and
  compositing. These cases are not yet claimed as validated by matched output.

### Transparent-film Combined alpha (2026-10-12)

- Blender 5.2.2 exposes `RenderSettings.film_transparent` as the
  `R_ALPHAPREMUL` bit of `Scene.r.alphamode`; Workbench uses that same bit to
  leave its render background transparent. In addition, `SceneState` starts
  with a transparent background and leaves it that way when the render uses a
  World background but no scene World exists. The importer retains the film
  setting with render-output metadata; the offscreen Combined path also carries
  the missing-World case as transparent. Interactive viewport rendering remains
  opaque. Pinned sources:
  [`rna_scene.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/makesrna/intern/rna_scene.cc#L7233),
  [`workbench_state.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_state.cc#L175-L184),
  and [`workbench_engine.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_engine.cc#L488-L494).
- For transparent-film and missing-World PNG/TIFF output, the presentation
  transform now
  unpremultiplies RGB before the nonlinear view transform and premultiplies the
  result afterward. Blender's color-managed output path predivides premultiplied
  render buffers before processing; see
  [`colormanagement.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/imbuf/intern/colormanagement.cc#L1574-L1582)
  and [lines 2124-2165](https://github.com/blender/blender/blob/v5.2.2/source/blender/imbuf/intern/colormanagement.cc#L2124-L2165).
  The same handling applies when `film_transparent` is false but the requested
  World background has no scene World: Workbench still leaves target alpha
  transparent. Scene-linear EXR/HDR Combined capture remains before the display
  transform and is unchanged.

### Weighted transparency resolve (2026-10-12)

- The GPU weighted-OIT resolve now divides accumulated RGB directly by the
  Blender-clamped accumulated weight without clamping negative channels to
  zero. Blender 5.2.2's pinned
  [`workbench_transparent_resolve_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_transparent_resolve_frag.glsl#L269-L286)
  performs that same signed division. The accumulation weighting, weight
  limits, and revealage resolve also match its pinned
  [`workbench_transparent_accum_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_transparent_accum_frag.glsl#L435-L470).
- classiCAD retains local camera-depth reconstruction and a resolve path that
  accounts for its scene-linear and display-transform stages; these remain
  renderer adaptations requiring matched visual comparison.
- Selected transparent ranges now decode their sRGB edit-selection tint before
  mixing it into the primary scene-linear OIT accumulation, matching the
  opaque selected-range path. Legacy display-space accumulation keeps its
  existing tint behavior.

### GPU Workbench outline alpha blending (2026-10-12)

- The outline shader emits straight RGB with fractional coverage in alpha.
  Its blend state now uses `SRC_ALPHA / ONE_MINUS_SRC_ALPHA` for RGB and
  `ONE / ONE_MINUS_SRC_ALPHA` for alpha. This keeps the existing RGB result
  while avoiding the previous second multiplication of outline coverage in
  the output alpha.
- Blender 5.2.2's pinned
  [`workbench_effect_outline.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_effect_outline.cc)
  uses premultiplied-alpha blending, and
  [`workbench_effect_outline_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_effect_outline_frag.glsl)
  multiplies its outline color by coverage. The classiCAD blend factors now
  produce equivalent alpha composition with its straight-alpha shader; matched
  GPU output comparison remains pending.

### GPU Workbench outline UI-scale sampling (2026-10-12)

- The outline pass now samples its four cardinal object-ID neighbors at the
  imported device/UI scale as a floating-point offset in normalized viewport
  coordinates. It no longer rounds fractional display scale to an integer
  texel step. This follows Blender 5.2.2's
  [`workbench_effect_outline_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_effect_outline_frag.glsl#L256-L271)
  and the `U.pixelsize` setup in
  [`workbench_resources.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_resources.cc#L100-L109).
  The integer ID texture uses nearest filtering and clamp-to-edge, so visible
  changes are limited to fractional-scale boundary cases; matched rendering
  remains open.

### GPU Studio lighting with specular disabled (2026-10-12)

- Studio lighting now always multiplies its ambient/specular accumulator by
  `specularColor`. When specular is disabled, `specularColor` is zero, so the
  accumulator contributes nothing; the enabled-specular path keeps the same
  Fresnel-adjusted calculation.
- This follows Blender 5.2.2's pinned
  [`workbench_world_light.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_world_light.bsl.hh#L57-L121):
  it sets `specular_color` to zero when disabled and multiplies
  `specular_light` by that value after the conditional. Matched GPU output
  comparison remains pending.

### QPainter Studio lighting with specular disabled (2026-10-12)

- The QPainter Studio helper now multiplies its ambient/specular accumulator
  by `specularColor` after the specular conditional. With specular disabled,
  that color is zero and no additive ambient specular term remains. This makes
  the fallback match the GPU helper and Blender's shader behavior.
- Blender 5.2.2 performs the multiplication unconditionally in
  [`workbench_world_light_lib.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_world_light_lib.glsl#L580-L713).
  Matched QPainter output comparison remains pending.

### Workbench no-material fallback (2026-10-12)

- Blender 5.2.2 `get_material()` selects `BKE_material_default_empty()` for
  Material, Attribute, and Texture color modes when no assigned material or
  supported input is available. That default material uses the DNA defaults
  `(0.8, 0.8, 0.8, 1.0)`; only Object and Random color modes use `ob.color`.
  Pinned sources: [`workbench_engine.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_engine.cc#L125-L145),
  [`workbench_materials.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_materials.cc#L19-L37),
  and [`DNA_material_types.h`](https://github.com/blender/blender/blob/v5.2.2/source/blender/makesdna/DNA_material_types.h#L372-L394).
- classiCAD tracks material presence with the persisted `hasWorkbenchMaterial`
  flag. Its GPU mesh/NURBS ranges and QPainter NURBS Material/Attribute/Texture
  paths now use the default linear 0.8 gray and opaque alpha when no material
  is assigned. QPainter mesh Material/Attribute/Texture already used that
  fallback. Object and Random modes continue to use object display color and
  alpha. Native documents write version 20; older documents retain their
  previous material appearance when loaded.

### GPU opaque backface normal orientation (2026-10-12)

- Opaque fragments now flip their interpolated view normal when the fragment
  faces away from the camera, matching Blender's Workbench opaque prepass.
  The shader enables this only for fully opaque ranges; weighted and sorted
  transparent ranges retain their existing normal orientation.
- Blender 5.2.2 passes `[[front_facing]]` to `normal_encode` in
  [`workbench_prepass.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_prepass.bsl.hh#L306-L314),
  and [`workbench_common.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_common.bsl.hh#L35-L43)
  flips the normal for backfaces. Its separate transparent shader uses the
  interpolated normal directly at
  [`workbench_prepass.bsl.hh:332-345`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_prepass.bsl.hh#L332-L345),
  so classiCAD gates the orientation fix to opaque draws. Matched GPU output
  comparison remains pending.

### GPU scene-stroke alpha composition (2026-10-12)

- The classiCAD GPU scene-stroke shader emits straight RGB and coverage-scaled
  alpha, so its blend state now uses source-over alpha factors separately from
  RGB. This avoids squaring antialias coverage in the destination alpha.
- This is specific to classiCAD's coverage-blended strokes. Blender 5.2.2's
  overlay wireframe pass writes its wire color directly and records line data
  for the later overlay antialias path; it does not use this classiCAD blend
  state. Pinned sources:
  [`overlay_wireframe.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/overlay/overlay_wireframe.hh#L948-L970)
  and
  [`overlay_wireframe_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/overlay/shaders/overlay_wireframe_frag.glsl#L400-L407).
- The C++ build passes; matched viewport image comparison remains pending.

### QPainter cavity silhouette samples (2026-10-12)

- The QPainter Solid cavity path now follows Blender's background-neighbor
  rule: it reconstructs the offset sample on the receiver-depth camera plane
  and offsets it by the configured cavity distance before evaluating its
  signed displacement response. Blender 5.2.2 performs this for background
  depth reads in
  [`workbench_cavity.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_cavity.bsl.hh#L535-L547).
- Empty samples outside the image and the software rasterizer's discrete
  neighborhood remain approximate; matched QPainter/Blender image comparison
  has not been performed.

### QPainter cavity under translucent surfaces (2026-10-12)

- This update supersedes earlier parity-matrix and implementation-plan wording
  that said cavity was skipped beneath visible translucent surfaces.
- The QPainter surface compositor retains its opaque surface color and its
  resolved transparent contribution for the later cavity pass. When a
  translucent surface covers an opaque cavity receiver, cavity now changes
  the opaque contribution first and preserves the transparent contribution,
  matching Blender's opaque cavity result followed by transparent color
  compositing. Blender 5.2.2 draws the opaque pass before the transparent pass
  in [`workbench_engine.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_engine.cc#L496-L499);
  its cavity effect is part of the opaque resolve and affects opaque surfaces
  in [`workbench_effect_cavity.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_effect_cavity.cc#L7-L13).
- Because the QPainter cavity effect runs after scene strokes are drawn, it
  applies the opaque-layer color delta to the final pixel to keep those strokes
  intact. Surface/stroke overlap can therefore remain approximate; matched
  QPainter/Blender image comparison has not been performed.

### QPainter scene-linear surface resolve (2026-10-12)

- **Blocker:** the QPainter Workbench path does not retain scene-linear
  surface color through cross-object composition. `ViewportRenderer` applies
  `workbenchSceneLinearToDisplaySrgb` while rasterizing mesh triangles and
  writes 8-bit display color; NURBS fills pass through `QColor`; then
  `compositeRasterWorkbenchSurfaces` resolves per-object `ARGB32` layers.
  That path loses HDR values before the resolve and cannot reproduce Blender's
  transform-after-composition ordering. Merely changing the intermediate
  `QImage` format to RGBA64 or RGBA32F is insufficient: it does not make the
  existing QColor path preserve values above 1, guarantee QPainter blending in
  scene-linear space, or convert the byte-oriented cavity/outline effects.
- **Next implementation contract:** add a production float fragment interface
  shared by mesh and NURBS surface rasterization. Emit shaded scene-linear RGB,
  alpha, and depth without passing surface colors through QColor or QPainter.
  Resolve visible opaque and transparent fragments into a premultiplied float
  buffer using depth-tested source-over, preserving RGB above 1. Apply the
  supported QPainter cavity/outline operations to this buffer in float space,
  then apply the selected display transform exactly once to the completed
  scene layer. Include the scene-linear background in that resolve when the
  output is opaque; keep grid, selection, annotation, and other display UI
  overlays after the transform. For transparent film, retain linear Combined
  RGB and alpha independently from the display preview so later background
  composition does not blend already transformed translucent colors.
- **Required offscreen regression:** exercise the production float resolver
  with a known opaque background and two overlapping translucent surface
  fragments. For far fragment `(C1, a1)` and near fragment `(C2, a2)`, assert
  the resolved linear RGB equals
  `a2*C2 + (1-a2)*(a1*C1 + (1-a1)*B)` and alpha remains one. Include at least
  one channel above 1 before display conversion; assert that it survives the
  resolve and that output equals one transform of the expected composite.
  Also assert the result differs from transforming each surface before
  blending. Keep the ordinary interactive Solid viewport at zero render
  dither. The current compositor has no float fragment API to test, so this
  regression belongs with that resolver when the implementation is introduced;
  the present QPainter output is explicitly not claimed to pass it.

### Transparent depth and Blender Object In Front (2026-10-12)

- Blender 5.2.2 stages opaque `OBJECT_IN_FRONT` geometry against scene depth,
  marks its pixels with `OBJECT_IN_FRONT` stencil, copies depth into
  `depth_in_front_tx`, and excludes marked pixels from regular opaque and
  transparent passes. Transparent accumulation then uses regular and in-front
  depth domains separately. `TransparentDepthPass` writes in-front depth,
  object ID and stencil, merges depth into scene depth only for marked pixels,
  and finally draws regular transparent depth while excluding those pixels.
  Pinned source:
  [`workbench_mesh_passes.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_mesh_passes.cc#L306-L369).
- The import and render-frame metadata path now carries Blender
  `Object.show_in_front` through `SceneObject` to `ViewportRenderObject`.
  Collection membership and CAD layer visibility remain independent.
- GPU opaque-surface support now uses per-sample in-front color, depth, normal,
  object-ID, and outline-ID attachments. The surface prepass renders opaque
  show-in-front ranges in their own depth domain, merges their metadata into the
  shared scene attachments, and merges color/depth before DOF, TAA, and SMAA
  resolve. The unjittered viewport depth path also has a dedicated in-front
  depth target for scene occlusion and picking. C++ build passes; GPU image
  comparison remains pending.
- In-front opaque color now has a packed depth-stencil target. The renderer
  counts shadow volumes against the in-front prepass depth and shades the front
  ranges through the zero/nonzero stencil split, matching Blender's shared
  shadow-stencil resolve path. If the front stencil-volume path cannot be set
  up, the existing shadow-map fallback covers all casters. No matched image
  comparison has been made.
- GPU shared-target rendering now accumulates transparent show-in-front ranges
  into independent weighted-OIT accumulation/weight targets against the
  front depth domain. Ordinary transparent accumulation excludes opaque
  front-owned coverage. The front result resolves over regular OIT color, then writes their
  depth/object/outline IDs into the front attachments and merges those into the
  shared depth/ID targets before regular transparent depth. The regular depth
  pass discards front-owned coverage. This follows Blender's regular OIT,
  front OIT, front transparent-depth merge, then regular transparent-depth
  order. Front transparent depth deliberately preserves the normal attachment;
  Blender's transparent-depth pass does not write normals, and front opaque
  geometry owns its normal/material attachments.
- In no-shared-target or target-allocation fallback cases, transparent ranges
  retain the existing sorted/regular OIT behavior. The QPainter fallback has
  no separate in-front surface domain. GPU pixel comparison remains pending;
  these paths are not claimed as Blender parity.
- Named pass reporting now includes front opaque color completion, and the
  offscreen Workbench result forwards populated scene attachments to the AA
  validity record, matching the interactive viewport without changing image
  composition. Front transparent color is reported as a completed pass and
  current-sample color output; no separate persistent attachment is claimed
  because its OIT result is composited directly into the current sample.
- Transparent-depth completion now advertises only depth and object-ID
  attachments. Blender 5.2.2's `TransparentDepthPass` binds depth, empty
  material/normal attachments, and object ID in
  [`workbench_mesh_passes.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_mesh_passes.cc#L305-L333).
  classiCAD likewise disables its normal draw buffer for this pass. View-space
  normals remain valid only when
  the earlier opaque effects prepass populated them. This prevents the AA
  attachment-validity record from claiming an uninitialized normal texture in
  transparent-only scenes without a cavity, outline, or front-object prepass.
- Mixed-alpha Solid scenes now request the shared outline prepass. The shared
  object/outline IDs are used only when regular transparent depth and any
  required front transparent depth merge both completed; sorted or failed
  target fallbacks retain the private outline-ID path. Regular transparent
  depth preserves front-owned coverage, including front opaque pixels.

### GPU Workbench two-blade aperture (2026-10-12)

- GPU DOF aperture shaping now follows Blender's `blades > 1` condition, so
  the allowed two-blade setting produces the corresponding slit aperture
  instead of falling back to a circular disk. The previous `>= 3` threshold
  disagreed with Blender 5.2.2 `DofPass::setup_samples`; see the pinned source
  link below. Polygon sector orientation now also matches Blender: rotation is
  applied once to sample coordinates after the sector radius is calculated.
  Other aperture geometry and image parity remain unverified.

### GPU Workbench DOF half-resolution sizing (2026-10-12)

- The GPU DOF targets now use `max(resolution / 2, 1)` per dimension and the
  resolve samples those floor-sized targets with Blender's half-resolution UV
  scale. This matches Blender 5.2.2 for odd-sized render targets. Pinned
  references: [`workbench_effect_dof.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_effect_dof.cc#L98-L100)
  and [`workbench_effect_dof_resolve_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_effect_dof_resolve_frag.glsl#L24-L32).
- The GPU path has Blender's CoC-aware three-level downsample, half-resolution
  ring gather, 3x3 median, and resolve. The 49 shader-generated square-ring
  samples follow `DofPass::setup_samples`' perimeter enumeration and
  square-to-circle radius mapping. Per-sample mip selection now follows the
  pinned gather equation: `log2(clamp((coc + min(coc, max_radius)) * 0.5 -
  21.0, 0.0, 16.0) * 0.25)`, with negative LOD clamped to the base level as
  the texture sampler does. Its per-pixel gather jitter now follows
  pinned `get_random_vector`: it combines the second channel of the shared
  64x64 EEVEE blue-noise LUT with interleaved gradient noise, applies
  `sqrt(ign)`, scales the offset by the maximum CoC, and uses Blender's
  full-resolution pixel scale for the half-resolution gather. The sample
  pattern is no longer rotated by per-pixel noise. The per-sample CoC weight
  and smoothstep interval also match the pinned shader. Aperture shaping follows
  Blender's `denom = T - (2π/blades) * floor((blades*T + π)/(2π))`, polygon
  radius `cos(π/blades) / cos(denom)`, `T += rotation`, and reciprocal X ratio.
  The sector angle is computed before adding aperture rotation, matching
  Blender's setup; the former shader subtracted rotation in the sector angle
  and then added it again for sample coordinates. Its denominator no longer
  has an epsilon floor, preserving Blender's two-blade edge equation.
  Aperture and overall output still need visual comparison.
  Pinned source: [`workbench_effect_dof_blur1_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_effect_dof_blur1_frag.glsl#L20-L31), with the texture populated in [`workbench_resources.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_resources.cc#L84-L100). The QPainter fallback remains a fixed 12-tap full-resolution gather and has no median pass. Its aperture shaping now uses Blender's `blades > 1` condition and polygon/rotation/reciprocal-X-ratio mapping, including the two-blade slit. Image comparison remains open. Pinned gather setup: [`workbench_effect_dof.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_effect_dof.cc#L30-L75); the GPU stages are defined by the adjacent pinned Workbench DOF shaders.

### GPU and QPainter DOF optical sensor fit (2026-10-12)

- Camera projection uses an effective sensor value adjusted for classiCAD's
  viewport framing math. DOF now uses a separate `opticalSensorSizeMillimeters`
  field, so projection zoom compensation cannot change CoC. Imported Blender
  camera fit follows `BKE_camera_sensor_size`: explicit VERTICAL selects
  `sensor_y`; HORIZONTAL and AUTO select `sensor_x`. This matches Blender's
  `DofPass::init`, which computes CoC pixels from the fitted sensor and
  multiplies by render width. Pinned sources:
  [`workbench_effect_dof.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_effect_dof.cc#L119-L147)
  and [`camera.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/blenkernel/intern/camera.cc#L324-L342).
- Offscreen scene rendering has no `RegionView3D` camera border factor, so it
  uses the fitted optical sensor directly. Blender applies
  `rv3d->viewcamtexcofac[0]` only when an `rv3d` exists; classiCAD does not
  currently import/use that factor for an interactive Blender camera-view
  viewport. That camera-view behavior remains unsupported. Native camera
  settings and session state preserve the optical sensor field; legacy files
  without it fall back to the previous sensor-width value. Render-result
  metadata keeps `sensorWidthMillimeters` as the source camera's actual X
  sensor dimension and records the fitted optical size separately. Visual
  comparison remains open.

### Background regression correction

Compared with GitHub `origin/feature/3d-workplanes` commit `6722914`, the theme
background retains its original radial gradient and RGB 61/48 endpoints.
The AA port changed the Qt widget to single-sample presentation but retained an
sRGB backing texture; Qt decoded the already display-encoded viewport during
composition, darkening the background, grid, and objects. The presentation
texture used RGBA8 while the private AA scene texture was SRGB8_ALPHA8. The
later scene-linear migration changes that private target to RGBA16F; the
presentation texture remains RGBA8.
Live GPU startup also exposed two AA initialization errors: SMAA metrics must be
declared before the library functions, and depth/stencil use one packed texture
for drivers that reject separate attachments. These are corrected; full
Workbench parity still requires the reviews and comparisons below.

Validation: `cmake --build build` and the dependency audit pass. Live X11 GPU
startup reports no shader/framebuffer initialization errors; the same sampled
background pixel changes from RGB (9, 9, 9) to (52, 52, 52), within the original
gradient range. Offscreen startup also succeeds. The user's unsaved scene was
not reopened or altered; the visual check used a separate empty viewport.

This is the status after the first implementation pass and the team's code review. “Partial” means the feature exists but still differs from Blender or lacks validation. Do not treat this snapshot as a parity sign-off.

| Area | Current behavior | Port status |
|---|---|---|
| Shading modes | Solid and Wireframe are present. GPU and QPainter paths each implement Studio, MatCap, and Flat with separate code. | Partial; visual parity between both paths is unverified |
| Studio lights and MatCaps | Blender 5.2.2 studio-light presets and 27 MatCaps are bundled. The studio-light equations and default light rig are adapted from Blender; scene lights are not used. MatCap horizontal flip is exposed in the shading popover, saved/restored with viewport settings, included in AA history invalidation, and applied to both GPU surface sampling and the settings preview. MatCap and AgX textures use linear filtering. | Foundation ported; final output parity remains unverified |
| Color management | Imported scenes retain display device, view transform, look, exposure, and gamma for Workbench render results. The interactive Solid/Wireframe GPU viewport uses Blender Workbench's default view transform; Blender applies scene render color settings there only for Rendered mode or when View3D Scene Lights/Scene World is enabled. classiCAD does not currently expose those View3D modes. Live GPU rendering keeps scene surfaces, transparency, pictures, and backgrounds in RGBA16F scene-linear color through depth of field and temporal AA, then applies exposure, the active sRGB AgX/Standard/PBR Neutral/Filmic or imported OCIO LUT, and gamma once during presentation. Offscreen Workbench results now run the configured viewport AA sample mode through the same scene-linear resolve and retain the resolved float Combined payload for Radiance HDR and OpenEXR when available. Other non-EXR display-referred output uses RGBA16/QImage, so HDR range is lost before 16-bit PNG/TIFF writing. QPainter Solid rendering retains its CPU display-transform path. Other sRGB view/look combinations use validated Blender-baked LUTs shared by scenes with identical table bytes. Custom 65³ LUTs use an analytic `log2_1p` shaper over [0, 16] (scale 64); older native LUTs retain their linear [0, 2] metadata. Inputs outside the stored domain clamp to its endpoints. | Partial; render-specific sample-count mapping, non-sRGB display devices, arbitrary OCIO configuration transfer, LUT precision/visual comparison, GPU/QPainter differences, and matched render comparisons remain open |
| Surface drawing | NURBS surfaces and solids are tessellated for display and shaded by classiCAD's custom forward GLSL renderer. Curves use the stroke path. | Different rendering architecture; visual parity remains unverified |
| Color source | Object, Random, Material, and Custom color plus their Blender-compatible alpha sources reach filled NURBS surfaces/solids in GPU and QPainter paths. When no Workbench material is assigned, Material/Attribute/Texture use Blender's opaque default gray material; Object and Random use object display color and alpha. Blender mesh import preserves object display colors, shared materials, per-face slots, active UV maps, and image references. GPU mesh Texture mode samples the active Image Texture Node per fragment through interpolated active-UV coordinates, with repeat wrapping and linear filtering; supported external raster files are loaded with size limits and cached by path/color space. QPainter mesh Texture mode rasterizes each triangle per target sample, uses reciprocal camera-forward depth for perspective-correct UVs, and bilinearly samples external and packed images with repeat wrapping. An audit found the reciprocal weight used the camera-to-scene direction with the wrong sign, collapsing varying positive depths to the clamp; mesh and NURBS raster paths now use positive camera-forward depth. UDIM tiles now preserve Blender 5.2.2 per-tile packed bytes in native documents and sample them in GPU and QPainter mesh and NURBS paths before falling back to external tile paths; import caps packed payloads at 32 MiB total per image, and absent tiles remain magenta. GPU and QPainter NURBS Texture sample trimmed and untrimmed surfaces using tessellation parameter UVs normalized over the exact knot domains. This matches Blender 5.2.2's index-normalized UVs because Blender samples uniformly over each active parameter domain. QPainter mesh and NURBS Texture honor the imported image sRGB flag: unpremultiply RGB as before, decode sRGB samples to scene-linear when requested, multiply with the material base color before display conversion, and apply the active exposure/gamma/OCIO or built-in transform. QPainter textured-mesh shading reconstructs its incident direction and normal per raster pixel: perspective samples use perspective-correct position/normal interpolation and orthographic samples use barycentric normal interpolation; corner normals are transformed by the world inverse transpose, with a flat geometric normal as fallback for invalid or absent corner data. QPainter untextured Workbench mesh triangles now rasterize per pixel using scene-linear material/object/random/custom/attribute colors, per-face roughness and metallic, interpolated corner normals, and perspective-correct incident direction. Random and Custom inputs use Blender-compatible color conversion; Attribute keeps the existing triangle-averaged color and decodes it only when its metadata says sRGB. Raster size limits retain the existing flat polygon fill for oversized untextured triangles. Orthographic incident direction remains constant. QPainter untextured NURBS tessellation triangles are also rasterized and shaded per pixel using their flat triangle normal; perspective samples reconstruct their world position, while orthographic incident direction remains constant. Textured NURBS samples continue through the per-pixel material lighting path. GPU and QPainter texture-alpha cutouts apply in color rendering for trimmed and untrimmed NURBS. GPU object-ID/outline and shadow-alpha paths, and QPainter cavity/outline effect-alpha paths, also cover trimmed and untrimmed tessellated NURBS using active-domain UVs and Blender's alpha rules. The alpha cutoff is 0.1 when enabled and is bypassed for NONE/Ignore and CHANNEL_PACKED. Packed Image Texture bytes up to 32 MiB are transported, persisted, and sampled by both paths. Missing images or UVs fall back to Material. Both renderers unpremultiply PREMUL images except at alpha 0 or 1. Blender Workbench's pinned [`workbench_image_lib.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_image_lib.glsl#L34-L60) performs UDIM lookup or regular image sampling, unpremultiplies PREMUL pixels, and discards fragments below `image_transparency_cutoff`; Workbench uses supported material display values and its active Image Texture input rather than evaluating arbitrary shader graphs. | Partial; color/effect alpha coverage is implemented for the described passes, while QPainter composition, trim tessellation, and final visual comparisons remain approximate or unverified |
| Blender scene import | File > Import > Blender Scene invokes the pinned Blender 5.2.2 extractor and offers original or evaluated mesh mode. File > Open now routes ordinary `.blend` files through the same original-mode extractor and loads the supported result as a new document; native classiCAD `.blend` files still restore their embedded authoritative snapshot. Imported polygon meshes retain geometry, custom split normals, object/material data, direct links to multiple Blender collections, collection hierarchy and global hide flags, plus per-view-layer collection-instance paths/exclusions/visibility and object hide state. Mesh loose edges are emitted as a companion CURVE item with a deterministic derived transport identity, separate from the source mesh identity. Each edge becomes a world-space degree-one NURBS component; the companion is named from the source object and its transport record carries the source object ID, while object display color, collection memberships, visibility, and per-view-layer state. Isolated vertices remain explicitly reported as omitted because classiCAD has no Blender point-item import path. Evaluated Blender FONT objects convert to baked polygon meshes with evaluated transforms and material assignments; original text/font/layout editability is lost and reported. Evaluated Blender METABALL objects also convert to baked polygon meshes with evaluated transforms and material assignments; original mode explicitly reports them unsupported, and editable metaball data and threshold settings are not retained. Exact Blender CURVE POLY splines import as dimension-3 degree-1 NURBS after matrix_world is applied; cyclic splines explicitly close. Each cubic Blender BEZIER span imports as a separate nonrational clamped degree-3 NURBS with its four transformed endpoint/handle points; cyclic spans include last-to-first, including the one-point cyclic loop. Blender CURVE NURBS splines preserve their weights, degree, generated knot multiplicities/domain, and cyclic wraparound control points in dimension-3 NURBS. Blender SURFACE NURBS splines import as separate exact NURBS surface objects, with world-space control nets, U/V knot domains, weights, cyclic directions, per-patch identities, and assigned Workbench material values preserved. Multiple source curve splines remain components of the same imported source object. Object display name/color and collection memberships are retained. Blender parent relationships are not represented; supported child objects retain their world-space geometry/transform and now report this hierarchy loss. Bevel/extrude, bevel-object, and 2D fill surfaces are explicitly reported as omitted while exact centerlines are kept. No curve approximation is emitted. The Collections panel selects among imported Blender view layers; selected view-layer visibility resolves each object's direct collection memberships across visible paths. Linked objects combine memberships and per-view-layer hide state. The default option preserves per-scene first-layer visibility. Collections remain independent of CAD layers. Transport identity keys use a SHA-256 namespace derived from the normalized absolute source .blend path for objects, materials, collections, scenes, view layers, and their references; moving or renaming the source project changes that namespace. Native document version 15 persists Blender camera, render-output, imported-scene, and deduplicated OCIO LUT metadata and reads versions 1–14; the scene extractor bridge accepts versions 10 through 14. Ordinary `.blend` Open and File > Import > Blender Scene now apply the selected saved `VIEW_3D` area's `SpaceView3D.shading` to the interactive viewport, with a deterministic policy when several areas exist, while `scene.display.shading` remains the scene-level render setting source and `scene.display.render_aa` remains available to offscreen results. | Partial; additional object types and visual/import coverage remain gaps. Cameras, lights, and empties currently have no matching committed classiCAD scene-object geometry type; the extractor reports these objects (the active camera's supported settings are separately retained as scene metadata). In particular, an Empty that instances a collection is metadata-only: its referenced collection's objects are not expanded into imported items at the instance transform. Blender Workbench Solid uses its studio-light rig rather than scene light datablocks. Adding camera/light/empty datablocks as editable scene objects needs an explicit model/serialization/rendering contract, so they are not fabricated as points or proxy meshes. |

**Import update:** The scene extractor bridge now accepts versions 10 through 14; the native document schema remains version 15 and reads versions 1 through 14. Ordinary Blender `.blend` Open now imports the first scene's `scene.display.shading` Solid settings into the initial classiCAD viewport and preserves `scene.display.render_aa` for offscreen Workbench render results. It maps bundled studio-light/MatCap presets, color source and background, shadow/cavity/curvature/outline controls, `show_xray`, the distinct `show_xray_wireframe`, `matcap_flip_x` when Blender exposes it, and studio-light rotation converted from radians to degrees. Custom unavailable presets, scene-light rendering, studio-light background opacity/blur, and Blender 5.2.2's RNA-hidden MatCap flip are reported as unsupported. Blender 5.2.2 Workbench does not use `studiolight_intensity`. Imported World backgrounds are retained and used for both interactive and offscreen rendering. In Original mesh mode, source geometry stays unevaluated while the active camera and focus-object transforms are resolved from the scene dependency graph. Existing classiCAD project opens do not apply imported Blender shading. Ordinary `.blend` Open creates a new classiCAD document; its source path is not used as the Save target, so users must use Save As to create a native project.
| Material values | Per-object base color, roughness, metallic, and alpha are serialized and consumed by both renderers. Imported Blender meshes retain shared material records and per-face slot references; GPU and QPainter mesh paths select face material values. Attribute colors also reach both mesh renderers. classiCAD currently has no UI for authoring appearance values. | Partial; material packing matches Blender's `Material::pack_data`; material authoring remains a gap |
| Culling and X-Ray | GPU surface culling works; X-Ray uses translucent fill plus depth/stroke passes. GPU and QPainter polygon-mesh paths classify every finite negative mesh-transform determinant as reflected by sign, without a magnitude threshold. GPU swaps triangle winding; QPainter reverses culling sense. QPainter applies backface culling to polygon-mesh triangles using transformed world-space winding and the active view direction. Its standalone outline-ID fallback draws regular ranges first, then clears only depth and draws `showInFront` ranges over the preserved regular IDs; the ID shader retains the same texture-alpha cutoff rules. Blender 5.2.2 routes X-Ray and transparent in-front objects through dedicated accumulation/depth passes in [`workbench_engine.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_engine.cc#L238-L250) and writes in-front IDs/depth before regular transparent coverage in [`workbench_mesh_passes.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_mesh_passes.cc#L309-L368). QPainter NURBS and solid face culling now uses projected tessellated triangle winding; opaque backface normals flip to face the camera, while transparent shading keeps its source normal orientation. Blender 5.2.2 tracks inverted handedness in `ResourceID` and applies it to front-facing state; duplicate transforms set the negative-scale flag using determinant sign in [`draw_context.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/intern/draw_context.cc#L933-L946), [`draw_handle.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/intern/draw_handle.hh#L60-L69), and [`draw_command.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/intern/draw_command.cc#L176-L176). Its opaque normal prepass receives `[[front_facing]]` and flips backface normals in [`workbench_prepass.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_prepass.bsl.hh#L306-L314) and [`workbench_common_lib.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_common_lib.glsl#L28-L33); the transparent shader uses its interpolated normal directly in [`workbench_transparent_accum_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_transparent_accum_frag.glsl#L59-L64). In Solid mode, classiCAD's bounded surface-batch compositor uses per-pixel nearest opaque depth for NURBS and polygon-mesh fills and sorts precomposited transparent object fragments by their per-pixel nearest tessellation depth, with a four-million-fragment cap. If prepared geometry or either memory budget is unavailable, the fallback draws surface objects in average view-depth order and sorts polygon-mesh and NURBS-surface triangles by projected centroid. It has no general scene depth buffer for strokes or separate X-Ray depth/stroke passes. | Partial; inter-object transparent ordering improves within the bounded compositor, while precomposited intra-object self-occlusion, X-Ray, and fallback ordering still differ |
| Shadows and outlines | Validated closed opaque `NurbsSolid` and imported `PolygonMesh3D` casters use depth-pass stencil side volumes when all source and extruded vertices clear the camera near plane; otherwise closed solids, open `NurbsSurface` casters, and transparent casters use the capped depth-fail stencil path. Opaque in-front receivers now use their own depth-stencil domain for volume shadows. Blender 5.2.2 uses stencil shadow volumes and has no directional depth-map fallback. classiCAD retains a 1024-pixel directional depth map with 3x3 PCF only as a GPU compatibility fallback for unsupported topology; this is not Blender behavior. GPU shadow-map alpha supports trimmed and untrimmed NURBS via tessellation UVs, while the GPU map fallback has narrower caster coverage. Stencil shadow-volume casters do not sample texture alpha. The QPainter Solid fallback does not render shadows; CPU shadow rendering is an out-of-scope approximation. Focus/shift and light direction use Blender's formulas in the GPU path. GPU and QPainter silhouette opacity use Blender Workbench's four-cardinal-neighbor object-ID resolve. For classiCAD's additional selection-color behavior, both backends now use four-cardinal-neighbor adjacency. Blender's outline pass uses one outline color and has no selected-color behavior. GPU object-ID/outline alpha and QPainter cavity/outline effect alpha cover trimmed and untrimmed NURBS texture cutouts. QPainter effects and the classiCAD-only GPU depth-map fallback remain approximate, and rendered output has not been visually compared. | Closed-solid and validated open-surface GPU shadow-volume paths are ported; the conservative depth-pass subset follows Blender's opaque-manifold route; the unsupported-caster GPU map is a documented classiCAD fallback, while QPainter shadows are not implemented and nonmanifold handling and outline composition remain partial |
| Cavity and depth of field | GPU Solid maps Blender's World type to SSAO and Screen type to normal-based curvature; Both uses both. The persisted settings expose Blender 5.2.2 defaults and ranges for SSAO distance/attenuation/sample count (`SceneDisplay`) and cavity/curvature ridge/valley factors (`View3DShading`); the renderer uses these values and distributes SSAO samples across AA iterations with Blender's 512-sample cap. Cavity jitter now recreates Blender's 64x64 blue-noise-derived RGB texture, including its clamped, total-sample-scaled radius offset. The GPU effect prepass uses opaque ranges as cavity inputs even when transparent ranges share the scene; transparent ranges are shaded without cavity. QPainter uses opaque-only cavity samples and applies the composite only where an opaque receiver is visible; cavity is skipped beneath a visible translucent surface. Its disk sampling follows Blender's deterministic radical-inverse/Hammersley-like sequence with per-pixel blue-noise rotation and sample-radius jitter. QPainter retains perspective-correct world-space surface positions from each rasterized triangle and evaluates Blender's signed displacement, length bias, distance attenuation, cavity/edge accumulation, and requested-sample normalization against those positions. Since dot products and lengths are invariant under view rotation, the world-space displacement/normal math matches those shader operations. The screen-space sample footprint remains an approximation. QPainter curvature controls now remain independent from SSAO cavity ridge/valley controls, matching Blender's separate control paths; its software curvature estimator remains an approximation. A scene-camera DOF mode stores focal length, sensor width, focus distance, f-stop, aperture blades, aperture rotation, and aperture ratio; QPainter applies DOF in both perspective and orthographic views, measuring depth relative to the perspective eye or orthographic view target. Scene extraction resolves an evaluated camera's focus-object distance along local +Z, matching Blender's camera DOF distance rule; pose-bone focus subtargets still use the numeric focus-distance setting. GPU and QPainter CoC convert lens and sensor millimeters to Blender scene units with the Workbench `0.001` scale while retaining imported focus distance and depth in scene coordinates; invalid optics leave the source image unblurred. GPU DOF uses half-resolution RGBA16F color and RG8 near/far CoC pyramids with preparation, two downsample, gather, 3x3 median, and full-resolution resolve passes before AA accumulation; viewport and offscreen render-result paths share this renderer. Blender adjusts sensor size for the camera viewport with `rv3d->viewcamtexcofac[0]`; classiCAD derives an effective fitted sensor width during camera import, and that mapping still needs visual comparison. The gather kernel accepts the three aperture shape controls, but exact aperture-sampling parity has not been visually verified. | GPU cavity sampling is source-derived; QPainter cavity remains an approximate software-buffer neighborhood pass; DOF is camera-gated in both paths. QPainter derives the physical CoC from the software depth buffer, gathers 12 same-or-behind-depth taps at full output resolution, and composites by CoC after cavity. Its orthographic and perspective paths now use the active view origin and Blender's aperture-radius parameter. This remains an approximation: it has no Blender half-resolution RGBA16F/CoC RG8 pyramid, three mip levels, temporal jitter, separate gather/blur passes, or depth-aware resolve; its depth buffer covers rasterized NURBS surfaces/solids and polygon meshes only, and transparent colors are already composited before the gather. QPainter color arithmetic is display-space and differs from Blender's linear intermediate; cavity is applied after that display transform, and opaque-only cavity is omitted under visible translucent surfaces because the color image has already composited them. GPU aperture blade/rotation/ratio shaping is implemented but awaits visual comparison; temporal jitter is unsupported. Blender focus-object distances are captured, while pose-bone subtargets remain unsupported. |
| Anti-aliasing preference | The UI offers Off, “Single Pass Anti-Aliasing,” and 5/8/11/16/32 samples; the RNA description identifies the single-pass method as FXAA. In Blender 5.2.2 Workbench, stored value 1 means one unjittered sample with TAA color filtering and SMAA resolve; Workbench does not invoke an FXAA shader. Values 5+ accumulate jittered frames and use SMAA while settling. During navigation or playback, Workbench resets to one unjittered sample and SMAA. classiCAD implements navigation reset and exposes a playback-state setter, but has no scene timeline, playback clock, or playback frame scheduler to call it. The grid is drawn afterward with its own unjittered MSAA path. QPainter renders committed geometry into a supersampled target for non-Off choices, without temporal accumulation or SMAA resolve; its wire categories use the separate Smooth Wires preferences. | Partial; GPU path follows Workbench's AA passes, but app playback integration, output equivalence, and fallback parity remain open |
| Smooth Wires | Overlay and Edit Mode preferences are independently stored and GPU strokes have separate categories. Committed curve strokes and wireframe surface lines use Overlay; component controls/cages and Solid-mode `NurbsSolid` face-boundary strokes use Edit Mode. The separate Workbench Outline effect draws object silhouettes. QPainter applies the preferences to supported curve, surface, and control-cage categories. If native GPU stroke drawing fails, the prepared-stroke fallback reads scene depth when available, approximates hidden/visible stroke visibility with sampled depth, applies endpoint fades, and handles continuous, dashed, dotted, and custom line patterns. Its remaining gaps are approximate depth sampling/edge coverage and unoccluded visible strokes when depth readback is unavailable; direct QPainter curve fallback paths also do not share the GPU stroke pass's full depth treatment. | Partial; the GPU `NurbsSolid` boundary pass and native stroke failure fallback are present, while fallback depth sampling and some component-edge geometry remain approximate or unavailable |
| Transparency | GPU surfaces are separated into opaque and partial-alpha ranges. Opaque ranges establish depth first; the primary scene renders transparent surfaces with weighted blended OIT into RGBA16F accumulation and R16F weight targets sharing scene depth-stencil. Transparent fragments depth-test without writing depth. Their lit RGB accumulates in scene-linear space and resolves over the scene-linear opaque target before DOF/AA and the single display transform at presentation. When no shared depth texture is available, auxiliary draws use range-level back-to-front order. The surface outline pass also runs for partial-alpha content. In the ordinary QPainter renderer, NURBS and polygon-mesh fills use temporary per-object color layers and prepared tessellation depth: opaque coverage establishes nearest per-pixel depth, then partial-alpha layers in caller-supplied average-depth back-to-front order are source-over composited in front of opaque depth before one resolve and scene strokes. The same compositor is attempted when the native renderer's main Solid surface draw fails, using a DPR-aware target; the fallback is presented before the normal native overlay sequence, followed by an unjittered depth rebuild, so scene strokes, grid, selected-face highlights, previews, control points, and arc HUD remain above it. Prepared QPainter stroke fallback also follows the surface fill. It falls back to the prior sorted painter draw if prepared geometry is missing or the bounded target exceeds one million pixels. Transparent layers are not depth-sorted against each other per pixel, and triangles within each object's color layer have already been QPainter-composited, so intersecting layers and self-occlusion remain approximate. Blender uses dedicated transparent and transparent-depth passes. QPainter retains its separate display-space implementation. | GPU primary scene uses weighted OIT; QPainter has a bounded opaque-depth/object-layer compositor but differs in transparent ordering, intra-object fragment ordering, color-transform placement, and coverage for its fallback cases |
| Stroke and overlay composition | Workbench surface silhouettes are drawn by its outline pass before AA resolve. CAD curve and wire strokes, like Blender's Overlay wireframe pass (`overlay_wireframe.hh`), draw afterward using the unjittered camera and regenerated scene depth; selected-face overlays also remain post-resolve, and the grid follows. Selected faces depth-test without depth writes. Committed Solid-mode scene strokes opt into depth writes per stroke; transform previews, control guides/cages, selected component edges, and custom outlines remain no-write. X-Ray's hidden wire pass stays no-write while the visible pass honors the per-stroke flag. The grid uses its own surface-depth framebuffer and composites with depth testing disabled, so scene-stroke depth writes do not change grid occlusion. The two Smooth Wires categories remain independent. GPU CAD strokes use analytic coverage, with depth bias enabled only for depth-tested strokes whose Smooth Wires category is enabled, matching Blender's `do_smooth_lines` gating. Solid Edit Mode strokes and Theme-mode wire strokes use separately configurable theme colors, saved with viewport shading state; selection, previews, and non-Theme wire colors retain their separate paths. QPainter now composites approximate Solid surface silhouettes from its object-ID raster; it still lacks selected component edges. The native-GPU stroke failure fallback uses sampled depth, endpoint fades, and line styles, but its visibility and edge coverage remain approximate; ordinary QPainter curve paths do not share its full depth treatment. | Theme wire/edit colors are configurable for supported stroke paths; Blender theme import, unsupported Smooth Wires geometry, transparency, and other fallback differences remain |
| Shadow controls | The Shift and Focus values feed Blender's normal/light falloff calculation in the GPU renderer, including the `1 - focus * (1 - shift)` upper edge. Blender's pinned implementation is volume-only; classiCAD's GPU map fallback keeps a fixed depth bias and approximates volume occlusion for unsupported casters. The QPainter Solid fallback does not render shadows. | Partial; GPU setting semantics are adapted, unsupported casters use a classiCAD-only depth-map fallback, and QPainter shadows are out of scope |
| GPU fallback | QPainter has a separate shading implementation. Its committed scene uses increasing supersampling for non-Off AA choices, without temporal accumulation or SMAA. Smooth Wires are independent of that scene AA hint for the supported categories: Overlay controls committed curve strokes and wireframe surface lines; Edit Mode controls cage/control-polygon lines and categorized prepared fallback segments. If native GPU stroke drawing fails, the prepared-stroke fallback uses scene depth when available, sampled hidden/visible visibility, endpoint fades, and line patterns; this remains approximate, and without depth readback the visible pass is unoccluded. If the native renderer's main Solid surface draw fails, the viewport exits native painting and presents the same bounded QPainter surface compositor used by the ordinary fallback before resuming the normal native overlay sequence. Missing prepared geometry or a target over one million pixels retains the average-depth sorted painter path. The unjittered scene depth is then rebuilt, and selected-face highlights, scene strokes, grid, picture/tool previews, control points, and arc HUD draw in their regular order on top; prepared QPainter stroke fallback also runs after the fill. The fallback composites object-ID surface outlines, but cannot reproduce selected component edges. | Partial; renderer fallback differences are documented but not closed |

**Update (2026-10-12):** trimmed NURBS use tessellation UVs for GPU Texture color, object-ID/outline alpha, and shadow alpha, plus QPainter Texture color and cavity/outline alpha. Blender's filtered alpha cutoff is 0.1 when enabled; alpha-ignoring modes bypass it. QPainter effect sampling interpolates tessellated UVs perspective-correctly, normalized over each active knot domain. Trim boundaries remain tessellation approximations. This supersedes earlier limitations below that described trimmed NURBS texture or alpha coverage as unsupported. QPainter perspective correction also uses positive camera-forward reciprocal depth for mesh and NURBS rasterization; visual comparison is still open.

**Shadow fallback audit (2026-10-12):** Blender 5.2.2's `workbench_shadow.cc` and `workbench_shadow.bsl.hh` implement stencil shadow volumes with pass/fail methods; the pinned Workbench code has no depth-map fallback. classiCAD's directional depth map is therefore an explicit renderer fallback for unsupported input geometry, not a parity port. The current shadow-volume path already adapts Blender's light-facing caps, silhouette side quads, and depth-pass/depth-fail stencil choice. No safe additional Blender-derived code change was identified in this bounded fallback audit. Sources: [workbench_shadow.cc](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_shadow.cc), [workbench_shadow.bsl.hh](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_shadow.bsl.hh).

### Remaining parity gaps

- **Render result:** File → Render Workbench Result (F12) renders visible document geometry into viewport, HD, Full HD, QHD, or 4K output. Imported Blender scene resolution percentage accepts Blender's 1–32767 range and computes each output dimension with integer truncation, matching `BKE_render_resolution`; the app retains its 8192-pixel output cap. It uses the evaluated scene camera when available, renders fills through the shared configured resolve, applies the existing camera DOF pass, then draws CAD strokes and composites committed dimensions. It saves Combined plus view-space Depth as PFM; when OpenEXR is available, Combined EXR also contains optional `Z`, written as HALF or FLOAT to match the selected color depth. The imported pixel aspect informs camera sensor fit and shift and is recorded in image metadata. The Combined image also writes per-axis pixel-density metadata for Qt image writers that preserve it; image pixels are not resampled. The GPU Workbench render passes are represented by a standalone result object rather than Blender's host-registered `RenderResult` datablock. Remaining render differences are covered by the pass, camera-DOF, color-output, and visual-comparison gaps below ([pinned Blender 5.2.2 source](https://github.com/blender/blender/blob/v5.2.2/source/blender/blenkernel/intern/scene.cc#L3128-L3139)).
- **Shading shortcut:** Z opens a hold-Z radial menu in idle Select mode with Solid, Wireframe, X-Ray, and settings directions. Blender's other shading modes are absent because classiCAD has no Material Preview or Rendered modes.
- **Color inputs and material assignment:** Blender scene mesh import preserves per-face material slots, color attributes, UV maps, and image references. Material/Object/Random/Custom/Attribute mesh colors render through GPU and QPainter paths. GPU Texture mode samples the active Image Texture Node through interpolated active-UV coordinates and a per-fragment sampler; external raster files use repeat wrapping and linear filtering. Missing images and missing UV maps fall back to face Material color. QPainter Texture mode rasterizes each triangle per target sample, performs reciprocal-depth perspective correction, and bilinearly samples external and packed images with repeat wrapping. Both paths apply PREMUL handling and the 0.1 cutoff to the filtered alpha sample except for NONE (Ignore) and CHANNEL_PACKED modes. Remaining QPainter differences include display color management and raster edge coverage. Blender-style object-name/library-path Random identity is used by both renderer paths, with ObjectId fallback for legacy nameless objects. The `.blend` adapter still round-trips classiCAD's embedded document snapshot instead of arbitrary Blender datablocks.
- **Transparent sorting:** The primary GPU scene now uses weighted blended OIT, so intersecting/cyclic triangle overlap no longer depends on centroid draw order and transparent surfaces draw once per range. The auxiliary GPU path uses range-level far-to-near sorting when it has no shared depth attachment. QPainter batches prepared surface fills into a bounded object-layer compositor, both in its normal Solid path and after a native-renderer main-surface failure. Opaque coverage uses nearest per-pixel tessellation depth; transparent object-layer fragments now sort far-to-near by their nearest per-pixel tessellation depth, with a four-million-fragment cap. This improves intersecting objects, but each object's triangles are already QPainter-composited into one layer, so self-intersection and intra-object transparent ordering can still be wrong. Missing prepared triangle data, a target over one million pixels, or an exhausted fragment budget uses the existing average-depth sorted QPainter fallback. On native surface failure, the QPainter fill now precedes the normal depth rebuild and overlay sequence, including selected-face highlights, scene strokes, grid, previews, control points, arc HUD, and prepared QPainter stroke fallback. A scene-wide per-triangle transparent pass would be needed to resolve intra-object intersections.
- **QPainter transparent sorting update (2026-10-12):** The bounded surface compositor now records transparent object-layer color and nearest tessellated depth per pixel, sorts those records far-to-near independently at each pixel, then source-over composites them above the nearest opaque result. Its maximum of four million records bounds the temporary memory; missing prepared geometry, a target over one million pixels, or a full fragment budget uses the existing sorted painter fallback. QPainter still precomposites triangles within each object's color layer, so self-intersecting surfaces do not receive per-triangle transparent ordering. A review also corrected premultiplied channel unassociation to use floating-point division rather than integer division. This improves inter-object QPainter fallback ordering; Blender continues to use weighted blended OIT in the GPU path.
- **Display transform:** imported scene display device, view transform, look, exposure, and gamma are retained for Workbench render results. The interactive Solid/Wireframe viewport keeps the default AgX settings; applying imported scene settings unconditionally there differed from Blender, where ordinary Workbench Solid uses the default view transform and only Rendered mode or View3D Scene Lights/Scene World selects scene render settings. classiCAD does not yet expose those View3D modes. Existing built-in sRGB AgX, Standard, Khronos PBR Neutral, and Filmic with the `None` look stay on their established transform paths. Other sRGB view/look combinations are baked to scene-specific 65³ RGB float LUTs by Blender's bundled PyOpenColorIO and applied in CPU/GPU rendering, with exposure before the table and gamma after it. This is an import-time snapshot of the machine's active OCIO configuration, not a runtime OCIO dependency; custom external config assets must be available to Blender during import. New LUTs sample a [0, 16] domain using an analytic `log2_1p` shaper (scale 64); older native documents preserve their linear [0, 2] metadata. Inputs outside a LUT's domain clamp to its endpoint. The document stores at most eight unique tables and 32 MiB decoded LUT data. A controlled Blender 5.2.2 PyOpenColorIO comparison of the AgX + Very High Contrast look against the 65³ log-shaped LUT sampled trilinearly at 41,000 deterministic scene-linear RGB inputs measured mean absolute channel error 0.000116, p95 0.000177, p99 0.00241, and maximum 0.0546 (0–1 display channel scale). For inputs in [0, 1]³, mean error was 0.000194 and p95 0.000371; the same probes with a linear [0, 16] LUT had 0.1758 mean error. This supports the current log-shaper as a bounded 65³ approximation, not exact transform parity. It compares OCIO evaluations to table interpolation, not Blender/classiCAD rendered pixels or final display quantization. Other display devices remain unsupported. Blender 5.2.2 selects the color-management context in [`draw_color_management.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/intern/draw_color_management.cc#L44-L57) and applies its default transform versus scene settings in [`viewport_settings_apply()`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/intern/draw_color_management.cc#L112-L140). Its display processor composes an explicit look before the DisplayViewTransform and applies exposure in scene-linear space and gamma afterward ([pinned source](https://github.com/blender/blender/blob/v5.2.2/source/blender/imbuf/opencolorio/intern/libocio/libocio_display_processor.cc)).
- **Camera depth of field:** the GPU viewport and offscreen render-result path share the Blender 5.2.2 `DofPass` shape: half-resolution RGBA16F color and RG8 near/far CoC pyramids, full-resolution CoC-aware preparation, quarter/eighth-resolution downsample levels, half-resolution 49-sample gather, 3x3 median cleanup, and a depth-derived CoC resolve before AA accumulation. The shader's sample perimeter and square-to-circle radius match `DofPass::setup_samples`; polygon blade shaping uses the algebraically equivalent sector formula, rotation, and reciprocal X aperture ratio. Its two-blade denominator no longer uses an extra epsilon floor. The aperture path remains unverified against Blender images, especially the visual two-blade slit. The textures and passes are kept separate from final presentation; the existing camera gate and per-temporal-sample placement remain. QPainter uses its separate full-resolution software approximation, with aperture shape controls on a fixed 12-tap gather and a repeating 64x64 blue-noise spatial rotation. Scene-camera optics persist with project, session, and user preferences; imported Blender scene cameras populate the inputs; DOF is gated off for ordinary orbit/orthographic views. Temporal jitter texture remains unsupported.
- **Additional shadow-map behavior:** unsupported casters and nonmanifold/edited solids use the current GPU 1024-pixel depth-map fallback with 3x3 PCF. Polygon mesh fallback ranges in Texture color mode filter active Image Texture alpha using interpolated UVs. Trimmed and untrimmed `NurbsSurface` ranges use their shared tessellation UVs and regular/UDIM texture data in the same fallback; the shadow fragment shader discards sampled alpha below 0.1, except for NONE, IGNORE, and CHANNEL_PACKED modes. This corrects the earlier claim that the map fallback left trimmed NURBS unmasked. The code path is `prepareGeometry` and the shadow shaders plus `renderShadowMap` in `src/ui/viewport/viewport_surface_renderer.cpp`; `appendNurbsSurfaceDepthMesh` in `src/ui/viewport/viewport_depth_geometry.cpp` supplies trim-aware tessellation UVs. Blender's pinned [`workbench_image_lib.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_image_lib.glsl#L34-L59) samples the Image/UDIM texture and discards alpha below `image_transparency_cutoff` (0.1). The map still has narrower caster coverage than the stencil path: NURBS solids and ranges without an available texture/UV alpha source do not use this mask. Open NURBS surfaces with valid tessellated topology use boundary-edge forced-fail stencil volumes. In mixed-alpha scenes, opaque surfaces remain shadow receivers, and validated closed transparent solids plus supported open surfaces cast stencil shadow volumes; the depth-map fallback is limited to unsupported casters when volumes are active, and transparent triangles are drawn once without shadow sampling. Blender's handling of branched, nonmanifold casters and additional fallback fidelity remain unported. The QPainter Solid fallback does not render shadows; this CPU shadow gap is an out-of-scope approximation. The GPU and QPainter outline silhouette opacity now uses Blender Workbench's four-cardinal-neighbor resolve. For classiCAD's separate selection-color extension, both renderers use four-cardinal-neighbor adjacency; Blender itself uses one outline color and has no selected-color choice.
- **NurbsSolid texture-alpha update (2026-10-12):** The existing object-level Image Texture now uses the derived solid-face UV stream in the shared GPU texture path. The same `useTextureColor`/alpha cutoff therefore applies to visible color, object-ID outlines, and the depth-map fallback, with texture metadata included in geometry-cache invalidation. A zero-stencil offscreen contract checks a fully transparent solid texture against the same caster with alpha mode `NONE`; color and outline output remain cut out, while disabling alpha handling changes both visible output and the depth-map shadow. Distinct per-face materials/images remain unsupported because `NurbsExtrusionSolid3D` has no per-face material/image indices and require a separate data-model contract.

Relevant classiCAD files include:

- src/ui/viewport/viewport_shading.h
- src/ui/viewport/viewport_surface_renderer.cpp
- src/ui/viewport/viewport_scene_renderer.cpp
- src/ui/viewport/viewport_gpu_surface.cpp
- src/ui/viewport/viewport_widget.cpp
- src/ui/panels/preferences_dialog.cpp
- src/ui/viewport/workbench_lighting.h
- resources/blender/studiolights/README.md
- resources/blender/colormanagement/README.md

QPainter Texture raster pixels now decode sRGB images into scene-linear RGB, multiply by the material base color, apply Studio or MatCap shading, then apply exposure and the configured display transform. Existing alpha-mode handling, premultiplication correction, and texture cutout thresholds are unchanged. For perspective polygon meshes, the fallback computes the incident view direction and normal per raster pixel using perspective-correct interpolation. Orthographic samples use barycentric normal interpolation with a constant incident direction. Imported corner normals are transformed by the world inverse transpose; the triangle's flat geometric normal remains the fallback when corner data is absent or invalid. Textured NURBS samples retain per-pixel material lighting; untextured tessellation triangles now also shade per pixel, using the flat triangle normal and perspective-correct sample position. Orthographic incident direction remains constant. Large untextured triangles retain the prior center-shaded polygon fallback above the 4M-pixel raster cap.

## Blender behavior to reproduce

### Findings from the original Solid viewport comparison

This checklist preserves the specific differences reported in the initial comparison and updates their status after subsequent ports. It is intentionally separate from the broader parity stages so an earlier finding does not disappear when its implementation changes.

#### 1. Geometry and render pipeline

- **Still an architectural difference:** classiCAD shades exact NURBS surfaces and solids through display tessellation and renders curves as strokes. Blender Workbench draws evaluated scene geometry through its own object and render-pass paths. classiCAD keeps exact NURBS data authoritative; parity is about the displayed result, not replacing that model with Blender's mesh representation.
- **Still different:** the QPainter fallback is a separate renderer. Its cavity composition, object-ID outline compositing, antialiasing, and transparency handling remain approximate compared with the GPU path. Polygon-mesh Texture cutouts honor active UVs, image interpolation/extension, and the supported filtered-alpha cutoff in both GPU and QPainter coverage. QPainter samples texture color per target sample with reciprocal-depth correction. GPU and QPainter NURBS Texture use exact-domain tessellation UVs on trimmed and untrimmed surfaces, matching Blender 5.2.2's uniformly sampled NURBS grid mapping. GPU and QPainter effect alpha coverage also handles trimmed NURBS through the color, object-ID, shadow, cavity, and outline paths. Trim boundaries follow tessellation approximation; selected component edges remain unsupported. QPainter Cubic/Smart filtering may differ from Blender's optimized helper.
- **Still different:** QPainter cavity uses opaque-only samples and is omitted where a visible translucent surface overlays an opaque receiver; GPU cavity shades opaque ranges while leaving transparent ranges out of the cavity pass. Blender composes its effects through dedicated Workbench passes. GPU and QPainter outline passes run with partial-alpha scene content. QPainter texture cutout coverage includes polygon meshes and trimmed or untrimmed NURBS surfaces; its raster/effect composition remains approximate.

#### 2. Color, materials, and shading effects

- **Ported since the initial comparison:** Attribute and Texture color inputs now reach imported polygon meshes; material records and per-face material slots are retained and used by both renderers. These findings should not be described as missing mesh inputs. General shader-node evaluation is not a Workbench parity requirement: Workbench Material uses material display fields, while Texture uses the active Image Texture Node. GPU and QPainter NURBS Texture sample trimmed and untrimmed surfaces with exact-domain tessellation UVs matching Blender's uniformly sampled NURBS grid. GPU Cubic/Smart interpolation may differ from Blender's optimized helper.
- **Ported in this continuation:** Blender Image Texture `interpolation` and `extension` are extracted, persisted with backward-compatible defaults, and consumed by GPU and QPainter mesh/NURBS Texture sampling plus GPU/QPainter alpha coverage for effects and outlines. Closest, Linear, Cubic/Smart and Repeat, Extend, Clip, Mirror are supported. QPainter alpha sampling preserves Mirror for arbitrary out-of-range UVs. GPU UDIM color, effect-alpha, shadow-alpha, and object-ID-alpha taps now apply the selected extension against each tile's actual dimensions; QPainter UDIM color and alpha sampling select the tile from the unflipped UV before mapping its local V coordinate to image rows. Missing UDIM tiles remain magenta for color and opaque for alpha coverage. Cubic filtering may still differ slightly from Blender's optimized helper, and filtering does not cross neighboring UDIM tile layers in Blender 5.2.2, so the per-tile behavior is intentional. Trimmed NURBS alpha coverage now follows their tessellated UV triangles. Rendered output has not been visually compared.
- **Random color hash corrected in this port:** when imported Blender object identity is available, both GPU and QPainter now hash the NUL-terminated `OB`-prefixed object ID name with Blender's Murmur2 helper, combine linked-library paths as `(objectHash * 13) ^ libraryPathHash`, and convert the result through Blender's `BLI_hash_int_01`. Native or older nameless objects retain the existing `ObjectId` color fallback. Source: Blender 5.2.2 [`workbench_materials.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_materials.cc#L19-L33), [`BLI_ghash_utils.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/blenlib/intern/BLI_ghash_utils.cc#L146-L151), [`hash_mm2a.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/blenlib/intern/hash_mm2a.cc#L96-L122), and [`BLI_hash.h`](https://github.com/blender/blender/blob/v5.2.2/source/blender/blenlib/BLI_hash.h#L50-L60). `BLI_hash_int_01`'s final normalization is at [lines 84–92](https://github.com/blender/blender/blob/v5.2.2/source/blender/blenlib/BLI_hash.h#L84-L92). Configurable theme wire/edit colors are also supported. Pixel-level color comparison remains open.
- **Still different:** the GPU shadow path uses validated stencil volumes for supported closed `NurbsSolid` geometry and validated open `NurbsSurface` meshes; unsupported/nonmanifold casters retain the GPU depth-map fallback. The QPainter Solid fallback does not render shadows, which remains an out-of-scope approximation. Outline detection uses classiCAD's object-ID edge pass. QPainter effects remain approximate. GPU and QPainter depth of field follow Blender's active-scene-camera DOF gate in perspective and orthographic views. The QPainter path uses coarse 12-tap spatially rotated sampling. Neither output has been visually verified against Blender; temporal jitter remains unsupported.
- **Corrected defaults:** Blender 5.2.2 enables object outlines by default; shadows and cavity default off. classiCAD now matches those defaults. Keep them checked in persisted settings as well as in a fresh viewport.

#### 3. Controls and rendered output

- **Bounded lifecycle parity:** `MainWindow` retains the last completed Workbench render result, including Combined and Depth, for the application session and exposes it through File > Show Last Workbench Render Result after the result dialog closes. The dialog labels it as the last completed result and explicitly says it is not live; scene, view, and render-setting changes do not update the cached image. The cache is not serialized to the project and does not implement Blender's Render Result datablock/pass registry lifecycle. Rendering again replaces the cached result.
- **Updated in current port:** Combined images now also set per-axis pixel-density metadata from Blender's pixel aspect ratio for Qt image writers that preserve those fields (such as PNG/TIFF pHYs/resolution metadata). The image pixels are not resampled; metadata support remains writer-dependent.
- **Resolved since the initial comparison:** the viewport now has a hold-Z shading pie. Solid/Wireframe and X-Ray controls remain available through classiCAD's viewport UI, but the original claim that the inspected toolbar lacked Blender's hold-Z interaction is stale.
- **Updated in current port:** the hold-Z pie activates its hovered direction on Z release or primary mouse release. Escape, right-click, and popup dismissal cancel without changing shading; non-primary releases cannot accidentally select a wedge. It remains available only in idle Select mode and offers this app's Solid, Wireframe, X-Ray, and shading settings directions. Material Preview and Rendered are unavailable because classiCAD has no such modes.
- **Resolved since the initial comparison:** classiCAD labels its user-selected color “Single” in the shading popover, matching Blender's label; its internal enum still calls the mode `Custom`.
- **Resolved in the current port:** committed dimensions are drawn into the render result at output resolution. The grid, selection controls, HUD, gizmos, and tool previews remain editor overlays and are omitted.

### Workbench shading

- Blender imports now keep two shading records: `workbench_shading` remains
  sourced from `scene.display.shading` and `scene.display` for scene-level
  Workbench render settings, while interactive viewport settings come from
  `workbench_viewport_shading`, captured from a saved `VIEW_3D` area's active
  `SpaceView3D.shading`. The extractor deterministically prefers the active
  window's screen when it is showing the scene being extracted; otherwise it
  scans non-temporary saved screens by name, then their 3D view areas top-to-
  bottom and left-to-right. If no saved 3D area exists, import falls back to
  the scene-level record. A .blend can have several 3D views with different
  shading, so classiCAD imports one representative view by this policy. Both
  opening a `.blend` and File > Import > Blender Scene apply the first imported
  scene record's viewport shading after a successful document import; the
  importer retains scene-level records independently for render use.
- Studio preset import prefers Blender's selected `StudioLight` datablock
  name and falls back to the saved `studio_light` enum when that pointer is
  unavailable. Blender 5.2.2 Workbench does not read
  `studiolight_intensity` in its engine source; the field is consumed by
  EEVEE and Grease Pencil paths instead ([`eevee_world.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/eevee/eevee_world.cc#L48-L58),
  [`gpencil_engine_c.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/gpencil/gpencil_engine_c.cc#L145-L154)).
  The extractor preserves the value but no longer reports non-default
  intensity as unsupported for this Workbench mapping. Blender's studio-light
  background opacity/blur still need their own background path. Scene
  Lights/Scene World render modes also remain outside this interactive Solid
  settings mapping.
- Blender's `studiolight_rotate_z` is stored in radians over [-π, π], while
  classiCAD's rotation slider uses [0°, 360°]. Import now wraps converted
  degrees into the slider range so a negative Blender angle displays the same
  orientation in the control instead of clamping visually to 0°.
- Background import audit found the `THEME`/`VIEWPORT`/`WORLD` gates mapped
  to Theme/Custom/World as Blender's enum defines them. The custom color is
  imported from scene-linear RGB and converted for classiCAD's background
  output; World uses the retained scene-linear World color. A missing World
  now falls back to the Theme background, matching Blender 5.2.2's
  `overlay_background.hh`: its World branch requires an assigned scene World,
  otherwise drawing reaches the theme background-type switch. Returning an
  invalid color delegates to classiCAD's existing Theme rendering path. See
  the pinned [overlay background source](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/overlay/overlay_background.hh#L50-L77).
- **Theme background preference implemented:** classiCAD now has an app-level
  Viewport preference for Single Color, Linear Gradient, or Radial Gradient,
  with high and low colors. The values persist in QSettings and flow through
  the viewport API to the GPU shader and QPainter fallback. Single Color uses
  the low color; Linear runs high at the top to low at the bottom; Radial runs
  high at the center to low at the edge. An offscreen contract checks known
  pixels for all three styles in both GPU and QPainter paths. This preference
  is deliberately not part of document shading state, and imported offscreen
  Workbench render results continue to use Blender's scene `background_type`,
  World color, and film transparency behavior.
- **Theme colors are host preferences, not `.blend` scene data:** Blender 5.2.2
  exposes the active background theme to headless Python at
  `bpy.context.preferences.themes[0].view_3d.space.gradients`, including
  `background_type`, `high_gradient`, and `gradient`. A headless Blender 5.2.2
  check confirmed these properties are available without a GUI. They describe
  the Blender process user's loaded theme preferences; the scene's saved
  `background_type = THEME` selects that host theme but does not carry the
  high/low theme colors inside the `.blend`. Importing those values into
  per-project classiCAD scene metadata would therefore bake the importing
  machine's preference into another user's project view. Keep this outside
  file-specific import parity. The classiCAD application preference described
  above makes style and high/low colors configurable across GPU and QPainter
  paths. Non-default Blender themes are not automatically imported; users can
  reproduce their Theme background by setting matching local colors. Blender
  theme matching remains a deliberate user-level setting or explicit import
  override, not implicit `.blend` project data. Blender's pinned
  [`rna_userdef.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/makesrna/intern/rna_userdef.cc#L1955-L1975)
  defines these as theme preference properties.
- Focused validation passed: `cmake --build build --target
  classicad_viewport_render_contracts_test classiCAD -j2` and
  `QT_QPA_PLATFORM=offscreen ./build/classicad_viewport_render_contracts_test`.
- Blender studio-light background opacity and blur are captured as
  `studiolight_background_alpha` and `studiolight_background_blur` and reported
  unsupported when non-default (`blender_scene_extract.py`). classiCAD has no
  corresponding background-image/opacity/blur rendering path. The flat
  Theme/Viewport/World color paths above do not cover these studio-light
  background settings; keep them explicitly unsupported pending a separate
  background-image path.
- **Object-alpha gate correction (2026-10-12):** Blender Workbench carries
  `ob.color.a` into the transparent pass for Object and Random color modes.
  Material, Attribute, and Texture modes instead take alpha from their
  Workbench material; when the object has no assigned material, Blender uses
  the opaque default material. classiCAD previously reused imported object
  alpha in those no-material modes. GPU, QPainter, and effect alpha selection
  now keep that fallback opaque while retaining object alpha in Object and
  Random modes. The source gate is visible in pinned
  [`workbench_engine.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_engine.cc#L134-L154)
  and [`workbench_materials.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_materials.cc#L22-L37).
- **No-material color fallback correction (2026-10-12):** The same
  `get_material()` source path selects `BKE_material_default_empty()` for
  Material, Attribute, and Texture when no material slot exists. Blender's
  `Material` DNA defaults set `r/g/b = 0.8` and `a = 1.0`; the default-empty
  material is created from those values. classiCAD's GPU and QPainter NURBS
  paths now use that linear 0.8 gray for the fallback. Its QPainter mesh path
  already used the same fallback, and the GPU surface path covers mesh and
  NURBS ranges. This supersedes the no-material object-color fallback wording
  in the summary matrix above. See pinned [`DNA_material_types.h`](https://github.com/blender/blender/blob/v5.2.2/source/blender/makesdna/DNA_material_types.h#L372-L394)
  and [`material.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/blenkernel/intern/material.cc#L2287-L2300).
- Studio lighting uses Blender's Workbench studio-light behavior and presets. MatCap uses the selected MatCap and its orientation. Flat uses an unlit flat result. Workbench does not shade objects with scene lights.
- The color modes have distinct data sources: Material reads Workbench material display values; Object reads the object's display color; Random derives a stable color from object identity; Attribute reads the chosen color attribute; Texture reads the supported image texture and UV mapping; Custom uses the user-selected color.
- Background choices, color management, object alpha, and material settings must feed the same final result as the chosen shading mode.
- Culling, X-Ray, shadows, outlines, specular highlights, cavity, curvature, and camera-gated depth of field must match Blender's enabled/disabled behavior and be composed in the correct order. GPU and QPainter depth of field follow the active scene camera's DOF setting in perspective and orthographic views.
- Depth of field applies when an active scene camera has DOF enabled and the Workbench shading option is on, regardless of whether the viewport uses perspective, orthographic, or free-view projection.
- Preserve Blender's setting dependencies. For example, a displayed option should not appear active when a required camera, material, attribute, or texture input is absent.

### Viewport anti-aliasing

Port the behavior behind Blender's Viewport Anti-Aliasing preference, not merely its sample-count label:

- Support the no-AA, single-pass, and multisample behavior exposed by the Blender 5.2.2 preference UI. Use Blender's preference definitions and Workbench state code as the source of truth for the exact mapping from UI values to rendering behavior.
- Blender's UI label for value 1 is “Single Pass Anti-Aliasing”; its RNA description says the method is FXAA. Workbench consumes value 1 as a one-sample setting: `draw_aa` is enabled, the view stays unjittered, its TAA color filter runs once, and SMAA resolves the result. The Workbench path does not invoke FXAA. Keep the visible label for preference parity, but do not implement the mode as FXAA. The multisample choices are 5, 8, 11, 16, and 32.
- At the pinned v5.2.2 tag, `source/blender/gpu/shaders/gpu_shader_2D_image_fxaa_frag.glsl` is absent. Workbench's actual AA call path is `SceneState` → `AntiAliasingPass` → `workbench_fx_taa.bsl.hh` and the Workbench SMAA shaders; a generic FXAA image shader is not part of this Workbench resolve.
- Workbench's TAA filter uses a 3x3 Blackman-Harris weighted color neighborhood in log2 color space. The jitter offset changes the filter weights and scene projection; it does not shift the filter's texture taps. SMAA is mixed fully on sample 0 and fades out with `1 - clamp(sample / 4, 0, 1)` as multisample accumulation progresses.
- In the multisample path, Blender accumulates jittered samples over viewport redraws while idle and resolves the image with its anti-aliasing path. It may use SMAA while the accumulation is still converging.
- Blender reduces the effective sample work during navigation and playback so interaction stays responsive. classiCAD resets during navigation. Its `ViewportWidgetApi::setViewportPlaybackActive(bool)` input is implemented and affects AA accumulation, but no application caller can currently supply it: the app has no scene timeline, playback clock, or playback frame scheduler. `NavigationController` only animates camera changes and is not a scene playback owner. A future scene playback owner must set the input during playback, clear it on stop, and schedule viewport frames; wiring playback state alone cannot provide frame updates.
- Restart or clear accumulation when the view, projection, viewport size, scene, shading settings, or relevant display state changes. Never leave a stale partially accumulated image after an edit.
- Match Blender's pass boundary: Workbench resolves its shaded scene (including its own outline, transparency, volume, and depth-of-field work) before the separate viewport overlay pass draws items such as the grid. Do not jitter or temporally accumulate the grid with the Workbench scene. Keep Smooth Wires behavior on its corresponding wire pass.
- Keep the AA history key limited to contributors rendered before the Workbench resolve. classiCAD redraws wires, previews, edit overlays, and the grid afterward, so their colors, smoothing preferences, and transient state must not reset scene accumulation.
- Remove the fixed four-sample main-widget setting as the controlling path once the preference drives the unified viewport render. Keep any hardware multisampling that remains as an implementation detail, not as a second conflicting preference.

### Smooth Wires: Overlay and Edit Mode

The screenshot's Overlay and Edit Mode checkboxes are Smooth Wires preferences. They control antialiasing for the corresponding wire categories. They are separate from Workbench surface shading and from the grid's own line rendering. Blender 5.2.2's RNA source (`rna_userdef.cc`, `use_overlay_smooth_wire` and `use_edit_mode_smooth_wire`) confirms Overlay updates live while Edit Mode says “requires restart”; both properties request a GPU update. The Overlay wireframe pass (`overlay_wireframe.hh`) gates custom depth bias on the Overlay preference. classiCAD routes the two settings to separate stroke categories and gates depth bias with the selected category.

- Overlay smoothing routes to committed curve strokes and wireframe surface lines; Edit Mode smoothing routes to component controls/cages and Solid-mode `NurbsSolid` boundaries. The GPU categories stay independent of each other, the grid, and surface antialiasing.
- The offscreen render contract now captures both GPU stroke categories with all four Overlay/Edit Mode preference combinations. It checks that changing one preference changes only its matching category. The grid continues through its own MSAA framebuffer, while filled surface edges and Workbench silhouettes stay on the separate scene AA/outline path.
- Preserve the restart requirement for Edit Mode Smooth Wires; Overlay Smooth Wires updates live in Blender 5.2.2 and classiCAD.
- Remaining fallback gap: the prepared-stroke fallback used when native GPU stroke drawing fails now applies depth-aware visibility when scene depth can be read, endpoint fades, and line patterns. Its visibility is sampled and approximate; without depth readback, visible strokes are unoccluded. The direct QPainter drawing paths still do not share the GPU stroke pass's full depth-aware treatment.

## Implementation stages

### Stage 0: Baseline audit

Status: completed against Blender 5.2.2 and classiCAD source. Updated after implementation review.

- Pin source review and screenshots to Blender 5.2.2.
- Trace each preference from its UI/RNA definition through Workbench state and draw passes.
- Trace each classiCAD preference through serialization and into the renderer.
- Mark each option as implemented, partial, or missing in the baseline table above.

### Stage 1: Make anti-aliasing control the viewport

Status: implemented for the application's current interaction model. The GPU path uses Blender's first jittered 5-sample location for its one-sample TAA plus SMAA at preference value 1 and during navigation, and jittered TAA accumulation plus SMAA for the 5+ modes. The viewport exposes `setViewportPlaybackActive(bool)`, but classiCAD has no scene timeline or playback owner; wiring that hook is deferred until playback exists. Scene strokes and selected-face overlays redraw after scene resolve using the unjittered camera and regenerated scene depth; the unjittered grid follows. Pixel-for-pixel equivalence has not been verified, and QPainter remains an approximation; those checks stay in Stage 5.

- Define one classiCAD AA setting model that matches Blender's UI choices and exact sample mapping.
- Route the setting through the complete Workbench scene render and resolve path, not only the grid framebuffer.
- Implement idle accumulation, responsive navigation/playback behavior, and invalidation on the changes listed above.
- Keep the Workbench scene and its supported scene strokes coherent. Draw the grid and other viewport overlays afterward with their own unjittered camera, as Blender does. Ensure resizing and device-pixel-ratio changes rebuild the Workbench targets correctly.
- Compare GPU output and fallback behavior, and make the fallback's supported behavior explicit.

Acceptance: changing each AA option visibly changes the expected full viewport output; no fixed independent sample setting overrides it; camera movement and scene edits do not leave stale accumulation.

### Stage 2: Complete Workbench color and material inputs

Status: implemented for supported Workbench inputs. Object, Random, Material, and Custom/Single inputs reach filled NURBS surfaces/solids in both renderers and persist in document version 11. Native objects persist display name and library path; both renderers use these for Blender-style Random identity, with an ObjectId fallback for older nameless objects. The Blender scene importer reads mesh identity, object color, shared materials, face material assignments, UV maps, color attributes, active selectors, and active Image Texture Node references. Material image reference metadata persists through document save/load, including node/image names, source paths, color-space and alpha modes, and packed bytes up to 32 MiB per image. GPU and QPainter mesh display support Material, Object, Random, Custom, Attribute, and Texture. Both Texture paths sample external and packed images with perspective-correct UVs; QPainter evaluates each target sample using reciprocal camera-forward depth. Both apply Blender's PREMUL unpremultiplication and conditional 0.1 alpha cutoff; QPainter cutout interpolation and filtering may differ. Missing images or UV maps fall back to Material. QPainter Attribute RGB now interpolates the triangle's per-corner values perspective-correctly, while attribute alpha continues to come from the Workbench material. QPainter Texture now replaces the material RGB with sampled image RGB, matching Blender and the GPU path. Offscreen contracts cover smooth RGB interpolation with ignored attribute alpha and texture replacement over a nonwhite material tint. GPU and QPainter NURBS Texture sample trimmed and untrimmed `NurbsSurface` geometry with exact-domain UVs from tessellation parameters. Blender's NURBS tessellation samples uniformly across the active parameter domain before normalizing grid indices, so these mappings match its legacy surface conversion. Texture-alpha cutout applies to trimmed and untrimmed NURBS in GPU color, object-ID, shadow, and QPainter color/effect paths; trim boundaries remain tessellated approximations. Solid Edit Mode strokes and Theme-mode wire strokes have distinct configurable theme colors in the shading popover, persisted with viewport update-session state; classiCAD does not import Blender user themes. Surface roughness/metallic/source-alpha values follow Blender 5.2.2 `Material::pack_data`: clamp to [0,1], square-root remap roughness, round all three to 8-bit UNorm, then apply X-Ray opacity to the packed source alpha. Preview fills also pack their default roughness 0.4 through this path, while retaining fixed 0.5 alpha. Blender's Custom source supplies RGB to `Material(float3)`, which packs alpha 1; classiCAD also forces the source opaque. classiCAD has no object/material appearance editor. NURBS transparency ordering remains approximate or unsupported.

- GPU Studio lighting now applies Blender's second roughness/metallic quantization after `Material::pack_data`: the opaque G-buffer truncates roughness to 5 bits and metallic to 3 bits through `float_pair_encode`. Alpha remains at the first 8-bit packing stage, and transparent accumulation retains the 8-bit values without the opaque G-buffer quantization. Offscreen contracts check both bucket boundaries and verify GPU lighting stays identical within one roughness bucket and changes across the next. Pinned Blender sources: [`workbench_common.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_common.bsl.hh#L42-L68), [`workbench_prepass.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_prepass.bsl.hh#L288), and [`workbench_transparent_accum_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_transparent_accum_frag.glsl).

- Blender Image Texture node interpolation and extension settings are now captured, stored per material image reference, persisted in native documents, and used by GPU/QPainter color and alpha sampling. Supported modes are Closest, Linear, Cubic/Smart and Repeat, Extend, Clip, Mirror. QPainter alpha reduces mirrored UVs modulo two before texel lookup, so Mirror works at arbitrary UV magnitudes. GPU UDIM samples now apply extension modes per tile; cross-tile filtering is not part of Blender 5.2.2's sampling path; cubic-filter details remain approximate.

- A source audit found that QPainter color sampling previously bilinearly interpolated encoded sRGB bytes and decoded the result afterward, while the GPU uploads sRGB images as `GL_SRGB8_ALPHA8`, so GPU filtering decodes each color texel before interpolation. QPainter mesh and NURBS color sampling now computes its sRGB linear RGB result per texel before bilinear interpolation; alpha filtering and PREMUL unpremultiplication still happen at their existing stages. Blender's sampling and material setup are in [`workbench_image_lib.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_image_lib.glsl#L34-L60) and [`workbench_materials.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_materials.cc#L35-L83); classiCAD's GPU sRGB texture allocation is in `viewport_surface_renderer.cpp` (`ensureImageTexture` and `ensureUdimTexture`), and the corrected QPainter sampling is in `viewport_renderer.cpp`. This narrows the color-sampling difference; visual parity remains unverified.

- Perspective textured NURBS in the QPainter fallback shades each pixel from its perspective-correct world position, matching the GPU path's per-fragment view direction for Studio specular and MatCap shading. Untextured NURBS tessellation triangles now rasterize and shade per pixel using their flat triangle normal; perspective samples reconstruct world position, while orthographic incident direction remains constant. Untextured Workbench polygon meshes now also use per-pixel Studio/MatCap shading with perspective-correct incident direction, scene-linear color sources, and the existing corner-normal interpolation. Non-Workbench mesh drawing and over-cap fallback retain polygon fills. Visual parity remains unverified.

- Imported scene display transforms now preserve the built-in sRGB view paths and bake other supported sRGB view/look combinations through Blender's bundled PyOpenColorIO into deduplicated 65³ float LUTs. The import bridge validates SHA-256, float finiteness, exact byte length, dimensions/domain/encoding, and limits the document to eight LUTs / 32 MiB. Native documents use version 15 and remain compatible with versions 1–14. CPU and GPU rendering sample the same red-fastest LUT layout; exposure precedes it and gamma follows it. The fixed [0, 2] input domain clamps HDR values, custom OCIO files must be available during import, other display devices remain unsupported, and a Blender import/render comparison is still needed.

- Keep the implemented Material, Object, Random, and Custom sources connected to filled surfaces/solids in both renderers. Keep Edit Mode strokes on their separate configurable `TH_WIRE_EDIT`-style setting and Theme-mode wire strokes on a separate configurable `TH_WIRE`-style setting, distinct from Workbench fill colors. Preserve selection, previews, and non-Theme wire colors. Preserve appearance values loaded from document data through the existing scene-object fields.
- Preserve the implemented Attribute source and Material fallback for missing attributes. Keep Texture's Material fallback when no active image, active UV map, or supported external image is available. Workbench does not evaluate arbitrary shader graphs; future Shader-mode graph evaluation is separate from this port.
- Preserve Blender object-name/library identity for Random colors; classiCAD stores those names and paths, with ObjectId fallback for older objects. The GPU and QPainter hash composition follows Blender 5.2.2's Murmur2, linked-library combination, and `BLI_hash_int_01` conversion (references in the finding above).
- Imported polygon meshes preserve material slots and per-face assignments. NURBS curves and surfaces still use one Workbench material payload per object; define an explicit parameter-space mapping before adding mesh-style color attributes or textures to NURBS.
- Verify color identity and appearance remain stable after save/reopen and object reorder.
- Keep custom shading inputs in the C++ scene/material model and serialization.

Acceptance: supported sources produce source-correct results; Attribute uses point, face, or corner data and falls back to Material only when absent. Mesh Texture uses the active UV and supported external image, with the documented QPainter approximation and Material fallback when an input is unavailable. Verify supported inputs after save/reopen. Unsupported inputs are reported rather than silently becoming gray.

### Stage 3: Complete Workbench effects

Status: partially implemented. GPU cavity/curvature and several existing effects are wired. Cavity settings now persist Blender 5.2.2 defaults/ranges, the renderer distributes SSAO samples across AA iterations with Blender's cap, and its jitter uses Blender's blue-noise-derived texture. In mixed-alpha scenes, the GPU effect prepass uses opaque ranges and supported texture-alpha cutouts; transparent ranges do not write cavity normals, IDs, or depth. The primary GPU scene uses weighted OIT with shared opaque depth: transparent lit RGB is composited over opaque scene-linear color, then depth of field and temporal AA run before exposure, active view transform/LUT, and gamma are applied once at presentation. Backgrounds, picture samples, and filled surfaces use the same scene-linear target; the offscreen result shares this final transform. This matches Blender's transform-after-AA order, while its sample weighting and SMAA output still need visual comparison. The auxiliary no-shared-depth path sorts ranges. QPainter's bounded object-layer compositor sorts transparent object layers by per-pixel nearest depth, but each object's triangles are precomposited; the fragment cap and unsupported geometry/large-target fallback retain average-depth sorting. QPainter cavity remains an approximate post-display-space effect and is skipped beneath visible translucent surfaces. Camera-gated GPU depth of field uses a shared viewport/offscreen pass with source-matched gather jitter, sample layout/weights, CoC-dependent LOD, and aperture shaping; the two-blade visual result, QPainter's separate 12-tap approximation, temporal jitter, and matched output comparison remain open. Validated closed opaque `NurbsSolid` casters use depth-pass stencil volumes only under the camera near-plane safety gate; closed fallback cases, open `NurbsSurface` casters, and transparent casters use capped depth-fail stencil volumes. Unsupported topology retains the depth-map fallback. GPU `NurbsSolid` face boundaries draw in the Edit Mode wire pass. Runtime/pixel comparison remains open.

- Compare and port culling, X-Ray, shadow, outline, specular, cavity, curvature, and depth-of-field behavior.
- GPU OIT accumulates shaded scene-linear RGB and composites into the RGBA16F opaque target. Its resolve clamps summed weights to Blender's `[1e-4, 5e4]` range and directly divides the signed RGB sum, matching Blender's pinned [`workbench_transparent_resolve_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_transparent_resolve_frag.glsl#L269-L286); accumulation weights and revealage also match [`workbench_transparent_accum_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_transparent_accum_frag.glsl#L435-L470). GPU color management now runs after scene-linear DOF and temporal AA, at presentation, using the same imported custom LUT or built-in transform. Background and image-preview inputs are decoded into scene-linear values before that target. QPainter remains a separate display-space compositor. Blender's pass order is recorded in its pinned [`workbench_engine.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_engine.cc#L2618-L2645) and [`workbench_effect_taa_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_effect_taa_frag.glsl#L9-L24); visual output comparison remains open.
- Map Blender's World cavity action to SSAO and Screen to curvature (the pinned View3DShading RNA enum definition); Both enables both. Persist Blender's SSAO distance/attenuation/sample settings and cavity/curvature ridge/valley factors with the source defaults and RNA ranges; drive the shader's corresponding uniforms and distribute the selected SSAO sample count across AA iterations, capped at 512 total samples.
- Match effect enable flags, setting dependencies, and opaque pass composition to Blender's source. Keep transparent-scene differences explicit; the source-derived cavity jitter is implemented but still needs visual comparison.
- For closed `NurbsSolid` casters, derive indexed geometry from the cached base/trim tessellation and extrusion walls, or from validated planar materialized quads. Require every indexed edge to have exactly two oppositely directed uses and reject zero-volume or degenerate meshes. For open `NurbsSurface` casters, accept only tessellations with consistently wound two-manifold interiors and closed, unbranched boundary loops; add forced-fail side quads along boundary edges. Use depth-pass stencil counting for closed opaque solids only when every source and extruded volume vertex lies safely beyond the camera near plane; retain the capped depth-fail path for open, translucent, near-plane-risk, and otherwise uncertain volumes. Extrude vertices to the current view's far plane with Blender's signed-distance/light-plane-dot formula and `1e-3` inward epsilon; use the `1e5` distance when the light direction does not intersect that plane. Tessellation remains display data derived from the persisted NURBS geometry.
- Pass audit against Blender 5.2.2 `workbench_shadow.cc`: Blender submits manifold geometry to a depth-pass `PASS`, while `FAIL` and `FORCED_FAIL` use depth-fail counting; translucent casters and selected non-manifold cases skip `PASS` and use `FORCED_FAIL`. classiCAD now uses Blender-style depth-pass stencil updates for the conservative closed opaque `NurbsSolid` and validated `PolygonMesh3D` subsets when all volume vertices are in the camera-visible half-space with a safety margin. It draws silhouette sides only for this route; capped depth-fail volumes remain the fallback for open surfaces, transparent casters, and any near-plane or geometry uncertainty. Compare near-plane crossings, camera-inside views, and perspective/orthographic projections against Blender before widening eligibility.
- The GPU stencil-volume path now also accepts imported `PolygonMesh3D` objects only when their transformed triangulated surface is finite, nondegenerate, closed, consistently oriented (each indexed edge has exactly two oppositely directed uses), has one connected cycle in each vertex link, and every connected component has positive nonzero signed volume. Every face material range for that object must be opaque under the active Workbench alpha source. Loose vertices, open boundaries, duplicate/overused edges, pinched/disconnected vertex links, inconsistent winding, degenerate or inward/zero-volume components, mixed-alpha objects, and failed near-plane safety checks stay on the existing depth-map or depth-fail fallback. The volume topology is stored once on the first per-face draw range while every face range is marked as belonging to that caster, preventing fallback-map double drawing after a successful volume pass. This extends Blender's evaluated-mesh topology dispatch without making tessellation authoritative; runtime comparison remains open.
- Keep the GPU depth-map fallback for unsupported/nonmanifold topology, nonplanar materialized solid faces, missing stencil buffers, and ranges that cannot use the volume path. The fallback rasterizes selected triangle ranges into a fixed 1024×1024 light-depth map. In Texture mode, polygon mesh casters with active UVs and per-face images, plus trimmed and untrimmed `NurbsSurface` ranges with tessellation UVs and texture data, filter alpha at Blender's 0.1 cutoff; NONE, IGNORE, and CHANNEL_PACKED modes bypass that mask. NURBS solids and ranges without a usable texture/UV alpha source remain unmasked. This describes the GPU depth-map fallback; the GPU stencil shadow path does not sample texture alpha. The QPainter Solid fallback does not render shadows; implementing CPU shadows remains out of scope for that fallback. Hard shadowing, map resolution, and remaining caster cases differ from Blender's ray-traced shadow option. In mixed-alpha scenes, opaque surfaces remain receivers and validated closed solids/open surfaces can cast volumes; transparent triangles draw afterward without shadow sampling. Branched/nonmanifold caster dispatch and Blender's ray-traced shadow option remain unsupported.
- Map the stored scene light direction through Blender's Y/Z swap and X/Z sign change. Clamp Shift to Blender's `[0,1]` range and compute `focus = clamp(shadow_focus, 0.0001, 0.99999)`, `shadow_focus_edge = 1 - focus * (1 - shadow_shift)`, then apply `smoothstep(shadow_shift, shadow_focus_edge, -dot(N, light_direction_view))`; shadow intensity mixes the result with the unshadowed contribution. Keep the fallback depth bias fixed so Shift no longer changes map bias.
- Keep GPU and QPainter depth of field camera-gated by an active scene camera with DOF enabled, regardless of view projection. The QPainter software path now uses camera-relative depth, applies the Blender aperture-radius factor, and has focused production-path tests for perspective and orthographic focus/defocus plus orthographic translation invariance. Validate GPU and QPainter aperture blade/rotation/ratio shaping against Blender; investigate Blender's temporal jitter only if a temporal sample lifecycle is added.
- Keep GPU cavity/curvature behavior aligned with Blender's composition; its parameter controls, sample scheduling, and blue-noise-derived jitter texture are wired. In mixed-alpha scenes the effect prepass and shader apply cavity only to opaque ranges. The QPainter fallback uses separate visible and opaque-only samples: transparent surfaces do not occlude cavity samples or receive cavity shading, and post-composite cavity is skipped where a translucent surface is visible above an opaque receiver. Its cavity pass on NURBS and imported polygon meshes remains approximate and needs visual comparison.
- Derive all surface display data from the authoritative NURBS geometry. Do not convert committed surfaces into authoritative meshes.

Acceptance: each enabled control affects the expected objects and pixels, and disabling an effect removes only that effect. Verify saved settings survive reopen and test SSAO sample scheduling at the 512-sample cap. Unsupported effects stay disabled or are clearly identified as approximations. Pixel-level cavity jitter comparison, transparent-scene cavity behavior in the GPU path, visual comparison of QPainter cavity on imported polygon meshes, and visual comparison of the closed-solid shadow path remain open parity checks.

### Stage 4: Complete Smooth Wires and viewport composition

Status: partially implemented. The two preferences are independent and GPU stroke categories are separated. Overlay smoothing applies live; the Edit Mode checkbox is saved for the next launch, matching Blender's restart requirement. Selected-face overlays and GPU scene strokes now redraw after Workbench resolve with the unjittered camera and scene depth, including cached-history frames; selected faces do not write depth, and X-Ray back/front behavior remains intact. Committed Solid-mode scene strokes opt into depth writes per stroke; X-Ray's hidden/back pass, control guides/cages, selected component edges, custom outlines, and transform previews remain no-write. The grid renders against its own surface-depth framebuffer and composites without depth testing. Solid-mode `NurbsSolid` faces now draw evaluated face boundaries through the GPU Edit Mode wire pass, independently of the Workbench silhouette outline. QPainter maps Overlay to committed curve strokes and wireframe surface lines, and Edit Mode to control-cage/control-polygon lines. If the native GPU stroke draw fails, the prepared-stroke QPainter fallback now reads the regenerated scene depth, separates hidden and visible strokes for X-Ray, applies endpoint fades, and resolves continuous, dashed, dotted, and custom line patterns. The fallback uses sampled software visibility and does not reproduce GPU edge coverage exactly. QPainter's object-ID outline composite now covers visible surface silhouettes and object boundaries, with four-cardinal-neighbor fractional edge opacity and supersampling-aware pixel mapping. GPU and QPainter now use matching four-cardinal-neighbor adjacency for classiCAD's selected-color extension; Blender's outline pass uses one outline color. Polygon-mesh Texture mode masks IDs using active UVs, per-face images, repeat wrapping, and supported alpha cutouts. GPU NURBS texture alpha masking covers trimmed and untrimmed surfaces using tessellation UVs; QPainter cavity/outline effects apply texture-alpha coverage to trimmed and untrimmed tessellated NURBS. Selected component edges remain unsupported. Category-to-Blender pass mapping and visual output still need verification.

GPU and QPainter NURBS Texture mapping follows exact parameter domains and
keeps endpoint UVs at 0/1 with repeat wrapping. `NurbsSurface3D` has no
cyclic-direction flags, so Blender's cyclic seam-corner rewrite is not
represented. QPainter cavity/outline effect alpha coverage uses the same
tessellated per-triangle UVs for trimmed and untrimmed surfaces, with
perspective-correct UV interpolation and Blender's 0.1 cutoff. GPU shadow-map
alpha masking covers the same ranges, while stencil shadow-volume casters do
not sample texture alpha. Trim boundaries remain tessellation approximations.

- Map overlay and edit-mode wire categories to their actual renderer passes.
- Implement the two Smooth Wires preferences independently.
- Match Blender's composition order: resolve Workbench scene content first, regenerate unjittered scene depth, then draw selected-face overlays and scene strokes before the grid. Both post-resolve redraws are implemented; verify stroke-category mapping and grid depth behavior.
- Check that changing wire smoothing does not change the grid or solid surface edges.

Acceptance: each checkbox affects its matching wire category only, while other viewport layers retain their expected antialiasing.

## Phase Two proposal: Workbench render and resolve convergence

### Source comparison

| Area | Blender 5.2.2 | classiCAD now | Phase Two direction |
|---|---|---|---|
| Renderer ownership | `workbench_engine.cc` owns named opaque, transparent, transparent-depth, volume, outline, depth-of-field, and anti-aliasing passes. It draws those passes in a defined order using Blender's evaluated scene, draw manager, and GPU APIs. | `ViewportSurfaceRenderer` owns surface drawing and several effect passes; `ViewportAaRenderer` owns DOF, TAA/SMAA, and presentation; `viewport_widget.cpp` coordinates surfaces, regenerated depth, strokes, overlays, and grid. | Extract a classiCAD-owned pass coordinator and explicit render-target contract. Mirror Blender's responsibilities/order where applicable, while retaining Qt/OpenGL and the authoritative NURBS model. Do not try to link Blender's engine classes directly. |
| Volume data and rendering | Workbench reads the active evaluated scalar grid, uses Blender's volume texture transforms and display density, draws after transparent depth, and ray-marches with per-axis five samples per voxel, Bayer/Halton jitter, and closest/linear/cubic sampling. The pinned Workbench volume shader leaves shadow sampling as a TODO. | Blender import transfers one bounded single-channel `FLOAT` grid through evaluated Geometry Nodes mesh attributes; native JSON stores sparse voxel/tile records and display controls. `ViewportSurfaceRenderer` reconstructs a bounded dense texture and draws a volume pass in the matching pass slot. The QPainter fallback has no volume renderer. | Preserve this first bounded scalar path and keep its caps visible. Multi/vector grids, animation, oversized dense bounds, and QPainter volume rendering remain separate follow-up work. |
| TAA/SMAA resolve | Blender uses a centered jitter sequence, a 3x3 Blackman-Harris filter in log color, accumulated weights, and an SMAA resolve that fades as temporal samples accumulate. | The GPU implementation follows those algorithms closely. The QPainter fallback instead supersamples and downsamples; it does not run the GPU TAA/SMAA sequence. | Controlled GPU renders match at AA Off and Single Pass. A source-derived AA-off camera-shift fixture now compares all eight raw samples; their silhouettes differ by at most two pixels per sample. The resolved scene-linear 8-sample silhouette masks now match exactly. Blender's background API still exposes only the final render, so per-sample comparison uses validated camera-shift renders rather than direct internal-buffer reads. Treat QPainter as a separate approximation. |
| Transparency/depth | Blender has separate transparent accumulation and transparent-depth passes following opaque drawing. `OBJECT_IN_FRONT` uses a second depth domain and merges into scene depth before regular transparent depth excludes its pixels. | The primary GPU shared-target path uses independent regular/front weighted-OIT targets, resolves front color after regular OIT, then merges front transparent depth and IDs before regular transparent depth. The no-shared-target fallback retains sorted/regular behavior. QPainter has no separate in-front domain. | GPU opaque and transparent in-front domains are implemented with named pass/attachment reporting; Blender's transparent depth pass does not write normals, so the earlier opaque front normal remains. Matched GPU image comparison is pending; fallback and QPainter paths remain different. |
| Outline and effects | Blender runs outline from its object-ID buffer, then DOF, then AA. Cavity/curvature uses Workbench-specific sample buffers and effect composition. | classiCAD generates normal/depth/object-ID inputs in its own prepass and uses custom outline/effect shaders. QPainter has separate software approximations. | Port and attribute the standalone shader/effect logic where inputs match. Preserve CAD-specific selection and NURBS tessellation handling. Validate each effect independently before combining them. |
| Geometry and materials | Workbench consumes Blender evaluated object batches and Workbench material records; its render engine is integrated with Blender scene, dependency-graph, draw-manager, GPU, and render systems. | classiCAD sends derived tessellation of exact NURBS surfaces/solids to its GPU renderer. The surface renderer has its own material packing and draw ranges. | Keep NURBS authoritative and pass only derived display data to the renderer. A future shader system should provide evaluated Workbench inputs; do not create a second Blender-shaped scene/geometry model. |

The previously supplied cube screenshots are not a controlled edge-quality comparison: the recorded views differ (`Ortho` versus `User Perspective`), the visible geometry differs, and the output dimensions differ. A same-geometry, same-camera, same-size GPU comparison confirms exact Off and Single Pass silhouettes. Per-sample references traced the 8-sample edge mismatch to the vertical projection-jitter sign: Blender adds positive Y to its orthographic projection, while classiCAD had negated it. After correcting that sign, the resolved scene-linear 8-sample silhouette masks match exactly.

Phase Two category audit: Blender 5.2.2 Workbench UDIM lookup in [`workbench_image_lib.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_image_lib.glsl#L348-L412) floors the UV to one tile, looks up that tile's array layer and coordinate scale/offset, and samples the resulting array coordinate. Filtering does not address neighboring layers. The classiCAD GPU samplers fetch taps from the selected layer and clamp/wrap/clip/mirror against that tile's dimensions; QPainter color/effect-alpha likewise resolve one tile before filtering. Therefore, cross-tile filter continuity is not a Blender 5.2.2 parity gap; retain the existing per-tile edge behavior and preserve magenta/opaque results for absent base tiles. Blender's outline shader [`workbench_effect_outline_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_effect_outline_frag.glsl#L10-L24) compares the center object ID with exactly four cardinal samples and applies one outline color. classiCAD's GPU and QPainter paths now use four-cardinal-neighbor adjacency for their additional selected-color behavior; this is backend consistency, not a Blender feature.

Phase Two Category 8 source audit: the GPU TAA/SMAA weights, log-space accumulation, SMAA edge/area/search stages, temporal mix, and resolve follow Blender 5.2.2 closely, so no duplicate shader rewrite was made. QPainter still uses supersampling rather than Workbench temporal accumulation. Custom display LUTs carry their domain and shaper through extraction, import, native serialization, CPU rendering, GPU surface/transparency rendering, and AA presentation. A controlled PyOpenColorIO-to-trilinear-LUT comparison for Blender 5.2.2's AgX + Very High Contrast combination used 41,000 deterministic RGB probes over [0, 16]³: the 65³ log-shaped table measured 0.000116 mean absolute channel error, 0.000177 p95, 0.00241 p99, and 0.0546 maximum; the subset in [0, 1]³ measured 0.000194 mean and 0.000371 p95. A linear [0, 16]³ 65³ table measured 0.1758 mean error on [0, 1]³ probes, supporting the existing log shaper. This isolates interpolation error against OCIO and does not compare full Blender/classiCAD rendered output, GPU texture precision, or display quantization. Retain the QPainter and display-transform differences explicitly; worst-case LUT and matched-render comparisons remain open.

Phase Two orthographic OIT depth audit: Blender 5.2.2's pinned
[`workbench_transparent_accum_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_transparent_accum_frag.glsl#L402-L470)
linearizes orthographic `gl_FragCoord.z` as distance from the near plane. The
GPU path previously used absolute distance from the view target, which differs
because the orthographic projection eye is offset from that target. It now
uses `eyeDistance - clipStart - dot(worldPosition - viewTarget, viewFacing)`;
`eyeDistance` matches classiCAD's orthographic projection setup, clamped by
`clipEnd`, and the result is clamped at zero. Perspective OIT retains absolute
eye-space depth. This change is limited to the primary GPU OIT weight; sorted
fallbacks and QPainter are unchanged. Pixel comparison remains open.

**Pass contract update:** Interactive viewport and offscreen Workbench result
now submit a `ViewportWorkbenchSceneColorPassRequest` to the same coordinator.
The surface renderer returns named completion flags for opaque color,
transparent color, weighted accumulation/resolve, and transparent depth, plus
the shared depth/normal/object-ID attachments it populated. The same scene
color call is shared, while grid/background, DOF/AA presentation, CAD strokes,
and editor overlays retain their existing caller-owned order. The render result
currently exposes Combined, Depth, and an optional scene-linear float Combined
payload. Blender's Combined and optional Z pass names/types are mirrored, and
the imported active view layer's `use_pass_combined` and `use_pass_z` values
control whether classiCAD offers those exports. The GPU outputs are represented
in classiCAD's standalone result object rather than Blender's host-registered
`RenderResult` datablock; this host lifecycle is not duplicated. The GPU
offscreen renderer converts depth-buffer values to view-space Z exactly once,
and the Z export writes
`1e10` for the background, matching Blender Workbench's pinned
[`write_render_z_output`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_engine.cc#L2925-L3011).
When OpenEXR support is available, the Combined EXR now also includes the
enabled view-space Z pass as a HALF or FLOAT `Z` channel matching the selected
color depth; the separate PFM export
remains available. Result assembly now preserves the returned view-Z values;
it no longer interprets them as normalized depth and converts them a second
time.
Committed CAD dimensions are classiCAD-only post-render, screen-space
annotations; Blender Workbench has no CAD dimension counterpart. They are
composited into display-referred PNG/TIFF Combined output, but the scene-linear
HDR/EXR Combined payload is captured before those annotations and omits them.
Their fixed-pixel labels/arrows have no corresponding Z samples. This is a
format behavior distinction, not a Blender parity requirement.
Offscreen rendering accumulates the imported Blender `scene.display.render_aa`
mode before capturing Combined and depth. For multi-sample TAA, classiCAD now
saves sample-zero main and in-front depth. It restores both depth domains on
each later interactive viewport sample and on the last scene-render sample,
matching Blender's Workbench `AntiAliasingPass`. The first jitter position is
centered. Older imports without the AA field use classiCAD's viewport AA
preference. Blender's render AA enum is `OFF`, `FXAA`, or 5/8/11/16/32 samples,
as declared in the pinned [SceneDisplay API](https://docs.blender.org/api/5.0/bpy.types.SceneDisplay.html).
Source: [Workbench AA depth preservation](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_effect_antialiasing.cc#L259-L280).
The depth routing is source-aligned; an image comparison against Blender's
Combined and Z outputs remains to verify the result visually.

**Depth precision update:** Blender 5.2.2 allocates scene depth and in-front
depth as `SFLOAT_32_DEPTH_UINT_8` in its Workbench render-target setup
([`workbench_engine.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_engine.cc#L555-L582)).
classiCAD's primary Solid scene depth, in-front depth, sample-zero depth
preservation target, cavity depth snapshot, and object-ID depth-stencil target
now use D32F/S8. The cavity snapshot retains stencil storage so its depth blits
match the scene and in-front source formats. Packed depth-stencil texture
allocations use `GL_FLOAT_32_UNSIGNED_INT_24_8_REV`; depth readback
continues through `GL_DEPTH_COMPONENT`/`GL_FLOAT`, so no CPU path unpacks a
24-bit depth field. This preserves Blender's 32-bit floating-point depth
precision across scene/in-front comparisons, AA depth restoration, and the
cavity snapshot blits. Explicit framebuffer completeness checks and existing
failure paths remain in place. The separate 1024-pixel shadow-map fallback
still uses D24 because it is classiCAD-only and not the shared Workbench scene
depth domain. Visual depth comparison remains open.

### Phase Two source audit update (2026-10-12)

- Focused audits of hard-edge rasterization/AA, GPU cavity and curvature,
  image/UDIM sampling, Combined/Z output, color resolve, and the shared GPU
  pass sequence found no further source-backed mismatch to port in those
  paths. This is a code review result, not a visual parity sign-off; the
  controlled image comparison below remains open.
- The shared GPU pass sequence follows Blender's opaque, transparent
  accumulation/resolve, transparent depth, outline, depth-of-field, and AA
  order. The `TransparentResolve` completion flag now becomes true immediately
  after the regular scene-domain resolve; the in-front resolve remains reported
  separately. This report-timing correction is in commit `eef0b6e`.
- A raster/AA review traced the cube surface through the single-sample scene
  target and internal TAA/SMAA resolve. The grid's separate MSAA target does
  not draw cube surfaces, so the available source does not identify the
  screenshot's soft cube edge. Distinguishing a filled silhouette from a
  post-resolve CAD stroke and making a matched GPU comparison are still
  required to localize it.
- A bounded Blender `VOLUME` path is now implemented. The Blender 5.2.2
  extractor uses Geometry Nodes (`Get Named Grid` → `Grid to Points` →
  `Points to Vertices`) to transfer active voxel/tile values through evaluated
  mesh attributes because the Python `VolumeGrid` API has no voxel-array
  accessor. classiCAD stores one single-channel `FLOAT` grid, sparse active
  voxel/tile records, bounds, transforms, display density, render space,
  interpolation, and slice controls in the native model and JSON. Blender's
  `density` grid is preferred; otherwise exactly one scalar `FLOAT` grid must
  exist. The import caps extraction at 262,144 active records, 16M expanded
  voxels, 16M dense-bounds voxels, and 16 MiB of grid payload. The GPU pass
  rebuilds a dense texture only for bounds at most 512 per axis and 16M voxels,
  then ray-marches after transparent depth and before outlines, DOF, and AA.
  Its sampling follows Blender's per-axis five samples per voxel, Bayer/Halton
  jitter, closest/linear/B-spline interpolation, and view-ray distance factor;
  the loop is capped at 2,560 iterations, matching the 512-axis texture cap.
  Blender's Workbench volume shader still has a TODO for volume shadows, so
  this path does not add them. Remaining limits are multiple/vector grids,
  animated grids, OpenVDB features beyond the extracted active scalar field,
  dense bounds above the GPU cap, and the QPainter fallback. References:
  [VolumeGrid API](https://docs.blender.org/api/5.2/bpy.types.VolumeGrid.html),
  [Workbench volume pass](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_volume.cc),
  [Workbench volume shader](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_volume.bsl.hh),
  and [Blender volume developer docs](https://developer.blender.org/docs/features/objects/volume/).
- The sRGB-only custom-LUT gate is required by the current output contract.
  OCIO's `DisplayViewTransform` emits values in the selected display's encoding
  and gamut; classiCAD consumes its LUT as ordinary sRGB RGB for the Qt view
  and image writers. Supporting Display P3, Rec.2020, or HDR display devices
  therefore needs explicit output-color-space metadata and conversion/profile
  handling across GPU, QPainter, and saved image paths. Removing the gate alone
  would silently misinterpret pixel values. References: [OCIO display and view
  color spaces](https://opencolorio.readthedocs.io/en/latest/guides/authoring/displays_views.html)
  and [Blender's pinned display color-management path](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/intern/draw_color_management.cc).

### Controlled Workbench cube baseline (2026-10-12)

- Added a repeatable background-render fixture at
  [`tests/workbench_cube_reference.py`](tests/workbench_cube_reference.py).
  It uses Blender 5.2.2 Workbench, a 640×480 orthographic 2-unit cube, a
  5-unit horizontal view, camera direction `(4, -6, 4)`, the Default studio
  light, AgX, and no selection, object outlines, cavity, or shadows. Run it
  with `blender --background --python tests/workbench_cube_reference.py --
  /tmp/workbench.png --aa OFF`, replacing `OFF` with `FXAA` or `8`.
- A direct offscreen classiCAD GPU capture was compared with Blender outputs
  using identical dimensions, camera, cube, and material. The background
  differs by one 8-bit code value (34 vs. 35) and was normalized independently
  for coverage. At an RGB distance threshold of 2, mask IoU is 0.99798 for AA
  Off, 0.99799 for Single Pass, and 0.99599 for 8 samples. On shared object
  pixels, RGB channel MAE is 0.396, 0.410, and 0.597 of 255 respectively.
  Off and Single Pass differ mainly at the cube's right boundary (224
  Blender-only pixels). After the jitter-scale correction below, 8-sample
  bounds match exactly; 450 classiCAD pixels and one Blender pixel remain
  unique under this threshold. A second threshold-20 baseline comparison
  measured mask IoU 0.99997 and exactly matching face probes (top
  `(142,144,145)`, front `(138,139,139)`). This is strong base-render
  evidence, not a parity sign-off; the remaining edge differences and effects
  still need isolation.
- The source comparison found that Blender 5.2.2's Single Pass uses the first
  jitter point from the 5-sample sequence even though it renders one sample.
  classiCAD was using zero jitter for that mode. Its camera sample and filter
  weights now both use `samplePositions(5)[0]`; Off remains unjittered, and
  5+ modes retain their requested sequences. The assertion in the offscreen
  render contract checks this distinction. The pinned reference is
  [`workbench_effect_antialiasing.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_effect_antialiasing.cc#L192-L219),
  with jitter generation in
  [`jitter_2d.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/blenlib/intern/jitter_2d.cc#L119-L162).
- The matched 8-sample edge profile also exposed a projection-scale mismatch.
  Blender passes `sampleOffset / resolution` to
  `window_translate_m4`, which adds that value directly to the orthographic
  projection translation. classiCAD's projection helper converts pixel
  translation to clip space with a factor of two, so it now receives half the
  physical-pixel sample offset. This restored the same 8-sample foreground
  bounds and improved mask IoU from 0.99417 to 0.99599. See Blender's pinned
  [`math_geom.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/blenlib/intern/math_geom.cc#L4417-L4441)
  and classiCAD's `viewportViewProjection` in
  `src/ui/viewport/blender_grid_renderer.cpp`.
- The comparison uses the native classiCAD GPU pass and does not launch its
  GUI. QPainter, navigation-history reset, and volume/effects/transparency
  scenes remain outstanding validation work.
- A numerical edge-profile pass over the six stored 640×480 captures used
  masks thresholded by RGB distance >8 from each image's own dominant
  background, so the one-code background offset does not set coverage. Off
  gives 111,257 classiCAD foreground pixels and 111,480 Blender pixels, with
  225 XOR pixels and IoU 0.99798. Single Pass versus Blender's FXAA-labeled
  Workbench value gives 111,646 vs. 111,869 pixels, 225 XOR pixels, and IoU
  0.99799. In both pairs the difference is a one-pixel shift at the right
  silhouette edge (224 Blender-only pixels there); the other contours align.
  The dominant background values are (34,34,34) and (35,35,35), and overall
  RGB channel MAE is about 0.99 with p95 2, so most whole-image error is a
  small background/display quantization offset.
- For 8 samples, both silhouettes have the same bounds. At the same >8
  threshold, they have 469 XOR pixels and IoU 0.99583; differences are
  distributed around the contour rather than concentrated on the right edge.
  In a 2-pixel boundary ring, mean coverage difference is 0.068 and p90 is
  0.102; partial coverage occurs at about 1.9% of classiCAD ring pixels and
  4.7% of Blender ring pixels. Interior RGB MAE is 0.44 versus 4.73 in that
  edge ring. This records an 8-sample edge-coverage/profile difference despite
  matching bounds, not a measured face-shading mismatch.
- Source review confirms classiCAD's seeded jitter generator and sample
  ordering match Blender 5.2.2's Workbench sequence. The Off/SinglePass
  right-edge shift was traced to float precision in classiCAD's orthographic
  view matrix: it placed the eye at least 1000 units away and multiplied that
  large translation through a float basis, producing a 3.05e-5 world-unit
  lateral residual. At 128 pixels per unit this shifted the projected edge by
  about 0.0039 pixel, enough to alter the 8-bit subpixel raster position. The
  view matrix now derives lateral translation from the view target and keeps
  eye depth separate; an offscreen contract checks the controlled Blender
  corner coordinates to 0.001 pixel. Fresh official-hook captures after this
  fix confirm that Off and Single Pass now have identical silhouette masks to
  Blender (XOR 0, IoU 1.0; shared bounds `(142,42)` to `(497,437)`). The
  repaired capture contract treats the centered first sample correctly for
  Single Pass and 8-sample AA, while requiring later temporal samples to carry
  jitter.
- At this stage, before correcting vertical projection jitter, the 8-sample
  post-fix capture still differed: classiCAD covered 112,370 pixels
  and Blender 111,933 at the >8 foreground threshold, with 469 XOR pixels
  (453 classiCAD-only, 16 Blender-only) and IoU 0.99583. Bounds match. The
  reference fixture can write scene-linear Combined and Workbench Z channels
  to a multilayer OpenEXR with `--linear-output`; run
  `tests/workbench_exr_to_pfm.py` with Blender's bundled OpenImageIO bindings
  to extract the channels as PFM. The offscreen contract writes classiCAD's
  resolved scene-linear texture through
  `CLASSICAD_VIEWPORT_AA_SAMPLES8_LINEAR_CAPTURE` and its final sample-zero
  depth through `CLASSICAD_VIEWPORT_AA_SAMPLES8_DEPTH_CAPTURE`. The Blender
  and classiCAD depth masks each contain 111,480 pixels with bounds
  `(142,42)` to `(497,437)`. Their view-depth MAE is 0.000040, p95 absolute
  error is 0.000095, maximum error is 0.000219, and center depths are
  6.793828 and 6.793762 respectively. This makes a meaningful projection or
  depth-scale mismatch unlikely for sample zero; it does not verify the other
  seven jittered samples. After subtracting each render's own background and
  thresholding scene-linear distance at 0.01, the bounds match `(142,41)` to
  `(497,438)`; classiCAD covers 112,316 pixels, Blender 111,863, with 455 XOR
  pixels and IoU 0.99595. Shared foreground
  linear RGB MAE is 0.00208. The edge mismatch therefore exists before the
  display transform; these resolved files still cannot separate per-sample
  raster coverage from temporal accumulation/SMAA. Source review finds the
  jitter ordering, 3x3 Blackman-Harris weights, temporal accumulation, and
  SMAA path aligned with Blender 5.2.2, so changing those algorithms is not
  justified by this evidence. The following per-sample capture and camera-shift
  comparison supersede this intermediate finding.
- The offscreen render contract has an opt-in sample capture prefix,
  `CLASSICAD_VIEWPORT_AA_SAMPLES8_SAMPLE_CAPTURE_PREFIX`. It writes eight raw
  scene-linear sample colors and per-sample view depths, then each sample's
  weighted log-color history and scene-linear SMAA resolve. The capture path
  reads the renderer's private depth texture before `finishSceneSample()`
  restores sample-zero depth; it does not affect normal rendering. A fresh
  640×480 run produced finite color/depth captures for all eight jitter
  positions. Sample-zero foreground coverage matches Blender's depth mask at
  111,480 pixels with identical bounds; raw sample coverage then varies with
  the later jitters as expected. Blender 5.2.2's installed background API
  still exposes only the final render result, so these classiCAD intermediates
  are not direct internal Blender-buffer reads. The following camera-shift
  reference fixture supplies the validated comparison route without a Blender
  source build.
- The single-sample fixture now reads that jitter metadata and renders eight
  Blender AA-off camera-shift references in one background session. The camera
  translation reproduces Blender's `window_translate_m4` projection shift;
  comparing all eight reference buffers against classiCAD showed at most two
  silhouette XOR pixels per sample and view-depth MAE below 0.00005. This
  isolated the remaining edge-profile mismatch to Y: Blender adds positive Y
  directly to the orthographic projection matrix, while classiCAD had negated
  its screen-space jitter Y. `viewportViewProjection` now uses the Blender
  sign. Fresh 8-sample output has identical normalized foreground masks in
  classiCAD and Blender (both 111,863 pixels, XOR 0, IoU 1.0); normalized
  scene-linear RGB MAE on shared foreground is 0.00155. The jitter capture and
  camera-shift options are diagnostic tooling; ordinary scene rendering is
  unchanged when capture options are unset.

### Controlled Workbench effect captures (2026-10-12)

- The Blender fixture now supports a two-quad `TRANSPARENT_OVERLAP` scene and
  explicitly selects World cavity for cavity captures. The offscreen GPU
  contract has opt-in captures for shadow, cavity, outline, and transparency
  scenes. `tests/workbench_pfm_compare.py` compares RGB PFM outputs by channel
  error and foreground-mask XOR/IoU.
- At 640×480 with the same cube scene, AA-off references, and an RGB change
  threshold of 2, shadow masks match exactly: 111,480 changed pixels on each
  renderer, XOR 0, IoU 1.0. The per-pixel effect-delta MAE over the union of
  those masks is 0.522/255, with maximum delta error 8/255. Outline masks also
  match exactly at 2,316 pixels each, XOR 0, IoU 1.0; effect-delta MAE is
  10.48/255 and maximum error 35/255. This confirms effect coverage, while
  outline intensity/edge values still differ.
- World cavity is not pixel-matched: classiCAD changes 20,503 pixels and
  Blender 26,048; mask XOR is 9,919, IoU 0.649, and effect-delta MAE over the
  mask union is 1.32/255 (maximum 14/255). Source review confirms
  `cavityViewPosition` uses negative forward view depth, the same sign as
  Blender. The earlier proposed background-offset sign change was therefore
  incorrect and was reverted; no cavity shader change is justified by that
  hypothesis. The sampling and edge-mask difference remains open.
- A transparent overlap capture currently does not provide a valid matched
  comparison: classiCAD's colored-region bounds are `(268,0)`–`(639,307)`,
  while Blender's are `(134,86)`–`(479,393)`. The shared target dimensions and
  final presentation path were checked; the discrepancy remains in the
  fixture-to-render path. Do not treat this capture as evidence of an OIT
  renderer mismatch. Correct the fixture projection and rerun the comparison
  before changing the shared weighted-OIT implementation. The no-shared-depth
  sorted-transparency fallback remains less capable than Blender's per-pixel
  OIT for intersecting ranges.

### Bounded scalar volume pass (2026-10-12)

- Blender 5.2.2 draws volumes after transparent depth and before outline, depth
  of field, and anti-aliasing. classiCAD now imports one bounded scalar grid,
  persists it through native JSON, and renders it in that same scene-pass slot.
- The pass adapts Blender's `resolution * 5` per-axis sample count, maximum
  sample count, transformed world step length, Bayer dither, Halton sample
  offset, closest/linear/cubic interpolation, slice opacity, and perspective
  view-ray correction. `BKE_volume_density_scale` is reproduced for OBJECT and
  WORLD render spaces. The renderer's dense proxy is capped at 512 voxels per
  axis and 16M voxels; the ray loop is capped at 2,560 samples.
- Ray-march and slice integration also match Blender's extinction floor:
  extinction is `max(density, 1e-4)` while scattering retains the signed
  density value, so premultiplied radiance includes `density / extinction`.
  Float RGBA16F offscreen contracts confirm both sub-cutoff positive density
  and negative density contribute Blender-style alpha/scattering. This follows
  Blender's pinned [`workbench_volume.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_volume.bsl.hh#L225-L250).
- A direct `QOpenGLContext` + `QOffscreenSurface` + FBO contract renders a
  synthetic active volume, confirms the `Volume` pass is reported, and checks
  that more than 100 pixels change. This does not launch the classiCAD window.
- The port remains bounded to a single-channel FLOAT grid. Multiple/vector
  grids, animation, volumes above the dense GPU bounds, and QPainter volume
  rendering remain unsupported. Blender's Workbench volume shader itself
  leaves shadows as a TODO. A matched Blender/classiCAD image comparison is
  still required for visual sign-off.

### Implementation plan

1. **Establish a comparable baseline.** Use the same hard-edged cube, viewport pixel dimensions, projection and camera pose, clipping range, color transform, and selection/overlay state in Blender 5.2.2 and classiCAD. Capture classiCAD's native GPU path separately from QPainter. Compare AA Off, Single Pass, and 8 samples, both idle and while navigating. Save pre-AA, post-AA scene-linear, and final display-transformed outputs so edge softness can be attributed to a specific stage. Do not call the AA port complete until the edge profiles and remaining differences are recorded.
2. **Make pass boundaries explicit.** Build a classiCAD render packet from `ViewportRenderFrame` and its derived geometry/material ranges, then move pass sequencing out of `viewport_widget.cpp` into a renderer coordinator. Give color, depth, normal, object-ID, and transparency attachments explicit owners and lifetimes. Use the same GPU scene-pass sequence for the interactive viewport and Workbench render result; keep CAD editing overlays and the grid in their existing post-resolve layer.
3. **Port source-level pass behavior in bounded groups.** Compare opaque/material and depth state first; then transparent accumulation/depth; then object-ID outline and cavity/curvature; then camera-gated DOF and final resolve/presentation. For each group, adapt Blender 5.2.2 source/shaders where they are separable, preserve Blender attribution/license headers, and record any input or backend adaptation. Keep the existing TAA/SMAA implementation unless the controlled comparison identifies a concrete mismatch.
4. **Use the existing Workbench material adapter seam.** The future material/shader evaluator should resolve only Workbench-supported material inputs: scene-linear base color, roughness, metallic, alpha, and the supported image-texture result plus its sampling/color-space/alpha settings. Blender 5.2.2's active image source includes Image Texture and Environment Texture nodes; classiCAD retains the active node's image record, using Extend when Environment Texture has no extension property and Repeat for the Image Texture fallback. This does not evaluate arbitrary shader-node graphs. Feed those values through `WorkbenchMaterialData` in `src/core/document/scene_object.h` and the per-slot conversion in `src/ui/viewport/viewport_render_frame.cpp`. Per-face material slot indices, active UVs, and color attributes stay in `PolygonMesh3D` (`src/core/geometry/polygon_mesh.h`) and travel through `ViewportRenderObject` (`src/ui/viewport/viewport_render_frame.h`) into `DrawRange` (`src/ui/viewport/viewport_surface_renderer.h`). Geometry normals remain sourced from evaluated/display geometry; they are not material-evaluator outputs. Blender UDIM tile numbers, external paths, and packed tile bytes survive import/native save. GPU color, effect-alpha, object-ID-alpha, and shadow-map-alpha paths sample the tiles; stencil shadow-volume casters do not sample texture alpha. QPainter color and cavity/outline effect-alpha sampling support meshes, trimmed/untrimmed NURBS, and prepared-UV `NurbsSolid` effect triangles. The depth-map fallback samples texture alpha for textured meshes and trimmed/untrimmed `NurbsSurface` using active or tessellation UVs; its caster/topology coverage remains narrower than the stencil path. The QPainter Solid fallback does not render shadows at all. GPU per-tile Repeat, Extend, Clip, and Mirror modes are implemented. Filtering across neighboring tile seams is not part of Blender 5.2.2 behavior. The GPU object-ID pass uses shared tessellation UV data for trimmed and untrimmed NURBS ranges, so UDIM alpha masking is supported there.
5. **Keep fallback scope honest.** Preserve the QPainter path for fallback, but compare and document it separately; do not claim that supersampling is equivalent to Blender's GPU pass graph. If CPU visual parity becomes a requirement, make it a separate rendering backend project after the GPU path is stable.

### Phase Two acceptance

- A repeatable, nonblank comparison uses identical geometry, camera, viewport size, color transform, AA setting, and overlay state.
- Intermediate outputs identify whether any edge/color difference appears in geometry shading, effect composition, AA resolve, or display conversion.
- Viewport and Workbench render-result GPU paths share the same named scene passes and attachment contract.
- Each newly ported pass has a pinned Blender 5.2.2 source reference, preserved attribution, and a comparison result; existing NURBS/surface contracts remain unchanged.
- The future shader system can provide Workbench's supported material values through `WorkbenchMaterialData` and existing mesh/draw-range data without changing the render-pass API or creating a second material model; arbitrary node graphs are out of scope. Imported UDIM metadata and packed tile bytes survive native save/reopen; GPU and QPainter mesh/trimmed-NURBS/untrimmed-NURBS color and effect-alpha paths consume tiles. GPU depth-map shadow alpha uses texture data and UVs for meshes, trimmed/untrimmed `NurbsSurface` ranges, and object-textured `NurbsSolid` ranges; geometry without an available image/UV alpha source remains unmasked, while stencil shadow-volume casters do not sample texture alpha. Distinct per-face materials on `NurbsSolid` remain unsupported. The QPainter Solid fallback has no shadow pass; CPU shadow rendering is out of scope. GPU per-tile extension modes are implemented. Cross-tile filter continuity is not part of Blender 5.2.2 behavior.
- A bounded single-channel Blender `VOLUME` grid imports, survives native save/reopen, and renders through the GPU Volume pass after transparent depth; sampling, density scale, slice controls, and Blender's pass order are source-aligned. Multiple/vector grids, animation, oversized dense bounds, and the QPainter fallback remain explicit limits.
- QPainter differences remain separately described until that backend is explicitly brought into scope.

### Stage 5: Parity review

Status: in progress; the implementation and source reviews found the blockers listed above. The team has not signed off on full parity.

- The user clarified that classiCAD's current viewport should be treated as Edit Mode; its separate Object Mode is planned for later. Do not use the missing Object Mode as an explanation for base surface, background, or grid differences. The supplied screenshots still are not a controlled render comparison: classiCAD reports `Ortho`, Blender reports `User Perspective`, and the visible scene geometry differs. Blender's mesh edit edges and curve edit wires are separate Overlay-engine geometry; classiCAD's Solid-mode CAD curve strokes and `NurbsSolid` face boundaries use the configurable edit-wire theme setting, while control geometry and selection behavior remain a distinct implementation. Compare base rendering separately from mode-specific overlays, with matching camera and geometry.
- This is an adaptation of selected Blender shading equations and passes into classiCAD's renderer, not a verbatim port of Blender's complete Workbench and Overlay engines. The table above and stage notes remain the parity record.
- Review execution confirmed the installed Blender runtime is 5.2.2 LTS and the classiCAD C++ build passes. Background extraction smoke checks now pass for original and evaluated mesh modes, POLY/BEZIER/NURBS curves, NURBS surface patches, nested collection parents and visibility records, and reporting omitted loose mesh geometry. An offscreen classiCAD startup reached viewport construction, but Qt could not create an OpenGL context; this does not validate the GPU renderer. A direct native-GPU offscreen cube render is now compared against Blender's background Workbench render at 640×480 for AA Off, Single Pass, and 8 samples. The Blender fixture can now independently render baseline, shadows, cavity, and outline modes. Blender 5.2.2 reference runs changed 111,858 pixels for shadows, 45,995 for cavity, and 2,410 for outline at an RGB threshold of 2. classiCAD's offscreen GPU contracts now require shadows to darken a pixel and World cavity to change a pixel against matching unselected, outline-free captures; cavity uses the shared-target scene-pass coordinator. These assert effect activation, not pixel parity. Scene-camera GPU projection now consumes Blender's sensor fit and dimensions, pixel aspect, camera shifts, orthographic scale, and exact clip range; depth readback uses that same range. A transform contract checks framing center, shift direction/magnitude, pixel-aspect scaling, and clip values. Matched Blender camera renders, annotation alignment with non-square pixels, direct internal Blender per-sample buffer reads, QPainter, and navigation cases remain open. Full parity is not signed off.
- The AA capture comparison has a numerical edge-profile follow-up recorded in the [controlled cube baseline](#controlled-workbench-cube-baseline-2026-10-12). Orthographic matrix precision and vertical jitter sign are corrected; fresh captures confirm exact silhouette masks at Off, Single Pass, and 8 samples. The source-derived per-sample fixture isolates the previous 8-sample mismatch to projection jitter Y. Scene-linear foreground RGB MAE remains 0.00155 in the controlled cube comparison, and QPainter/navigation comparisons plus the broader parity review remain open.

### Final display dither (2026-10-12)

- Offscreen Workbench results now apply Blender 5.2.2's integer-coordinate
  `hash_iqint3_f` and triangular-noise remap to the final display-referred
  RGBA64 result after CAD annotations are composited. RGB receives
  `0.0033 * render.dither_intensity`; alpha is unchanged. Premultiplied
  transparent captures are unpremultiplied for the RGB perturbation and
  premultiplied again. GPU presentation dither is disabled for this result, so
  the export image is dithered exactly once. The ordinary interactive
  viewport retains zero dither, and EXR/HDR scene-linear Combined data plus
  16-bit PNG/TIFF output bypass the CPU operation.
- Qt's top-down image rows are mapped to bottom-left render coordinates
  (`y = height - 1 - row`), matching Blender's integer pixel convention; its
  chunked CPU conversion likewise uses `y + start_y`. Contracts check the
  Blender pixel vector, repeatability, RGB changes, alpha and
  premultiplied-alpha preservation, 8-bit gating, and unchanged 16-bit output.
  The focused render-contract test passes. References:
  [Blender hash and triangular remap](https://github.com/blender/blender/blob/v5.2.2/source/blender/blenlib/intern/math_color_inline.cc#L230-L255), [integer pixel-coordinate contract](https://github.com/blender/blender/blob/v5.2.2/source/blender/blenlib/BLI_math_color.h#L144-L152), and [float RGBA byte conversion](https://github.com/blender/blender/blob/v5.2.2/source/blender/imbuf/intern/conversion.cc#L1131-L1143).

### Scene-history invalidation and outline color (2026-10-12)

- The temporal AA history key now includes polygon meshes as well as NURBS
  surfaces/solids. It records geometry and prepared-geometry revisions,
  transforms, selection and in-front state, object/material colors, per-face
  mesh materials, texture source metadata and packed bytes, and component
  tint-suppression state. It also keys the resolved scene-linear World color,
  including the distinction between no World color and a present color.
  Changes to mesh membership, geometry, selection, materials, textures,
  NURBS texture paths, World color, or component selection therefore restart
  accumulation instead of presenting stale history. An offscreen contract
  checks key changes and verifies a completed sample sequence restarts at
  sample zero after a mesh input changes.
- Blender Workbench uses the configured object-outline color for all object
  IDs. The GPU outline pass now uses that color for both selected and
  unselected objects; CAD selection feedback remains in its separate overlay
  path. An offscreen contract compares selected and unselected silhouette
  pixels against matching outline-free baselines. QPainter outline composition
  and broader effect pixel comparisons remain open.

- Run the comparison matrix below against Blender 5.2.2.
- Record remaining visual differences with a source or implementation explanation; capture images only when both applications show the same nonblank scene and matched view.
- Update the baseline table and this plan as each stage is completed.

## Comparison and completion checklist

Use the same machine and display where possible. Match viewport pixel dimensions, view and projection, clipping range, scene contents, selection/edit state, color management, and preference values. Compare Blender and classiCAD side by side or with aligned screenshots.

- Studio, MatCap, and Flat modes, including light/MatCap choice and rotation.
- Every color source, using objects with material values, object colors, color attributes, and UV-mapped textures.
- Background choices and color management.
- Culling, X-Ray, shadows, outlines, specular, and cavity/curvature, both individually and in combinations. Verify camera DOF now that classiCAD imports and persists camera optics, including aperture shape controls.
- No AA, the preference's “Single Pass Anti-Aliasing” value (Workbench one-sample TAA+SMAA), and 5/8/11/16/32-sample multisampling, both idle and during navigation.
- Accumulation reset after camera movement, geometry edits, selection changes, setting changes, resize, and display-scale changes.
- Overlay Smooth Wires and Edit Mode Smooth Wires independently, including confirming the grid remains antialiased when overlay wire smoothing is off.
- Save/reopen behavior for all persistent settings and any new material or color data.
- GPU renderer and QPainter fallback behavior.

The port is complete when every enabled option has matching Blender 5.2.2 behavior, no enabled saved setting is inert, and the comparison checklist passes. Disabled unsupported features must remain clearly documented. Workbench shadows now use validated stencil volumes for closed `NurbsSolid` and imported `PolygonMesh3D` subsets, plus validated open surfaces; full parity still requires comparison and a matching path for the currently disclosed unsupported-geometry and transparency fallbacks.

## Blender 5.2.2 source references

Use these pinned source files as the implementation authority:

- [Workbench engine and pass sequence](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_engine.cc)
- [Workbench state and AA selection](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_state.cc)
- [Workbench material and color setup](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_materials.cc)
- [Workbench Random HSV palette and object alpha source](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_materials.cc#L20-L33)
- [Rec.709-to-scene-linear gamut conversion (identity for the default Rec.709 scene-linear space)](https://github.com/blender/blender/blob/v5.2.2/source/blender/imbuf/intern/colormanagement_inline.h#L46-L53)
- [Workbench private settings and pass data](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_private.hh)
- [Workbench `Material` constructors, including opaque `Material(float3)` defaults](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_private.hh#L1580-L1587)
- [Workbench material packing (`Material::pack_data`)](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_private.hh#L1609-L1623)
- [Blender's float-to-UNorm8 conversion](https://github.com/blender/blender/blob/v5.2.2/source/blender/blenlib/intern/math_base_inline.cc#L579-L583)
- [Workbench composite shader](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_composite.bsl.hh)
- [Workbench anti-aliasing implementation](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_effect_antialiasing.cc)
- [Workbench TAA filter shader](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_fx_taa.bsl.hh)
- [Workbench SMAA fragment shader](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_effect_smaa_frag.glsl)
- [Workbench cavity implementation](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_effect_cavity.cc)
- [Workbench depth-of-field setup and aperture sampling](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_effect_dof.cc#L53-L85)
- [Workbench cavity jitter texture generation](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_resources.cc#L74-L98)
- [Workbench cavity jitter sampling shader](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_cavity.bsl.hh#L41-L63)
- [Workbench curvature sampling shader](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_curvature.bsl.hh#L17-L49)
- [Blender blue-noise LUT used by Workbench](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/eevee/eevee_lut.cc#L6190-L6200)
- [Workbench shading cavity and curvature RNA ranges](https://github.com/blender/blender/blob/v5.2.2/source/blender/makesrna/intern/rna_space.cc#L4268-L4396)
- [Scene Display SSAO RNA ranges](https://github.com/blender/blender/blob/v5.2.2/source/blender/makesrna/intern/rna_scene.cc#L7245-L7257)
- [Scene Display SSAO defaults](https://github.com/blender/blender/blob/v5.2.2/source/blender/makesdna/DNA_scene_types.h#L2378-L2397)
- [View3D cavity and curvature factor defaults](https://github.com/blender/blender/blob/v5.2.2/source/blender/makesdna/DNA_view3d_types.h#L576-L585)
- [Workbench outline implementation](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_effect_outline.cc)
- [Workbench shadow-volume pass and focus/shift setup](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_shadow.cc)
- [Workbench shadow-volume geometry shader](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_shadow.bsl.hh)
- [Workbench shadow falloff composition](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_world_light.bsl.hh)
- [Viewport preference definitions, including Smooth Wires](https://github.com/blender/blender/blob/v5.2.2/source/blender/makesrna/intern/rna_userdef.cc)
- [Blender user-preferences UI](https://github.com/blender/blender/blob/v5.2.2/release/scripts/startup/bl_ui/space_userpref.py)
- [Overlay wire anti-aliasing pass](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/overlay/overlay_antialiasing.hh)
- [Overlay edit-mesh edges](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/overlay/overlay_mesh.hh)
- [Overlay curve edit wires](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/overlay/overlay_curve.hh)
- [Blender default 3D View wire and edit-wire theme colors](https://github.com/blender/blender/blob/v5.2.2/release/datafiles/userdef/userdef_default_theme.c#L365-L390)
- [Overlay wireframe drawing](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/overlay/overlay_wireframe.hh)
- [Overlay grid drawing](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/overlay/overlay_grid.hh)

Blender 5.2 manual pages provide the user-facing behavior to cross-check against the code:

- [Workbench sampling](https://docs.blender.org/manual/en/5.2/render/workbench/sampling.html)
- [Workbench color](https://docs.blender.org/manual/en/5.2/render/workbench/color.html)
- [Workbench lighting](https://docs.blender.org/manual/en/5.2/render/workbench/lighting.html)
- [Workbench options](https://docs.blender.org/manual/en/5.2/render/workbench/options.html)
