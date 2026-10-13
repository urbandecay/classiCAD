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

### GPU opaque material G-buffer and initial deferred resolve (2026-10-13)

- The shared GPU sample now writes opaque Workbench base color and Blender's
  mode-dependent material payload into an `RGBA16F` material attachment during
  the same depth-tested draw as opaque color. Studio stores Blender's
  roughness/metallic bit-pair payload; MatCap stores face orientation. The
  in-front opaque domain has its own material attachment and merges it with
  the main sample alongside color, depth, normals, and IDs.
- A fullscreen resolve now reads depth and material data to produce Flat
  lighting from the G-buffer before transparency accumulation. It is enabled
  only for scene-linear Flat renders with shadows, cavity, X-Ray, preview, and
  selection tint inactive. Other settings retain the existing forward shader
  until their deferred resolve inputs and effects are ported.
- This is the first working material G-buffer writer and resolve path. Studio
  and MatCap deferred lighting, shared normals in scenes that do not request
  effects, G-buffer based cavity/shadow integration, and matched runtime image
  comparisons remain open. Blender's reference opaque pass uses the material
  and normal attachments in
  [`workbench_mesh_passes.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_mesh_passes.cc#L98-L198);
  its mode-dependent material payload and resolve are in
  [`workbench_common.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_common.bsl.hh#L368-L456)
  and
  [`workbench_composite.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_composite.bsl.hh#L558-L620).

**Studio G-buffer resolve update (2026-10-13):** The fullscreen material
resolve now also consumes the shared view-normal attachment and evaluates the
Blender 5.2.2 Studio diffuse/specular lighting from the packed roughness and
metallic payload. Perspective views reconstruct world position from depth and
the inverse view-projection matrix to derive the incident direction;
orthographic views use Blender's constant view direction. Light directions,
wrap, ambient, specular toggle, rotation, and world-space mode come from the
existing Workbench Studio-light adapter. The shared normal prepass is requested
for eligible scene-linear Studio draws, including draws with shadows using the
shared per-sample shadow factor. Studio remains on the forward path when
X-Ray, preview, or cavity diagnostic output is active. The C++ build passes;
shader runtime and matched Blender image
comparison remain open because this environment has not provided a working
OpenGL comparison context.

**MatCap G-buffer resolve update (2026-10-13):** The same eligible deferred
resolve now handles MatCap using the existing Blender-derived MatCap UV
mapping and two-layer linear half-float texture: diffuse multiplies the
scene-linear material color, while specular is added when enabled. Horizontal
flip, perspective incident reconstruction, orthographic view direction, and
the independent front-object material/normal merge are retained. If the MatCap
resource cannot be prepared, the existing forward output is left in place.
This path shares the Studio restrictions above. The C++ build passes; GPU
shader execution and matched Blender image comparison remain open.

**Deferred MatCap back-face orientation (2026-10-13):** The deferred MatCap
resolve now applies the sign stored in the material payload to the decoded
normal before computing MatCap UVs. This matches Blender 5.2.2's prepass
facing payload and composite normal flip, while leaving the payload magnitude
available for the selected marker. Cavity keeps the unflipped prepass normal,
matching Blender's independent cavity normal input. The C++ build passes; a
two-sided GPU capture and matched Blender image comparison remain open.

**Opaque cavity resolve update (2026-10-13):** Eligible scene-linear opaque
resolves now apply Workbench cavity and curvature from the shared depth,
view-normal, object-ID, and blue-noise inputs before transparent accumulation.
The resolve uses the same per-AA-sample cavity sequence and settings as the
forward shader. If the shared prepass or required SSAO depth snapshot is
unavailable, it keeps the forward color result. X-Ray, previews, and cavity
diagnostic output still use the forward route. The C++ build passes; the GLSL
path has not yet been executed in a working GL context, so matched output
validation remains open.

**Deferred selection-tint update (2026-10-13):** Selected opaque ranges can now
use the material resolve. The G-buffer payload preserves Blender's Flat,
Studio roughness/metallic, and MatCap face-orientation data while carrying a
selected marker in otherwise unused payload values. The resolve applies the
same orange theme tint and 51/255 mix after lighting and cavity. This also
survives the existing `showInFront` material-data merge. Preview and selection
overlay draws remain on their existing path. The C++ build passes; GPU
execution and matched visual comparison remain open.

**Opaque shadow-mask resolve update (2026-10-13):** The shared scene sample now
owns an `R16F` shadow-factor attachment, with a separate attachment for the
`showInFront` domain. The existing opaque shader writes the exact multiplier
it already applies, including stencil-volume visibility, depth-map PCF,
shadow focus/offset, and intensity. The front factor merges with front color,
material, depth, and normal data; the deferred resolve applies it after cavity
and before selected tint. The mask clears to neutral visibility 1. Transparent
objects keep their existing direct shading path. The C++ build passes; GL
execution and matched Blender output comparison remain open.

**Deferred resolve shader scope correction (2026-10-13):** A source audit found
that the embedded GLSL referenced `selected`, `normal`, and `incident` outside
the blocks where they had been declared. These values now have function scope;
the view normal is fetched only for Studio, MatCap, or cavity, and the
perspective incident direction is reconstructed only for Studio or MatCap.
The C++ build passes, but this environment has no GLSL validator or working GL
context, so shader compilation and execution remain unverified.

**Translucent tool-preview dispatch correction (2026-10-13):** Solid preview
fills now enter the transparency dispatch even though their source geometry is
excluded from the ordinary scene opaque/alpha classification. This preserves
their fixed 0.5 preview alpha. The sorted fallback also keeps preview fills
depth-disabled, matching the preview draw state, so source-surface depth does
not hide the preview. The C++ build passes; a runtime preview comparison
remains open. Tool previews are editor feedback and are not Blender Workbench
scene passes.

**In-front transparency fallback ordering (2026-10-13):** In-front transparent
ranges now remain separate if the dedicated front weighted-OIT framebuffer
cannot be created. Regular transparent geometry resolves first; the fallback
then blends in-front ranges back-to-front over that result. This preserves
Blender's regular-then-in-front domain order, while explicitly retaining the
fallback limitation that sorted blending cannot reproduce per-pixel depth
ordering among overlapping in-front surfaces. When the front depth attachment
is available, the fallback now draws through a scratch framebuffer using that
depth, so front opaque geometry still occludes transparent front geometry
without allowing ordinary scene depth to hide it. If that scratch framebuffer
cannot be formed, the final fallback keeps front-last sorted blending. Blender
5.2.2 routes
`showInFront` through a separate transparent accumulation/depth pass and
resolves it after regular transparency (`workbench_engine.cc` and
`workbench_mesh_passes.cc`). The C++ build passes; allocation-failure runtime
behavior and matched image comparison remain open.

**Render-result stroke sample timing (2026-10-13):** Committed CAD scene strokes
now render inside every Workbench scene sample for PNG/TIFF as well as HDR and
EXR. The post-resolve callback is depth-only for every format, preventing
double color drawing while preserving the exported depth pass. This matches
the shared AA stage used for Blender Workbench geometry; dimensions remain
post-resolve annotations. The C++ build passes; matched curve-edge captures
across output formats remain open.

**Transparent render-result readback format (2026-10-13):** Offscreen GPU
readback now tags film-transparent and missing-World-background output as
`QImage::Format_RGBA64_Premultiplied`, matching the display transform's
premultiplied framebuffer values. Opaque-background output remains straight
alpha. QPainter annotation composition and later conversion to straight-alpha
PNG/TIFF writer formats can therefore interpret the captured pixels
consistently. The C++ build passes; a semitransparent-pixel annotation capture
and saved-file comparison remain open.

### QPainter perspective cavity footprint (2026-10-13)

- The QPainter SSAO sample step now scales per receiver in perspective views by
  `viewTargetDepth / receiverDepth`, using the viewport camera position,
  direction, and target. The existing view-scale-derived radius clamp
  (`1..64` pixels), minimum step, sampling count, attenuation, and curvature
  pass are unchanged. Orthographic SSAO keeps its prior constant footprint.
- This is scoped to the QPainter cavity approximation. It does not change the
  GPU cavity shader, radius/depth attenuation, background reconstruction,
  curvature sampling, or claim visual parity with Blender; a matched visual
  comparison remains pending.

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
  output comparison remains pending. The offscreen GPU contract now uses a
  known opaque background, confirms that coverage remains opaque with DOF on,
  checks that neighboring high-contrast surface colors smooth, and verifies
  that disabling camera DOF returns the baseline image. A diagnostic Blender
  5.2.2 Workbench cube capture also changed the backdrop with DOF enabled, so
  the exact background behavior still needs a matched capture comparison.
  The separate QPainter path retains its lower-resolution sampling and its own
  visual-comparison gap.

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
- Polygon aperture taps now derive their blade sector from the unrotated sample
  angle, then apply aperture rotation once when mapping the shaped radius to
  screen offsets. A focused contract checks the expected rotated four-blade
  tap. This follows Blender 5.2.2's `DofPass::setup_samples`, which shapes
  radius before adding `rotation_` ([`workbench_effect_dof.cc:71-79`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_effect_dof.cc#L71-L79)).
  The QPainter path remains a fixed 12-tap approximation rather than Blender's
  multi-pass half-resolution gather; temporal jitter, depth-aware resolve, and
  matched visual comparison remain open.

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

### Workbench Image Texture source color spaces (2026-10-13)

- Remaining mismatch: the GPU and QPainter paths currently reduce an image's
  declared color space to an sRGB-versus-linear flag. That covers the common
  sRGB, Linear, and Non-Color inputs, but not arbitrary OCIO color spaces such
  as Display P3 or Adobe RGB. Blender 5.2.2's
  [`image_gpu.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/blenkernel/intern/image_gpu.cc#L821-L844)
  converts byte images from their declared color space through
  `IMB_colormanagement_imbuf_to_byte_texture` or
  `IMB_colormanagement_imbuf_to_float_texture`. classiCAD's Blender import and
  native document serializer preserve the color-space name, but
  `viewport_render_frame.cpp` and the surface adapter reduce it to
  `textureImageSrgb`; the GPU upload then chooses `GL_SRGB8_ALPHA8` or
  `GL_RGBA8`, and QPainter either applies the sRGB transfer curve or treats
  input bytes as scene-linear. The source-space name is therefore lost before
  either renderer can convert non-sRGB primaries/transfers.
- A bounded conversion is not available in the current dependency set:
  CMake links no OpenColorIO library, while the Blender extractor's Python
  OCIO use is limited to baking display transforms, not converting image
  samples. Qt's optional `QColorSpace` conversion would not guarantee the
  same OCIO config, named spaces, or transfer behavior as Blender and would
  also require a new conversion path shared by packed/external UDIM and regular
  images, GPU uploads, and QPainter sampling. Alpha remains the original
  channel used by Workbench cutout rules; RGB conversion must preserve the
  image's alpha association for PREMUL images. Preserve the original name and
  bytes for now. Correct support should be handled as a separate OCIO-backed
  feature that transports a validated source-to-scene transform through both
  color renderers, with offscreen contracts for regular and tiled images.
  Existing sRGB, Linear, and Non-Color behavior remains supported; other
  declared spaces are unsupported and can render with incorrect color.

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
  captured from the final post-DOF scene-linear texture. Committed CAD curve
  strokes are drawn into each scene-linear AA sample with sRGB-to-linear color
  conversion and depth writes disabled; transient editor overlays remain in
  the display-only pass. Committed CAD dimensions are rasterized to a
  transparent annotation layer and alpha-composited into both display Combined
  and scene-linear Combined, converting their sRGB annotation colors to linear
  before blending. The additional stroke draw leaves the depth attachment
  unchanged. When OpenEXR is found by CMake, OPEN_EXR writes the complete
  scene-linear Combined channels directly from the float payload without
  routing through QImage.
- The offscreen render contract now renders a real linear dimension into a
  transparent QPainter annotation layer, composites it through the production
  helper into both display and scene-linear Combined, and saves the resulting
  scene-linear buffer through the HDR Combined writer. The OpenEXR writer check
  runs when the optional OpenEXR library is available. GPU checks draw a
  committed curve stroke into an RGBA16F target using the production
  scene-linear stroke shader, verify that color drawing does not write depth,
  then use the production color-masked depth replay and verify the depth value
  changes while the color remains unchanged. A display-target check contrasts
  one translucent curve draw with two draws to detect duplicate composition.
  The focused viewport_render_contracts check passes without opening the
  application window; matched Blender image comparison remains pending.
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
  alpha. Native documents write version 21; older documents retain their
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
- The later 2026-10-13 implementation now folds cavity into retained opaque
  color before transparency composition. Weighted OIT does this in scene-linear
  RGB; the bounded sorted fallback does it in its display-RGB opaque layer.
  This section is retained as history only; current behavior and its remaining
  display-space limitation are recorded under
  [QPainter cavity and outline order](#qpainter-cavity-and-outline-order-2026-10-13).

### QPainter scene-linear surface resolve (2026-10-12)

- **Stage 1 implemented (2026-10-13):** added the standalone CPU module
  `viewport_raster_workbench_linear.{h,cpp}` with a premultiplied scene-linear
  RGBA pixel and depth-ordered straight-color fragment resolver. Its focused
  `workbench_scene_linear_resolve_contracts` test checks far-to-near source-over,
  transparent-film alpha, preservation above 1.0, and Standard sRGB conversion
  after composition versus per-fragment conversion before composition. The
  resolver is not yet called by the viewport; this stage changes no output.
- **Blender OIT contract implemented (2026-10-13):** the same module now also
  exposes Blender Workbench 5.2.2's Eq. 7 depth weight, weighted RGB/alpha
  accumulation, multiplicative revealage, and resolve over a scene-linear
  base. The contract checks the weight clamps, expected composite, order
  independence, and invalid-input behavior. The ordinary QPainter viewport
  now feeds its mesh and NURBS surface fragments through this resolver, using
  perspective eye depth or Blender's orthographic near-plane depth offset. The
  native-renderer failure fallback remains on its old path because it runs
  after the GPU scene has been display transformed.
- **Remaining blocker:** the QPainter Workbench path now retains scene-linear
  shaded fragments for transparent OIT, but opaque fills and the scene
  background still use display-referred `QImage` layers. It transforms the
  weighted transparent surface color before blending it over those layers, so
  this does not yet match Blender's one transform after full scene
  composition. QPainter cavity/outline also remain byte-oriented. Merely
  changing the intermediate `QImage` format to RGBA64 or RGBA32F is
  insufficient: the existing QColor path does not preserve HDR values or
  guarantee scene-linear blending.
- **Next implementation contract:** route opaque mesh and NURBS colors plus
  the scene-linear background into the same per-pixel float target as the
  transparent fragments. Preserve nearest-depth visibility, HDR values, and
  the Workbench weighted OIT output. Apply cavity/outline in float space, then
  apply the selected display transform exactly once to the completed scene
  layer. Keep grid, selection, annotation, and editor overlays in their
  existing later display pass. For transparent film, retain linear Combined
  RGB and alpha independently from the display preview so later background
  composition does not blend already transformed translucent colors.
- **Production-path regression status:** the QPainter overlap capture now uses
  the scene-linear weighted resolver. Against the Blender 5.2.2 top-down
  reference, overlap-region MAE improved from 29.77 to 18.00/255 (p95 29/255);
  the center pixel moved from `[152,109,103]` to `[150,118,143]`, versus
  Blender `[169,112,172]`. The standalone resolver test covers the exact
  weighted formula, order independence, revealage, and HDR preservation. Add a
  float-base viewport regression when opaque/background color moves into that
  target. Keep ordinary Solid render dither at zero and check the result
  differs from transforming each fragment before blending.
- **Matched QPainter/Blender overlap capture (2026-10-13):** the headless
  viewport contract now renders the same two translucent quads separately and
  overlapped, and the Blender 5.2.2 reference script accepts an explicit camera
  direction so both captures use a top-down orthographic view. With Flat
  lighting, Standard transform, AA off, and zero dither, the QPainter output
  differs from Blender by full-frame MAE 12.67/255 and overlap-region MAE
  29.77/255 (p95 69/255). At the shared overlap center, classiCAD is
  `[152,109,103]` and Blender is `[169,112,172]`. This confirms a real
  transparent composition mismatch in the controlled fixture; the comparison
  is diagnostic and does not yet exercise the new float OIT resolver.

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
  show-in-front ranges in their own depth domain and merges their actual device
  depth and metadata into shared scene attachments. The AA coordinator applies
  Blender's separate near-plane override only after scene effects, immediately
  before AA/presentation. The unjittered viewport depth path also has a
  dedicated in-front depth target for scene occlusion and picking. This keeps
  physical depth available to transparency, volume, outline, and DOF while
  preserving final near-plane depth for editor overlays and render-result Z.
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
  depth/object/outline IDs into the front attachments and merges sampled front
  depth into the shared depth/ID targets before regular transparent depth. The
  regular depth pass discards front-owned coverage. Blender then applies its
  overlay-only near-plane depth override after scene effects, immediately
  before AA, so this early merge retains physical depth. This follows
  Blender's regular OIT, front OIT, front transparent-depth merge, regular
  transparent-depth, volume, outline, DOF, overlay-depth, then AA order. Front
  transparent depth deliberately preserves the normal attachment; Blender's
  transparent-depth pass does not write normals, and front opaque geometry
  owns its normal/material attachments.
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
  A matched 640x480 Blender 5.2.2 capture found that both camera DOF gates
  must be enabled (`scene.display.shading.use_dof` and
  `camera.data.dof.use_dof`) for Workbench's render path to run. With the same
  25 mm lens, 36 mm horizontal sensor, focus distance, clip range, f/0.1,
  six-blade aperture, and cube scene, classiCAD and Blender both preserve the
  opaque gray background and soften 521/524 versus 522/524 qualifying high
  contrast adjacent edge pairs. The baseline silhouette masks match exactly
  (IoU 1.0); the DOF output differs by mean absolute RGB channel delta 0.78
  across classiCAD's frame versus 0.81 for Blender's own DOF/baseline delta.
  This validates spatial blur and background coverage, while exact bokeh
  parity remains open.
  The remaining zero-CoC boundary mismatch was corrected in both prefilter
  stages: pinned prepare computes the far sign from `-cocs_near`, so its
  `step(0, coc)` includes zero for both near and far; pinned downsample does
  the same for the separately encoded nonnegative near/far channels. The GPU
  shader now uses `c >= 0` / `c <= 0` in prepare and `channel >= 0` in
  downsample. The existing focused offscreen capture exercises the focus
  boundary and still checks edge blur, opacity, and camera-DOF gating; an
  isolated zero-CoC pixel contract is not exposed by the current fixture.
  Pinned source: [`workbench_effect_dof_blur1_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_effect_dof_blur1_frag.glsl#L20-L31), with the texture populated in [`workbench_resources.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_resources.cc#L84-L100). The QPainter fallback remains a fixed 12-tap full-resolution gather and has no median pass. Its aperture shaping now uses Blender's `blades > 1` condition and polygon/rotation/reciprocal-X-ratio mapping, including the two-blade slit. Image comparison remains open. Pinned gather setup: [`workbench_effect_dof.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_effect_dof.cc#L30-L75); the GPU stages are defined by the adjacent pinned Workbench DOF shaders.

- The matched capture exposed two source-level issues. CoC reconstruction was
  using the stored clip end even when classiCAD had tightened the actual
  perspective projection range; `setDepthOfField` now receives that exact
  projection range from `viewportProjectionClipRange`, keeping the depth
  buffer decode aligned with the scene matrix. The half-resolution color and
  CoC textures allocate three mip levels, so their GL mipmapped samplers now
  set `GL_TEXTURE_MAX_LEVEL` to 2. Without that limit OpenGL treated the
  textures as incomplete and stage-one `texelFetch` returned zero colors and
  CoCs, creating the black wedges and background. Blender's pinned
  `DofPass::init` creates these textures with exactly three levels and
  `DofPass::sync` samples their mip views. The focused offscreen contract and
  matched capture pass with both corrections.

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

### GPU Workbench DOF temporal jitter sequence (2026-10-13)

- Audited the existing GPU gather against Blender 5.2.2. The implementation
  already uses the pinned `get_random_vector()` equation: blue-noise channel 1
  supplies the angle, interleaved gradient noise supplies the square-root
  radius, and `sampleIndex / sampleCount` offsets both sequences. The sample
  vector translates the gather origin only; it does not rotate the fixed DOF
  kernel. The shader now documents those source-level details, and the sample
  offset is centralized in `workbenchDofNoiseOffset` so GPU temporal sample
  indexing has focused C++ contract coverage. The contract also checks a
  deterministic vector against the pinned shader equation.
- References: Blender 5.2.2
  [`DofPass::setup_samples` and `init`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_effect_dof.cc#L53-L141),
  [`get_random_vector()`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_effect_dof_blur1_frag.glsl#L370-L415),
  and [`SceneResources::load_jitter_tx`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_resources.cc#L74-L98).
- The source audit confirms the GPU temporal-offset/random-vector sequence
  already matched Blender; this update centralizes and contract-tests the
  sample-index mapping rather than changing shader behavior. The earlier
  summary's “Temporal jitter texture remains unsupported” is stale. The
  QPainter DOF approximation remains distinct. The full C++ build and focused
  `workbench_dof_jitter_contracts` CTest pass; matched per-sample GPU output
  remains unverified.

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
| Color management | Imported scenes retain display device, view transform, look, exposure, and gamma for Workbench render results. The interactive Solid/Wireframe GPU viewport uses Blender Workbench's default view transform; Blender applies scene render color settings there only for Rendered mode or when View3D Scene Lights/Scene World is enabled. classiCAD does not currently expose those View3D modes. Live GPU rendering keeps scene surfaces, transparency, pictures, and backgrounds in RGBA16F scene-linear color through depth of field and temporal AA, then applies exposure, the active sRGB AgX/Standard/PBR Neutral/Filmic or imported OCIO LUT, and gamma once during presentation. Offscreen Workbench results now run the configured viewport AA sample mode through the same scene-linear resolve and retain the resolved float Combined payload for Radiance HDR and OpenEXR when available. Other non-EXR display-referred output uses RGBA16/QImage, so HDR range is lost before 16-bit PNG/TIFF writing. QPainter Solid rendering retains its CPU display-transform path. Other sRGB view/look combinations use validated Blender-baked LUTs shared by scenes with identical table bytes. AgX uses a 97³ float LUT with an analytic `log2_1p` shaper over [0, 16] (scale 64); PBR Neutral, Filmic, and imported custom LUTs use 65³ tables. Older native LUTs retain their linear [0, 2] metadata. Inputs outside each stored domain clamp to its endpoints. Standard, AgX, exposure, and gamma transforms are checked against Blender 5.2.2; gamma zero is preserved and follows Blender's `1 / FLT_EPSILON` behavior. | Partial; render-specific sample-count mapping, non-sRGB display devices, arbitrary OCIO configuration transfer, imported-LUT GPU precision, GPU/QPainter differences, HDR-limited non-EXR output, and broader matched render comparisons remain open |
| Surface drawing | NURBS surfaces and solids are tessellated for display and shaded by classiCAD's custom forward GLSL renderer. Curves use the stroke path. | Different rendering architecture; visual parity remains unverified |
| Color source | Object, Random, Material, and Custom color plus their Blender-compatible alpha sources reach filled NURBS surfaces/solids in GPU and QPainter paths. When no Workbench material is assigned, Material/Attribute/Texture use Blender's opaque default gray material; Object and Random use object display color and alpha. Blender mesh import preserves object display colors, shared materials, per-face slots, active UV maps, and image references. GPU mesh Texture mode samples the active Image Texture Node per fragment through interpolated active-UV coordinates, with repeat wrapping and linear filtering; supported external raster files are loaded with size limits and cached by path/color space. QPainter mesh Texture mode rasterizes each triangle per target sample, uses reciprocal camera-forward depth for perspective-correct UVs, and bilinearly samples external and packed images with repeat wrapping. An audit found the reciprocal weight used the camera-to-scene direction with the wrong sign, collapsing varying positive depths to the clamp; mesh and NURBS raster paths now use positive camera-forward depth. UDIM tiles now preserve Blender 5.2.2 per-tile packed bytes in native documents and sample them in GPU and QPainter mesh and NURBS paths before falling back to external tile paths; import caps packed payloads at 32 MiB total per image, and absent tiles remain magenta. GPU and QPainter NURBS Texture sample trimmed and untrimmed surfaces using tessellation parameter UVs normalized over the exact knot domains. This matches Blender 5.2.2's index-normalized UVs because Blender samples uniformly over each active parameter domain. QPainter mesh and NURBS Texture honor the imported image sRGB flag: unpremultiply RGB as before, decode sRGB samples to scene-linear when requested, multiply with the material base color before display conversion, and apply the active exposure/gamma/OCIO or built-in transform. QPainter textured-mesh shading reconstructs its incident direction and normal per raster pixel: perspective samples use perspective-correct position/normal interpolation and orthographic samples use barycentric normal interpolation; corner normals are transformed by the world inverse transpose, with a flat geometric normal as fallback for invalid or absent corner data. QPainter untextured Workbench mesh triangles now rasterize per pixel using scene-linear material/object/random/custom/attribute colors, per-face roughness and metallic, interpolated corner normals, and perspective-correct incident direction. Random and Custom inputs use Blender-compatible color conversion; Attribute keeps the existing triangle-averaged color and decodes it only when its metadata says sRGB. Raster size limits retain the existing flat polygon fill for oversized untextured triangles. Orthographic incident direction remains constant. QPainter untextured NURBS tessellation triangles are also rasterized and shaded per pixel using their flat triangle normal; perspective samples reconstruct their world position, while orthographic incident direction remains constant. Textured NURBS samples continue through the per-pixel material lighting path. GPU and QPainter texture-alpha cutouts apply in color rendering for trimmed and untrimmed NURBS. GPU object-ID/outline and shadow-alpha paths, and QPainter cavity/outline effect-alpha paths, also cover trimmed and untrimmed tessellated NURBS using active-domain UVs and Blender's alpha rules. The alpha cutoff is 0.1 when enabled and is bypassed for NONE/Ignore and CHANNEL_PACKED. Packed Image Texture bytes up to 32 MiB are transported, persisted, and sampled by both paths. Missing images or UVs fall back to Material. Both renderers unpremultiply PREMUL images except at alpha 0 or 1. Blender Workbench's pinned [`workbench_image_lib.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_image_lib.glsl#L34-L60) performs UDIM lookup or regular image sampling, unpremultiplies PREMUL pixels, and discards fragments below `image_transparency_cutoff`; Workbench uses supported material display values and its active Image Texture input rather than evaluating arbitrary shader graphs. | Partial; color/effect alpha coverage is implemented for the described passes, while QPainter composition, trim tessellation, and final visual comparisons remain approximate or unverified |
| Blender scene import | File > Import > Blender Scene invokes the pinned Blender 5.2.2 extractor and offers original or evaluated mesh mode. File > Open now routes ordinary `.blend` files through the same original-mode extractor and loads the supported result as a new document; native classiCAD `.blend` files still restore their embedded authoritative snapshot. Imported polygon meshes retain geometry, custom split normals, object/material data, direct links to multiple Blender collections, collection hierarchy and global hide flags, plus per-view-layer collection-instance paths/exclusions/visibility and object hide state. Mesh loose edges are emitted as a companion CURVE item with a deterministic derived transport identity, separate from the source mesh identity. Each edge becomes a world-space degree-one NURBS component; the companion is named from the source object and its transport record carries the source object ID, while object display color, collection memberships, visibility, and per-view-layer state. Isolated vertices remain explicitly reported as omitted because classiCAD has no Blender point-item import path. Evaluated Blender FONT objects convert to baked polygon meshes with evaluated transforms and material assignments; original text/font/layout editability is lost and reported. Evaluated Blender METABALL objects also convert to baked polygon meshes with evaluated transforms and material assignments; original mode explicitly reports them unsupported, and editable metaball data and threshold settings are not retained. Exact Blender CURVE POLY splines import as dimension-3 degree-1 NURBS after matrix_world is applied; cyclic splines explicitly close. Each cubic Blender BEZIER span imports as a separate nonrational clamped degree-3 NURBS with its four transformed endpoint/handle points; cyclic spans include last-to-first, including the one-point cyclic loop. Blender CURVE NURBS splines preserve their weights, degree, generated knot multiplicities/domain, and cyclic wraparound control points in dimension-3 NURBS. Blender SURFACE NURBS splines import as separate exact NURBS surface objects, with world-space control nets, U/V knot domains, weights, cyclic directions, per-patch identities, and assigned Workbench material values preserved. Multiple source curve splines remain components of the same imported source object. Object display name/color and collection memberships are retained. Blender parent relationships are not represented; supported child objects retain their world-space geometry/transform and now report this hierarchy loss. Bevel/extrude, bevel-object, and 2D fill surfaces are explicitly reported as omitted while exact centerlines are kept. No curve approximation is emitted. The Collections panel selects among imported Blender view layers; selected view-layer visibility resolves each object's direct collection memberships across visible paths. Linked objects combine memberships and per-view-layer hide state. The default option preserves per-scene first-layer visibility. Collections remain independent of CAD layers. Transport identity keys use a SHA-256 namespace derived from the normalized absolute source .blend path for objects, materials, collections, scenes, view layers, and their references; moving or renaming the source project changes that namespace. Native document version 15 persists Blender camera, render-output, imported-scene, and deduplicated OCIO LUT metadata and reads versions 1–14; the scene extractor bridge accepts versions 10 through 14. Ordinary `.blend` Open and File > Import > Blender Scene now apply the selected saved `VIEW_3D` area's `SpaceView3D.shading` to the interactive viewport, with a deterministic policy when several areas exist, while `scene.display.shading` remains the scene-level render setting source and `scene.display.render_aa` remains available to offscreen results. | Partial; additional object types and visual/import coverage remain gaps. Cameras, lights, and empties currently have no matching committed classiCAD scene-object geometry type; the extractor reports these objects (the active camera's supported settings are separately retained as scene metadata). In particular, an Empty that instances a collection is metadata-only: its referenced collection's objects are not expanded into imported items at the instance transform. Blender Workbench Solid uses its studio-light rig rather than scene light datablocks. Adding camera/light/empty datablocks as editable scene objects needs an explicit model/serialization/rendering contract, so they are not fabricated as points or proxy meshes. |

**Import update:** The scene extractor bridge now accepts versions 10 through 14; the native document schema writes version 21 and reads versions 1 through 20. Ordinary Blender `.blend` Open now imports the first scene's `scene.display.shading` Solid settings into the initial classiCAD viewport and preserves `scene.display.render_aa` for offscreen Workbench render results. It maps bundled studio-light/MatCap presets, color source and background, shadow/cavity/curvature/outline controls, `show_xray`, the distinct `show_xray_wireframe`, `matcap_flip_x` when Blender exposes it, and studio-light rotation converted from radians to degrees. Custom unavailable presets, scene-light rendering, studio-light background opacity/blur, and Blender 5.2.2's RNA-hidden MatCap flip are reported as unsupported. Blender 5.2.2 Workbench does not use `studiolight_intensity`. Imported World backgrounds are retained and used for both interactive and offscreen rendering. In Original mesh mode, source geometry stays unevaluated while the active camera and focus-object transforms are resolved from the scene dependency graph. Existing classiCAD project opens do not apply imported Blender shading. Ordinary `.blend` Open creates a new classiCAD document; its source path is not used as the Save target, so users must use Save As to create a native project.
| Material values | Per-object base color, roughness, metallic, and alpha are serialized and consumed by both renderers. Imported Blender meshes retain shared material records and per-face slot references; GPU and QPainter mesh paths select face material values. Native NurbsSolid objects can reference shared DocumentMaterial records by stable seed-face ID; GPU draw ranges, QPainter color, and QPainter effect alpha resolve the per-face values, with object material fallback. ClassiCAD `.blend` display proxies preserve and round-trip assignments through polygon face attributes tied to source object and semantic face IDs. Materialized edited boundary faces have no seed-face lineage, so their seed assignments are ignored. Attribute colors also reach both mesh renderers. classiCAD currently has no UI for authoring appearance values. | Partial; material packing matches Blender's `Material::pack_data`; appearance authoring and materialized-face lineage remain gaps |
**Native schema update:** Document version 21 adds `nurbsSolidFaceMaterialIds` references by stable seed-face ID and reads prior versions through 20. The Blender project adapter merges uniform proxy-polygon material assignments back to those references; mixed assignments to a single semantic face remain unchanged.
| Culling and X-Ray | GPU surface culling works; X-Ray uses translucent fill plus depth/stroke passes. GPU and QPainter polygon-mesh paths classify every finite negative mesh-transform determinant as reflected by sign, without a magnitude threshold. GPU swaps triangle winding; QPainter reverses culling sense. QPainter applies backface culling to polygon-mesh triangles using transformed world-space winding and the active view direction. Its standalone outline-ID fallback draws regular ranges first, then clears only depth and draws `showInFront` ranges over the preserved regular IDs; the ID shader retains the same texture-alpha cutoff rules. Blender 5.2.2 routes X-Ray and transparent in-front objects through dedicated accumulation/depth passes in [`workbench_engine.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_engine.cc#L238-L250) and writes in-front IDs/depth before regular transparent coverage in [`workbench_mesh_passes.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_mesh_passes.cc#L309-L368). QPainter NURBS and solid face culling now uses projected tessellated triangle winding; opaque backface normals flip to face the camera, while transparent shading keeps its source normal orientation. Blender 5.2.2 tracks inverted handedness in `ResourceID` and applies it to front-facing state; duplicate transforms set the negative-scale flag using determinant sign in [`draw_context.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/intern/draw_context.cc#L933-L946), [`draw_handle.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/intern/draw_handle.hh#L60-L69), and [`draw_command.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/intern/draw_command.cc#L176-L176). Its opaque normal prepass receives `[[front_facing]]` and flips backface normals in [`workbench_prepass.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_prepass.bsl.hh#L306-L314) and [`workbench_common_lib.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_common_lib.glsl#L28-L33); the transparent shader uses its interpolated normal directly in [`workbench_transparent_accum_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_transparent_accum_frag.glsl#L59-L64). In Solid mode, classiCAD's bounded surface-batch compositor uses per-pixel nearest opaque depth for NURBS and polygon-mesh fills and sorts precomposited transparent object fragments by their per-pixel nearest tessellation depth, with a four-million-fragment cap. If prepared geometry or either memory budget is unavailable, the fallback draws surface objects in average view-depth order and sorts polygon-mesh and NURBS-surface triangles by projected centroid. It has no general scene depth buffer for strokes or separate X-Ray depth/stroke passes. | Partial; inter-object transparent ordering improves within the bounded compositor, while precomposited intra-object self-occlusion, X-Ray, and fallback ordering still differ |
| Shadows and outlines | Validated closed opaque `NurbsSolid` and imported `PolygonMesh3D` casters use depth-pass stencil side volumes when all source and extruded vertices clear the camera near plane; otherwise closed solids, open `NurbsSurface` casters, and transparent casters use capped depth-fail stencil volumes. Opaque in-front receivers use their own depth-stencil domain. Blender 5.2.2 uses stencil shadow volumes and has no directional depth-map fallback; classiCAD retains a 1024-pixel depth-map fallback with 3x3 PCF for unsupported GPU topology. Stencil casters do not sample texture alpha; the map fallback has narrower caster coverage. QPainter now has a bounded software light-depth shadow for opaque tessellated NURBS and polygon meshes, with texture-alpha cutouts; it excludes transparent surfaces and remains an approximation. Focus/shift and light direction use Blender's formulas in the GPU path. GPU and QPainter outlines use four-cardinal object-ID neighbors; Blender uses one outline color and has no selected-color behavior. GPU object-ID/outline alpha and QPainter cavity/outline alpha cover trimmed and untrimmed NURBS texture cutouts. Matched 640×480 GPU cube captures now compare opaque shadows, WORLD cavity, and outlines: shadow and outline masks match exactly; cavity mask IoU is 0.972. Cavity/shadow/outline magnitudes, broader geometry and camera cases, combined/transparency effects, QPainter output, and the depth-map fallback remain open. | GPU stencil shadows, cavity, and outline passes are implemented; QPainter has a bounded opaque shadow fallback, while broader cases and outline-intensity attribution remain open |
| Cavity and depth of field | GPU Solid maps Blender's World type to SSAO and Screen type to normal-based curvature; Both uses both. The persisted settings expose Blender 5.2.2 defaults and ranges for SSAO distance/attenuation/sample count (`SceneDisplay`) and cavity/curvature ridge/valley factors (`View3DShading`); the renderer uses these values and distributes SSAO samples across AA iterations with Blender's 512-sample cap. Cavity jitter now recreates Blender's 64x64 blue-noise-derived RGB texture, including its clamped, total-sample-scaled radius offset. The GPU effect prepass uses opaque ranges as cavity inputs even when transparent ranges share the scene; transparent ranges are shaded without cavity. QPainter uses opaque-only cavity samples and applies the composite only where an opaque receiver is visible; cavity is skipped beneath a visible translucent surface. Its disk sampling follows Blender's deterministic radical-inverse/Hammersley-like sequence with per-pixel blue-noise rotation and sample-radius jitter. QPainter retains perspective-correct world-space surface positions from each rasterized triangle and evaluates Blender's signed displacement, length bias, distance attenuation, cavity/edge accumulation, and requested-sample normalization against those positions. Since dot products and lengths are invariant under view rotation, the world-space displacement/normal math matches those shader operations. The screen-space sample footprint remains an approximation. QPainter curvature controls now remain independent from SSAO cavity ridge/valley controls, matching Blender's separate control paths; its software curvature estimator remains an approximation. A scene-camera DOF mode stores focal length, sensor width, focus distance, f-stop, aperture blades, aperture rotation, and aperture ratio; QPainter applies DOF in both perspective and orthographic views, measuring depth relative to the perspective eye or orthographic view target. Scene extraction resolves an evaluated camera's focus-object distance along local +Z, matching Blender's camera DOF distance rule; pose-bone focus subtargets still use the numeric focus-distance setting. GPU and QPainter CoC convert lens and sensor millimeters to Blender scene units with the Workbench `0.001` scale while retaining imported focus distance and depth in scene coordinates; invalid optics leave the source image unblurred. GPU DOF uses half-resolution RGBA16F color and RG8 near/far CoC pyramids with preparation, two downsample, gather, 3x3 median, and full-resolution resolve passes before AA accumulation; viewport and offscreen render-result paths share this renderer. Blender adjusts sensor size for the camera viewport with `rv3d->viewcamtexcofac[0]`; classiCAD derives an effective fitted sensor width during camera import, and that mapping still needs visual comparison. The gather kernel accepts the three aperture shape controls, but exact aperture-sampling parity has not been visually verified. | GPU cavity sampling is source-derived; QPainter cavity remains an approximate software-buffer neighborhood pass; DOF is camera-gated in both paths. QPainter derives the physical CoC from the software depth buffer, gathers 12 same-or-behind-depth taps at full output resolution, and composites by CoC after cavity. Its orthographic and perspective paths now use the active view origin and Blender's aperture-radius parameter. This remains an approximation: it has no Blender half-resolution RGBA16F/CoC RG8 pyramid, three mip levels, temporal jitter, separate gather/blur passes, or depth-aware resolve; its depth buffer covers rasterized NURBS surfaces/solids and polygon meshes only, and transparent colors are already composited before the gather. QPainter color arithmetic is display-space and differs from Blender's linear intermediate; cavity is applied after that display transform, and opaque-only cavity is omitted under visible translucent surfaces because the color image has already composited them. GPU aperture blade/rotation/ratio shaping and the `sample / samples_len` temporal gather jitter are implemented; matched visual comparison remains open. Blender focus-object distances are captured, while pose-bone subtargets remain unsupported. |
| Anti-aliasing preference | The UI offers Off, “Single Pass Anti-Aliasing,” and 5/8/11/16/32 samples; the RNA description identifies the single-pass method as FXAA. In Blender 5.2.2 Workbench, stored value 1 means one unjittered sample with TAA color filtering and SMAA resolve; Workbench does not invoke an FXAA shader. Values 5+ accumulate jittered frames and use SMAA while settling. During navigation or playback, Workbench resets to one unjittered sample and SMAA. classiCAD implements navigation reset and exposes a playback-state setter, but has no scene timeline, playback clock, or playback frame scheduler to call it. The grid is drawn afterward with its own unjittered MSAA path. QPainter renders committed geometry into a supersampled target for non-Off choices, without temporal accumulation or SMAA resolve; its wire categories use the separate Smooth Wires preferences. | Partial; GPU path follows Workbench's AA passes, but app playback integration, output equivalence, and fallback parity remain open |
| Smooth Wires | Overlay and Edit Mode preferences are independently stored and GPU strokes have separate categories. Committed curve strokes and wireframe surface lines use Overlay; component controls/cages and Solid-mode `NurbsSolid` face-boundary strokes use Edit Mode. The separate Workbench Outline effect draws object silhouettes. QPainter applies the preferences to supported curve, surface, and control-cage categories. If native GPU stroke drawing fails, the prepared-stroke fallback reads scene depth when available, approximates hidden/visible stroke visibility with sampled depth, applies endpoint fades, and handles continuous, dashed, dotted, and custom line patterns. Its remaining gaps are approximate depth sampling/edge coverage and unoccluded visible strokes when depth readback is unavailable; direct QPainter curve fallback paths also do not share the GPU stroke pass's full depth treatment. | Partial; the GPU `NurbsSolid` boundary pass and native stroke failure fallback are present, while fallback depth sampling and some component-edge geometry remain approximate or unavailable |
| Transparency | GPU surfaces are separated into opaque and partial-alpha ranges. Opaque ranges establish depth first; the primary scene renders transparent surfaces with weighted blended OIT into RGBA16F accumulation and R16F weight targets sharing scene depth-stencil. Transparent fragments depth-test without writing depth. Their lit RGB accumulates in scene-linear space and resolves over the scene-linear opaque target before DOF/AA and the single display transform at presentation. When no shared depth texture is available, auxiliary draws use range-level back-to-front order. The surface outline pass also runs for partial-alpha content. In the ordinary QPainter renderer, NURBS and polygon-mesh fills use temporary per-object color layers and prepared tessellation depth: opaque coverage establishes nearest per-pixel depth, then partial-alpha layers in caller-supplied average-depth back-to-front order are source-over composited in front of opaque depth before one resolve and scene strokes. The same compositor is attempted when the native renderer's main Solid surface draw fails, using a DPR-aware target; the fallback is presented before the normal native overlay sequence, followed by an unjittered depth rebuild, so scene strokes, grid, selected-face highlights, previews, control points, and arc HUD remain above it. Prepared QPainter stroke fallback also follows the surface fill. It falls back to the prior sorted painter draw if prepared geometry is missing or the bounded target exceeds one million pixels. Transparent layers are not depth-sorted against each other per pixel, and triangles within each object's color layer have already been QPainter-composited, so intersecting layers and self-occlusion remain approximate. Blender uses dedicated transparent and transparent-depth passes. QPainter retains its separate display-space implementation. | GPU primary scene uses weighted OIT; QPainter has a bounded opaque-depth/object-layer compositor but differs in transparent ordering, intra-object fragment ordering, color-transform placement, and coverage for its fallback cases |
| Stroke and overlay composition | Workbench surface silhouettes are drawn by its outline pass before AA resolve. CAD curve and wire strokes, like Blender's Overlay wireframe pass (`overlay_wireframe.hh`), draw afterward using the unjittered camera and regenerated scene depth; selected-face overlays also remain post-resolve, and the grid follows. Selected faces depth-test without depth writes. Committed Solid-mode scene strokes opt into depth writes per stroke; transform previews, control guides/cages, selected component edges, and custom outlines remain no-write. X-Ray's hidden wire pass stays no-write while the visible pass honors the per-stroke flag. The grid uses its own surface-depth framebuffer and composites with depth testing disabled, so scene-stroke depth writes do not change grid occlusion. The two Smooth Wires categories remain independent. GPU CAD strokes use analytic coverage, with depth bias enabled only for depth-tested strokes whose Smooth Wires category is enabled, matching Blender's `do_smooth_lines` gating. Solid Edit Mode strokes and Theme-mode wire strokes use separately configurable theme colors, saved with viewport shading state; selection, previews, and non-Theme wire colors retain their separate paths. QPainter now composites approximate Solid surface silhouettes from its object-ID raster; it still lacks selected component edges. The native-GPU stroke failure fallback uses sampled depth, endpoint fades, and line styles, but its visibility and edge coverage remain approximate; ordinary QPainter curve paths do not share its full depth treatment. | Theme wire/edit colors are configurable for supported stroke paths; Blender theme import, unsupported Smooth Wires geometry, transparency, and other fallback differences remain |
| Shadow controls | The Shift and Focus values feed Blender's normal/light falloff calculation in the GPU renderer, including the `1 - focus * (1 - shift)` upper edge. Blender's pinned implementation is volume-only; classiCAD's GPU map fallback keeps a fixed depth bias and approximates volume occlusion for unsupported casters. The QPainter Solid fallback does not render shadows. | Partial; GPU setting semantics are adapted, unsupported casters use a classiCAD-only depth-map fallback, and QPainter shadows are out of scope |
| GPU fallback | QPainter has a separate shading implementation. Its committed scene uses increasing supersampling for non-Off AA choices, without temporal accumulation or SMAA. Smooth Wires are independent of that scene AA hint for the supported categories: Overlay controls committed curve strokes and wireframe surface lines; Edit Mode controls cage/control-polygon lines and categorized prepared fallback segments. If native GPU stroke drawing fails, the prepared-stroke fallback uses scene depth when available, sampled hidden/visible visibility, endpoint fades, and line patterns; this remains approximate, and without depth readback the visible pass is unoccluded. If the native renderer's main Solid surface draw fails, the viewport exits native painting and presents the same bounded QPainter surface compositor used by the ordinary fallback before resuming the normal native overlay sequence. Missing prepared geometry or a target over one million pixels retains the average-depth sorted painter path. The unjittered scene depth is then rebuilt, and selected-face highlights, scene strokes, grid, picture/tool previews, control points, and arc HUD draw in their regular order on top; prepared QPainter stroke fallback also runs after the fill. The fallback composites object-ID surface outlines, but cannot reproduce selected component edges. | Partial; renderer fallback differences are documented but not closed |

**Update (2026-10-12):** trimmed NURBS use tessellation UVs for GPU Texture color, object-ID/outline alpha, and shadow alpha, plus QPainter Texture color and cavity/outline alpha. Blender's filtered alpha cutoff is 0.1 when enabled; alpha-ignoring modes bypass it. QPainter effect sampling interpolates tessellated UVs perspective-correctly, normalized over each active knot domain. Trim boundaries remain tessellation approximations. This supersedes earlier limitations below that described trimmed NURBS texture or alpha coverage as unsupported. QPainter perspective correction also uses positive camera-forward reciprocal depth for mesh and NURBS rasterization; visual comparison is still open.

**Shadow fallback audit (2026-10-12):** Blender 5.2.2's `workbench_shadow.cc` and `workbench_shadow.bsl.hh` implement stencil shadow volumes with pass/fail methods; the pinned Workbench code has no depth-map fallback. classiCAD's directional depth map is therefore an explicit renderer fallback for unsupported input geometry, not a parity port. The current shadow-volume path already adapts Blender's light-facing caps, silhouette side quads, and depth-pass/depth-fail stencil choice. No safe additional Blender-derived code change was identified in this bounded fallback audit. Sources: [workbench_shadow.cc](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_shadow.cc), [workbench_shadow.bsl.hh](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_shadow.bsl.hh).

### Remaining parity gaps

- **Render result:** File → Render Workbench Result (F12) renders visible document geometry into viewport, HD, Full HD, QHD, or 4K output. Imported Blender scene resolution percentage accepts Blender's 1–32767 range and computes each output dimension with integer truncation, matching `BKE_render_resolution`; the app retains its 8192-pixel output cap. Imported render borders preserve `use_border`, `use_crop_to_border`, and normalized bottom-left border coordinates. Border endpoints truncate after scaling by the full frame dimensions, then convert Blender's bottom-left Y origin to the image's top-left rows. The renderer resolves the camera against the complete requested frame before applying the region: crop enabled crops Combined, depth, and scene-linear buffers to the selected rectangle; crop disabled retains full dimensions and clears pixels outside the rectangle, using Blender Workbench's `1e10` background depth sentinel. It uses the evaluated scene camera when available, renders fills through the shared configured resolve, applies the existing camera DOF pass, then draws CAD strokes and composites committed dimensions. It saves Combined plus view-space Depth as PFM; when OpenEXR is available, Combined EXR also contains optional `Z`, written as HALF or FLOAT to match the selected color depth. The imported pixel aspect informs camera sensor fit and shift and is recorded in image metadata. The Combined image also writes per-axis pixel-density metadata for Qt image writers that preserve it; image pixels are not resampled. The resolved active-scene Blender render filepath now seeds the Combined save dialog, with a generic `WorkbenchRender` filename when no path was imported; Blender frame tokens are retained literally and not expanded. The GPU Workbench render passes are represented by a standalone result object rather than Blender's host-registered `RenderResult` datablock. Blender 5.2.2 runtime comparison of border rounding and the non-cropped outside-region alpha is still open because the installed Snap Blender cannot start in this container (`snap-confine` lacks `cap_dac_override`). Remaining render differences are covered by the pass, camera-DOF, color-output, and visual-comparison gaps below ([pinned Blender 5.2.2 source](https://github.com/blender/blender/blob/v5.2.2/source/blender/blenkernel/intern/scene.cc#L3128-L3139)).
- **Shading shortcut:** Z opens a hold-Z radial menu in idle Select mode with Solid, Wireframe, X-Ray, and settings directions. Blender's other shading modes are absent because classiCAD has no Material Preview or Rendered modes.
- **Color inputs and material assignment:** Blender scene mesh import preserves per-face material slots, color attributes, UV maps, and image references. Material/Object/Random/Custom/Attribute mesh colors render through GPU and QPainter paths. GPU Texture mode samples the active Image Texture Node through interpolated active-UV coordinates and a per-fragment sampler; external raster files use repeat wrapping and linear filtering. Missing images and missing UV maps fall back to face Material color. QPainter Texture mode rasterizes each triangle per target sample, performs reciprocal-depth perspective correction, and bilinearly samples external and packed images with repeat wrapping. Both paths apply PREMUL handling and the 0.1 cutoff to the filtered alpha sample except for NONE (Ignore) and CHANNEL_PACKED modes. Remaining QPainter differences include display color management and raster edge coverage. Blender-style object-name/library-path Random identity is used by both renderer paths, with ObjectId fallback for legacy nameless objects. The `.blend` adapter still round-trips classiCAD's embedded document snapshot instead of arbitrary Blender datablocks.
- **Transparent sorting:** The primary GPU scene now uses weighted blended OIT, so intersecting/cyclic triangle overlap no longer depends on centroid draw order and transparent surfaces draw once per range. The auxiliary GPU path uses range-level far-to-near sorting when it has no shared depth attachment. The ordinary QPainter Solid path and native-renderer failure fallback use a bounded scene-linear weighted OIT compositor: opaque coverage uses nearest per-pixel tessellation depth, and every eligible transparent triangle sample from each object is accumulated after opaque-depth rejection. The fragment sink removes only coincident same-pixel samples with matching depth, color, and alpha to limit duplicate shared-edge coverage. Targets above one million pixels or missing prepared geometry use the existing average-depth painter fallback; fragment overflow also returns to that fallback. The QPainter implementation remains a CPU approximation and does not share the GPU's depth attachment or raster coverage.
- **QPainter transparent sorting update (2026-10-13):** The weighted OIT compositor now retains depth-separated transparent samples within one object, fixing the previous nearest-fragment-per-object collapse. A headless contract uses one mesh object with two overlapping translucent sheets at separate depths. The captured and accumulated fragment buffers each retain the four-million-sample limit; fallback behavior remains available when a limit is exceeded. The GPU path remains the source-matched implementation, while QPainter raster edge coverage and overflow fallback still differ.
- **Display transform:** imported scene display device, view transform, look, exposure, and gamma are retained for Workbench render results. The interactive Solid/Wireframe viewport keeps the default AgX settings; applying imported scene settings unconditionally there differed from Blender, where ordinary Workbench Solid uses the default view transform and only Rendered mode or View3D Scene Lights/Scene World selects scene render settings. classiCAD does not yet expose those View3D modes. Existing built-in sRGB AgX, Standard, Khronos PBR Neutral, and Filmic with the `None` look stay on their established transform paths. Other sRGB view/look combinations are baked to scene-specific 65³ RGB float LUTs by Blender's bundled PyOpenColorIO and applied in CPU/GPU rendering, with exposure before the table and gamma after it. This is an import-time snapshot of the machine's active OCIO configuration, not a runtime OCIO dependency; custom external config assets must be available to Blender during import. New LUTs sample a [0, 16] domain using an analytic `log2_1p` shaper (scale 64); older native documents preserve their linear [0, 2] metadata. Inputs outside a LUT's domain clamp to its endpoint. The document stores at most eight unique tables and 32 MiB decoded LUT data. A controlled Blender 5.2.2 PyOpenColorIO comparison of the AgX + Very High Contrast look against the 65³ log-shaped LUT sampled trilinearly at 41,000 deterministic scene-linear RGB inputs measured mean absolute channel error 0.000116, p95 0.000177, p99 0.00241, and maximum 0.0546 (0–1 display channel scale). For inputs in [0, 1]³, mean error was 0.000194 and p95 0.000371; the same probes with a linear [0, 16] LUT had 0.1758 mean error. This supports the current log-shaper as a bounded 65³ approximation, not exact transform parity. It compares OCIO evaluations to table interpolation, not Blender/classiCAD rendered pixels or final display quantization. Other display devices remain unsupported. Blender 5.2.2 selects the color-management context in [`draw_color_management.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/intern/draw_color_management.cc#L44-L57) and applies its default transform versus scene settings in [`viewport_settings_apply()`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/intern/draw_color_management.cc#L112-L140). Its display processor composes an explicit look before the DisplayViewTransform and applies exposure in scene-linear space and gamma afterward ([pinned source](https://github.com/blender/blender/blob/v5.2.2/source/blender/imbuf/opencolorio/intern/libocio/libocio_display_processor.cc)).
- **Camera depth of field:** the GPU viewport and offscreen render-result path share the Blender 5.2.2 `DofPass` shape: half-resolution RGBA16F color and RG8 near/far CoC pyramids, full-resolution CoC-aware preparation, quarter/eighth-resolution downsample levels, half-resolution 49-sample gather, 3x3 median cleanup, and a depth-derived CoC resolve before AA accumulation. The shader's sample perimeter and square-to-circle radius match `DofPass::setup_samples`; polygon blade shaping uses the algebraically equivalent sector formula, rotation, and reciprocal X aperture ratio. Its two-blade denominator no longer uses an extra epsilon floor. The gather-origin temporal jitter now follows Blender's `sample / samples_len` offset and exact IGN/blue-noise vector equation; focused sequence contract coverage is added, but matched per-sample GPU output remains unverified. The aperture path remains unverified against Blender images, especially the visual two-blade slit. The textures and passes are kept separate from final presentation; the existing camera gate and per-temporal-sample placement remain. QPainter uses its separate full-resolution software approximation, with aperture shape controls on a fixed 12-tap gather and a repeating 64x64 blue-noise spatial rotation. Scene-camera optics persist with project, session, and user preferences; imported Blender scene cameras populate the inputs; DOF is gated off for ordinary orbit/orthographic views.
- **Additional shadow-map behavior:** unsupported casters and nonmanifold/edited solids use the current GPU 1024-pixel depth-map fallback with 3x3 PCF. Polygon mesh fallback ranges in Texture color mode filter active Image Texture alpha using interpolated UVs. Trimmed and untrimmed `NurbsSurface` ranges use their shared tessellation UVs and regular/UDIM texture data in the same fallback; the shadow fragment shader discards sampled alpha below 0.1, except for NONE, IGNORE, and CHANNEL_PACKED modes. This corrects the earlier claim that the map fallback left trimmed NURBS unmasked. The code path is `prepareGeometry` and the shadow shaders plus `renderShadowMap` in `src/ui/viewport/viewport_surface_renderer.cpp`; `appendNurbsSurfaceDepthMesh` in `src/ui/viewport/viewport_depth_geometry.cpp` supplies trim-aware tessellation UVs. Blender's pinned [`workbench_image_lib.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_image_lib.glsl#L34-L59) samples the Image/UDIM texture and discards alpha below `image_transparency_cutoff` (0.1). The map still has narrower caster coverage than the stencil path: NURBS solids and ranges without an available texture/UV alpha source do not use this mask. Open NURBS surfaces and imported polygon meshes with manifold interiors and simple closed boundary loops use boundary-edge forced-fail stencil volumes. Polygon meshes with branched boundaries, loose vertices, inconsistent shared-edge winding, or nonmanifold edges remain on the depth-map fallback. In mixed-alpha scenes, opaque surfaces remain shadow receivers, and validated closed transparent solids plus supported open surfaces cast stencil shadow volumes; the depth-map fallback is limited to unsupported casters when volumes are active, and transparent triangles are drawn once without shadow sampling. Blender's handling of branched, nonmanifold casters and additional fallback fidelity remain unported. The QPainter Solid fallback does not render shadows; this CPU shadow gap is an out-of-scope approximation. The GPU and QPainter outline silhouette opacity now uses Blender Workbench's four-cardinal-neighbor resolve. For classiCAD's separate selection-color extension, both renderers use four-cardinal-neighbor adjacency; Blender itself uses one outline color and has no selected-color choice.
- **NurbsSolid texture-alpha update (2026-10-12):** The existing object-level Image Texture now uses the derived solid-face UV stream in the shared GPU texture path. The same `useTextureColor`/alpha cutoff therefore applies to visible color, object-ID outlines, and the depth-map fallback, with texture metadata included in geometry-cache invalidation. A zero-stencil offscreen contract checks a fully transparent solid texture against the same caster with alpha mode `NONE`; color and outline output remain cut out, while disabling alpha handling changes both visible output and the depth-map shadow. Per-face material rendering is now supported through stable seed-face IDs; per-face image overrides remain unsupported.
- **NurbsSolid semantic face identity and Blender proxy assignments (2026-10-13):** Unmaterialized affine extrusion seeds persist opaque IDs for the base cap, displaced cap, and each wall associated with a base trim loop (or the single rectangular perimeter wall). Blender proxy polygons carry a face-domain token table resolving to source object ID plus semantic seed-face ID, including in grouped large-project proxies. Project load merges homogeneous per-polygon material-slot assignments back into the embedded exact solid by those IDs; mixed or unknown assignments within one semantic face preserve its prior value, while a uniform `classiCAD.Unassigned` assignment clears it. Materialized `boundaryFaces` remain unsupported for face assignments because their panels do not retain seed-face lineage. Rendering and authoring UI status is tracked in the Material values row.

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

### QPainter in-front transparency composition (2026-10-13)

- The QPainter surface compositor keeps regular and `showInFront` geometry in
  independent opaque depth/color buffers and transparent fragment lists.
  Weighted transparency resolves regular scene content first, then in-front
  opaque color and a separate in-front transparent resolve. Composition stays
  scene-linear through both domains and applies the display transform once.
- The sorted CPU fallback composes in the same order: regular opaque, regular
  transparent, in-front opaque, then in-front transparent. Each domain tests
  its transparent fragments against its own opaque depth. Retained cavity
  layers contain the composed opaque receiver and ordered transparent overlay.
- The QPainter cavity/outline sample raster also resolves separate regular
  and in-front samples, choosing nearest depth within each domain before
  allowing in-front coverage to replace the regular sample. This keeps effect
  IDs and cavity receivers aligned with the final in-front color composition.
- Weighted QPainter composition computes separate cavity factor maps from
  regular opaque and in-front opaque samples, then applies each map only to
  its matching scene-linear opaque buffer. Outline and DOF continue to use the
  single composed visible sample view.
- Contracts cover an in-front opaque quad overriding a regular opaque quad,
  in-front transparency over regular opaque and transparent surfaces, and the
  sorted fallback ordering where regular transparency lies geometrically in
  front of the regular opaque surface but behind an in-front opaque surface.
  A nested-surface outline contract verifies that effect sample selection
  exposes the in-front object's boundary despite its farther world depth.
- Blender 5.2.2 Workbench draws its in-front G-buffer before the regular
  G-buffer in [`workbench_mesh_passes.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_mesh_passes.cc#L1114-L1239).
  Its transparent pass resolves regular OIT against `depth_tx`, then performs
  another OIT resolve against `depth_in_front_tx` into the same scene color
  target ([pass setup and draw](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_mesh_passes.cc#L1311-L1414)).
- Limitation: QPainter still approximates transparency in software and uses
  tessellated depth coverage. The domain ordering and cavity factor ownership
  match Workbench, while rasterization and per-fragment OIT weights are not
  pixel-identical to the GPU path.

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
- **Still different:** the GPU shadow path uses validated stencil volumes for supported closed `NurbsSolid` geometry and validated open `NurbsSurface` meshes; unsupported/nonmanifold casters retain the GPU depth-map fallback. The QPainter Solid fallback does not render shadows, which remains an out-of-scope approximation. Outline detection uses classiCAD's object-ID edge pass. QPainter effects remain approximate. GPU and QPainter depth of field follow Blender's active-scene-camera DOF gate in perspective and orthographic views. The QPainter path uses coarse 12-tap spatially rotated sampling. Neither output has been visually verified against Blender; the QPainter DOF path still lacks Blender's temporal gather jitter.
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
- Overlay Smooth Wires updates live in both Blender 5.2.2 and classiCAD. Edit Mode Smooth Wires takes effect after restart in both applications; classiCAD saves the requested value while keeping the active viewport value until relaunch.
- The grid renderer's independent MSAA coverage is tested in isolation. The composed viewport check showing that grid antialiasing remains active while Overlay Smooth Wires is off is still open.
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

- Imported scene display transforms now preserve the built-in sRGB view paths and bake other supported sRGB view/look combinations through Blender's bundled PyOpenColorIO into deduplicated 65³ float LUTs. The import bridge validates SHA-256, float finiteness, exact byte length, dimensions/domain/encoding, and limits the document to eight LUTs / 32 MiB. These fields were introduced in native document version 15; the current schema is version 21 and remains compatible with versions 1–20. CPU and GPU rendering sample the same red-fastest LUT layout; exposure precedes it and gamma follows it. The fixed [0, 2] input domain clamps HDR values, custom OCIO files must be available during import, other display devices remain unsupported, and a Blender import/render comparison is still needed.

- The CPU Workbench mesh and NURBS surface raster paths now have an optional observational fragment sink. At the existing per-pixel shading point it records physical target coordinates, signed camera-forward depth (`-dot(world - camera, viewDirection)`, larger is nearer), scene-linear RGB, material alpha, and source object ID after interpolation, texture-alpha cutoff, and lighting. The sink is not consumed by the QPainter presentation path; renders with it enabled remain byte-identical to the prior display path. A standalone offscreen contract covers PolygonMesh and NURBS coverage, HDR linear values, alpha, depth ordering, source IDs, and unchanged output. This creates a capture seam for later scene-linear composition without changing color/order behavior in this step.

- Keep the implemented Material, Object, Random, and Custom sources connected to filled surfaces/solids in both renderers. Keep Edit Mode strokes on their separate configurable `TH_WIRE_EDIT`-style setting and Theme-mode wire strokes on a separate configurable `TH_WIRE`-style setting, distinct from Workbench fill colors. Preserve selection, previews, and non-Theme wire colors. Preserve appearance values loaded from document data through the existing scene-object fields.
- Preserve the implemented Attribute source and Material fallback for missing attributes. Keep Texture's Material fallback when no active image, active UV map, or supported external image is available. Workbench does not evaluate arbitrary shader graphs; future Shader-mode graph evaluation is separate from this port.
- Preserve Blender object-name/library identity for Random colors; classiCAD stores those names and paths, with ObjectId fallback for older objects. The GPU and QPainter hash composition follows Blender 5.2.2's Murmur2, linked-library combination, and `BLI_hash_int_01` conversion (references in the finding above).
- Imported polygon meshes preserve material slots and per-face assignments. NURBS curves and surfaces still use one Workbench material payload per object; define an explicit parameter-space mapping before adding mesh-style color attributes or textures to NURBS.
- Verify color identity and appearance remain stable after save/reopen and object reorder.
- Keep custom shading inputs in the C++ scene/material model and serialization.

Acceptance: supported sources produce source-correct results; Attribute uses point, face, or corner data and falls back to Material only when absent. Mesh Texture uses the active UV and supported external image, with the documented QPainter approximation and Material fallback when an input is unavailable. Verify supported inputs after save/reopen. Unsupported inputs are reported rather than silently becoming gray.

### Stage 3: Complete Workbench effects

Status: partially implemented. GPU cavity/curvature and several existing effects are wired. Cavity settings now persist Blender 5.2.2 defaults/ranges, the renderer distributes SSAO samples across AA iterations with Blender's cap, and its jitter uses Blender's blue-noise-derived texture. In mixed-alpha scenes, the GPU effect prepass uses opaque ranges and supported texture-alpha cutouts; transparent ranges do not write cavity normals, IDs, or depth. The primary GPU scene uses weighted OIT with shared opaque depth: transparent lit RGB is composited over opaque scene-linear color, then depth of field and temporal AA run before exposure, active view transform/LUT, and gamma are applied once at presentation. Backgrounds, picture samples, and filled surfaces use the same scene-linear target; the offscreen result shares this final transform. This matches Blender's transform-after-AA order, while its sample weighting and SMAA output still need visual comparison. The auxiliary no-shared-depth path sorts ranges. QPainter's bounded object-layer compositor sorts transparent object layers by per-pixel nearest depth, but each object's triangles are precomposited; the fragment cap and unsupported geometry/large-target fallback retain average-depth sorting. QPainter cavity remains an approximate post-display-space effect and is skipped beneath visible translucent surfaces. Camera-gated GPU depth of field uses a shared viewport/offscreen pass with source-matched gather jitter, sample layout/weights, CoC-dependent LOD, and aperture shaping; the two-blade visual result, QPainter's separate 12-tap approximation, and matched output comparison remain open; GPU temporal jitter follows the pinned sequence. Validated closed opaque `NurbsSolid` casters use depth-pass stencil volumes only under the camera near-plane safety gate; closed fallback cases, open `NurbsSurface` casters, and transparent casters use capped depth-fail stencil volumes. Unsupported topology retains the depth-map fallback. GPU `NurbsSolid` face boundaries draw in the Edit Mode wire pass. Runtime/pixel comparison remains open.

- Compare and port culling, X-Ray, shadow, outline, specular, cavity, curvature, and depth-of-field behavior.
- GPU OIT accumulates shaded scene-linear RGB and composites into the RGBA16F opaque target. Its resolve clamps summed weights to Blender's `[1e-4, 5e4]` range and directly divides the signed RGB sum, matching Blender's pinned [`workbench_transparent_resolve_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_transparent_resolve_frag.glsl#L269-L286); accumulation weights and revealage also match [`workbench_transparent_accum_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_transparent_accum_frag.glsl#L435-L470). GPU color management now runs after scene-linear DOF and temporal AA, at presentation, using the same imported custom LUT or built-in transform. Background and image-preview inputs are decoded into scene-linear values before that target. QPainter remains a separate display-space compositor. Blender's pass order is recorded in its pinned [`workbench_engine.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_engine.cc#L2618-L2645) and [`workbench_effect_taa_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_effect_taa_frag.glsl#L9-L24); visual output comparison remains open.
- Map Blender's World cavity action to SSAO and Screen to curvature (the pinned View3DShading RNA enum definition); Both enables both. Persist Blender's SSAO distance/attenuation/sample settings and cavity/curvature ridge/valley factors with the source defaults and RNA ranges; drive the shader's corresponding uniforms and distribute the selected SSAO sample count across AA iterations, capped at 512 total samples.
- Match effect enable flags, setting dependencies, and opaque pass composition to Blender's source. Keep transparent-scene differences explicit; the source-derived cavity jitter is implemented but still needs visual comparison.
- For closed `NurbsSolid` casters, derive indexed geometry from the cached base/trim tessellation and extrusion walls, or from validated planar materialized quads. Require every indexed edge to have exactly two oppositely directed uses and reject zero-volume or degenerate meshes. For open `NurbsSurface` casters, accept only tessellations with consistently wound two-manifold interiors and closed, unbranched boundary loops; add forced-fail side quads along boundary edges. Use depth-pass stencil counting for closed opaque solids only when every source and extruded volume vertex lies safely beyond the camera near plane; retain the capped depth-fail path for open, translucent, near-plane-risk, and otherwise uncertain volumes. Extrude vertices to the current view's far plane with Blender's signed-distance/light-plane-dot formula and `1e-3` inward epsilon; use the `1e5` distance when the light direction does not intersect that plane. Tessellation remains display data derived from the persisted NURBS geometry.
- Pass audit against Blender 5.2.2 `workbench_shadow.cc`: Blender submits manifold geometry to a depth-pass `PASS`, while `FAIL` and `FORCED_FAIL` use depth-fail counting; translucent casters and selected non-manifold cases skip `PASS` and use `FORCED_FAIL`. classiCAD now uses Blender-style depth-pass stencil updates for the conservative closed opaque `NurbsSolid` and validated `PolygonMesh3D` subsets when all volume vertices are in the camera-visible half-space with a safety margin. It draws silhouette sides only for this route; capped depth-fail volumes remain the fallback for open surfaces, transparent casters, and any near-plane or geometry uncertainty. Compare near-plane crossings, camera-inside views, and perspective/orthographic projections against Blender before widening eligibility.
- The GPU stencil-volume path now also accepts imported `PolygonMesh3D` objects only when their transformed triangulated surface is finite, nondegenerate, closed, consistently oriented (each indexed edge has exactly two oppositely directed uses), has one connected cycle in each vertex link, and every connected component has positive nonzero signed volume. Every face material range for that object must be opaque under the active Workbench alpha source. Loose vertices, open boundaries, duplicate/overused edges, pinched/disconnected vertex links, inconsistent winding, degenerate or inward/zero-volume components, mixed-alpha objects, and failed near-plane safety checks stay on the existing depth-map or depth-fail fallback. The volume topology is stored once on the first per-face draw range while every face range is marked as belonging to that caster, preventing fallback-map double drawing after a successful volume pass. This extends Blender's evaluated-mesh topology dispatch without making tessellation authoritative; runtime comparison remains open.
- Keep the GPU depth-map fallback for unsupported/nonmanifold topology, nonplanar materialized solid faces, missing stencil buffers, and ranges that cannot use the volume path. The fallback rasterizes selected triangle ranges into a fixed 1024×1024 light-depth map. In Texture mode, polygon mesh casters with active UVs and per-face images, plus trimmed and untrimmed `NurbsSurface` ranges with tessellation UVs and texture data, filter alpha at Blender's 0.1 cutoff; NONE, IGNORE, and CHANNEL_PACKED modes bypass that mask. NURBS solids and ranges without a usable texture/UV alpha source remain unmasked. This describes the GPU depth-map fallback; the GPU stencil shadow path does not sample texture alpha. The QPainter Solid fallback does not render shadows; implementing CPU shadows remains out of scope for that fallback. Hard shadowing, map resolution, and remaining caster cases differ from Blender's ray-traced shadow option. In mixed-alpha scenes, opaque surfaces remain receivers and validated closed solids/open surfaces can cast volumes; transparent triangles draw afterward without shadow sampling. Branched/nonmanifold caster dispatch and Blender's ray-traced shadow option remain unsupported.
- Map the stored scene light direction through Blender's Y/Z swap and X/Z sign change. Clamp Shift to Blender's `[0,1]` range and compute `focus = clamp(shadow_focus, 0.0001, 0.99999)`, `shadow_focus_edge = 1 - focus * (1 - shadow_shift)`, then apply `smoothstep(shadow_shift, shadow_focus_edge, -dot(N, light_direction_view))`; shadow intensity mixes the result with the unshadowed contribution. Keep the fallback depth bias fixed so Shift no longer changes map bias.
- Keep GPU and QPainter depth of field camera-gated by an active scene camera with DOF enabled, regardless of view projection. The QPainter software path now uses camera-relative depth, applies the Blender aperture-radius factor, and has focused production-path tests for perspective and orthographic focus/defocus plus orthographic translation invariance. Validate GPU and QPainter aperture blade/rotation/ratio shaping against Blender. The GPU temporal sample lifecycle and DOF gather jitter now follow Blender's pinned sample-offset/vector equations; the focused contract is added, while matched per-sample output remains open.
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
| Renderer ownership | `workbench_engine.cc` owns named opaque, transparent, transparent-depth, volume, outline, depth-of-field, and anti-aliasing passes. It draws those passes in a defined order using Blender's evaluated scene, draw manager, and GPU APIs. | `ViewportWorkbenchScenePassCoordinator` dispatches both live and offscreen samples from a value packet built from `ViewportRenderFrame`; `ViewportAaRenderer` owns sampled/resolved attachments, and `ViewportSurfaceRenderer` owns its effect/transparency framebuffers and surface pass sequence. The packet's copied object vector is implicitly shared; its target handles are borrowed only for the active synchronous AA sample. `viewport_widget.cpp` still owns AA lifecycle, background, depth regeneration, strokes, overlays, and grid. | Continue moving GPU scene-pass sequencing and its named attachment production contract into the coordinator. Keep caller-owned background and CAD/editor post-resolve overlays outside that scene pass, while preserving the same sequence for live and offscreen results. |
| Volume data and rendering | Workbench reads the active evaluated scalar grid, uses Blender's volume texture transforms and display density, draws after transparent depth, and ray-marches with per-axis five samples per voxel, Bayer/Halton jitter, and closest/linear/cubic sampling. The pinned Workbench volume shader leaves shadow sampling as a TODO. | Blender import transfers one bounded single-channel `FLOAT` grid through evaluated Geometry Nodes mesh attributes; native JSON stores sparse voxel/tile records and display controls. `ViewportSurfaceRenderer` reconstructs a bounded dense texture and draws a volume pass in the matching pass slot. The QPainter fallback has no volume renderer. | Preserve this first bounded scalar path and keep its caps visible. Multi/vector grids, animation, oversized dense bounds, and QPainter volume rendering remain separate follow-up work. |
| TAA/SMAA resolve | Blender uses a centered jitter sequence, a 3x3 Blackman-Harris filter in log color, accumulated weights, and an SMAA resolve that fades as temporal samples accumulate. | The GPU implementation follows those algorithms closely. The QPainter fallback instead supersamples and downsamples; it does not run the GPU TAA/SMAA sequence. | Controlled GPU renders match at AA Off and Single Pass. A source-derived AA-off camera-shift fixture now compares all eight raw samples; their silhouettes differ by at most two pixels per sample. The resolved scene-linear 8-sample silhouette masks now match exactly. Blender's background API still exposes only the final render, so per-sample comparison uses validated camera-shift renders rather than direct internal-buffer reads. Treat QPainter as a separate approximation. |
| Transparency/depth | Blender has separate transparent accumulation and transparent-depth passes following opaque drawing. `OBJECT_IN_FRONT` uses a second depth domain and merges sampled physical depth into scene depth before regular transparent depth excludes its pixels. After scene effects, the overlay-depth pass writes device depth zero at in-front pixels before AA, giving final render-result Z its near-plane value. | The primary GPU shared-target path uses independent regular/front weighted-OIT targets and merges front depth/IDs before regular transparent depth. It keeps physical front depth through volume, outline, and DOF, then applies a late overlay-only near-plane override before AA. The no-shared-target fallback retains sorted/regular behavior. QPainter has no separate in-front domain. | A matched 640×480 GPU fixture verifies mid-pass merged physical front depth, front overlap color, and final near-plane Z. Fallback and QPainter paths remain different; broader scenes and combinations still need comparison. |
| Outline and effects | Blender runs outline from its object-ID buffer, then DOF, then AA. Cavity/curvature uses Workbench-specific sample buffers and effect composition. | classiCAD generates normal/depth/object-ID inputs in its own prepass and uses custom outline/effect shaders. QPainter has separate software approximations. | Port and attribute the standalone shader/effect logic where inputs match. Preserve CAD-specific selection and NURBS tessellation handling. Validate each effect independently before combining them. |
| Geometry and materials | Workbench consumes Blender evaluated object batches and Workbench material records; its render engine is integrated with Blender scene, dependency-graph, draw-manager, GPU, and render systems. | classiCAD sends derived tessellation of exact NURBS surfaces/solids to its GPU renderer. The surface renderer has its own material packing and draw ranges. | Keep NURBS authoritative and pass only derived display data to the renderer. A future shader system should provide evaluated Workbench inputs; do not create a second Blender-shaped scene/geometry model. |

The previously supplied cube screenshots are not a controlled edge-quality comparison: the recorded views differ (`Ortho` versus `User Perspective`), the visible geometry differs, and the output dimensions differ. A same-geometry, same-camera, same-size GPU comparison confirms exact Off and Single Pass silhouettes. Per-sample references traced the 8-sample edge mismatch to the vertical projection-jitter sign: Blender adds positive Y to its orthographic projection, while classiCAD had negated it. After correcting that sign, the resolved scene-linear 8-sample silhouette masks match exactly.

Phase Two category audit: Blender 5.2.2 Workbench UDIM lookup in [`workbench_image_lib.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_image_lib.glsl#L348-L412) floors the UV to one tile, looks up that tile's array layer and coordinate scale/offset, and samples the resulting array coordinate. Filtering does not address neighboring layers. The classiCAD GPU samplers fetch taps from the selected layer and clamp/wrap/clip/mirror against that tile's dimensions; QPainter color/effect-alpha likewise resolve one tile before filtering. Therefore, cross-tile filter continuity is not a Blender 5.2.2 parity gap; retain the existing per-tile edge behavior and preserve magenta/opaque results for absent base tiles. Blender's outline shader [`workbench_effect_outline_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_effect_outline_frag.glsl#L10-L24) compares the center object ID with exactly four cardinal samples and applies one outline color. classiCAD's GPU and QPainter paths now use four-cardinal-neighbor adjacency for their additional selected-color behavior; this is backend consistency, not a Blender feature.

Phase Two Studio/MatCap/Flat comparison: the offscreen cube fixture now selects the Workbench lighting mode and preset, rotates Studio lighting, can use world-space lighting, and disables output dithering for direct pixel comparisons. The classiCAD contract captures final display PNG and scene-linear PFM for Studio Default, world-space Studio Paint rotated 90 degrees, MatCap `metal_bronze`, and Flat. All four strict nine-probe comparisons pass without threshold changes: Default Studio display max error 0/255; rotated Paint max 1/255; bronze MatCap MAE 0.3333/255 and max 1/255; Flat exact. Scene-linear MAE/max are respectively 0.0002441, 0.0003255/0.0003662, 0.0000140/0.0000153, and 0. The MatCap comparison required preserving the Blender 5.2.2 EXR diffuse/specular RGB channels in half-float resources; classiCAD now packages all 27 presets as `.qmc` payloads. It also exposed dark-range interpolation error in the uniform 65-cube AgX LUT. The LUT now uses a `log2(1 + 16x)` input shaper generated from Blender's bundled OCIO processor, applied on the CPU and GPU; the bronze display error fell from 4/255 to 1/255. These captures cover the named settings and geometry fixture only; other preset combinations, full-frame edges, and QPainter parity remain open. Reproduce the classiCAD captures with `CLASSICAD_VIEWPORT_LIGHTING_CAPTURE_DIR=/tmp/classicad-workbench-lighting QT_QPA_PLATFORM=offscreen ctest --test-dir build -R '^viewport_render_contracts$' --output-on-failure`, then run the Blender fixture and `tests/workbench_display_probe_compare.py` for each matching PNG/PFM pair.

Phase Two color-source comparison: matched 640×480 Flat captures for Material, Object, and Random use the same cube, name, camera, and RGB inputs. All nine scene-linear and display probes match exactly for each source. Feature contracts verify interpolated Attribute RGB with material alpha, active-then-default color-attribute selection, and Texture RGB replacing material RGB while retaining material alpha. Blender import and native JSON now retain the default color attribute separately from the active attribute; both renderers use it when the active attribute is unset, and keep the material fallback when neither exists, matching Blender's active/default selection in [`workbench_state.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_state.cc). Texture extraction now selects Blender's separately marked active texture node (`show_texture`) even when `nodes.active` points elsewhere, following [`node_get_active_texture`](https://github.com/blender/blender/blob/v5.2.2/source/blender/editors/uvedit/uvedit_ops.cc#L98-L152). Meshes also retain Blender's default/render UV layer separately; GPU, QPainter, and raster paths use active UV first and default/render UV when active is unset, matching [`active_or_default_uv_map_name`](https://github.com/blender/blender/blob/v5.2.2/source/blender/blenkernel/intern/mesh.cc#L1300-L1324) and the Workbench mesh batch setup in [`draw_cache_impl_mesh.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/intern/draw_cache_impl_mesh.cc#L628-L635). Blender 5.2.2's active-texture selection contract and classiCAD's UV fallback/serialization contracts pass. Material/Object/Random captures do not validate attribute/texture image output against Blender; full Attribute/Texture matched renders and broader texture alpha/UDIM cases remain open. The CPU fallback alpha test uses classiCAD's byte cutoff and remains to be checked at Blender's exact render boundary. The focused viewport contract passes; `core_contracts` still reports perspective/zoom, snapping, curve-tool, and dimension failures outside this color-source category. Reproduce the three exact color-source comparisons with `tests/workbench_cube_reference.py --lighting FLAT --color-source MATERIAL|OBJECT|RANDOM` and compare each output pair with `tests/workbench_display_probe_compare.py`.

Phase Two Category 8 source audit: the GPU TAA/SMAA weights, log-space accumulation, SMAA edge/area/search stages, temporal mix, and resolve follow Blender 5.2.2 closely, so no duplicate shader rewrite was made. QPainter still uses supersampling rather than Workbench temporal accumulation. Custom display LUTs carry their domain and shaper through extraction, import, native serialization, CPU rendering, GPU surface/transparency rendering, and AA presentation. A controlled PyOpenColorIO-to-trilinear-LUT comparison for Blender 5.2.2's AgX + Very High Contrast combination used 41,000 deterministic RGB probes over [0, 16]³: the 65³ log-shaped table measured 0.000116 mean absolute channel error, 0.000177 p95, 0.00241 p99, and 0.0546 maximum; the subset in [0, 1]³ measured 0.000194 mean and 0.000371 p95. A linear [0, 16]³ 65³ table measured 0.1758 mean error on [0, 1]³ probes, supporting the existing log shaper. This isolates interpolation error against OCIO and does not compare full Blender/classiCAD rendered output, GPU texture precision, or display quantization. Retain the QPainter and display-transform differences explicitly; worst-case LUT and matched-render comparisons remain open.

### Phase Two image source-space conversion category

The importer now has a bounded RGBA32F texel payload path for Blender images whose ordinary QImage/GL sRGB route cannot represent Workbench's source behavior. Blender 5.2.2's byte-image float-texture helper converts each byte texel through the active OCIO source-to-scene-linear processor before filtering; ACEScg is the only additional byte color space accepted by this first implementation, and only when its active-config processor validates as linear. It is stored as an OCIO-config-specific transform ID and converted to little-endian float RGBA per texel. Byte sRGB, Linear Rec.709, and Non-Color retain their existing identity/sRGB/data sampling routes; unsupported byte spaces are reported by extraction rather than silently passed through. Blender's float-image Workbench path directly copies its float buffer without OCIO, including float images labeled sRGB; those payloads are identified as `blender-float-buffer`, not as an identity color-space conversion. PREMUL follows Blender's per-texel multiply behavior and alpha is copied exactly.

The float texels survive Blender import and native JSON, including UDIM tiles, with a 32 MiB per-image payload ceiling and a 64 MiB deduplicated scene payload ceiling. GPU texture creation and QPainter mesh plus NURBS texture sampling consume float texels before nearest/bilinear filtering, preserving negative and above-one RGB until shading/display conversion. Mixed float/byte UDIM arrays convert byte tiles into the linear float array while retaining their alpha; texture cache keys include payload dimensions. Native deserialization validates integral dimensions, exact little-endian RGBA32F byte counts, and finite components. Missing/invalid float payloads retain the packed/path image record. Alpha-only shadow/cutout inputs continue to use alpha without applying RGB conversion. Verification on 2026-10-13: the full C++ build passes; `tests/blender_texture_source_contract.py` passes on Blender 5.2.2; the focused native Workbench persistence contract passes; QPainter signed/HDR texture sampling and alpha assertions pass within `viewport_render_contracts`. The viewport test then cannot create its OpenGL context in this environment, so GPU float upload and the new mixed-UDIM array path are compile-checked but not runtime-captured here. Full frame comparison remains open. Primary source behavior: Blender 5.2.2 [`IMB_colormanagement_imbuf_to_float_texture`](https://github.com/blender/blender/blob/v5.2.2/source/blender/imbuf/intern/colormanagement.cc), [`IMB_colormanagement_imbuf_to_byte_texture`](https://github.com/blender/blender/blob/v5.2.2/source/blender/imbuf/intern/colormanagement.cc), and [Workbench image sampling](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_image_lib.glsl).

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
now submit a `ViewportWorkbenchScenePassPacket` built from their
`ViewportRenderFrame` to the same coordinator. The packet carries the sample
camera, effective draw objects, target size, shading state, and scene-linear
intent; its attachment handles are borrowed for the active AA sample only. The
surface renderer returns named completion flags for opaque color, transparent
color, weighted accumulation/resolve, and transparent depth, plus the shared
depth/normal/object-ID attachments it populated. It reports opaque and
show-in-front depth/normal/object-ID prepasses only when drawable geometry
reaches those prepasses. The same scene-color call is shared, while
grid/background, DOF/AA presentation, CAD strokes, and editor overlays retain
their existing caller-owned order. The render result
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
composited into display-referred and scene-linear Combined output. Their
fixed-pixel labels/arrows have no corresponding Z samples. This is a classiCAD
render-result behavior, not a Blender parity requirement.
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

The render AA enum conversion is centralized in
`blenderRenderAntiAliasingMode` and covered for `OFF`, `FXAA`, all five
multisample counts, and missing/unknown values. Supported imported values
override the viewport AA preference; missing or unknown identifiers retain
the existing viewport preference. The GPU AA contract separately checks the
sample counts and output for Off, Single Pass, and 8 samples. A full
File → Render Workbench Result comparison against Blender Combined and Z is
still open.

**Direct Combined/Z pass capture contract:** `workbench_render_pass_capture`
uses a direct `QOpenGLContext` plus `QOffscreenSurface`, the production
`ViewportSurfaceRenderer`, `ViewportAaRenderer`, and
`ViewportWorkbenchScenePassCoordinator` to capture the controlled 640×480
polygon cube. It checks the resolved scene-linear RGBA dimensions, a projected
top-face sample using top-down image coordinates, finite positive foreground
view-Z, and Blender's `1e10` background depth sentinel. Set
`CLASSICAD_WORKBENCH_PASS_CAPTURE_DIR=/path/to/output` to write
`classiCAD-combined.pfm`, `classiCAD-z.pfm`, and `classiCAD-display.png`.
These captures exercise the production GPU scene pass, AA resolve, and shared
depth conversion helper. The capture test creates its own result FBO and
D32F/S8 attachment; it does not call `ViewportGpuSurface::renderOffscreen()`
or verify that method's allocation/readback path. It also does not invoke
`ViewportWidget::renderWorkbenchResult()` or test its format selection,
metadata, CAD annotations, or overlay ordering.
The Blender reference remains available through
`tests/workbench_cube_reference.py -- /path/to/reference.png --aa OFF
--linear-output /path/to/reference.exr`; convert its Combined and Depth EXR
subimages with Blender's bundled OpenImageIO via
`blender --background --python tests/workbench_exr_to_pfm.py -- input.exr
color.pfm depth.pfm` before using the RGB comparator. The new opt-in
`--match-classicad-ortho` option sets the reference camera eye distance to
1000 and clipping to 0.01/2000 without changing the fixture defaults. A fresh
640x480 AA-off cube reference with this option, the existing 5.0 orthographic
scale and direction `(4,-6,4)`, matches classiCAD's projection eye and clip
range. Comparing its Depth PFM to the unchanged classiCAD capture gives
111256 common finite foreground pixels, 195720 common background pixels at
the exact `1e10` sentinel, and 224 foreground-mask XOR pixels. On common
foreground pixels the unadjusted absolute view-Z error is MAE `0.00006355`,
p95 `0.00018311`, maximum `0.00018311`; signed mean (classiCAD minus Blender)
is `0.00003050`. This removes the prior 992-unit camera-origin offset. The
original RGB comparison against the fixture's default 8-unit camera is
retained unchanged. With the matched-camera reference, scene-linear RGB MAE
is `0.000438` per channel and foreground-mask IoU is `0.99382` (493 XOR
pixels); this separate color result does not replace the retained RGB capture.
The focused target built and passed under the offscreen platform. Its initial
depth assertion exposed a real presentation-target issue: Qt's default
CombinedDepthStencil target used D24/S8, so the production AA renderer's
D32F/S8 depth blit raised `GL_INVALID_OPERATION` and left result Z entirely at
the background sentinel. `ViewportGpuSurface::renderOffscreen()` now attaches a
matching D32F/S8 texture. The direct capture contract independently verifies
that this matching format produces no GL error and preserves per-pixel scene
depth within `1e-6`; it does not exercise the production method's attachment
code. A capture compared to the
existing Blender 640×480 OFF fixture has scene-linear RGB MAE about `0.000126`
per channel, foreground-mask IoU `0.99898`, and 81 XOR pixels. That initial
8-unit-camera depth comparison had a 992-unit camera-origin offset; the
explicit matched-camera reference and unadjusted depth metrics above supersede
the earlier offset-adjusted measurement. This remains a production GPU
pass/result-target capture, not a full File command comparison.

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
- A second controlled AA-Off comparison now checks nine interior pixels on
  the same 640×480 cube. Both capture scripts reject blank output. The
  classiCAD offscreen contract writes final display PNG and scene-linear PFM;
  Blender writes Combined EXR, converted to PFM. The nine probes measured
  scene-linear RGB MAE/max `0.0002441` and final display MAE `0.4074/255`,
  max `1/255`. These are interior probes, not a full-frame or edge-parity
  result. Reproduce with:
  `CLASSICAD_VIEWPORT_POLYGON_CUBE_CAPTURE=/tmp/classicad-cube.png CLASSICAD_VIEWPORT_POLYGON_CUBE_LINEAR_CAPTURE=/tmp/classicad-cube.pfm QT_QPA_PLATFORM=offscreen ./build/classicad_viewport_render_contracts_test`,
  then render Blender using
  `blender --background --python tests/workbench_cube_reference.py -- /tmp/blender-cube.png --scene CUBE --aa OFF --ortho-scale 3.75 --match-classicad-ortho --linear-output /tmp/blender-cube.exr --linear-pfm-output /tmp/blender-cube.pfm`,
  and compare with
  `python3 tests/workbench_display_probe_compare.py /tmp/blender-cube.png /tmp/classicad-cube.png /tmp/blender-cube.pfm /tmp/classicad-cube.pfm`.
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
- **Edge-coverage residual metric (2026-10-13):**
  `tests/workbench_pfm_compare.py` now reports a separate approximate coverage
  error in the union of the two-pixel silhouette bands. It estimates each
  image's corner background, finds locally nearest stable foreground and
  background samples, and projects edge RGB between those colors using a
  separate 0.01 scene-linear RGB-distance threshold. The 8-neighbor distance
  defines a Chebyshev pixel band. The report includes coverage MAE, p90, signed
  bias, and partial-coverage fractions;
  existing foreground-mask and full RGB metrics remain separate. Six
  synthetic Python contracts cover identical hard edges, an AA ramp with the
  same mask, a one-pixel shift, background offsets, face-color changes, and
  uniform-background rejection. These synthetic checks pass. The PFM data has
  no object-ID or depth attachment, so this estimate assumes locally stable
  face/background colors; no stored Blender/classiCAD capture was available in
  this worktree to produce a new measured coverage residual. This improves
  comparison diagnostics and does not close the AA visual-parity item.

### Shared render-pass color attachment reporting (2026-10-13)

- The shared named pass report now marks `CurrentSampleColor` when opaque
  ranges draw, when regular or in-front transparency resolves into the current
  sample (including sorted fallbacks), when in-front opaque color merges, and
  when the deferred opaque resolve succeeds. The isolated in-front color
  buffer is reported as `FrontColor`. This makes the attachment contract
  describe the actual color outputs consumed by the live and offscreen AA
  paths. Focused capture assertions cover opaque, deferred, transparent, and
  in-front color reporting.
  The C++ build succeeds, but running the capture executable with Qt's
  offscreen platform cannot create the required OpenGL 3.3 context, so those
  runtime assertions remain unverified here.

### QPainter X-Ray outline behavior (2026-10-13)

- The QPainter effects path no longer exits for X-Ray. It preserves the object
  outline while suppressing cavity, shadow, and depth-of-field work, matching
  Blender 5.2.2's X-Ray state setup and later outline pass. A focused CPU
  contract checks that an X-Ray surface still produces its configured outline;
  that assertion passed in the offscreen contract run. The test later exited
  at its existing OpenGL context requirement for the NURBS-solid display case.
  Blender references: [`workbench_state.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_state.cc#L1240-L1258),
  [`workbench_engine.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_engine.cc#L2554-L2595),
  and [`workbench_effect_outline.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_effect_outline.cc#L315-L367).

### QPainter float-texture alpha cutoff (2026-10-13)

- Polygon-mesh float texture sampling now carries the material alpha mode into
  its per-triangle data and applies Blender's `0.1` image cutoff only when
  alpha is not ignored or channel-packed. This closes a CPU-only mismatch
  where low float alpha incorrectly removed geometry in `IGNORE`,
  `CHANNEL_PACKED`, and legacy `NONE` modes. The contract covers STRAIGHT,
  IGNORE, and CHANNEL_PACKED float textures; its CPU assertions passed before
  the test reached the later OpenGL-only NURBS-solid check. Blender references:
  [`workbench_materials.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_materials.cc#L536-L553)
  and [`workbench_image_lib.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_image_lib.glsl#L416-L433).

### Render-result visibility (2026-10-13)

- Offscreen Workbench result assembly now uses a render-specific visibility
  predicate. It honors object and collection `hide_render` plus active
  view-layer collection membership and exclusion, while ignoring viewport-only
  object and collection hiding. Interactive viewport frame construction keeps
  using the existing viewport visibility predicate. This follows Blender
  5.2.2 Workbench's render sync, which filters with
  `DRW_object_is_renderable` and `DRW_object_visibility_in_active_context` in
  [`workbench_engine.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_engine.cc#L153-L162).
- Focused render-frame coverage checks viewport-hidden/render-visible objects,
  render-hidden/viewport-visible objects, collection `hide_viewport`, and
  collection `hide_render` through the selected view-layer membership path.
  `cmake --build build -j4` completes successfully, and the focused
  `classicad_viewport_render_contracts_test` passes with Qt's offscreen
  platform. Matched Blender render-result image comparison remains open.

### Controlled Workbench effect captures (2026-10-12)

- The Blender fixture now supports a two-quad `TRANSPARENT_OVERLAP` scene and
  explicitly selects World cavity for cavity captures. The offscreen GPU
  contract has opt-in captures for shadow, cavity, outline, and transparency
  scenes. `tests/workbench_pfm_compare.py` compares RGB PFM outputs by channel
  error and foreground-mask XOR/IoU. The overlap fixture now defaults to a
  1000-unit orthographic scene-camera distance with 0.01/2000 clipping,
  matching classiCAD's current free-view projection depth. Its earlier
  8-unit camera produced an invalid OIT color comparison despite matching
  screen-space bounds. This controlled scene-camera comparison isolates OIT
  depth weighting; it does not capture a live Blender `RegionView3D` viewport.

### QPainter scene-linear transparency integration (2026-10-13)

- The ordinary QPainter Solid surface path now records scene-linear opaque and
  transparent fragments, retains nearest per-pixel opaque depth, filters
  transparent fragments against that depth, resolves Blender's weighted OIT
  over opaque color and a scene-linear World/Custom background, and applies
  the configured display transform once. The existing per-object QPainter
  coverage image supplies edge coverage. Theme gradients remain on the prior
  composition path because the current background input is a flat color; the
  native-renderer failure fallback also keeps its post-display legacy path.
- Polygon-mesh X-Ray now multiplies material/source alpha by `xrayAlpha` before
  the fragment sink and display raster, matching the Blender 5.2.2 transparent
  accumulation shader. Per-object fragment selection keeps only the nearest
  captured triangle at each pixel, and the opaque pass selects the same object
  as the existing projected-depth buffer.
- Headless contracts cover scene-linear per-pixel bases, transparent overlap,
  per-fragment opaque-depth rejection, and matching X-Ray/material alpha.
  Against the matched 640×480 Blender `TRANSPARENT_OVERLAP` capture, the
  overlap region has display MAE `0.332/255`, p95 and maximum `1/255`; the
  center is `[170,112,172]` versus `[169,112,172]`. The initial full-frame
  capture had 615 pixels above a 2-value threshold because classiCAD used
  rounded rear-quad bounds while Blender projected fractional bounds. Aligning
  the fixture to `(134.4,86.4)–(441.6,393.6)` removed those mismatches: the
  updated full-frame MAE is `0.071/255`, maximum `1/255`, with zero pixels
  above 2. Edge probes now confirm matching covered/uncovered pixel centers.
  QPainter cavity under transparency and native-fallback composition remain
  open.

### QPainter Theme background integration (2026-10-13)

- The ordinary bounded QPainter surface compositor now accepts per-pixel
  scene-linear background samples. The widget builds the Theme gradient with
  Blender's `viewport_background.frag` factor for Single Color, Linear, and
  Radial styles, decodes the sRGB theme endpoints, and interpolates them in
  scene-linear RGB before the surface/OIT resolve. World and Custom continue
  to use their scalar scene-linear bases; missing-World uses the Theme samples.
  The existing coverage cap also bounds gradient allocation to the compositor's
  one-million-pixel limit; oversized targets keep the prior fallback.
- The headless viewport contract feeds a top-to-bottom linear gradient through
  the production surface compositor and checks an uncovered pixel after the
  configured display transform. It also renders full 640×480 Single, Linear,
  and Radial Theme backgrounds through production QPainter and GPU paths and
  checks their display pixels with a two-code-value allowance for GPU Bayer
  dither. Maximum RGB error is 1/255 for all three styles. Matching the radial
  shader exposed that QPainter's circular radius did not match Blender's
  normalized screen-space radial factor, so the CPU path now samples that same
  factor per physical pixel, including high-DPI targets. A 2×-DPR radial
  comparison also passes at the same tolerance. This closes the classiCAD-only
  formula check. Matched
  Blender live Workbench captures remain open; they are unavailable in the
  headless run.
- Reference: [Blender 5.2.2 Workbench background shader](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/viewport_background.frag).

### Native-renderer failure fallback composition (2026-10-13)

- The native-renderer failure path now uses the same bounded scene-linear
  weighted transparency compositor as the ordinary QPainter path. It supplies
  scene-linear World and Custom colors or per-pixel Theme gradient samples,
  including the World-without-imported-color Theme fallback. Native painting
  still ends before the QPainter surface pass and resumes before the regular
  overlays; unjittered scene depth is rebuilt afterward.
- A headless contract covers the failure-fallback compositor inputs with a
  nonuniform Theme base, checking a display-transformed uncovered pixel and a
  translucent overlap against the legacy display-space path. An opt-in capture
  is available through
  `CLASSICAD_VIEWPORT_QPAINTER_FAILURE_FALLBACK_CAPTURE`. A forced live GPU
  failure capture remains open.

### QPainter cavity and outline order (2026-10-13)

- The CPU effects path now applies cavity before the object outline, matching
  Blender's opaque cavity resolve followed by transparency resolve and the
  later outline pass. A headless integration contract confirms cavity changes
  the surface image and the outline remains visible over it. The focused
  `viewport_render_contracts` test passes.
- The ordinary QPainter surface path now applies its opaque-only cavity factor
  to scene-linear opaque RGB before weighted OIT, leaves the scene-linear
  background and transparent fragments unchanged, and then applies the display
  transform once. The later effects stage skips cavity for that resolved layer,
  preventing a second application. A focused contract checks that the opaque
  retained layer changes while the transparent contribution stays the same.
  The bounded sorted fallback also folds cavity into its retained opaque
  premultiplied layer before sorted transparent fragments are composed, and
  skips the later cavity effect. Its retained RGB is already display encoded,
  so that fallback still differs from Blender's scene-linear resolve under
  nonlinear display transforms. Both paths still use the QPainter software
  cavity sampling approximation and need matched Blender image comparison.
  Full build passed after the change. The offscreen viewport contract reached
  its NURBS-solid display case, then exited because this environment could not
  create an OpenGL context; preceding CPU cavity assertions passed.
- The offscreen AA-off presentation contract now exercises the final scene
  resolve with a scene-linear World color, an sRGB viewport background, a
  missing World background, and transparent film. It checks the AgX display
  values for opaque World/viewport backgrounds and transparent black for the
  two alpha-preserving cases. A float premultiplied sample also checks that
  AgX presentation unpremultiplies before the nonlinear transform and returns
  the display result to premultiplied RGBA. These cases validate the resolve
  boundary without opening the application window.
- At 640×480 with the same cube scene, AA-off references, and an RGB change
  threshold of 2, shadow masks match exactly: 111,480 changed pixels on each
  renderer, XOR 0, IoU 1.0. After routing classiCAD's shadow capture through
  the production scene-linear pass and display transform, effect-delta MAE
  over the mask union is 0.388/255, p95 is 1/255, and maximum error is 8/255.
  The first outline
  comparison also had identical masks (2,316 pixels, XOR 0, IoU 1.0), but its
  reported 10.48/255 effect-delta MAE came from a test capture that applied
  classiCAD's display transform before the outline. The outline capture now
  goes through the same scene-linear pass coordinator and final display
  transform as the production viewport. On that matched path, the masks remain
  identical and effect-delta MAE is 1.77/255, p95 is 4/255, and maximum error
  is 16/255. A separate source audit found that custom outline colors also
  needed a color-space correction:
  Blender's outline setting is scene-linear, while classiCAD stores the UI
  color as sRGB. The GPU pass now converts the outline color to scene-linear
  when drawing into the shared Workbench target, then blends it before the
  final display transform. An offscreen contract checks a 50%-gray outline
  against a black baseline at a quarter-covered silhouette pixel (14/255 in
  the RGBA8 scene-linear test target). The ordinary display-space fallback
  still receives the encoded sRGB value.
- To isolate the earlier cube edge-color discrepancy, the offscreen contract
  can capture a classiCAD `PolygonMesh3D` cube with the same eight vertices and
  six quad faces as Blender's default cube (`CLASSICAD_VIEWPORT_POLYGON_CUBE_CAPTURE`).
  At 640×480 with AA off and matching AgX scene-linear resolve, the polygon
  captures have identical foreground masks (111,480 pixels each, XOR 0, IoU
  1.0). Shared-pixel RGB error is 0.60/255 MAE and 2/255 at p95. This is far
  closer than the earlier comparison using a NURBS extrusion solid, supporting
  face tessellation/ownership as the source of most of that discrepancy. The
  test fixture keeps the NURBS solid contracts unchanged; it adds only an
  opt-in matched-topology diagnostic capture.
- The same matched polygon cube can capture outline-on and outline-off frames
  through the production scene-linear resolve. Blender and classiCAD change
  the same 2,316 pixels (XOR 0, IoU 1.0); comparing each renderer's outline
  effect against its own baseline gives RGB delta MAE 1.74/255, p95 4/255,
  maximum 8/255. This validates the outline pass on Blender's default mesh
  topology as well as the earlier NURBS-solid fixture.
- The matched polygon cube shadow capture changes 111,480 pixels in both
  renderers (XOR 0, IoU 1.0). Effect-delta RGB error is 2.10/255 MAE,
  p95 5/255, maximum 11/255. This independently verifies the shadow coverage
  on the default six-quad mesh; the earlier production-path measurement on a
  NURBS extrusion solid remains 0.388/255 MAE.

**Opaque hard-shadow capture rerun (2026-10-13):** Reused the
`viewport_render_contracts` opt-in polygon-cube shadow and baseline captures
with Qt's offscreen platform, then rendered the Blender 5.2.2 `CUBE` fixture
twice (`--aa OFF --effect SHADOWS|BASELINE --zero-dither`). Both renderers had
cavity and object outlines disabled. Comparing the final 640×480 PNG effect
deltas at a per-pixel RGB threshold of 2/255 gives 111,480 changed pixels on
each side, XOR 0, IoU 1.0; signed effect-delta RGB error over the union is
2.23/255 MAE, p95 5/255, maximum 10/255. This reproduces the earlier exact
coverage result without identifying a renderer defect. A second Blender run
with `--match-classicad-ortho` (1000-unit eye and 0.01/2000 clipping) changed
111,256 pixels, producing XOR 224 and IoU 0.99799 against classiCAD. Shadow
mask coverage is therefore sensitive to the camera-depth setup; keep this
case explicit when constructing a genuinely matched camera comparison. The
PNG rerun compares display-space effect deltas; the existing 2.10/255 result
above is from scene-linear captures and remains the preferred color metric.
- **WORLD cavity and object-outline effect-delta rerun (2026-10-13):** Added
  `tests/workbench_effect_delta_compare.py` to compare each renderer's enabled
  PNG against its own baseline before comparing effect deltas. The changed
  mask is a per-pixel max-channel RGB delta of at least 2/255; the report
  records each changed-pixel count, XOR/IoU, max-channel delta magnitudes,
  and signed RGB-delta error over the mask union. Blender references use the
  existing 640×480 `CUBE` fixture, orthographic scale 5, its established
  default 8-unit eye distance, direction `(4, -6, 4)`, AA Off, Studio
  `Default`, Material color, AgX, and zero dither. The fixture explicitly
  sets WORLD cavity, ridge/valley factors 1, SSAO samples 16, distance 0.2,
  attenuation 1, and black object-outline color. These cavity values match
  classiCAD's capture defaults. ClassiCAD uses the existing offscreen
  polygon-cube captures, with outline off for the cavity pair and World
  cavity off for its baseline; the outline pair changes only the outline
  toggle.
- At 2/255, WORLD cavity changes 47,927 Blender pixels and 48,018 classiCAD
  pixels (XOR 1,353; IoU 0.972189). Mean max-channel effect magnitude is
  4.0443/255 for Blender and 4.0348/255 for classiCAD. Signed RGB-delta error
  over the union is 0.06615/255 MAE, p95 1/255, maximum 16/255.
- The outline masks both contain 2,316 pixels (XOR 0; IoU 1.0). Mean
  max-channel effect magnitude is 19.1788/255 for Blender and 13.9650/255 for
  classiCAD. Signed RGB-delta error over the union is 3.55584/255 MAE, p95
  11/255, maximum 17/255. Coverage matches; edge intensity still differs.
- Normalizing each capture's per-pixel maximum darkening by its own maximum
  baseline channel, while excluding baseline maxima below 8/255, gives mean
  ratios 0.19156 (Blender, 2,312 pixels) and 0.21237 (classiCAD, 2,316
  pixels); medians are 0.14894 and 0.17647. The aggregate darkening-to-base
  ratios are nearly identical: 0.18491 and 0.18538. Thus the differing mean
  byte deltas track different baseline brightness/color distributions, not a
  clear global opacity mismatch. The per-pixel ratio distributions still
  differ, and display-space ratios are only a proxy because AgX is nonlinear;
  a matched scene-linear baseline is needed to attribute that residual to
  alpha versus shading.
- These are display-PNG delta comparisons; classiCAD's opt-in polygon effect
  capture currently exports PNG rather than scene-linear PFM. No production
  renderer change is justified by these metrics alone. The classiCAD capture
  command was `ctest --test-dir build -R '^viewport_render_contracts$'
  --output-on-failure` with `CLASSICAD_VIEWPORT_POLYGON_CUBE_CAVITY_CAPTURE`,
  `CLASSICAD_VIEWPORT_POLYGON_CUBE_CAVITY_BASELINE_CAPTURE`,
  `CLASSICAD_VIEWPORT_POLYGON_CUBE_OUTLINE_CAPTURE`, and
  `CLASSICAD_VIEWPORT_POLYGON_CUBE_OUTLINE_BASELINE_CAPTURE` directed to
  separate files. Blender used
  `blender --background --factory-startup --python
  tests/workbench_cube_reference.py -- <capture.png> --aa OFF --lighting
  STUDIO --light-preset Default --color-source MATERIAL --zero-dither --effect
  <BASELINE|CAVITY|OUTLINE>`; the comparison command is
  `python3 tests/workbench_effect_delta_compare.py <blender-baseline.png>
  <blender-effect.png> <classiCAD-baseline.png> <classiCAD-effect.png>`. The
  1000-unit
  `--match-classicad-ortho` variant is excluded: changing orthographic depth
  and clipping changes effect masks even when framing is unchanged.
- **Depth-of-field comparison (2026-10-13):** Added
  `tests/workbench_dof_reference.py` and `tests/workbench_dof_compare.py` for
  matched 640×480 Blender 5.2.2 and classiCAD baseline/DOF captures. The
  Blender fixture disables render dithering and matches the classiCAD test's
  perspective camera, cube, focus, f-stop, and aperture shape. At a 2/255
  per-pixel max-channel effect threshold, the Blender/classiCAD masks contain
  11,647/11,391 pixels (IoU 0.9274; XOR 868); at 3/255 they contain
  10,059/10,001 pixels (IoU 0.9592; XOR 418). Mean max-channel effect
  magnitudes are 0.8155 and 0.7967/255. The unblurred baseline images already
  differ by 1.4048/255 mean max-channel error, with 98,729 pixels above 2/255
  and 44,770 above 3/255. The mask result is therefore sensitive to display
  quantization. Source review found the tested CoC, aperture mapping, sample
  layout/jitter, gather, median, and resolve equations align with Blender;
  there is not yet evidence for a production renderer fix. Broader cameras,
  aperture settings, temporal sampling, and output-space parity remain open.
- The expanded effect comparisons currently use final PNGs. A future capture
  should expose scene-linear per-pass inputs and outputs to separate outline
  opacity from differences in the underlying face color and display encoding.
- World cavity's first measured mask mismatch was caused in part by the depth
  sampler: classiCAD set its copied `cavityDepthTexture_` to linear filtering,
  while Blender binds its Workbench depth texture with the default sampler,
  which disables linear filtering. classiCAD now uses nearest filtering so
  jittered cavity samples do not blend depths across face and silhouette
  boundaries. Pinned references are Blender's
  [`workbench_mesh_passes.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_mesh_passes.cc#L1153-L1165)
  depth binding and [`GPU_texture.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/gpu/GPU_texture.hh#L2660-L2678)
  default filtering definition. Blender's cavity function also reads a
  per-pixel normal from the normal G-buffer; classiCAD now supplies its shared
  prepass normal texture rather than the forward color pass interpolant. A
  later precision audit found that direct XYZ storage only matches Blender's
  encoded G-buffer normal before quantization, not after storage and decode;
  classiCAD now follows the two-channel encoding described in
  [`workbench_cavity.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_cavity.bsl.hh#L463-L480)
  and [`workbench_common.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_common.bsl.hh#L368-L405).
- At 640×480 with an RGB effect-delta threshold of 2, the classiCAD changed
  mask moved from 20,503 to 27,002 pixels after nearest filtering; Blender's
  reference mask is 26,048 pixels. XOR fell from 9,919 to 3,862, and IoU rose
  from 0.649 to 0.864. Reading the shared per-pixel normal then reduced XOR to
  3,847 and raised IoU to 0.865. Effect-delta MAE over the final mask union is
  0.400/255, p95 is 1/255, and maximum error is 5/255. The depth-filter change
  explains most of the mismatch; residual mask coverage remains open. The
  earlier proposed background-offset sign change remains reverted: source
  review confirms `cavityViewPosition` has Blender's negative-forward depth
  sign. A follow-up comparison of the remaining 3,847-pixel mask XOR against
  Blender's cavity shader, sample generation, and 64×64 jitter construction
  found no further semantic mismatch for the tested projection. Do not alter
  the cavity equation based on the mask metric alone; the next useful
  diagnostic is to compare cavity and edge amounts plus each sampled
  depth/normal at mismatch pixels, separating prepass alignment from numerical
  threshold differences.
- Repeating the cavity comparison with the matched six-quad `PolygonMesh3D`
  cube leaves a changed-mask XOR of 3,055 pixels (25,813 classiCAD versus
  26,048 Blender, IoU 0.889). Shared changed-pixel effect-delta RGB MAE is
  12.18/255, p95 29/255, max 55/255. Matching topology reduces the mask
  mismatch from the NURBS-solid capture but does not explain it away; the
  sampled-depth/normal and cavity-amount diagnostic remains the next step.
- The subsequent source check found a normal-path difference: Blender's
  effect prepass flips back-facing fragment normals toward the camera and
  stores them using the two-channel sphere-map encoding, while classiCAD had
  stored direct XYZ normals. The classiCAD prepass now applies Blender's
  `normal_encode(front_facing, normal)` formula in the existing RGBA16F
  attachment; cavity and curvature resolve decode the quantized XY channels
  with Blender's `normal_decode` formula. The attachment allocation remains
  unchanged. On the matched cube at
  RGB threshold 2, classiCAD's changed mask is 27,008 pixels versus Blender's
  26,048 (XOR 3,848, IoU 0.865); within the shared changed mask, effect-delta
  RGB error drops to MAE 0.387/255, p95 1/255, max 6/255. This aligns effect
  intensity but leaves mask coverage less exact, so the cavity comparison is
  still open. A subsequent focused offscreen contract passed after verifying
  the encoder was compiled into the effect-prepass shader. The encoded-normal
  capture changed effect-delta MAE/RMSE only from 0.131194/0.388267 to
  0.131150/0.388183; threshold-2 mask IoU moved from 0.864322 to 0.864119.
  This confirms the source-aligned path runs but shows no material coverage
  improvement. A threshold sweep found that the residual XOR is concentrated
  at the 2-versus-3 LSB boundary: at threshold 3, 2,400 of 2,418 classiCAD-only
  pixels have effect deltas of 3 versus Blender's 2, while 1,448 of 1,450
  Blender-only pixels have the reverse. The mask XOR falls from 3,868 pixels
  at threshold 3 to 541 at threshold 8; shared-mask effect-delta MAE is
  0.399/255, p95 1/255. This indicates a near-threshold output difference
  rather than broad missing cavity coverage. Blender reference:
  [`workbench_common.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_common.bsl.hh#L368-L405).
- A further source comparison found that Blender's opaque G-buffer state uses
  `DRW_STATE_DEPTH_LESS_EQUAL` in
  [`workbench_mesh_passes.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_mesh_passes.cc#L1160-L1172),
  while classiCAD's effect prepass used `GL_LESS`. The prepass now uses
  `GL_LEQUAL` so equal-depth fragments at shared mesh edges select normals
  consistently with Blender. This is a source-backed rasterization mismatch;
  a fresh matched-cube capture changed 83 cavity-render pixels and no baseline
  pixels. At the RGB max-channel threshold of 3, the classiCAD effect mask is
  27,005 pixels versus Blender's 26,048; mask XOR is 3,861 and IoU is 0.864.
  Relative to the capture before this state change, XOR moved from 3,848 to
  3,861, so the depth comparison difference is real but does not explain the
  residual mask coverage. On the 24,596-pixel shared changed mask, effect
  delta RGB channel MAE is 0.400/255, p95 1/255, maximum 16/255. No further
  source-backed discrepancy in the tested centered projection was found.
- An opt-in `CLASSICAD_VIEWPORT_CAVITY_DIAGNOSTIC_PREFIX` capture now runs the
  fixed six-quad cube through the production scene-pass coordinator with
  `Both` cavity effects and writes the pre-composite scalar channels, shared
  depth, and encoded normal before display resolve. The diagnostic shader
  writes into the existing RGBA16F color attachment only when explicitly
  enabled; it does not alter MRT setup or the default render path. The focused
  `viewport_render_contracts` test passes with capture enabled. The 640×480
  capture has 111,480 geometry pixels (depth 0.49915–0.50036); R cavity amount
  ranges 0–0.34790 (mean 0.01234), G edge amount 0–0.46631 (mean 0.03963),
  and B screen-curvature amount −0.71387–1.0 (mean 0.00979). The shared normal
  attachment stores the two-channel sphere-map encoding in RG; B is reserved
  zero, not decoded normal Z. Outside the geometry mask, the color target
  retains its 0.02478 clear value, so diagnostic scalar analysis must mask by
  shared depth. Raw depth and encoded XY are available at every geometry
  pixel. This capture validates the inputs and makes local amount inspection
  repeatable, but does not by itself identify the remaining Blender output
  mismatch: Blender's exposed render result does not provide its internal
  scalar cavity/edge/curvature buffers for direct per-pixel comparison.
- The first transparent overlap capture isolated a resolve-coordinate bug, not
  a geometry or OIT accumulation bug. The weighted resolve used an oversized
  fullscreen triangle with `gl_Position = p * 2 - 1` but `vUv = p * 0.5`, so
  it sampled only the lower-left quarter of the accumulation texture and
  stretched that quarter over the viewport. The resolve now uses `vUv = p`.
  The offscreen contract checks the known scene bounds to within two pixels.
  At 640×480, both classiCAD and Blender cover `(134,86)`–`(479,393)`; at RGB
  distance thresholds 8 and 20, the masks have 34 and 4 XOR pixels, with IoU
  0.99968 and 0.99996. On the corrected depth-matched PNG captures, the same
  masks remain (34 and 4 XOR pixels), background-subtracted shared-foreground
  RGB MAE is 2.70/255, p95 is 4/255, and maximum error is 16/255. Center
  overlap probes are `(117,90,124)` and `(117,90,125)`. The earlier 8-unit
  reference probes `(121,89,121)` used different OIT depths and are superseded.
  This validates the OIT shader against a Workbench scene camera with matched
  projection depth, not Blender's live orthographic viewport matrix. The
  no-shared-depth sorted-transparency fallback remains less capable than
  Blender's per-pixel OIT for intersecting ranges.

### Transparent and in-front overlap probes (2026-10-13)

- Added a matched top-orthographic fixture with regular opaque, regular
  transparent, `show_in_front` opaque, and `show_in_front` transparent quads.
  The Blender 5.2.2 fixture runs AA Off at 640×480 with the same 1000-unit
  projection eye and 0.01/2000 clip range as the classiCAD offscreen capture.
  Isolated regular-transparency and front-domain overlap probes are sampled
  from scene-linear Combined and view-Z.
- The first run exposed a shared front-attachment merge bug. Its fullscreen
  triangle used `vUv = p * 0.5` despite `gl_Position = p * 2 - 1`, stretching
  the lower-left quarter of the front color/depth/ID attachments over the
  output. The merge also disabled depth testing while writing `gl_FragDepth`,
  suppressing the shared-depth update. The merge now samples `vUv = p` and
  uses `GL_ALWAYS` with depth writes enabled. A focused offscreen contract
  checks the projected front overlap color and foreground depth.
- After that correction, scene-linear RGB at the front overlap probe `(365,
  240)` matches Blender within 0.001 per channel: Blender `[0.4192, 0.2170,
  0.0574]`, classiCAD `[0.4194, 0.2170, 0.0574]`. At `(450, 240)`, both return
  `[0.2457, 0.1956, 0.0857]`. The front depth-mask probe is clear (`1.0`) at
  `(150, 240)` and about `0.4998` over
  the front patch. The regular transparent range is in classiCAD's OIT draw
  list with alpha `0.55`, and its depth output is `999.75`, but the regular
  accumulation and weight attachments remained zero. A focused draw-state
  diagnosis found that the front merge releases its fullscreen
  `outlineVertexArray_`, leaving VAO 0 active in the core profile. The regular
  OIT path then called `glDrawArrays` through the surface draw helper without
  restoring `vertexArray_`, so its fragments never reached either color
  attachment. The OIT entry now rebinds the Solid program, surface VAO, and
  vertex buffer before drawing. The regression capture asserts the isolated
  Blender probe within 0.01 per scene-linear channel. Before the fix,
  classiCAD returned `[0.08563, 0.29346, 0.11285]` (opaque base) versus Blender
  `[0.0776, 0.2106, 0.2900]`; after the fix classiCAD returns
  `[0.07770, 0.21057, 0.29004]`. The front mask is clear (`1.0`) at this
  probe. Blender's render-result Z reads the main depth attachment after the
  late overlay-depth pass; for in-front geometry the measured result is the
  near plane (`0.0100135`). Blender first merges actual front-domain depth into
  scene depth for later scene passes: `workbench_merge_depth_frag.glsl` copies
  the sampled front depth. `AntiAliasingPass::draw` then submits
  `overlay_depth_ps_` after Workbench's volume, outline, and DOF passes, and
  `workbench_overlay_depth_frag.glsl` sets in-front pixels to device depth zero
  so the Overlay engine stays behind them. classiCAD now follows that split:
  the front merge preserves sampled depth, and `finishSceneSample` applies the
  near-plane override immediately before AA. The offscreen contract checks
  merged scene depth against the isolated physical front depth before the
  override, and still checks final front overlap Z against the active near
  clip. Regular transparent view-Z at `(150, 240)` remains `999.75` in both
  apps.
- Pinned Blender references: [`workbench_mesh_passes.cc` opaque/front
  routing and depth merge](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_mesh_passes.cc#L109-L201),
  [`workbench_mesh_passes.cc` transparent and depth passes](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_mesh_passes.cc#L232-L371),
  [`workbench_merge_depth_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_merge_depth_frag.glsl#L7-L13),
  [`workbench_effect_antialiasing.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_effect_antialiasing.cc#L133-L140) and its late invocation at [lines 243-253](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_effect_antialiasing.cc#L243-L253),
  [`workbench_overlay_depth_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_overlay_depth_frag.glsl#L7-L13),
  and [`workbench_engine.cc` pass order](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_engine.cc#L496-L504).
- Separate QPainter caveat: weighted OIT applies cavity in scene-linear RGB
  before transparency. The bounded sorted fallback applies cavity before
  transparency composition to its ARGB32 display-RGB opaque layer, so nonlinear
  display transforms still produce different cavity results from Blender. The
  GPU overlap fixture does not validate this QPainter color-space difference.
- QPainter weighted OIT also accumulates every depth-visible transparent
  triangle sample now, including depth-separated surfaces within one object,
  after opaque-depth rejection. Per-object sorting removes duplicate samples
  only when pixel, depth, color, and alpha all match, avoiding double weight
  from shared triangle edges while retaining distinct surfaces. Captured and
  accumulated storage is capped at four million fragments and overflow returns
  to the existing fallback. The headless `viewport_render_contracts` test
  checks one object with two separated translucent sheets.
- These 2026-10-13 updates supersede older matrix and stage notes below that
  describe QPainter's regular surface composition as display-space object-layer
  sorting or say it collapses all transparent triangles within each object.
  The bounded fallback used when data or memory limits prevent this compositor
  remains average-depth sorted. The cavity section above likewise supersedes
  older descriptions of omitting cavity under visible translucent surfaces;
  its display-space fallback limitation remains current.

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

### Background and color-management comparisons (2026-10-13)

- Expanded the AgX built-in display LUT from 65³ over [0, 2] to 97³ over
  [0, 16], retaining the `log2_1p` shaper (`log2(1 + 64x)`). Against Blender
  5.2.2 OCIO, the generated table's sampled maximum absolute error is
  0.000186 through scene-linear 16. The wider range prevents bright exposed
  values from flattening at the former LUT ceiling. Values above 16 still
  clamp. Other built-in transforms remain 65³.
- Fixed Blender scene color-management extraction to use the supported
  `getRoleColorSpace(ROLE_SCENE_LINEAR)` API. A Blender 5.2.2 contract checks
  custom view/look LUT baking and the Standard transform's values above 1.
  Gamma zero is retained on import and implemented with Blender's
  `1 / FLT_EPSILON` exponent; display values are bounded before exponentiation
  to avoid overflow, and exact Standard white is preserved across float
  rounding.
- Compared five background/color cases using a matched cube fixture and eight
  background probes per capture: WORLD and VIEWPORT backgrounds under AgX and
  Standard, plus Standard with exposure 1 and gamma 2. All five scene-linear
  outputs matched exactly. Display output matched exactly except AgX/Standard
  WORLD, whose maximum difference was 1/255 (mean 0.3333/255). THEME could not
  be compared headlessly because it uses viewport preferences and Blender's
  OpenGL capture is unavailable in this environment; a scene render is not a
  valid substitute for that viewport path.
- Verification: the C++ viewport render contract passed (1/1), the Blender
  color-management contract passed on Blender 5.2.2, the AgX LUT verifier
  passed, and `git diff --check` passed. The full core contract executable
  still reports existing perspective/zoom, snapping, tangent/perpendicular
  tool, and dimension failures outside this category; its serialization
  contract passed.

### QPainter interactive World background conversion (2026-10-13)

- Blender's Solid viewport overlay supplies the World color as scene-linear
  data and relies on the framebuffer's sRGB conversion. The QPainter viewport
  fallback had applied the scene view transform, exposure, and gamma to this
  background. It now uses only scene-linear-to-sRGB conversion, matching the
  viewport background path. The Workbench render-result background continues
  to use its separate scene-linear resolve.
- Source: Blender 5.2.2
  [`overlay_background.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/overlay/overlay_background.hh#L539-L563).
- A CPU contract checks the converted viewport color. `cmake --build build -j4`
  passed. Running `QT_QPA_PLATFORM=offscreen
  ./build/classicad_viewport_render_contracts_test` produced no CPU contract
  failures, then stopped at the existing NURBS solid OpenGL-context
  requirement; this did not validate the GPU path or provide an image match.

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
- The future shader system can provide Workbench's supported material values through `WorkbenchMaterialData` and existing mesh/draw-range data without changing the render-pass API or creating a second material model; arbitrary node graphs are out of scope. Imported UDIM metadata and packed tile bytes survive native save/reopen; GPU and QPainter mesh/trimmed-NURBS/untrimmed-NURBS color and effect-alpha paths consume tiles. GPU depth-map shadow alpha uses texture data and UVs for meshes, trimmed/untrimmed `NurbsSurface` ranges, and object-textured `NurbsSolid` ranges; geometry without an available image/UV alpha source remains unmasked, while stencil shadow-volume casters do not sample texture alpha. Per-face seed materials on `NurbsSolid` resolve through stable semantic IDs and classiCAD `.blend` proxy assignments round-trip by face ID; materialized-face lineage remains unsupported. The QPainter Solid fallback has no shadow pass; CPU shadow rendering is out of scope. GPU per-tile extension modes are implemented. Cross-tile filter continuity is not part of Blender 5.2.2 behavior.
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
- **Theme-background history invalidation (2026-10-13):** the AA key now
  includes the active theme background style and both colors. Changing the
  theme therefore restarts accumulation instead of presenting a cached frame
  with stale background pixels; unused theme values do not reset World or
  Custom backgrounds. Both key assertions passed in the offscreen viewport
  contract before that test reached its later OpenGL-only NURBS-solid check.
  Blender resets Workbench TAA when shading state changes and clears each
  sample with the current background ([`workbench_state.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_state.cc#L25-L89),
  [`workbench_effect_antialiasing.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_effect_antialiasing.cc#L145-L149)).
- **Wheel zoom interactive AA state (2026-10-13):** wheel zoom now enters the
  same interactive one-sample mode used by other camera navigation while a
  200 ms single-shot settle timer is active. Each new wheel event restarts the
  timer; expiry requests a frame after `isNavigating()` returns false so idle
  temporal accumulation resumes. This matches Blender Workbench's navigation
  sample clamp and TAA reset path in
  [`workbench_state.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_state.cc#L1303-L1382).
  A focused viewport interaction contract covers entry, repeated-wheel timer
  restart, settling, and the redraw that resumes idle accumulation. The settle
  duration is classiCAD's interaction policy; Blender source establishes the interactive AA behavior,
  not this timer value.
- Blender Workbench uses the configured object-outline color for all object
  IDs. The GPU outline pass now uses that color for both selected and
  unselected objects; CAD selection feedback remains in its separate overlay
  path. An offscreen contract compares selected and unselected silhouette
  pixels against matching outline-free baselines. QPainter outline composition
  and broader effect pixel comparisons remain open.
- **Workbench effect UI-scale sampling:** GPU outline, QPainter outline, and
  QPainter curvature cardinal taps now share an integer scale adapter
  (`max(1, qRound(DPR))`), matching Blender's integer `U.pixelsize` footprint
  for the available Qt scale. Blender assigns `U.pixelsize` to Workbench's
  `world_data.ui_scale` in [`workbench_resources.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_resources.cc#L100-L109)
  and derives it from DPI and the user line-width adjustment in
  [`wm_window.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/windowmanager/intern/wm_window.cc#L682-L718).
  Contracts cover DPR 1, 1.5, and 2, including the QPainter edge footprint at
  1.5. The custom Blender DPI and line-width adjustments have no classiCAD
  setting and remain unsupported; broader image comparison also remains open.
- **QPainter screen curvature:** the software effect now follows Blender's
  four-cardinal object-ID outline rejection, view-space normal-component
  differences, signed ridge/valley response, and soft clamp from
  [`workbench_curvature.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_curvature.bsl.hh#L9-L50).
  The focused contract checks ridge and valley signs, separate ridge-control
  response, outline rejection, and background rejection. Its neighbor step
  rounds Qt DPR, which can differ from Blender's DPI-derived `ui_scale`; the
  retained QPainter surface layers also remain display RGB, so the sorted
  fallback cavity/effect composition does not match Blender's scene-linear
  ordering.
  Opaque effect normals now also pass through the Blender prepass facing
  adjustment and sphere-map encode/decode at half-float precision before the
  QPainter cavity/curvature consumers use them. This closes the prior CPU-only
  difference where interpolated full-precision normals bypassed the GPU's
  RG16F normal buffer. The contract checks equivalent front- and back-facing
  normal inputs. See Blender 5.2.2
  [`workbench_prepass.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_prepass.bsl.hh)
  and [`workbench_common.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_common.bsl.hh#L368-L407).

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

## QPainter opaque shadow fallback (2026-10-13)

- Added a software light-depth pass to QPainter Workbench effects for opaque
  triangulated NURBS tessellation and polygon meshes. The pass uses the scene
  display shadow direction, intensity, and offset. Transparent surfaces are
  excluded as casters and receivers; texture-cutout pixels use the existing
  alpha sampler in the light-depth pass. This is a bounded approximation;
  Blender Workbench uses stencil shadow volumes in its normal GPU path.
- Contracts cover shadow activation, direction-driven mask movement, zero
  intensity, monotonic intensity response, and a fully transparent textured
  caster. The full build passed. The offscreen viewport contract reported no
  failed assertions through the CPU shadow checks, then exited 1 at its later
  existing OpenGL-context requirement for the NURBS-solid display case; no
  GUI/OpenGL validation is claimed.

## Phase Two: imported scene materials and result contracts (2026-10-13)

- **Render-result shading source:** an imported Blender scene's saved
  `Scene.display.shading` settings now drive its Workbench render result.
  Interactive `View3D` shading remains the render setting fallback for native
  classiCAD documents. Scene render anti-aliasing/output values remain sourced
  from their render settings. The shading-source selection contract is covered
  in `viewport_render_contracts`; the test's later GPU case cannot initialize
  an OpenGL context here, so this change has no rendered image comparison yet.
  Blender's render engine and Workbench state initialization are in pinned
  [`workbench_engine.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_engine.cc)
  and [`workbench_state.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_state.cc).
- **NURBS Surface image materials:** imported surface objects now resolve the
  referenced Workbench material, including its scene-linear color and scalar
  values plus the first usable active image record. Resolution preserves UDIM
  tiles first, then packed regular image bytes, then an existing external
  image path. A standalone C++ contract covers those choices and image
  metadata; the Blender-side fixture covers extraction only. Blender's active
  image sampling and UDIM lookup are documented by pinned
  [`workbench_image_lib.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_image_lib.glsl#L348-L433).
- **Native object-image persistence:** object-level Workbench regular image
  paths, packed bytes, and alpha mode now survive native save/reopen. Legacy
  records receive empty path/packed data and `STRAIGHT` alpha; malformed or
  oversized base64 payloads are rejected. The focused persistence contract
  passes. Existing UDIM tile persistence remains covered in the same contract.
- **Combined output format honesty:** Workbench Combined output no longer
  silently changes unsupported Blender formats to PNG. It reports unavailable
  Qt/OpenEXR writers, rejects multilayer EXR in the single-layer writer,
  requires a suffix matching the selected format, and normalizes a mismatched
  suffix in the save dialog. The isolated format contract passes for supported
  and unsupported formats, suffix checking, and HDR output. This addresses
  format reporting and encoding, not Blender image-codec parity for every
  compression/depth option.
- Validation for this batch: `cmake --build build -j4` succeeds.
  `blender_surface_material_resolution_contracts`,
  `workbench_persistence_contracts`,
  `workbench_render_result_format_contracts`, and
  `viewport_wheel_navigation_aa` pass. `viewport_render_contracts` reaches its
  existing OpenGL-only NURBS-solid check and exits because this environment
  cannot create the required context; no GPU visual validation is claimed.

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

### Render output extension preference (2026-10-13)

- Blender scene extraction now records `RenderSettings.use_file_extension`;
  native scene metadata retains it, and validation rejects a non-boolean value.
  Workbench Combined export uses the imported preference: enabled appends or
  normalizes to the format extension, while disabled preserves the exact path
  chosen in the save dialog and writes the selected format without adding a
  suffix. Empty paths use `WorkbenchRender` when extensions are disabled.
- Contracts cover native scene metadata round-trip, both path policies, and an
  actual extensionless PNG write. `cmake --build build -j4` and the focused
  `workbench_persistence_contracts` and
  `workbench_render_result_format_contracts` CTests pass. The Blender
  extraction fixture itself was not run in this environment.
- Blender documents the behavior in the pinned [RenderSettings API](https://docs.blender.org/api/5.2/bpy.types.RenderSettings.html#bpy.types.RenderSettings.use_file_extension).

### Update-session color management persistence (2026-10-13)

- Update-session snapshots now retain the viewport color transform, imported LUT
  ID, LUT bytes and metadata, exposure, gamma, and dither intensity. If the
  active shading snapshot refers to an imported LUT whose bytes are held by the
  document, the session writer copies those bytes into the snapshot. The reader
  validates transform ranges, LUT metadata and canonical Base64, scalar bounds,
  and custom-LUT byte count and SHA-256 identity. Older version-6 sessions that
  omit these fields keep the established defaults.
- `update_session_contracts` checks custom LUT/scalar round-trip, legacy
  defaults, and rejection of a malformed transform. `cmake --build build -j4`
  completed and the focused CTest passed. The broader `trim_seam` executable
  still reports three pre-existing box-selection assertion failures and does
  not serve as evidence for this focused persistence contract.
