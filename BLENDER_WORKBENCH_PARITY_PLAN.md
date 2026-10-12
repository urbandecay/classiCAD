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
  PACKBITS writes LZW. classiCAD also records the requested `tiffCodec` in
  metadata, not the actual encoding. CMake has no TIFF/OIIO writer dependency
  and this environment has no TIFF development headers, so exact codec parity
  requires adding a suitable encoder; until then, DEFLATE/PACKBITS are known
  output approximations.

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
  the scene-linear RGB directly, while the QPainter fallback converts linear
  sRGB channels for its display-referred fill. `THEME`, `VIEWPORT`, and Custom
  backgrounds keep their prior paths. Blender's pinned state selection above
  provides the World-versus-viewport behavior; the implementation still lacks
  full view-transform handling for HDR World colors in the QPainter fallback.

### Transparent-film Combined alpha (2026-10-12)

- Blender 5.2.2 exposes `RenderSettings.film_transparent` as the
  `R_ALPHAPREMUL` bit of `Scene.r.alphamode`; Workbench uses that same bit to
  leave its render background transparent. The importer retains this setting
  with render-output metadata. The offscreen Combined path clears to zero and
  skips its viewport background only when transparent film is enabled;
  interactive viewport rendering remains opaque. Pinned sources:
  [`rna_scene.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/makesrna/intern/rna_scene.cc#L7233),
  [`workbench_state.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_state.cc#L175-L184),
  and [`workbench_engine.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_engine.cc#L488-L494).
- For transparent-film PNG/TIFF output, the presentation transform now
  unpremultiplies RGB before the nonlinear view transform and premultiplies the
  result afterward. Blender's color-managed output path predivides premultiplied
  render buffers before processing; see
  [`colormanagement.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/imbuf/intern/colormanagement.cc#L1574-L1582)
  and [lines 2124-2165](https://github.com/blender/blender/blob/v5.2.2/source/blender/imbuf/intern/colormanagement.cc#L2124-L2165).
  The scene-linear Combined EXR payload remains premultiplied and unchanged.

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
  link below. Other aperture geometry and image parity remain unverified.

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
  The shader's sector/modulo form is algebraically equivalent. Its denominator
  no longer has an epsilon floor, preserving Blender's two-blade edge equation.
  Aperture and overall output still need visual comparison.
  Pinned source: [`workbench_effect_dof_blur1_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_effect_dof_blur1_frag.glsl#L20-L31), with the texture populated in [`workbench_resources.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_resources.cc#L84-L100). The QPainter fallback remains a fixed 12-tap
  full-resolution gather and has no median pass. Its aperture shaping now uses
  Blender's `blades > 1` condition and polygon/rotation/reciprocal-X-ratio
  mapping, including the two-blade slit. Image comparison remains open. Pinned
  gather setup: [`workbench_effect_dof.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_effect_dof.cc#L783-L899); the GPU stages are defined by the adjacent pinned Workbench DOF shaders.

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
| Color management | Imported scenes retain display device, view transform, look, exposure, and gamma. Live GPU rendering keeps scene surfaces, transparency, pictures, and backgrounds in RGBA16F scene-linear color through depth of field and temporal AA, then applies exposure, the active sRGB AgX/Standard/PBR Neutral/Filmic or imported OCIO LUT, and gamma once during presentation. Offscreen Workbench results now run the configured viewport AA sample mode through the same scene-linear resolve and retain the resolved float Combined payload for Radiance HDR and OpenEXR when available. Other non-EXR display-referred output uses RGBA16/QImage, so HDR range is lost before 16-bit PNG/TIFF writing. QPainter Solid rendering retains its CPU display-transform path. Other sRGB view/look combinations use validated Blender-baked LUTs shared by scenes with identical table bytes. Custom 65³ LUTs use an analytic `log2_1p` shaper over [0, 16] (scale 64); older native LUTs retain their linear [0, 2] metadata. Inputs outside the stored domain clamp to its endpoints. | Partial; render-specific sample-count mapping, non-sRGB display devices, arbitrary OCIO configuration transfer, LUT precision/visual comparison, GPU/QPainter differences, and matched render comparisons remain open |
| Surface drawing | NURBS surfaces and solids are tessellated for display and shaded by classiCAD's custom forward GLSL renderer. Curves use the stroke path. | Different rendering architecture; visual parity remains unverified |
| Color source | Object, Random, Material, and Custom color plus their Blender-compatible alpha sources reach filled NURBS surfaces/solids in GPU and QPainter paths. Blender mesh import preserves object display colors, shared materials, per-face slots, active UV maps, and image references. GPU mesh Texture mode samples the active Image Texture Node per fragment through interpolated active-UV coordinates, with repeat wrapping and linear filtering; supported external raster files are loaded with size limits and cached by path/color space. QPainter mesh Texture mode rasterizes each triangle per target sample, uses reciprocal camera-forward depth for perspective-correct UVs, and bilinearly samples external and packed images with repeat wrapping. An audit found the reciprocal weight used the camera-to-scene direction with the wrong sign, collapsing varying positive depths to the clamp; mesh and NURBS raster paths now use positive camera-forward depth. UDIM tiles now preserve Blender 5.2.2 per-tile packed bytes in native documents and sample them in GPU and QPainter mesh and NURBS paths before falling back to external tile paths; import caps packed payloads at 32 MiB total per image, and absent tiles remain magenta. GPU and QPainter NURBS Texture sample trimmed and untrimmed surfaces using tessellation parameter UVs normalized over the exact knot domains. This matches Blender 5.2.2's index-normalized UVs because Blender samples uniformly over each active parameter domain. QPainter mesh and NURBS Texture honor the imported image sRGB flag: unpremultiply RGB as before, decode sRGB samples to scene-linear when requested, multiply with the material base color before display conversion, and apply the active exposure/gamma/OCIO or built-in transform. QPainter textured-mesh shading reconstructs its incident direction and normal per raster pixel: perspective samples use perspective-correct position/normal interpolation and orthographic samples use barycentric normal interpolation; corner normals are transformed by the world inverse transpose, with a flat geometric normal as fallback for invalid or absent corner data. QPainter untextured Workbench mesh triangles now rasterize per pixel using scene-linear material/object/random/custom/attribute colors, per-face roughness and metallic, interpolated corner normals, and perspective-correct incident direction. Random and Custom inputs use Blender-compatible color conversion; Attribute keeps the existing triangle-averaged color and decodes it only when its metadata says sRGB. Raster size limits retain the existing flat polygon fill for oversized untextured triangles. Orthographic incident direction remains constant. QPainter untextured NURBS tessellation triangles are also rasterized and shaded per pixel using their flat triangle normal; perspective samples reconstruct their world position, while orthographic incident direction remains constant. Textured NURBS samples continue through the per-pixel material lighting path. GPU and QPainter texture-alpha cutouts apply in color rendering for trimmed and untrimmed NURBS. GPU object-ID/outline and shadow-alpha paths, and QPainter cavity/outline effect-alpha paths, also cover trimmed and untrimmed tessellated NURBS using active-domain UVs and Blender's alpha rules. The alpha cutoff is 0.1 when enabled and is bypassed for NONE/Ignore and CHANNEL_PACKED. Packed Image Texture bytes up to 32 MiB are transported, persisted, and sampled by both paths. Missing images or UVs fall back to Material. Both renderers unpremultiply PREMUL images except at alpha 0 or 1. Blender Workbench's pinned [`workbench_image_lib.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_image_lib.glsl#L34-L60) performs UDIM lookup or regular image sampling, unpremultiplies PREMUL pixels, and discards fragments below `image_transparency_cutoff`; Workbench uses supported material display values and its active Image Texture input rather than evaluating arbitrary shader graphs. | Partial; color/effect alpha coverage is implemented for the described passes, while QPainter composition, trim tessellation, and final visual comparisons remain approximate or unverified |
| Blender scene import | File > Import > Blender Scene invokes the pinned Blender 5.2.2 extractor and offers original or evaluated mesh mode. File > Open now routes ordinary `.blend` files through the same original-mode extractor and loads the supported result as a new document; native classiCAD `.blend` files still restore their embedded authoritative snapshot. Imported polygon meshes retain geometry, custom split normals, object/material data, direct links to multiple Blender collections, collection hierarchy and global hide flags, plus per-view-layer collection-instance paths/exclusions/visibility and object hide state. Mesh loose edges are emitted as a companion CURVE item with a deterministic derived transport identity, separate from the source mesh identity. Each edge becomes a world-space degree-one NURBS component; the companion is named from the source object and its transport record carries the source object ID, while object display color, collection memberships, visibility, and per-view-layer state. Isolated vertices remain explicitly reported as omitted because classiCAD has no Blender point-item import path. Evaluated Blender FONT objects convert to baked polygon meshes with evaluated transforms and material assignments; original text/font/layout editability is lost and reported. Evaluated Blender METABALL objects also convert to baked polygon meshes with evaluated transforms and material assignments; original mode explicitly reports them unsupported, and editable metaball data and threshold settings are not retained. Exact Blender CURVE POLY splines import as dimension-3 degree-1 NURBS after matrix_world is applied; cyclic splines explicitly close. Each cubic Blender BEZIER span imports as a separate nonrational clamped degree-3 NURBS with its four transformed endpoint/handle points; cyclic spans include last-to-first, including the one-point cyclic loop. Blender CURVE NURBS splines preserve their weights, degree, generated knot multiplicities/domain, and cyclic wraparound control points in dimension-3 NURBS. Blender SURFACE NURBS splines import as separate exact NURBS surface objects, with world-space control nets, U/V knot domains, weights, cyclic directions, per-patch identities, and assigned Workbench material values preserved. Multiple source curve splines remain components of the same imported source object. Object display name/color and collection memberships are retained. Blender parent relationships are not represented; supported child objects retain their world-space geometry/transform and now report this hierarchy loss. Bevel/extrude, bevel-object, and 2D fill surfaces are explicitly reported as omitted while exact centerlines are kept. No curve approximation is emitted. The Collections panel selects among imported Blender view layers; selected view-layer visibility resolves each object's direct collection memberships across visible paths. Linked objects combine memberships and per-view-layer hide state. The default option preserves per-scene first-layer visibility. Collections remain independent of CAD layers. Transport identity keys use a SHA-256 namespace derived from the normalized absolute source .blend path for objects, materials, collections, scenes, view layers, and their references; moving or renaming the source project changes that namespace. Native document version 15 persists Blender camera, render-output, imported-scene, and deduplicated OCIO LUT metadata and reads versions 1–14; the scene extractor bridge accepts versions 10 through 14. Ordinary `.blend` Open also maps supported `scene.display.shading` Solid settings into the initial viewport and preserves `scene.display.render_aa` for offscreen render results; the separate File > Import > Blender Scene flow does not yet apply the shading settings to the viewport. | Partial; additional object types and visual/import coverage remain gaps. Cameras, lights, and empties currently have no matching committed classiCAD scene-object geometry type; the extractor reports these objects (the active camera's supported settings are separately retained as scene metadata). In particular, an Empty that instances a collection is metadata-only: its referenced collection's objects are not expanded into imported items at the instance transform. Blender Workbench Solid uses its studio-light rig rather than scene light datablocks. Adding camera/light/empty datablocks as editable scene objects needs an explicit model/serialization/rendering contract, so they are not fabricated as points or proxy meshes. |

**Import update:** The scene extractor bridge now accepts versions 10 through 14; the native document schema remains version 15 and reads versions 1 through 14. Ordinary Blender `.blend` Open now imports the first scene's `scene.display.shading` Solid settings into the initial classiCAD viewport and preserves `scene.display.render_aa` for offscreen Workbench render results. It maps bundled studio-light/MatCap presets, color source and background, shadow/cavity/curvature/outline controls, `show_xray`, the distinct `show_xray_wireframe`, `matcap_flip_x` when Blender exposes it, and studio-light rotation converted from radians to degrees. Custom unavailable presets, scene-light rendering, studio-light background/intensity differences, and Blender 5.2.2's RNA-hidden MatCap flip are reported as unsupported. Imported World backgrounds are retained and used for both interactive and offscreen rendering. Existing classiCAD project opens do not apply imported Blender shading. Ordinary `.blend` Open creates a new classiCAD document; its source path is not used as the Save target, so users must use Save As to create a native project.
| Material values | Per-object base color, roughness, metallic, and alpha are serialized and consumed by both renderers. Imported Blender meshes retain shared material records and per-face slot references; GPU and QPainter mesh paths select face material values. Attribute colors also reach both mesh renderers. classiCAD currently has no UI for authoring appearance values. | Partial; material packing matches Blender's `Material::pack_data`; material authoring remains a gap |
| Culling and X-Ray | GPU surface culling works; X-Ray uses translucent fill plus depth/stroke passes. QPainter applies backface culling to polygon-mesh triangles using their transformed world-space winding and the active view direction. In Solid mode, its bounded surface-batch compositor uses per-pixel nearest opaque depth for NURBS and polygon-mesh fills and sorts precomposited transparent object fragments by their per-pixel nearest tessellation depth, with a four-million-fragment cap. If prepared geometry or either memory budget is unavailable, the fallback draws surface objects in average view-depth order and sorts polygon-mesh and NURBS-surface triangles by projected centroid. It has no general scene depth buffer for strokes or separate X-Ray depth/stroke passes. | Partial; inter-object transparent ordering improves within the bounded compositor, while precomposited intra-object self-occlusion, X-Ray, and fallback ordering still differ |
| Shadows and outlines | Validated closed opaque `NurbsSolid` and imported `PolygonMesh3D` casters use depth-pass stencil side volumes when all source and extruded vertices clear the camera near plane; otherwise closed solids, open `NurbsSurface` casters, and transparent casters use the capped depth-fail stencil path. Opaque in-front receivers now use their own depth-stencil domain for volume shadows. Blender 5.2.2 uses stencil shadow volumes and has no directional depth-map fallback. classiCAD retains a 1024-pixel directional depth map with 3x3 PCF only as a GPU compatibility fallback for unsupported topology; this is not Blender behavior. GPU shadow-map alpha supports trimmed and untrimmed NURBS via tessellation UVs, while the GPU map fallback has narrower caster coverage. Stencil shadow-volume casters do not sample texture alpha. The QPainter Solid fallback does not render shadows; CPU shadow rendering is an out-of-scope approximation. Focus/shift and light direction use Blender's formulas in the GPU path. GPU and QPainter silhouette opacity use Blender Workbench's four-cardinal-neighbor object-ID resolve. For classiCAD's additional selection-color behavior, both backends now use four-cardinal-neighbor adjacency. Blender's outline pass uses one outline color and has no selected-color behavior. GPU object-ID/outline alpha and QPainter cavity/outline effect alpha cover trimmed and untrimmed NURBS texture cutouts. QPainter effects and the classiCAD-only GPU depth-map fallback remain approximate, and rendered output has not been visually compared. | Closed-solid and validated open-surface GPU shadow-volume paths are ported; the conservative depth-pass subset follows Blender's opaque-manifold route; the unsupported-caster GPU map is a documented classiCAD fallback, while QPainter shadows are not implemented and nonmanifold handling and outline composition remain partial |
| Cavity and depth of field | GPU Solid maps Blender's World type to SSAO and Screen type to normal-based curvature; Both uses both. The persisted settings expose Blender 5.2.2 defaults and ranges for SSAO distance/attenuation/sample count (`SceneDisplay`) and cavity/curvature ridge/valley factors (`View3DShading`); the renderer uses these values and distributes SSAO samples across AA iterations with Blender's 512-sample cap. Cavity jitter now recreates Blender's 64x64 blue-noise-derived RGB texture, including its clamped, total-sample-scaled radius offset. The GPU effect prepass uses opaque ranges as cavity inputs even when transparent ranges share the scene; transparent ranges are shaded without cavity. QPainter uses opaque-only cavity samples and applies the composite only where an opaque receiver is visible; cavity is skipped beneath a visible translucent surface. Its disk sampling follows Blender's deterministic radical-inverse/Hammersley-like sequence with per-pixel blue-noise rotation and sample-radius jitter. QPainter retains perspective-correct world-space surface positions from each rasterized triangle and evaluates Blender's signed displacement, length bias, distance attenuation, cavity/edge accumulation, and requested-sample normalization against those positions. Since dot products and lengths are invariant under view rotation, the world-space displacement/normal math matches those shader operations. The screen-space sample footprint remains an approximation. QPainter curvature controls now remain independent from SSAO cavity ridge/valley controls, matching Blender's separate control paths; its software curvature estimator remains an approximation. A perspective scene-camera mode stores focal length, sensor width, focus distance, f-stop, aperture blades, aperture rotation, and aperture ratio. Scene extraction resolves an evaluated camera's focus-object distance along local +Z, matching Blender's camera DOF distance rule; pose-bone focus subtargets still use the numeric focus-distance setting. GPU and QPainter CoC now convert lens and sensor millimeters to Blender scene units with the Workbench `0.001` scale while retaining imported focus distance and depth in scene coordinates; singular or invalid optics leave the source image unblurred. GPU DOF uses half-resolution RGBA16F color and RG8 near/far CoC pyramids with preparation, two downsample, gather, 3x3 median, and full-resolution resolve passes before AA accumulation; viewport and offscreen render-result paths share this renderer. Blender adjusts sensor size for the camera viewport with `rv3d->viewcamtexcofac[0]`; classiCAD derives an effective fitted sensor width during camera import, and that mapping still needs visual comparison. The gather kernel accepts the three aperture shape controls, but exact aperture-sampling parity has not been visually verified. | GPU cavity sampling is source-derived; QPainter cavity remains an approximate software-buffer neighborhood pass; DOF is camera-gated in both paths. QPainter derives the physical CoC from the software depth buffer, gathers 12 same-or-behind-depth taps at full output resolution, and composites by CoC after the cavity effect. It uses the same optics gate as the GPU (enabled setting, perspective view, active scene-camera optics). This remains an approximation: it has no Blender half-resolution RGBA16F/CoC RG8 pyramid, three mip levels, temporal jitter, separate gather/blur passes, or depth-aware resolve; it applies aperture blade/rotation/ratio shaping to its 12 taps and uses a repeating 64x64 blue-noise spatial rotation; its depth buffer covers rasterized NURBS surfaces/solids and polygon meshes only, and transparent colors are already composited before the gather. QPainter color arithmetic is display-space and differs from Blender's linear intermediate; cavity is applied after that display transform, and opaque-only cavity is omitted under visible translucent surfaces because the color image has already composited them. GPU aperture blade/rotation/ratio shaping is implemented but awaits visual comparison; temporal jitter is unsupported. Blender focus-object distances are captured, while pose-bone subtargets remain unsupported. |
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
- **Display transform:** imported scene display device, view transform, look, exposure, and gamma are retained. Existing built-in sRGB AgX, Standard, Khronos PBR Neutral, and Filmic with the `None` look stay on their established transform paths. Other sRGB view/look combinations are baked to scene-specific 65³ RGB float LUTs by Blender's bundled PyOpenColorIO and applied in CPU/GPU rendering, with exposure before the table and gamma after it. This is an import-time snapshot of the machine's active OCIO configuration, not a runtime OCIO dependency; custom external config assets must be available to Blender during import. New LUTs sample a [0, 16] domain using an analytic `log2_1p` shaper (scale 64); older native documents preserve their linear [0, 2] metadata. Inputs outside a LUT's domain clamp to its endpoint. The document stores at most eight unique tables and 32 MiB decoded LUT data. The wider domain improves HDR coverage but the fixed 65³ table trades away some uniform low-range sample density; precision and output still need matched comparison. Other display devices remain unsupported. Blender 5.2.2's display processor composes an explicit look before the DisplayViewTransform and applies exposure in scene-linear space and gamma afterward ([pinned source](https://github.com/blender/blender/blob/v5.2.2/source/blender/imbuf/opencolorio/intern/libocio/libocio_display_processor.cc)).
- **Camera depth of field:** the GPU viewport and offscreen render-result path share the Blender 5.2.2 `DofPass` shape: half-resolution RGBA16F color and RG8 near/far CoC pyramids, full-resolution CoC-aware preparation, quarter/eighth-resolution downsample levels, half-resolution 49-sample gather, 3x3 median cleanup, and a depth-derived CoC resolve before AA accumulation. The shader's sample perimeter and square-to-circle radius match `DofPass::setup_samples`; polygon blade shaping uses the algebraically equivalent sector formula, rotation, and reciprocal X aperture ratio. Its two-blade denominator no longer uses an extra epsilon floor. The aperture path remains unverified against Blender images, especially the visual two-blade slit. The textures and passes are kept separate from final presentation; the existing camera gate and per-temporal-sample placement remain. QPainter uses its separate full-resolution software approximation, with aperture shape controls on a fixed 12-tap gather and a repeating 64x64 blue-noise spatial rotation. Scene-camera optics persist with project, session, and user preferences; imported Blender scene cameras populate the inputs; DOF is gated off for ordinary orbit/orthographic views. Temporal jitter texture remains unsupported.
- **Additional shadow-map behavior:** unsupported casters and nonmanifold/edited solids use the current GPU 1024-pixel depth-map fallback with 3x3 PCF. Polygon mesh fallback ranges in Texture color mode filter active Image Texture alpha using interpolated UVs and the supported Blender alpha cutoff. The GPU map fallback still leaves material-only and trimmed or untextured NURBS ranges unmasked; untrimmed textured NURBS use generated UVs and the supported cutoff. Separately, the GPU shadow-alpha path supports trimmed and untrimmed NURBS through tessellation UVs. Open NURBS surfaces with valid tessellated topology use boundary-edge forced-fail stencil volumes. In mixed-alpha scenes, opaque surfaces remain shadow receivers, and validated closed transparent solids plus supported open surfaces cast stencil shadow volumes; the depth-map fallback is limited to unsupported casters when volumes are active, and transparent triangles are drawn once without shadow sampling. Blender's handling of branched, nonmanifold casters and additional fallback fidelity remain unported. The QPainter Solid fallback does not render shadows; this CPU shadow gap is an out-of-scope approximation. The GPU and QPainter outline silhouette opacity now uses Blender Workbench's four-cardinal-neighbor resolve. For classiCAD's separate selection-color extension, both renderers use four-cardinal-neighbor adjacency; Blender itself uses one outline color and has no selected-color choice.

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
- **Ported since the initial comparison:** Blender Random color uses imported object names and library paths when available, with `ObjectId` as the fallback for objects without Blender identity metadata. Configurable theme wire/edit colors are supported. Verify both against Blender during the parity comparison.
- **Still different:** the GPU shadow path uses validated stencil volumes for supported closed `NurbsSolid` geometry and validated open `NurbsSurface` meshes; unsupported/nonmanifold casters retain the GPU depth-map fallback. The QPainter Solid fallback does not render shadows, which remains an out-of-scope approximation. Outline detection uses classiCAD's object-ID edge pass. QPainter effects remain approximate. GPU and QPainter depth of field are camera-gated approximations; both apply aperture blade/rotation/ratio shaping, with the QPainter path using coarse 12-tap spatially rotated sampling. Neither output has been visually verified against Blender; temporal jitter remains unsupported.
- **Corrected defaults:** outline, shadows, and cavity default off, matching Blender 5.2.2. Keep these defaults checked in persisted settings as well as in a fresh viewport.

#### 3. Controls and rendered output

- **Bounded lifecycle parity:** `MainWindow` retains the last completed Workbench render result, including Combined and Depth, for the application session and exposes it through File > Show Last Workbench Render Result after the result dialog closes. The dialog labels it as the last completed result and explicitly says it is not live; scene, view, and render-setting changes do not update the cached image. The cache is not serialized to the project and does not implement Blender's Render Result datablock/pass registry lifecycle. Rendering again replaces the cached result.
- **Updated in current port:** Combined images now also set per-axis pixel-density metadata from Blender's pixel aspect ratio for Qt image writers that preserve those fields (such as PNG/TIFF pHYs/resolution metadata). The image pixels are not resampled; metadata support remains writer-dependent.
- **Resolved since the initial comparison:** the viewport now has a hold-Z shading pie. Solid/Wireframe and X-Ray controls remain available through classiCAD's viewport UI, but the original claim that the inspected toolbar lacked Blender's hold-Z interaction is stale.
- **Updated in current port:** the hold-Z pie activates its hovered direction on Z release or primary mouse release. Escape, right-click, and popup dismissal cancel without changing shading; non-primary releases cannot accidentally select a wedge. It remains available only in idle Select mode and offers this app's Solid, Wireframe, X-Ray, and shading settings directions. Material Preview and Rendered are unavailable because classiCAD has no such modes.
- **Resolved since the initial comparison:** classiCAD labels its user-selected color “Single” in the shading popover, matching Blender's label; its internal enum still calls the mode `Custom`.
- **Resolved in the current port:** committed dimensions are drawn into the render result at output resolution. The grid, selection controls, HUD, gizmos, and tool previews remain editor overlays and are omitted.

### Workbench shading

- Studio lighting uses Blender's Workbench studio-light behavior and presets. MatCap uses the selected MatCap and its orientation. Flat uses an unlit flat result. Workbench does not shade objects with scene lights.
- The color modes have distinct data sources: Material reads Workbench material display values; Object reads the object's display color; Random derives a stable color from object identity; Attribute reads the chosen color attribute; Texture reads the supported image texture and UV mapping; Custom uses the user-selected color.
- Background choices, color management, object alpha, and material settings must feed the same final result as the chosen shading mode.
- Culling, X-Ray, shadows, outlines, specular highlights, cavity, curvature, and camera-gated depth of field must match Blender's enabled/disabled behavior and be composed in the correct order. Depth of field remains off for ordinary free-view and orthographic views.
- Depth of field applies when viewing through a camera with depth of field enabled. It should not blur an ordinary free viewport.
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

Status: implemented for the application's current interaction model. The GPU path uses Blender's unjittered one-sample TAA filter plus SMAA for preference value 1 and during navigation, and jittered TAA accumulation plus SMAA for the 5+ modes. The viewport exposes `setViewportPlaybackActive(bool)`, but classiCAD has no scene timeline or playback owner; wiring that hook is deferred until playback exists. Scene strokes and selected-face overlays redraw after scene resolve using the unjittered camera and regenerated scene depth; the unjittered grid follows. Pixel-for-pixel equivalence has not been verified, and QPainter remains an approximation; those checks stay in Stage 5.

- Define one classiCAD AA setting model that matches Blender's UI choices and exact sample mapping.
- Route the setting through the complete Workbench scene render and resolve path, not only the grid framebuffer.
- Implement idle accumulation, responsive navigation/playback behavior, and invalidation on the changes listed above.
- Keep the Workbench scene and its supported scene strokes coherent. Draw the grid and other viewport overlays afterward with their own unjittered camera, as Blender does. Ensure resizing and device-pixel-ratio changes rebuild the Workbench targets correctly.
- Compare GPU output and fallback behavior, and make the fallback's supported behavior explicit.

Acceptance: changing each AA option visibly changes the expected full viewport output; no fixed independent sample setting overrides it; camera movement and scene edits do not leave stale accumulation.

### Stage 2: Complete Workbench color and material inputs

Status: implemented for supported Workbench inputs. Object, Random, Material, and Custom/Single inputs reach filled NURBS surfaces/solids in both renderers and persist in document version 11. Native objects persist display name and library path; both renderers use these for Blender-style Random identity, with an ObjectId fallback for older nameless objects. The Blender scene importer reads mesh identity, object color, shared materials, face material assignments, UV maps, color attributes, active selectors, and active Image Texture Node references. Material image reference metadata persists through document save/load, including node/image names, source paths, color-space and alpha modes, and packed bytes up to 32 MiB per image. GPU and QPainter mesh display support Material, Object, Random, Custom, Attribute, and Texture. Both Texture paths sample external and packed images with perspective-correct UVs; QPainter evaluates each target sample using reciprocal camera-forward depth. Both apply Blender's PREMUL unpremultiplication and conditional 0.1 alpha cutoff; QPainter cutout interpolation and filtering may differ. Missing images or UV maps fall back to Material. GPU and QPainter NURBS Texture sample trimmed and untrimmed `NurbsSurface` geometry with exact-domain UVs from tessellation parameters. Blender's NURBS tessellation samples uniformly across the active parameter domain before normalizing grid indices, so these mappings match its legacy surface conversion. Texture-alpha cutout applies to trimmed and untrimmed NURBS in GPU color, object-ID, shadow, and QPainter color/effect paths; trim boundaries remain tessellated approximations. Solid Edit Mode strokes and Theme-mode wire strokes have distinct configurable theme colors in the shading popover, persisted with viewport update-session state; classiCAD does not import Blender user themes. Surface roughness/metallic/source-alpha values follow Blender 5.2.2 `Material::pack_data`: clamp to [0,1], square-root remap roughness, round all three to 8-bit UNorm, then apply X-Ray opacity to the packed source alpha. Preview fills also pack their default roughness 0.4 through this path, while retaining fixed 0.5 alpha. Blender's Custom source supplies RGB to `Material(float3)`, which packs alpha 1; classiCAD also forces the source opaque. classiCAD has no object/material appearance editor. NURBS transparency ordering remains approximate or unsupported.

- Blender Image Texture node interpolation and extension settings are now captured, stored per material image reference, persisted in native documents, and used by GPU/QPainter color and alpha sampling. Supported modes are Closest, Linear, Cubic/Smart and Repeat, Extend, Clip, Mirror. QPainter alpha reduces mirrored UVs modulo two before texel lookup, so Mirror works at arbitrary UV magnitudes. GPU UDIM samples now apply extension modes per tile; cross-tile filtering is not part of Blender 5.2.2's sampling path; cubic-filter details remain approximate.

- Perspective textured NURBS in the QPainter fallback shades each pixel from its perspective-correct world position, matching the GPU path's per-fragment view direction for Studio specular and MatCap shading. Untextured NURBS tessellation triangles now rasterize and shade per pixel using their flat triangle normal; perspective samples reconstruct world position, while orthographic incident direction remains constant. Untextured Workbench polygon meshes now also use per-pixel Studio/MatCap shading with perspective-correct incident direction, scene-linear color sources, and the existing corner-normal interpolation. Non-Workbench mesh drawing and over-cap fallback retain polygon fills. Visual parity remains unverified.

- Imported scene display transforms now preserve the built-in sRGB view paths and bake other supported sRGB view/look combinations through Blender's bundled PyOpenColorIO into deduplicated 65³ float LUTs. The import bridge validates SHA-256, float finiteness, exact byte length, dimensions/domain/encoding, and limits the document to eight LUTs / 32 MiB. Native documents use version 15 and remain compatible with versions 1–14. CPU and GPU rendering sample the same red-fastest LUT layout; exposure precedes it and gamma follows it. The fixed [0, 2] input domain clamps HDR values, custom OCIO files must be available during import, other display devices remain unsupported, and a Blender import/render comparison is still needed.

- Keep the implemented Material, Object, Random, and Custom sources connected to filled surfaces/solids in both renderers. Keep Edit Mode strokes on their separate configurable `TH_WIRE_EDIT`-style setting and Theme-mode wire strokes on a separate configurable `TH_WIRE`-style setting, distinct from Workbench fill colors. Preserve selection, previews, and non-Theme wire colors. Preserve appearance values loaded from document data through the existing scene-object fields.
- Preserve the implemented Attribute source and Material fallback for missing attributes. Keep Texture's Material fallback when no active image, active UV map, or supported external image is available. Workbench does not evaluate arbitrary shader graphs; future Shader-mode graph evaluation is separate from this port.
- Preserve Blender object-name/library identity for Random colors; classiCAD stores those names and paths, with ObjectId fallback for older objects.
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
- Keep the GPU depth-map fallback for unsupported/nonmanifold topology, nonplanar materialized solid faces, missing stencil buffers, and ranges that cannot use the volume path. The fallback rasterizes selected triangle ranges into a fixed 1024×1024 light-depth map. In Texture mode, polygon mesh casters with active UVs and per-face images filter alpha using the supported Blender cutoff; material fallback ranges and trimmed or untextured NURBS remain unmasked; untrimmed textured NURBS use generated UVs and the same cutoff. This restriction describes only the GPU depth-map fallback: the GPU stencil shadow-alpha path supports trimmed NURBS using tessellation UVs. The QPainter Solid fallback does not render shadows; implementing CPU shadows remains out of scope for that fallback. Hard shadowing, map resolution, and remaining caster cases differ from Blender's ray-traced shadow option. In mixed-alpha scenes, opaque surfaces remain receivers and validated closed solids/open surfaces can cast volumes; transparent triangles draw afterward without shadow sampling. Branched/nonmanifold caster dispatch and Blender's ray-traced shadow option remain unsupported.
- Map the stored scene light direction through Blender's Y/Z swap and X/Z sign change. Clamp Shift to Blender's `[0,1]` range and compute `focus = clamp(shadow_focus, 0.0001, 0.99999)`, `shadow_focus_edge = 1 - focus * (1 - shadow_shift)`, then apply `smoothstep(shadow_shift, shadow_focus_edge, -dot(N, light_direction_view))`; shadow intensity mixes the result with the unshadowed contribution. Keep the fallback depth bias fixed so Shift no longer changes map bias.
- Keep depth of field camera-gated: enable it only when a scene camera with DOF active is in use. Validate GPU and QPainter aperture blade/rotation/ratio shaping against Blender; investigate Blender's temporal jitter only if a temporal sample lifecycle is added.
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
| TAA/SMAA resolve | Blender uses a centered jitter sequence, a 3x3 Blackman-Harris filter in log color, accumulated weights, and an SMAA resolve that fades as temporal samples accumulate. | The GPU implementation follows those algorithms closely. The QPainter fallback instead supersamples and downsamples; it does not run the GPU TAA/SMAA sequence. | Do not replace the GPU AA code based on the supplied screenshots. First compare matching GPU renders at AA Off, Single Pass, and 8 samples; inspect scene-linear output before display conversion. Treat the QPainter result as a separate approximation. |
| Transparency/depth | Blender has separate transparent accumulation and transparent-depth passes following opaque drawing. `OBJECT_IN_FRONT` uses a second depth domain and merges into scene depth before regular transparent depth excludes its pixels. | The primary GPU shared-target path uses independent regular/front weighted-OIT targets, resolves front color after regular OIT, then merges front transparent depth and IDs before regular transparent depth. The no-shared-target fallback retains sorted/regular behavior. QPainter has no separate in-front domain. | GPU opaque and transparent in-front domains are implemented with named pass/attachment reporting; Blender's transparent depth pass does not write normals, so the earlier opaque front normal remains. Matched GPU image comparison is pending; fallback and QPainter paths remain different. |
| Outline and effects | Blender runs outline from its object-ID buffer, then DOF, then AA. Cavity/curvature uses Workbench-specific sample buffers and effect composition. | classiCAD generates normal/depth/object-ID inputs in its own prepass and uses custom outline/effect shaders. QPainter has separate software approximations. | Port and attribute the standalone shader/effect logic where inputs match. Preserve CAD-specific selection and NURBS tessellation handling. Validate each effect independently before combining them. |
| Geometry and materials | Workbench consumes Blender evaluated object batches and Workbench material records; its render engine is integrated with Blender scene, dependency-graph, draw-manager, GPU, and render systems. | classiCAD sends derived tessellation of exact NURBS surfaces/solids to its GPU renderer. The surface renderer has its own material packing and draw ranges. | Keep NURBS authoritative and pass only derived display data to the renderer. A future shader system should provide evaluated Workbench inputs; do not create a second Blender-shaped scene/geometry model. |

The previously supplied cube screenshots are not a controlled edge-quality comparison: the recorded views differ (`Ortho` versus `User Perspective`), the visible geometry differs, and the output dimensions differ. The source audit found no clear defect in the GPU TAA/SMAA equations that explains the soft edge. The cause remains unverified until a same-geometry, same-camera, same-size comparison separates rasterization, AA, and display conversion.

Phase Two category audit: Blender 5.2.2 Workbench UDIM lookup in [`workbench_image_lib.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_image_lib.glsl#L348-L412) floors the UV to one tile, looks up that tile's array layer and coordinate scale/offset, and samples the resulting array coordinate. Filtering does not address neighboring layers. The classiCAD GPU samplers fetch taps from the selected layer and clamp/wrap/clip/mirror against that tile's dimensions; QPainter color/effect-alpha likewise resolve one tile before filtering. Therefore, cross-tile filter continuity is not a Blender 5.2.2 parity gap; retain the existing per-tile edge behavior and preserve magenta/opaque results for absent base tiles. Blender's outline shader [`workbench_effect_outline_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_effect_outline_frag.glsl#L10-L24) compares the center object ID with exactly four cardinal samples and applies one outline color. classiCAD's GPU and QPainter paths now use four-cardinal-neighbor adjacency for their additional selected-color behavior; this is backend consistency, not a Blender feature.

Phase Two Category 8 source audit: the GPU TAA/SMAA weights, log-space accumulation, SMAA edge/area/search stages, temporal mix, and resolve follow Blender 5.2.2 closely, so no duplicate shader rewrite was made. QPainter still uses supersampling rather than Workbench temporal accumulation. Custom display LUTs now carry their domain and shaper through extraction, import, native serialization, CPU rendering, GPU surface/transparency rendering, and AA presentation; the 65³ log-shaped [0, 16] table still needs a matched output/precision comparison. Keep the QPainter and display-transform differences explicit until those comparisons are available.

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
Committed CAD dimensions remain post-render, screen-space annotations in
Combined; their fixed-pixel labels/arrows have no corresponding Z samples.
Offscreen rendering accumulates the imported Blender `scene.display.render_aa`
mode before capturing Combined and depth. Older imports without that field use
classiCAD's viewport AA preference. Blender's render AA enum is `OFF`, `FXAA`,
or 5/8/11/16/32 samples, as declared in the pinned
[SceneDisplay API](https://docs.blender.org/api/5.0/bpy.types.SceneDisplay.html).

### Implementation plan

1. **Establish a comparable baseline.** Use the same hard-edged cube, viewport pixel dimensions, projection and camera pose, clipping range, color transform, and selection/overlay state in Blender 5.2.2 and classiCAD. Capture classiCAD's native GPU path separately from QPainter. Compare AA Off, Single Pass, and 8 samples, both idle and while navigating. Save pre-AA, post-AA scene-linear, and final display-transformed outputs so edge softness can be attributed to a specific stage. Do not call the AA port complete until the edge profiles and remaining differences are recorded.
2. **Make pass boundaries explicit.** Build a classiCAD render packet from `ViewportRenderFrame` and its derived geometry/material ranges, then move pass sequencing out of `viewport_widget.cpp` into a renderer coordinator. Give color, depth, normal, object-ID, and transparency attachments explicit owners and lifetimes. Use the same GPU scene-pass sequence for the interactive viewport and Workbench render result; keep CAD editing overlays and the grid in their existing post-resolve layer.
3. **Port source-level pass behavior in bounded groups.** Compare opaque/material and depth state first; then transparent accumulation/depth; then object-ID outline and cavity/curvature; then camera-gated DOF and final resolve/presentation. For each group, adapt Blender 5.2.2 source/shaders where they are separable, preserve Blender attribution/license headers, and record any input or backend adaptation. Keep the existing TAA/SMAA implementation unless the controlled comparison identifies a concrete mismatch.
4. **Use the existing Workbench material adapter seam.** The future material/shader evaluator should resolve only Workbench-supported material inputs: scene-linear base color, roughness, metallic, alpha, and the supported image-texture result plus its sampling/color-space/alpha settings. Blender 5.2.2's active image source includes Image Texture and Environment Texture nodes; classiCAD retains the active node's image record, using Extend when Environment Texture has no extension property and Repeat for the Image Texture fallback. This does not evaluate arbitrary shader-node graphs. Feed those values through `WorkbenchMaterialData` in `src/core/document/scene_object.h` and the per-slot conversion in `src/ui/viewport/viewport_render_frame.cpp`. Per-face material slot indices, active UVs, and color attributes stay in `PolygonMesh3D` (`src/core/geometry/polygon_mesh.h`) and travel through `ViewportRenderObject` (`src/ui/viewport/viewport_render_frame.h`) into `DrawRange` (`src/ui/viewport/viewport_surface_renderer.h`). Geometry normals remain sourced from evaluated/display geometry; they are not material-evaluator outputs. Blender UDIM tile numbers, external paths, and packed tile bytes survive import/native save. GPU color, effect-alpha, object-ID-alpha, and shadow-map-alpha paths sample the tiles; stencil shadow-volume casters do not sample texture alpha. QPainter color and cavity/outline effect-alpha sampling support meshes and trimmed/untrimmed NURBS. The GPU depth-map fallback's alpha sampling remains narrower than its GPU stencil-shadow alpha path; the QPainter Solid fallback does not render shadows at all. GPU per-tile Repeat, Extend, Clip, and Mirror modes are implemented. Filtering across neighboring tile seams is not part of Blender 5.2.2 behavior. The GPU object-ID pass uses shared tessellation UV data for trimmed and untrimmed NURBS ranges, so UDIM alpha masking is supported there.
5. **Keep fallback scope honest.** Preserve the QPainter path for fallback, but compare and document it separately; do not claim that supersampling is equivalent to Blender's GPU pass graph. If CPU visual parity becomes a requirement, make it a separate rendering backend project after the GPU path is stable.

### Phase Two acceptance

- A repeatable, nonblank comparison uses identical geometry, camera, viewport size, color transform, AA setting, and overlay state.
- Intermediate outputs identify whether any edge/color difference appears in geometry shading, effect composition, AA resolve, or display conversion.
- Viewport and Workbench render-result GPU paths share the same named scene passes and attachment contract.
- Each newly ported pass has a pinned Blender 5.2.2 source reference, preserved attribution, and a comparison result; existing NURBS/surface contracts remain unchanged.
- The future shader system can provide Workbench's supported material values through `WorkbenchMaterialData` and existing mesh/draw-range data without changing the render-pass API or creating a second material model; arbitrary node graphs are out of scope. Imported UDIM metadata and packed tile bytes survive native save/reopen; GPU and QPainter mesh/trimmed-NURBS/untrimmed-NURBS color and effect-alpha paths consume tiles. The GPU depth-map shadow-alpha fallback remains limited to its supported mesh and untrimmed-NURBS inputs; GPU shadow-map and object-ID alpha masking support trimmed surfaces using tessellation UVs, while stencil shadow-volume casters do not sample texture alpha. The QPainter Solid fallback has no shadow pass; CPU shadow rendering is out of scope. GPU per-tile extension modes are implemented. Cross-tile filter continuity is not part of Blender 5.2.2 behavior.
- QPainter differences remain separately described until that backend is explicitly brought into scope.

### Stage 5: Parity review

Status: in progress; the implementation and source reviews found the blockers listed above. The team has not signed off on full parity.

- The user clarified that classiCAD's current viewport should be treated as Edit Mode; its separate Object Mode is planned for later. Do not use the missing Object Mode as an explanation for base surface, background, or grid differences. The supplied screenshots still are not a controlled render comparison: classiCAD reports `Ortho`, Blender reports `User Perspective`, and the visible scene geometry differs. Blender's mesh edit edges and curve edit wires are separate Overlay-engine geometry; classiCAD's Solid-mode CAD curve strokes and `NurbsSolid` face boundaries use the configurable edit-wire theme setting, while control geometry and selection behavior remain a distinct implementation. Compare base rendering separately from mode-specific overlays, with matching camera and geometry.
- This is an adaptation of selected Blender shading equations and passes into classiCAD's renderer, not a verbatim port of Blender's complete Workbench and Overlay engines. The table above and stage notes remain the parity record.
- Review execution confirmed the installed Blender runtime is 5.2.2 LTS and the classiCAD C++ build passes. Background extraction smoke checks now pass for original and evaluated mesh modes, POLY/BEZIER/NURBS curves, NURBS surface patches, nested collection parents and visibility records, and reporting omitted loose mesh geometry. An offscreen classiCAD startup reached viewport construction, but Qt could not create an OpenGL context; this does not validate the GPU renderer. No controlled, matched Blender/classiCAD scene render was produced in this review, so the visual comparison matrix below remains open and full parity is not signed off.

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
