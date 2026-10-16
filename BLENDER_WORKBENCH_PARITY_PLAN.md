# Blender Workbench and Viewport Anti-Aliasing Port Plan

## Goal

classiCAD is being built as a Blender-like modeling application with NURBS as
its native geometry foundation and Blender project import as an interoperability
path. Its objects should look and feel like Blender's for equivalent scenes and
settings, while remaining editable through classiCAD's own NURBS geometry and
tools.

This Phase Two mission establishes that foundation through the Solid viewport
and render path. Port Blender 5.2.2 Workbench's visible shading, material,
color-management, effects, and anti-aliasing behavior into classiCAD's C++/Qt
application. Preserve the Workbench material adapter and classiCAD's exact
geometry contracts so a later shader system can supply Blender-compatible
material values without replacing the native model or tessellation data.

Blender's relevant rendering behavior and pass structure are implementation
references; classiCAD remains a separate application rather than importing
Blender's entire internal architecture. The work should leave clean seams for
later Blender-like features, including an Outliner, Object Mode, and Edit Mode.
Those interaction modes and panels are future work, not a reason to fabricate
their geometry or UI contracts during this viewport phase.

Use Blender 5.2.2 as the reference version. The bundled Workbench lights,
MatCaps, and color-management data in this repository already come from
Blender 5.2.2.

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
X-Ray, preview, or cavity diagnostic output is active. The C++ build and
offscreen `viewport_render_contracts` now pass with an active OpenGL context.
Matched Blender image comparison remains open.

**MatCap G-buffer resolve update (2026-10-13):** The same eligible deferred
resolve now handles MatCap using the existing Blender-derived MatCap UV
mapping and two-layer linear half-float texture: diffuse multiplies the
scene-linear material color, while specular is added when enabled. Horizontal
flip, perspective incident reconstruction, orthographic view direction, and
the independent front-object material/normal merge are retained. If the MatCap
resource cannot be prepared, the existing forward output is left in place.
This path shares the Studio restrictions above. The C++ build and offscreen
`viewport_render_contracts` pass with an active OpenGL context. Matched Blender
image comparison remains open.

**Deferred MatCap back-face orientation (2026-10-13):** The deferred MatCap
resolve now applies the sign stored in the material payload to the decoded
normal before computing MatCap UVs. This matches Blender 5.2.2's prepass
facing payload and composite normal flip, while leaving the payload magnitude
available for the selected marker. Cavity keeps the unflipped prepass normal,
matching Blender's independent cavity normal input. The C++ build and offscreen
`viewport_render_contracts` pass with an active OpenGL context. A later
two-sided GPU capture and matched Blender image comparison are recorded below.

**Deferred MatCap back-face comparison (2026-10-14):** Added an opt-in paired
GPU capture and Blender 5.2.2 `TWO_SIDED_MATCAP` fixture using two adjacent
35-degree tilted faces with opposite winding, `check_normal+y`, no specular,
cavity, outline, or shadows, and the same 640x480 orthographic camera. The
contract now samples each panel and requires a visible green-channel response
between the two orientations, so a fixture collapsed to the center MatCap
sample cannot silently pass. `tests/workbench_matcap_backface_compare.py`
compares 31x31 scene-linear interior probes at (190, 240) and (450, 240): the
front-wound Blender/classiCAD RGB values are (0.171143, 0.026321, 0.649414)
and (0.171631, 0.026321, 0.649902); the reversed-wound values are
(0.172119, 0.466064, 0.650391) and (0.171631, 0.466064, 0.650391). Maximum
per-probe channel error is 0.0004883 scene-linear, while the green separation
is 0.439743 in both captures. This verifies the deferred orientation path for
this preset and view; other MatCaps, camera directions, specular combinations,
and full-frame edge parity remain open. Reproduce with
`CLASSICAD_VIEWPORT_MATCAP_BACKFACE_CAPTURE_DIR=/tmp/classicad-two-sided-matcap QT_QPA_PLATFORM=offscreen ./build/classicad_viewport_render_contracts_test`,
the Blender fixture command used by `tests/workbench_cube_reference.py`, and
`python3 tests/workbench_matcap_backface_compare.py /tmp/blender-two-sided-matcap.pfm /tmp/classicad-two-sided-matcap/two-sided-matcap.pfm`.

**Opaque cavity resolve update (2026-10-13):** Eligible scene-linear opaque
resolves now apply Workbench cavity and curvature from the shared depth,
view-normal, object-ID, and blue-noise inputs before transparent accumulation.
The resolve uses the same per-AA-sample cavity sequence and settings as the
forward shader. If the shared prepass or required SSAO depth snapshot is
unavailable, it keeps the forward color result. X-Ray, previews, and cavity
diagnostic output still use the forward route. The C++ build and offscreen
`viewport_render_contracts` pass with an active OpenGL context; matched output
validation remains open.

**Deferred selection-tint update (2026-10-13):** Selected opaque ranges can now
use the material resolve. The G-buffer payload preserves Blender's Flat,
Studio roughness/metallic, and MatCap face-orientation data while carrying a
selected marker in otherwise unused payload values. The resolve applies the
same orange theme tint and 51/255 mix after lighting and cavity. This also
survives the existing `showInFront` material-data merge. Preview and selection
overlay draws remain on their existing path. The C++ build and offscreen
`viewport_render_contracts` pass with an active OpenGL context. Matched visual
comparison remains open.

**Opaque shadow-mask resolve update (2026-10-13):** The shared scene sample now
owns an `R16F` shadow-factor attachment, with a separate attachment for the
`showInFront` domain. The existing opaque shader writes the exact multiplier
it already applies, including stencil-volume visibility, depth-map comparison,
shadow focus/offset, and intensity. The front factor merges with front color,
material, depth, and normal data; the deferred resolve applies it after cavity
and before selected tint. The mask clears to neutral visibility 1. Transparent
objects keep their existing direct shading path. The C++ build and offscreen
`viewport_render_contracts` pass with an active OpenGL context. Matched Blender
output comparison remains open.

**Deferred resolve shader scope correction (2026-10-13):** A source audit found
that the embedded GLSL referenced `selected`, `normal`, and `incident` outside
the blocks where they had been declared. These values now have function scope;
the view normal is fetched only for Studio, MatCap, or cavity, and the
perspective incident direction is reconstructed only for Studio or MatCap.
Headless OpenGL validation later exposed `packed` as a reserved GLSL identifier
in the material resolve. It is now named `materialBits`. The C++ build passes,
and both `viewport_render_contracts` and `workbench_render_pass_capture` pass
with `QT_QPA_PLATFORM=offscreen`; matched Blender image comparison remains
open.

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
alpha. A focused contract composites a 50%-alpha annotation over a 50%-alpha
display pixel and premultiplied scene-linear pixel, verifies source-over RGB
and alpha, preserves an untouched transparent pixel, then round-trips the
display result through PNG and TIFF. The build and format contract pass. This
verifies classiCAD's alpha-composition contract; matched Blender saved-file
annotation comparison remains open.

**Transparent-film Combined comparison (2026-10-14):** Added a paired
`TRANSPARENT_OVERLAP` capture for Blender 5.2.2 and classiCAD with a transparent
film, a zero-alpha background, and overlapping semitransparent surfaces. The
capture exports scene-linear RGB and alpha PFM files plus display PNGs. Alpha
matches exactly at all 307,200 pixels: the corner is zero and the overlap is
0.764648. On 104,592 common covered pixels, mean scene-linear RGB channel
errors are 0.0000114, 0.0000651, and 0.0001577; the 95th percentile maximum
channel error is 0.0002442. Display PNG alpha matches exactly and RGB differs
by at most one 8-bit code value. The comparator enforces these bounds. PNG and
TIFF writer contracts also round-trip a 50%-alpha RGBA64 premultiplied pixel
and transparent black. The build, render-pass capture, and writer contract
pass. Capture with `CLASSICAD_WORKBENCH_PASS_SCENE=TRANSPARENT_OVERLAP`, then
run `tests/workbench_transparent_film_compare.py` against the Blender reference
outputs. Annotation composition over semitransparent scene pixels remains
open.

### QPainter perspective cavity footprint (2026-10-13)

- The QPainter SSAO sample step now scales per receiver in perspective views by
  `viewTargetDepth / receiverDepth`, using the viewport camera position,
  direction, and target. The view-scale-derived radius now has only a 1-pixel
  minimum; the temporary 64-pixel ceiling was removed after a source-backed
  large-distance comparison. Sampling count, attenuation, and curvature pass
  are unchanged. Orthographic SSAO keeps its prior constant footprint.
- This is scoped to the QPainter cavity approximation. It does not change the
  GPU cavity shader, radius/depth attenuation, background reconstruction,
  curvature sampling, or claim visual parity with Blender; the later matched
  comparisons below record the remaining fallback differences.

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

**X-Ray and culling interaction contract (2026-10-15):** The offscreen GPU
contract now renders an overlapping front/back cube with Solid X-Ray and
backface culling both enabled and disabled. It requires culling to change the
center pixel by removing the rear contribution while preserving the nearest
front depth. This covers the color/depth interaction; a matched Blender frame
comparison remains open.

### GPU Workbench X-Ray transparency, depth, and effects (2026-10-12)

- Solid X-Ray now routes every non-preview surface range through transparent
  accumulation, including ranges whose source material alpha is one. The
  Workbench color shader multiplies interpolated source alpha by the resolved
  X-Ray alpha once; the existing classiCAD `workbenchEffectiveAlpha` supplies
  that same single multiplication. The accumulation pass disables depth
  testing in X-Ray so the separate X-Ray depth-only pass used for CAD strokes
  cannot hide rear surfaces. The transparent-depth pass then records nearest
  surface depth and object IDs for the shared viewport targets.
- Blender's `xray_alpha` and `xray_alpha_wireframe` are now imported and kept
  separately in viewport session state. Solid X-Ray uses the former;
  Wireframe X-Ray uses the latter in surface, stroke, and fallback paths, and
  the X-Ray checkbox and opacity control edit the value for the active shading
  mode. Older classiCAD sessions default the added wireframe value to Blender's
  0.5 default.
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
  color falls back to depth-disabled sorted blending. The independent
  transparent-depth/object-ID pass now still runs when its shared depth,
  normal, and ID attachments are valid, so post effects retain nearest
  transparent coverage even when color OIT is unavailable. If those shared
  targets are unavailable too, the sorted fallback still cannot provide that
  coverage. X-Ray color, outline, and allocation-failure behavior still need
  matched visual comparison.
- The Solid X-Ray depth-only pass now applies the same texture-alpha cutoff to
  meshes and NURBS ranges as the GPU color and object-ID paths, including packed
  images and UDIMs; `NONE`, `IGNORE`, and `CHANNEL_PACKED` continue to
  bypass alpha masking. Blender 5.2.2's `TransparentDepthPass` creates opaque material
  subpasses in
  [`workbench_mesh_passes.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_mesh_passes.cc#L1438-L1478),
  which use the shared texture cutoff setup from `MeshPass` in the same file's
  [`init_subpasses` and texture subpass setup](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_mesh_passes.cc#L999-L1102).
  The GPU viewport render contract verifies a fully transparent packed image
  leaves X-Ray depth clear while alpha mode `NONE` writes depth. A matched
  Blender image comparison and a direct CAD-stroke-through-cutout image check
  remain open.

### GPU Workbench display-transform captures (2026-10-14)

- Added optional matched 640×480 polygon-cube captures for AgX at default
  exposure, Standard at default exposure, and AgX at +4 exposure. Blender 5.2.2
  and classiCAD use Studio Default lighting, Material color, AA Off, zero
  dither, the same orthographic camera, and the same scene-linear World color.
  The classiCAD contract confirms the scene-linear source pixels remain
  identical across all three display settings while the displayed images
  change as expected.
- `tests/workbench_display_transform_compare.py` compares stable cube
  interiors, excluding pixels whose 8-neighbor scene-linear change exceeds
  0.025. Across 107,888 common interior pixels, Blender and classiCAD match
  exactly in both scene-linear RGB and final display RGB for all three states.
  The +4 exposure case scales source-linear values by 16; the effective
  display-transform input reaches 4.7539 and has 77,195 interior pixels above
  1.0 in each renderer. Silhouette and face-boundary raster differences are
  outside this interior comparison; other lighting presets, colors, custom
  OCIO views, and broader image combinations remain open.
- Validation: full `cmake --build build -j4` passed with the 316-source
  dependency audit; offscreen `viewport_render_contracts` passed with all
  display captures enabled; Blender reported 5.2.2 LTS; and the comparison
  script completed on captures under `/tmp/classicad-display-transform`.

### GPU Workbench outline and world-cavity combination capture (2026-10-14)

- The optional viewport contract now captures a matched polygon cube in four
  states: baseline, outline, WORLD cavity, and outline plus WORLD cavity. Each
  state records the final display PNG and the scene-linear Combined RGB PFM.
  The capture uses the same 640×480 perspective view as the Blender reference,
  with Workbench Studio/Default lighting, Material color, AA Off, zero output
  dither, and AgX. The Blender 5.2.2 reference harness accepts the combined
  `OUTLINE_CAVITY` effect state. A comparison script reports per-effect change
  mask IoU and the scene-linear interaction residual
  `(outline+cavity − outline − cavity + baseline)`.
- With masks defined by per-pixel maximum channel change above 2/255, outline
  IoU is 1.0 (2,454 changed pixels in each renderer), cavity IoU is 0.9760,
  and combined IoU is 0.9771. At 3/255, outline remains 1.0, cavity is
  0.9863, and the combined state is 0.9875. Blender and classiCAD interaction
  residuals have mean max-channel magnitudes `3.8474e-5` and `3.8211e-5` in
  scene-linear RGB; their cross-renderer mean difference is `5.0639e-7`. The
  95th-percentile residual is zero because nonzero interaction is confined to
  fewer than five percent of pixels; maximum residual is `0.05969` in both.
  These measurements verify this one polygon-cube view and the effect
  combination. They do not close cavity magnitude parity, other geometry or
  camera cases, transparency combinations, or QPainter effect parity.
- Validation: full `cmake --build build -j4` passed with the 316-source
  dependency audit; offscreen `viewport_render_contracts` passed with the
  capture enabled. Blender reported 5.2.2 LTS. Captures and reports are under
  `/tmp/classicad-effect-combo`.

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
  before anti-aliasing, and classiCAD does the same. The offscreen GPU
  contract uses a known opaque background, confirms that coverage remains
  opaque with DOF on, checks that neighboring high-contrast surface colors
  smooth, and verifies
  that disabling camera DOF returns the baseline image. A diagnostic Blender
  5.2.2 Workbench cube capture also changed the backdrop with DOF enabled.
  The separate QPainter path retains its lower-resolution sampling and its own
  visual-comparison gap.

### Matched GPU orthographic-view DOF comparison (2026-10-15)

- Extended the opt-in production GPU capture and Blender 5.2.2 reference
  helper with an orthographic projection. Both render the same 640×480 cube,
  5-unit horizontal frame, 1000-unit camera/focus distance, 2000 mm lens,
  f/0.1 aperture, six blades, 0.2 rad rotation, and 1.2 aperture ratio. The
  long lens makes the cube's front/back depth separation visible without
  changing orthographic framing. The contract verifies both captures, opaque
  corner background, a measurable DOF delta, and exact baseline output when
  camera DOF is disabled.
- Against Blender 5.2.2, threshold-2 DOF-effect masks contain 7,052 Blender
  pixels and 5,057 classiCAD pixels (IoU `0.685317`, 2,261 XOR); at threshold
  3 the counts are 1,202 and 1,031 (IoU `0.555014`, 639 XOR). Mean per-pixel
  maximum-channel effect magnitude is `0.2060/255` for Blender and
  `0.1907/255` for classiCAD. Baseline output already differs by mean
  `1.1368/255` with 79,758 pixels above 2; DOF output differs by
  `1.1836/255` with 79,993 pixels above 2. The baseline contribution and
  differing effect masks prevent attributing the full output error to the DOF
  gather. This confirms measurable orthographic DOF and its camera gate, while
  leaving visual parity open; no production equation change was justified.
- Validation: `cmake --build build -j4` passed with the 317-source dependency
  audit; capture-enabled offscreen `viewport_render_contracts` passed (1/1);
  Blender reported 5.2.2 LTS; and `workbench_dof_compare.py` completed on the
  paired images. Reproduce classiCAD captures with
  `CLASSICAD_VIEWPORT_GPU_DOF_PROJECTION=ORTHOGRAPHIC
  CLASSICAD_VIEWPORT_GPU_DOF_CAPTURE=<path>_dof.png
  CLASSICAD_VIEWPORT_GPU_DOF_BASELINE_CAPTURE=<path>_baseline.png
  QT_QPA_PLATFORM=offscreen ctest --test-dir build -R
  '^viewport_render_contracts$' --output-on-failure`; render the Blender pair
  with `tests/workbench_dof_reference.py -- <prefix> --projection
  ORTHOGRAPHIC`.

**DOF gate correction (2026-10-12):** This supersedes older summary bullets
and the `Cavity and depth of field` matrix row below that describe GPU DOF as
perspective-gated or say QPainter shares the GPU gate. Blender's GPU path runs
whenever an active scene camera has DOF enabled and the shading option is on,
including orthographic and free-view projections. The QPainter approximation
remains perspective-only. Any later statement below that says GPU DOF is off
in ordinary free views or orthographic views is superseded by this correction.

### QPainter Workbench DOF median cleanup (2026-10-14)

- The CPU fallback now keeps the gathered image separate from the sharp-source
  resolve and applies Blender's component-wise 3x3 RGBA median when the CoC is
  above 9 pixels. The filter radius follows `clamp(CoC - 9, 0, 1)`; because the
  CPU image is full resolution, the maximum filter footprint spans two output
  pixels per side, matching one half-resolution texel. Contracts check the
  component-wise median of asymmetric channel values and suppression of an
  isolated impulse after a large-CoC gather. The production DOF contract also
  checks focus, subpixel CoC, and translation invariance.
- Blender 5.2.2 documents the stage order in the [`DofPass` overview](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_effect_dof.cc#L754-L768)
  and schedules the pass in [`blur2_ps_` setup](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_effect_dof.cc#L1065-L1076);
  the CoC gate, sampling radius, and component-wise 3x3 filter are in
  [`workbench_effect_dof_blur2_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_effect_dof_blur2_frag.glsl#L410-L540).
  QPainter gathers into its full-resolution CPU buffer, then maps the median
  cleanup to Blender's half-resolution texel centers and bilinearly samples
  that result during the full-resolution resolve. This adapts the cleanup
  stage without changing the separate CPU gather.

### QPainter DOF half-resolution median capture (2026-10-15)

- The QPainter 3x3 component-wise median now runs on a half-resolution buffer,
  using the output-to-half texel scale for its radius offsets. The final
  full-resolution resolve bilinearly samples the filtered half-resolution
  buffer, matching Blender 5.2.2's blur2 and resolve scale relationship while
  leaving classiCAD's CPU gather and optical inputs unchanged.
- In the matched 640x480 perspective extrusion capture, the baseline remains
  aligned with Blender (mean maximum-channel delta 0.00032; two pixels exceed
  delta 2). With DOF enabled, mean maximum-channel delta improved from 0.604
  to 0.591 and effect-mask IoU improved from 0.437 to 0.463 at threshold 2.
  QPainter now changes 9,129 pixels versus Blender's 11,647. This is a small
  measured improvement; full-frame DOF parity remains open.
- Validation: `cmake --build build -j4` passed with the dependency audit;
  the capture-enabled offscreen `classicad_viewport_render_contracts_test`
  passed; and `tests/workbench_dof_compare.py` completed against the Blender
  5.2.2 baseline/effect pair. The comparison also records threshold-3
  sensitivity (IoU 0.456). No GUI was launched.

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
  matched image parity with Blender. QPainter now uses Blender's 49 generated
  aperture positions in its full-resolution software gather. At this point in
  the port it lacked temporal jitter and Blender's near/far depth-aware
  pyramid, and composited transparent colors before the gather. Its full-screen resolve
  approximates background spill by using the camera far clip for clear-depth
  pixels near geometry.
- Polygon aperture taps now derive their blade sector from the unrotated sample
  angle, then apply aperture rotation once when mapping the shaped radius to
  screen offsets. A focused contract checks the expected rotated four-blade
  tap. This follows Blender 5.2.2's `DofPass::setup_samples`, which shapes
  radius before adding `rotation_` ([`workbench_effect_dof.cc:71-79`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_effect_dof.cc#L71-L79)).
  The QPainter path remains a full-resolution approximation rather than Blender's
  multi-pass half-resolution gather; temporal jitter, near/far weighted
  downsampling, and matched visual comparison remain open.

### QPainter DOF far-plane background spill (2026-10-14)

- The software resolve now handles clear-depth pixels within the maximum
  bounded CoC radius of rasterized geometry. It uses the camera's far clip as
  the background depth, gathers nearby covered colors, and applies the same
  one-to-three-pixel CoC blend as surface pixels. A separable coverage dilation
  keeps the candidate search linear in output pixel count rather than running
  the 49-tap gather over the whole background.
- The focused `viewport_render_contracts` case uses a defocused bright surface
  against an opaque dark background and verifies a pixel outside the silhouette
  receives foreground color. Blender 5.2.2's pinned resolve is explicitly a
  full-screen pass and blends from `abs(coc)` in
  [`workbench_effect_dof_resolve_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_effect_dof_resolve_frag.glsl#L296-L319).
  Its prepare pass selects far-plane pixels in priority and applies near/far
  CoC weights in
  [`workbench_effect_dof_prepare_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_effect_dof_prepare_frag.glsl#L354-L415).
- This remains an approximation: it does not implement Blender's separate
  near/far pyramid or bilateral weights, uses a square dilated candidate mask,
  and still gathers at full resolution without Blender's temporal jitter.

### QPainter Workbench DOF aperture kernel (2026-10-14)

- The CPU gather now generates Blender 5.2.2's radius-three square-ring
  perimeter: 49 samples mapped to a disk by `square_to_circle`, then shaped by
  blade count, aperture rotation, and reciprocal X ratio. It removes the
  unrelated fixed 12-point disk and per-pixel whole-kernel rotation. The
  offscreen contract checks the complete kernel's radius distribution and a
  six-blade, rotated, anamorphic sample against the pinned equations.
- Blender's sample count and perimeter enumeration are pinned in
  [`workbench_private.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_private.hh#L473-L483)
  and [`DofPass::setup_samples`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_effect_dof.cc#L839-L897).
  The GPU gather's sample use and CoC weights are in
  [`workbench_effect_dof_blur1_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_effect_dof_blur1_frag.glsl#L21-L57).
- QPainter still gathers at full resolution, has no temporal jitter, and
  composites transparent colors upstream. Matched QPainter image comparison
  remains open.

### QPainter texture-alpha shadow review (2026-10-15)

- Texture alpha remains a color-pass cutout and does not cut holes into
  Workbench shadow casters. Blender 5.2.2 builds geometry-based shadow volumes
  without UV or image-alpha sampling in
  [`workbench_shadow.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_shadow.bsl.hh)
  and
  [`workbench_shadow.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_shadow.cc).
  QPainter therefore keeps full-triangle shadow coverage. The GPU stencil,
  depth-map, and QPainter shadow paths all follow this rule. An earlier entry
  that said the QPainter shadow rasterizer used texture-alpha cutouts was
  incorrect and has been superseded. C++ build passed; matched shadow image
  comparison remains open.

### QPainter Workbench DOF near/far CoC weighting (2026-10-14)

- The QPainter gather now builds Blender's first half-resolution 2x2
  CoC-aware color reduction. It keeps separate maximum near/far CoCs, applies
  the source's sign gate and one-pixel bilateral difference weight, and gives
  far samples priority when their weights are nonzero. Each aperture tap uses
  its unshaped radial sample coordinate with Blender's CoC smoothstep weight;
  accepted colors are normalized by the weight sum.
- At the time of this entry, the bounded CPU adaptation omitted Blender's
  second reduction, mip selection, and temporal jitter. Later code added the
  CoC-aware half-, quarter-, and eighth-resolution pyramid and adaptive mip
  sampling; temporal accumulation is still absent, so its temporal jitter
  offset remains zero. The full-resolution resolve, clear-depth spill policy,
  sharp source alpha, and upstream transparent composition remain CPU-path
  approximations. The CoC prefilter replaces the prior single nearer-depth
  rejection with Blender's source-derived near/far color selection.
- The scalar offscreen contracts check independent near/far maxima, bilateral
  rejection, far priority, and exact gather-weight values. Production contracts
  continue to cover focus preservation, background spill, and large-CoC median
  cleanup. Matched QPainter image output remains open.
- Blender 5.2.2 references: [`workbench_effect_dof_prepare_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_effect_dof_prepare_frag.glsl#L24-L58),
  [`workbench_effect_dof_downsample_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_effect_dof_downsample_frag.glsl#L22-L53),
  [`workbench_effect_dof_blur1_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_effect_dof_blur1_frag.glsl#L39-L58),
  and [`workbench_effect_dof_lib.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_effect_dof_lib.glsl#L17-L32).
- Validation: `cmake --build build -j4` passed with the dependency audit;
  offscreen `viewport_render_contracts` and
  `workbench_raster_fragment_contracts` passed.

### QPainter opaque face depth resolve (2026-10-15)

- Opaque QPainter fragments now select the smallest captured camera distance
  at each pixel. This is the nearest fragment under the existing positive
  camera-distance convention and preserves the per-face scene-linear shading
  when a NURBS solid's projected faces overlap.
- A matched 640x480 perspective extrusion capture against Blender 5.2.2 now
  has a baseline mean maximum-channel delta of 0.00032 (two pixels exceed a
  delta of 2), down from 19.41 before this correction. This confirms the
  previously observed QPainter face-color mismatch was in opaque fragment
  selection, not the prepared NURBS face order.
- With DOF enabled, the same capture has mean maximum-channel delta 0.604 and
  effect-mask IoU 0.437. The opaque baseline is now aligned; QPainter's DOF
  footprint and gather result still do not match Blender closely and remain
  open. This capture does not verify multiple overlapping objects or the
  sorted transparent fallback.
- Reproduce the offscreen capture with `CLASSICAD_VIEWPORT_QPAINTER_DOF_CAPTURE`
  and `CLASSICAD_VIEWPORT_QPAINTER_DOF_BASELINE_CAPTURE` set to output paths,
  then compare against the paired Blender 5.2.2 output with
  `tests/workbench_dof_compare.py`. The build and
  `classicad_viewport_render_contracts_test` pass with the capture enabled.

### QPainter DOF fractional gather coordinates (2026-10-15)

- The CPU gather now preserves each aperture tap's fractional output-pixel
  coordinate through its existing bilinear color-mip sampler. It also
  bilinearly samples the signed CoC at that coordinate before selecting the
  mip level. Blender 5.2.2 builds a continuous texture coordinate for each
  aperture tap, samples the CoC texture there, and uses filtered mip sampling
  for color in [`workbench_effect_dof_blur1_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_effect_dof_blur1_frag.glsl#L395-L425);
  its DOF setup enables linear texture filtering in
  [`workbench_effect_dof.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_effect_dof.cc#L1002-L1032).
- On the matched 640x480 perspective extrusion capture, mean maximum-channel
  DOF delta improved from `0.591` to `0.579`, and effect-mask IoU improved
  from `0.463` to `0.489` at threshold 2. The QPainter effect mask changed
  from 9,129 to 8,173 pixels, moving farther from Blender's 11,647-pixel mask;
  so the mask overlap improved while its total footprint did not. The opaque
  baseline remained aligned at mean delta `0.000319` (two pixels over 2).
  This is a measurable partial improvement, not a closure of DOF parity.
- Validation: `cmake --build build --target
  classicad_viewport_render_contracts_test -j4`, full `cmake --build build
  -j4`, and the capture-enabled offscreen
  `classicad_viewport_render_contracts_test` passed. The paired Blender
  5.2.2 comparison completed with `tests/workbench_dof_compare.py`. No GUI was
  launched.

### QPainter prepared perspective object depth (2026-10-15)

- Corrected the QPainter prepared-geometry depth buffer's perspective sign.
  The camera view direction points outward, so the rasterizer now negates its
  dot product to obtain positive camera distance for reciprocal interpolation
  and restores a negative signed depth after perspective correction. Nearer
  values therefore compare greater in the existing max-depth resolve instead
  of collapsing to the positive epsilon and depending on object submission
  order.
- Added an offscreen production-path contract with overlapping prepared NURBS
  planes at different perspective depths. The nearer red plane wins when
  objects are submitted in either order. This covers the controlled opaque
  case; broader perspective scenes and transparent ordering remain separate
  parity gaps.
- Validation: `cmake --build build -j4` and offscreen
  `classicad_viewport_render_contracts_test` passed. `git diff --check` passed;
  no GUI was launched.

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

### Workbench Image Texture source color spaces (2026-10-13; extended 2026-10-14)

- Blender 5.2.2's
  [`image_gpu.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/blenkernel/intern/image_gpu.cc#L821-L844)
  converts byte images from their declared color space to scene-linear through
  the active OCIO processor before Workbench filters them. classiCAD's Blender
  extractor now bakes that per-texel conversion into its bounded RGBA32F
  transport for every source space present in the active Blender OCIO config,
  including nonlinear spaces such as `Gamma 2.2 Encoded AdobeRGB`. The
  transform ID hashes the OCIO config cache ID plus source and scene-linear
  names, so native records and caches distinguish conversions made under
  different configs. Float image buffers retain Blender's direct-upload path;
  sRGB byte, Linear Rec.709, and Non-Color byte images retain their existing
  routes. PREMUL RGB is multiplied per texel after conversion and alpha is
  copied unchanged.
- The existing GPU and QPainter float-image paths filter the transported
  scene-linear texels. Regular byte ACEScg and nonlinear Adobe RGB contracts
  compare extractor payloads to Blender's OCIO processor. A UDIM contract
  checks independent tiles through the same nonlinear transform. The bounded
  transport still rejects missing source spaces, non-finite output, and
  images exceeding the per-image or scene-wide limits. This validates texel
  transport, not a matched Blender/classiCAD rendered frame.
- Validation on 2026-10-14: `blender_texture_source_contract.py` and
  `blender_project_proxy_contract.py` passed under Blender 5.2.2; the C++ build
  and 314-file dependency audit passed. Contracts cover ACEScg, nonlinear
  Adobe RGB, direct float buffers, tiled Adobe RGB with PREMUL alpha, and
  packed proxy save/reopen. Matched rendered-pixel comparison remains open.

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
  sample scheduling are unchanged. A matched curvature-only visual capture now
  uses identical 32-segment, 16-ring faceted spheres at 640×480, orthographic
  scale 5, direction `(4, -6, 4)`, AA Off, zero dither, and explicit ridge and
  valley factors of 1.0. The first flat-cube fixture produced no measurable
  Blender curvature signal, so the sphere fixture replaces it. At a 2/255
  effect threshold, Blender and classiCAD change 12,083 and 12,081 pixels
  (118 XOR; IoU 0.990281); at 3/255 they change 11,552 and 11,515 pixels
  (209 XOR; IoU 0.982042). Mean effect magnitudes are 10.4016 and
  10.3890/255 at threshold 2. On the effect-mask union, signed RGB-delta MAE
  is 0.23927/255, p95 1/255, and maximum 16/255; full-frame delta MAE is
  0.00962/255. This confirms the curvature-only response for this faceted
  sphere and view. Smooth shading, fractional DPR/UI scale, other meshes, and
  broader normal/raster cases remain open. Blender's pinned curvature shader
  uses the same fractional `ui_scale / viewport_size` offsets and separate
  curvature ridge/valley controls in
  [`workbench_curvature.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_curvature.bsl.hh#L9-L50).
  The offscreen GPU contract writes baseline/effect captures when
  `CLASSICAD_VIEWPORT_GPU_CURVATURE_CAPTURE_DIR` is set; Blender uses
  `tests/workbench_cube_reference.py --scene CURVATURE_SPHERE --effect
  BASELINE|CURVATURE --match-classicad-ortho --view-direction 4 -6 4
  --ortho-scale 5 --aa OFF --zero-dither`. Compare with
  `tests/workbench_effect_delta_compare.py` at thresholds 2 and 3. Validation:
  `cmake --build build -j4` passed with the 317-file dependency audit, the
  capture-enabled offscreen `viewport_render_contracts` test passed (1/1), and
  both Blender captures reported 5.2.2 LTS.

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
  and an offscreen GPU contract now renders a polygon scene over a controlled
  above-one scene-linear background, reads the RGBA16F scene target as floats,
  writes a temporary HDR image from that readback, decodes its RGBE pixel, and
  confirms the above-one red value survives within 0.04. This checks scene
  target readback and the HDR writer together without opening the application
  window. Broader Blender render-result image comparison and HDR edge cases
  remain open.

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

### QPainter Studio monochromatic specular energy (2026-10-14)

- The CPU Studio helper now multiplies the mean specular color by Blender's
  pinned `0.33333` coefficient, matching the GPU shader. A focused contract
  uses full Fresnel reflectance to distinguish Blender's truncated coefficient
  from an exact `1/3` average.
- Blender 5.2.2's
  [`workbench_world_light_lib.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_world_light_lib.glsl#L122-L126)
  uses `dot(specular_color, float3(0.33333f))` for its energy-conservation
  term. Broader QPainter/Blender image comparisons remain open.

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
- The later 2026-10-13 implementation folds cavity into retained opaque color
  before transparency composition. Weighted OIT and the bounded sorted
  fallback now both do this in scene-linear RGB. This section is retained as
  history only; current behavior and remaining sampling limitations are recorded under
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
- **Historical blocker, resolved for the bounded surface compositor
  (2026-10-14):** the ordinary QPainter path no longer blends transformed OIT
  color over display-referred opaque fills/background. The widget supplies
  scene-linear World/Custom colors or per-pixel Theme samples to
  `compositeRasterWorkbenchSurfaces`; that resolver retains opaque fragments
  and visibility in float scene-linear pixels, resolves transparent OIT over
  them, and applies the display transform to the completed pixel once. The
  native-renderer failure fallback uses the same bounded compositor when its
  geometry and memory budgets permit. Its per-object `QImage` is used for
  raster coverage, while color comes from the scene-linear fragment sink.
  This closes the blocker for the bounded paths, not the fallback cases below.
  QPainter cavity/outline and exceptional legacy fallbacks still have
  separate limitations; merely changing an intermediate `QImage` format
  would not preserve HDR or guarantee scene-linear blending.
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
  retain the existing sorted/regular OIT behavior. The direct QPainter painter
  fallback has no separate in-front surface domain; its bounded surface
  compositor does keep regular and in-front domains separate. GPU pixel
  comparison remains pending; these fallback paths are not claimed as Blender
  parity.
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
  downsample. Scalar contracts now cover an all-zero 2x2 block, where both
  maxima stay zero and all four zero samples receive unit weight, plus a mixed
  signed block that retains far-sample priority. The existing focused offscreen
  capture still checks edge blur, opacity, and camera-DOF gating; it does not
  independently read back the GPU zero-CoC channels. Blender 5.2.2's inclusive
  sign behavior is visible in pinned
  [`workbench_effect_dof_prepare_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_effect_dof_prepare_frag.glsl#L24-L58)
  and [`workbench_effect_dof_downsample_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_effect_dof_downsample_frag.glsl#L24-L52).
Pinned source: [`workbench_effect_dof_blur1_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_effect_dof_blur1_frag.glsl#L20-L31), with the texture populated in [`workbench_resources.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_resources.cc#L84-L100). The QPainter fallback uses Blender’s generated 49-tap aperture kernel and a CoC-gated component-wise 3x3 median. It now uses Blender's signed 100-pixel CoC bound, builds CoC-aware half-, quarter-, and eighth-resolution color/CoC levels, and selects trilinear mip samples with Blender's adaptive LOD expression. The scalar contracts cover LOD boundaries and signs; a production-path fixture verifies blur spill beyond 12 pixels. To keep dense software resolves practical, it retains all 49 taps for sparse candidate regions and evenly reduces the kernel so targets with up to 24 million candidate pixels stay within a 24-million-tap budget; exceptionally larger targets retain one tap per candidate. This is a documented QPainter quality/performance approximation. Its aperture shaping uses Blender's `blades > 1` condition and polygon/rotation/reciprocal-X-ratio mapping, including the two-blade slit. Its resolve blend uses Blender's `smoothstep(1, 3, abs(CoC))`; contracts cover blend boundaries and an orthographic subpixel scene. Pinned gather/LOD source: [`workbench_effect_dof_blur1_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_effect_dof_blur1_frag.glsl#L39-L56); mip allocation/downsample source: [`workbench_effect_dof.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_effect_dof.cc#L98-L107) and [`workbench_effect_dof_downsample_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_effect_dof_downsample_frag.glsl#L24-L52). Matched image comparison remains open.

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
| Saved grid and selection overlays | Blender's saved `show_overlays`, `show_floor`, `show_ortho_grid`, `show_axis_x/y/z`, and `show_outline_selected` settings are imported and persisted with viewport shading. The grid renderer gates perspective/orthographic floor lines and each world axis in GPU and QPainter paths. Front/Right orthographic planes recognize Z as in-plane when enabled. The selected-object contour is a separate pass from Workbench's general object outline and is gated by both saved overlay flags. Older imports keep floor/grid visible, Z off, and selected outlines on. Matched viewport comparisons remain open. | Implemented; visual parity remains unverified |
| Studio lights and MatCaps | Blender 5.2.2 studio-light presets and 27 MatCaps are bundled. The studio-light equations and default light rig are adapted from Blender; scene lights are not used. MatCap horizontal flip is exposed in the shading popover, saved/restored with viewport settings, included in AA history invalidation, and applied to GPU and QPainter surface sampling and the settings preview. MatCap and AgX textures use linear filtering. Matched AA-Off GPU comparisons cover Studio Default with specular on/off, Flat, and `metal_bronze` with specular on/off; QPainter comparison covers `metal_bronze` with specular. Other presets, orientations, AA modes, and broader parity remain open. | Foundation ported; final output parity remains unverified |
| Color management | Imported scenes retain display device, view transform, look, exposure, and gamma for Workbench render results. The interactive Solid/Wireframe GPU viewport uses Blender Workbench's default view transform; Blender applies scene render color settings there only for Rendered mode or when View3D Scene Lights/Scene World is enabled. classiCAD does not currently expose those View3D modes. Live GPU rendering keeps scene surfaces, transparency, pictures, and backgrounds in RGBA16F scene-linear color through depth of field and temporal AA, then applies exposure, the active sRGB AgX/Standard/PBR Neutral/Filmic or imported OCIO LUT, and gamma once during presentation. Offscreen Workbench results now run the configured viewport AA sample mode through the same scene-linear resolve and retain the resolved float Combined payload for Radiance HDR and OpenEXR when available. The `render_aa` enum/sample mapping follows Blender 5.2.2: OFF disables AA, FXAA selects one jittered sample plus SMAA, and 5/8/11/16/32 select those TAA sample counts, matching [`workbench_state.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_state.cc#L197-L225) and the render sample loop in [`workbench_engine.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_engine.cc#L499-L535). Other non-EXR display-referred output uses RGBA16/QImage, so HDR range is lost before 16-bit PNG/TIFF writing. QPainter Solid rendering retains its CPU display-transform path. Other sRGB view/look combinations use validated Blender-baked LUTs shared by scenes with identical table bytes. AgX uses a 97³ float LUT with an analytic `log2_1p` shaper over [0, 16] (scale 64); PBR Neutral, Filmic, and imported custom LUTs use 65³ tables. Older native LUTs retain their linear [0, 2] metadata. Inputs outside each stored domain clamp to its endpoints. Standard, AgX, exposure, and gamma transforms are checked against Blender 5.2.2; gamma zero is preserved and follows Blender's `1 / FLT_EPSILON` behavior. | Partial; runtime coverage of imported render-AA override precedence and completion at each sample count, non-sRGB display devices, arbitrary OCIO configuration transfer, imported-LUT GPU precision, GPU/QPainter differences, HDR-limited non-EXR output, and broader matched render comparisons remain open |
| Surface drawing | NURBS surfaces and solids are tessellated for display and shaded by classiCAD's custom forward GLSL renderer. Curves use the stroke path. | Different rendering architecture; visual parity remains unverified |
| Color source | Object, Random, Material, and Custom color plus their Blender-compatible alpha sources reach filled NURBS surfaces/solids in GPU and QPainter paths. When no Workbench material is assigned, Material/Attribute/Texture use Blender's opaque default gray material; Object and Random use object display color and alpha. Blender mesh import preserves object display colors, shared materials, per-face slots, active UV maps, and image references. GPU mesh Texture mode samples the active Image Texture Node per fragment through interpolated active-UV coordinates, with repeat wrapping and linear filtering; supported external raster files are loaded with size limits and cached by path/color space. QPainter mesh Texture mode rasterizes each triangle per target sample, uses reciprocal camera-forward depth for perspective-correct UVs, and bilinearly samples external and packed images with repeat wrapping. An audit found the reciprocal weight used the camera-to-scene direction with the wrong sign, collapsing varying positive depths to the clamp; mesh and NURBS raster paths now use positive camera-forward depth. UDIM tiles now preserve Blender 5.2.2 per-tile packed bytes in native documents and sample them in GPU and QPainter mesh and NURBS paths before falling back to external tile paths; import caps packed payloads at 32 MiB total per image, and absent tiles remain magenta. GPU and QPainter NURBS Texture sample trimmed and untrimmed surfaces using tessellation parameter UVs normalized over the exact knot domains. This matches Blender 5.2.2's index-normalized UVs because Blender samples uniformly over each active parameter domain. QPainter mesh and NURBS Texture honor the imported image sRGB flag: unpremultiply PREMUL RGB as before, decode sRGB samples to scene-linear when requested, replace material RGB with valid image samples while preserving material alpha, and apply the active exposure/gamma/OCIO or built-in transform; unavailable images or UVs retain Material color. QPainter textured-mesh shading reconstructs its incident direction and normal per raster pixel: perspective samples use perspective-correct position/normal interpolation and orthographic samples use barycentric normal interpolation; corner normals are transformed by the world inverse transpose, with a flat geometric normal as fallback for invalid or absent corner data. QPainter untextured Workbench mesh triangles now rasterize per pixel using scene-linear material/object/random/custom/attribute colors, per-face roughness and metallic, interpolated corner normals, and perspective-correct incident direction. Random and Custom inputs use Blender-compatible color conversion; Attribute keeps the existing triangle-averaged color and decodes it only when its metadata says sRGB. Raster size limits use the bounded coarse raster for oversized triangles, with less accurate detail and edge coverage. Orthographic incident direction remains constant. QPainter untextured NURBS triangles are shaded per pixel with perspective-correct interpolation of prepared smooth corner normals; when prepared normals are absent or invalid, they retain the flat geometric normal. Perspective samples reconstruct their world position, while orthographic incident direction remains constant. Textured NURBS samples use per-pixel material lighting and interpolate prepared smooth normals with perspective-correct weights; missing or invalid prepared normals fall back to flat. GPU and QPainter texture-alpha cutouts apply in color rendering for trimmed and untrimmed NURBS. GPU object-ID/outline and QPainter cavity/outline effect-alpha paths also cover trimmed and untrimmed tessellated NURBS using active-domain UVs and Blender's alpha rules. Workbench's shadow-volume pass does not apply Image Texture alpha to caster geometry; classiCAD's depth-map fallback now follows that rule. The alpha cutoff is 0.1 when enabled and is bypassed for NONE/Ignore and CHANNEL_PACKED. Packed Image Texture bytes up to 32 MiB are transported, persisted, and sampled by both paths. Missing images or UVs fall back to Material. Both renderers unpremultiply PREMUL images except at alpha 0 or 1. Blender Workbench's pinned [`workbench_image_lib.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_image_lib.glsl#L34-L60) performs UDIM lookup or regular image sampling, unpremultiplies PREMUL pixels, and discards fragments below `image_transparency_cutoff`; Workbench uses supported material display values and its active Image Texture input rather than evaluating arbitrary shader graphs. | Partial; color/effect alpha coverage is implemented for the described passes, while QPainter composition, trim tessellation, and final visual comparisons remain approximate or unverified |
| Blender scene import | File > Import > Blender Scene invokes the pinned Blender 5.2.2 extractor and offers original or evaluated mesh mode. File > Open now routes ordinary `.blend` files through the same original-mode extractor and loads the supported result as a new document; native classiCAD `.blend` files still restore their embedded authoritative snapshot. Imported polygon meshes retain geometry, custom split normals, object/material data, direct links to multiple Blender collections, collection hierarchy and global hide flags, plus per-view-layer collection-instance paths/exclusions/visibility and object hide state. Mesh loose edges are emitted as a companion CURVE item with a deterministic derived transport identity, separate from the source mesh identity. Each edge becomes a world-space degree-one NURBS component; the companion is named from the source object and its transport record carries the source object ID, while object display color, collection memberships, visibility, and per-view-layer state. Isolated vertices import as bounded, separately selectable Point items; the dated Blender mesh isolated vertex import entry below records their limits. Blender FONT objects convert their visible glyphs to baked polygon meshes in both import modes, with evaluated transforms and material assignments; original text/font/layout editability is lost and reported. Blender METABALL objects convert to their current evaluated polygon surface in both import modes, with editable metaball data and threshold settings not retained. Both conversions explicitly acquire a dependency graph when the selected global mode is original, so evaluated geometry extraction still works in that mode. Collection instances of FONT and META objects now use the same baked evaluated-mesh representation while retaining each instance transform and visibility. Exact Blender CURVE POLY splines import as dimension-3 degree-1 NURBS after matrix_world is applied; cyclic splines explicitly close. Each cubic Blender BEZIER span imports as a separate nonrational clamped degree-3 NURBS with its four transformed endpoint/handle points; cyclic spans include last-to-first, including the one-point cyclic loop. Blender CURVE NURBS splines preserve their weights, degree, generated knot multiplicities/domain, and cyclic wraparound control points in dimension-3 NURBS. Curves with visible bevel, extrusion, bevel-object, 2D fill, or modifier output now also import a deterministic companion mesh baked from Blender's evaluated curve, while retaining source spline data as the editable centerline. This proxy uses the current evaluated state and does not preserve its surface settings for editing. Blender SURFACE NURBS splines import as separate exact NURBS surface objects, with world-space control nets, U/V knot domains, weights, cyclic directions, per-patch identities, and assigned Workbench material values preserved. Multiple source curve splines remain components of the same imported source object. Object display name/color and collection memberships are retained. Blender parent relationships are not represented; supported child objects retain their world-space geometry/transform and now report this hierarchy loss. No curve approximation is emitted. The Collections panel selects among imported Blender view layers; selected view-layer visibility resolves each object's direct collection memberships across visible paths. Linked objects combine memberships and per-view-layer hide state. The default option preserves per-scene first-layer visibility. Collections remain independent of CAD layers. Transport identity keys use a SHA-256 namespace derived from the normalized absolute source .blend path for objects, materials, collections, scenes, view layers, and their references; moving or renaming the source project changes that namespace. Native document version 15 persists Blender camera, render-output, imported-scene, and deduplicated OCIO LUT metadata and reads versions 1–14; the scene extractor bridge accepts versions 10 through 14. Ordinary `.blend` Open and File > Import > Blender Scene now apply the selected saved `VIEW_3D` area's `SpaceView3D.shading` to the interactive viewport, with a deterministic policy when several areas exist, while `scene.display.shading` remains the scene-level render setting source and `scene.display.render_aa` remains available to offscreen results. Static file-backed Image Empties import as Picture objects with image placement and opacity; projection-specific visibility/depth rules and generated or animated images remain unsupported. | Partial; additional object types and visual/import coverage remain gaps. Cameras, lights, non-image empties, and unsupported Blender data types still have no matching classiCAD scene-object geometry type; the extractor reports these objects (the active camera's supported settings are separately retained as scene metadata). Blender Workbench Solid uses its studio-light rig rather than scene light datablocks. Adding camera/light datablocks as editable scene objects needs an explicit model/serialization/rendering contract, so they are not fabricated as points or proxy meshes. |

**Oversized QPainter raster correction (2026-10-15):** The Color source row above still describes the old flat-polygon fallback for oversized untextured mesh triangles. That sentence is stale: oversized mesh triangles now use a bounded coarse raster that retains interpolated corner colors; oversized textured mesh and NURBS triangles also remain visible through bounded coarse rasters. Fine detail and edge accuracy decrease at these size limits, as recorded in the dated entries below.

**Image Empty import update (2026-10-15):** Static file-backed Blender Image
Empties with readable still images now import through classiCAD's existing
Picture geometry and retain their transformed quad and image opacity. Other
Empty types remain metadata-only. Image projection visibility, side, and depth
preference are represented and applied by the GPU and QPainter paths; QPainter
depth masking and transparent-surface behavior remain bounded approximations,
as described in the dated Image Empty sections below. Generated or animated
images and image tint remain unsupported. Quad sizing and offset follow Blender 5.2.2's
[`overlay_empty.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/overlay/overlay_empty.hh#L1432-L1548).

**Import update:** The scene extractor bridge now accepts versions 10 through 14; the native document schema writes version 21 and reads versions 1 through 20. Ordinary Blender `.blend` Open now imports the first scene's `scene.display.shading` Solid settings into the initial classiCAD viewport and preserves `scene.display.render_aa` for offscreen Workbench render results. It maps bundled studio-light/MatCap presets, color source and background, shadow/cavity/curvature/outline controls, `show_xray`, the distinct `show_xray_wireframe`, `matcap_flip_x` when Blender exposes it, and studio-light rotation converted from radians to degrees. Custom unavailable presets, scene-light rendering, and Blender 5.2.2's RNA-hidden MatCap flip are reported as unsupported. `studiolight_background_alpha` and `studiolight_background_blur` are preserved in the extracted record but belong to Eevee Lookdev rather than Workbench Solid. Blender 5.2.2 Workbench does not use `studiolight_intensity`. Imported World backgrounds are retained and used for both interactive and offscreen rendering. In Original mesh mode, source geometry stays unevaluated while the active camera and focus-object transforms are resolved from the scene dependency graph. Existing classiCAD project opens do not apply imported Blender shading. Ordinary `.blend` Open creates a new classiCAD document; its source path is not used as the Save target, so users must use Save As to create a native project.
| Material values | Per-object base color, roughness, metallic, and alpha are serialized and consumed by both renderers. Imported Blender meshes retain shared material records and per-face slot references; GPU and QPainter mesh paths select face material values. A real Blender 5.2.2 material-slot `.blend` now passes through the production importer, Document records, viewport frame adapter, and both GPU and QPainter renderers; matched Studio captures verify imported base colors, roughness, and metallic. Native NurbsSolid objects can reference shared DocumentMaterial records by stable seed-face ID; GPU draw ranges, QPainter color, and QPainter effect alpha resolve the per-face values, with object material fallback. ClassiCAD `.blend` display proxies preserve and round-trip assignments through polygon face attributes tied to source object and semantic face IDs, and export base color, roughness, and metallic to generated Blender material datablocks. Proxy materials recreate supported captured active Image Texture and Environment Texture nodes when their packed or existing image sources are available. Generated NURBS proxy meshes carry normalized per-corner parameter UVs, with separate domains retained for each solid face. Active Image Texture and Environment Texture proxies also reconstruct supported UDIM tile sets under the existing 32 MiB aggregate image limit. Materialized edited boundary faces have no seed-face lineage, so their seed assignments are ignored. Attribute colors also reach both mesh renderers. classiCAD currently has no UI for authoring appearance values. | Partial; Blender-side roughness/metallic values are imported, while in-app appearance authoring, textured/transparent per-face slots, materialized-face lineage, and broader rendered parity remain open; scene-linear float proxy texels round-trip in packed EXR images |
**Native schema update:** Document version 21 adds `nurbsSolidFaceMaterialIds` references by stable seed-face ID and reads prior versions through 20. The Blender project adapter merges uniform proxy-polygon material assignments back to those references; mixed assignments to a single semantic face remain unchanged.
| Culling and X-Ray | GPU surface culling works; X-Ray uses translucent fill plus depth/stroke passes. GPU and QPainter polygon-mesh paths classify every finite negative mesh-transform determinant as reflected by sign, without a magnitude threshold. GPU swaps triangle winding; QPainter reverses culling sense. QPainter applies backface culling to polygon-mesh triangles using transformed world-space winding and the active view direction. Its standalone outline-ID fallback draws regular ranges first, then clears only depth and draws `showInFront` ranges over the preserved regular IDs; the ID shader retains the same texture-alpha cutoff rules. Blender 5.2.2 routes X-Ray and transparent in-front objects through dedicated accumulation/depth passes in [`workbench_engine.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_engine.cc#L238-L250) and writes in-front IDs/depth before regular transparent coverage in [`workbench_mesh_passes.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_mesh_passes.cc#L309-L368). QPainter NURBS, solid-face, and polygon-mesh culling now uses projected tessellated triangle winding; opaque backface normals flip to face the camera, while transparent shading keeps its source normal orientation. Blender 5.2.2 tracks inverted handedness in `ResourceID` and applies it to front-facing state; duplicate transforms set the negative-scale flag using determinant sign in [`draw_context.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/intern/draw_context.cc#L933-L946), [`draw_handle.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/intern/draw_handle.hh#L60-L69), and [`draw_command.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/intern/draw_command.cc#L176-L176). Its opaque normal prepass receives `[[front_facing]]` and flips backface normals in [`workbench_prepass.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_prepass.bsl.hh#L306-L314) and [`workbench_common_lib.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_common_lib.glsl#L28-L33); the transparent shader uses its interpolated normal directly in [`workbench_transparent_accum_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_transparent_accum_frag.glsl#L59-L64). In Solid mode, classiCAD's bounded surface-batch compositor uses per-pixel nearest opaque depth for NURBS and polygon-mesh fills and sorts precomposited transparent object fragments by their per-pixel nearest tessellation depth, with a four-million-fragment cap. If prepared geometry or either memory budget is unavailable, the fallback draws surface objects in average view-depth order and sorts polygon-mesh and NURBS-surface triangles by projected centroid. It has no general scene depth buffer for strokes or separate X-Ray depth/stroke passes. | Partial; inter-object transparent ordering improves within the bounded compositor, while precomposited intra-object self-occlusion, X-Ray, and fallback ordering still differ |
| Shadows and outlines | Validated closed opaque `NurbsSolid` and all-opaque imported `PolygonMesh3D` casters use depth-pass stencil side volumes when all source and extruded vertices clear the camera near plane; transparent polygon-mesh objects force depth-fail across their validated shared volume, as do transparent solids and open `NurbsSurface` casters. Opaque in-front receivers use their own depth-stencil domain. Blender 5.2.2 uses geometry-based stencil shadow volumes and has no directional depth-map fallback; classiCAD retains a 1024-pixel depth-map fallback with one bilinear depth comparison for unsupported GPU topology. Both classiCAD caster paths ignore Image Texture alpha, matching Blender's shadow-volume input; the map fallback still has narrower caster coverage and is not a binary stencil mask. QPainter has a bounded software light-depth shadow for tessellated NURBS and polygon meshes; its light-depth map is 2x per axis while within a 16M-texel budget, otherwise it keeps viewport resolution. Opaque and transparent-material casters cast binary geometry shadows without material or Image Texture alpha scaling. Opaque receiver shadows contribute beneath retained translucent layers, and the nearest visible translucent receiver contribution receives its own factor; stacked-layer receiver separation remains approximate. Focus/shift and light direction use Blender's formulas in GPU and QPainter paths. GPU and QPainter outlines use four-cardinal object-ID neighbors; Blender uses one outline color and has no selected-color behavior. GPU object-ID/outline alpha and QPainter cavity/outline alpha cover trimmed and untrimmed NURBS texture cutouts. Blender and the GPU path blend outline color into scene-linear color before display conversion. QPainter now does the same for its bounded weighted compositor when shadows and DOF are inactive, including X-Ray where those effects are suppressed. Its software Workbench outline now samples opaque-only IDs, matching Blender's outline source; selected-object contours continue to use the visible-layer ID buffer. Combined shadow/DOF cases and legacy fallbacks still blend outlines after display conversion. Matched 640×480 GPU cube captures compare opaque shadows, WORLD cavity, and outlines: the standalone cube shadow effect mask matched for its tested view, and cavity mask IoU is 0.972. The new caster-only planar-receiver comparison gives GPU shadow-mask IoU 0.972 and QPainter shadow-mask IoU 0.494 at a 0.001 scene-linear threshold. An earlier outline capture matched exactly; a fresh classiCAD-ortho outline capture has IoU 0.823483 and 448 XOR pixels, exactly attributed to a 224-pixel difference in the per-renderer baseline silhouettes. Shadow magnitude outside the matched receiver, broader geometry and camera cases, combined/transparency effects, QPainter output, and the depth-map fallback remain open. | GPU stencil shadows, cavity, and outline passes are implemented; QPainter has a bounded CPU shadow fallback for opaque and transparent-material casters, while broader cases and outline-intensity attribution remain open |
| Cavity and depth of field | GPU Solid maps Blender's World type to SSAO and Screen type to curvature; Both uses both. The persisted settings use Blender 5.2.2 defaults and ranges, and GPU cavity sampling follows Blender's sample cap and blue-noise jitter. QPainter evaluates cavity from its software depth and normals, but its sampling footprint and curvature estimate remain approximate. DOF is camera-gated in both paths and uses imported camera optics and Blender's millimeter-to-scene conversion. GPU DOF uses Blender's half-resolution color/CoC pyramid, gather, median, and resolve before AA. QPainter uses a bounded software gather with Blender's 49 aperture taps, a CoC-aware half/quarter/eighth-resolution pyramid, adaptive mip sampling, and half-resolution median cleanup. Its temporal offset stays zero because QPainter has no temporal accumulation; transparent colors are already composited before its gather, and its CPU work is tap-budgeted. Camera sensor-fit mapping and broader matched DOF output remain open. | Partial; GPU cavity/DOF are source-derived with matched comparisons still limited. QPainter follows the CoC and mip stages but remains a bounded single-frame approximation; cavity, transparency interactions, and matched DOF output remain open. |
| Anti-aliasing preference | The UI offers Off, “Single Pass Anti-Aliasing,” and 5/8/11/16/32 samples; the RNA description identifies the single-pass method as FXAA. In Blender 5.2.2 Workbench, stored value 1 means one unjittered sample with TAA color filtering and SMAA resolve; Workbench does not invoke an FXAA shader. Values 5+ accumulate jittered frames and use SMAA while settling. During navigation or playback, Workbench resets to one unjittered sample and SMAA. classiCAD implements navigation reset and exposes a playback-state setter, but has no scene timeline, playback clock, or playback frame scheduler to call it. The grid is drawn afterward with its own unjittered MSAA path. QPainter renders committed geometry into a supersampled target for non-Off choices, without temporal accumulation or SMAA resolve; its mesh and NURBS triangle rasterizers use pixel-center coverage at Off and keep the 4×4 edge estimate for non-Off modes. Its wire categories use the separate Smooth Wires preferences. | Partial; GPU path follows Workbench's AA passes, but app playback integration, output equivalence, and fallback parity remain open |
| Smooth Wires | Overlay and Edit Mode preferences are independently stored and GPU strokes have separate categories. Committed curve strokes and wireframe surface lines use Overlay; component controls/cages and Solid-mode `NurbsSolid` face-boundary strokes use Edit Mode. The imported Blender `show_overlays` setting gates Solid-mode global and per-object mesh/NURBS wire overlays; Wireframe shading mode still draws its geometry when overlays are hidden. The separate Workbench Outline effect draws object silhouettes. QPainter applies these preferences to supported curve, surface, control-cage, and selected surface/solid component-edge paths. If native GPU stroke drawing fails, the prepared-stroke fallback reads scene depth when available, approximates hidden/visible stroke visibility with sampled depth, applies endpoint fades, and handles continuous, dashed, dotted, and custom line patterns. The ordinary raster fallback now draws selected component edges after scene effects and tests them against retained regular/in-front surface depth when available; if no depth was retained, that overlay is unoccluded. Remaining gaps include approximate depth sampling/edge coverage and direct QPainter curve paths that do not share the GPU stroke pass's full depth treatment. | Partial; the GPU `NurbsSolid` boundary pass, per-pixel GPU resolve subset, QPainter component highlights, and native stroke failure fallback are present, while fallback coverage and matched output remain approximate or unverified |
| Transparency | GPU surfaces are separated into opaque and partial-alpha ranges. Opaque ranges establish depth first; the primary scene renders transparent surfaces with weighted blended OIT into RGBA16F accumulation and R16F weight targets sharing scene depth-stencil. Transparent fragments depth-test without writing depth. Their lit RGB accumulates in scene-linear space and resolves over the scene-linear opaque target before DOF/AA and the single display transform at presentation. When no shared depth texture is available, auxiliary draws use range-level back-to-front order. The surface outline pass also runs for partial-alpha content. In the ordinary QPainter renderer, NURBS and polygon-mesh fills use prepared tessellation depth: opaque coverage establishes nearest per-pixel depth, then partial-alpha fragments use Blender-style weighted OIT in scene-linear space before one display transform and scene strokes. If prepared fragments exceed the four-million cap, the older average-depth painter fallback remains available. The same compositor is attempted when the native renderer's main Solid surface draw fails, using a DPR-aware target; the fallback is presented before the normal native overlay sequence, followed by an unjittered depth rebuild, so scene strokes, grid, selected-face highlights, previews, control points, and arc HUD remain above it. Prepared QPainter stroke fallback also follows the surface fill. It falls back to the prior sorted painter draw if prepared geometry is missing or the bounded target exceeds one million pixels. The bounded sorted compositor retains individual triangle fragments, but shared-edge deduplication and per-pixel ordering remain approximations for intersecting/self-overlapping geometry; the cap and unavailable prepared geometry use the older painter-composited fallback. Blender uses dedicated transparent and transparent-depth passes. QPainter's bounded sorted compositor now resolves scene-linear fragments before display conversion; the older average-depth fallback retains display-space composition. | GPU primary scene uses weighted OIT; QPainter has a bounded opaque-depth/object-layer compositor but differs in transparent ordering, intra-object fragment ordering, color-transform placement, and coverage for its fallback cases |
| Stroke and overlay composition | Workbench surface silhouettes are drawn by its outline pass before AA resolve. CAD curve and wire strokes, like Blender's Overlay wireframe pass (`overlay_wireframe.hh`), draw afterward using the unjittered camera and regenerated scene depth; selected-face overlays also remain post-resolve, and the grid follows. Selected faces depth-test without depth writes. Committed Solid-mode scene strokes opt into depth writes per stroke; transform previews, control guides/cages, selected component edges, and custom outlines remain no-write. X-Ray's hidden wire pass stays no-write while the visible pass honors the per-stroke flag. The grid uses its own surface-depth framebuffer and composites with depth testing disabled, so scene-stroke depth writes do not change grid occlusion. The two Smooth Wires categories remain independent. Blender's Overlay curve-wire shader has no mesh depth offset; classiCAD leaves Overlay curve strokes unbiased, applies the view-dependent mesh-wire offset or smooth per-pixel resolve only to Blender mesh wires, and keeps the fixed CAD Edit Mode overlay offset separate. Opaque depth-writing GPU strokes use a center-plus-four-cardinal per-pixel resolve for the supported single-sample case. Solid Edit Mode strokes and Theme-mode wire strokes use separately configurable theme colors, saved with viewport shading state; selection, previews, and non-Theme wire colors retain their separate paths. QPainter composites approximate Solid surface silhouettes from its object-ID raster and now draws selected surface/solid component edges after the scene effect resolve, using retained regular/in-front depth to hide occluded segments when available. The native-GPU stroke failure fallback uses sampled depth, endpoint fades, and line styles, but its visibility and edge coverage remain approximate. QPainter committed curve paths test prepared or bounded sampled segments against retained scene depth: regular objects use regular opaque plus transparent depth; `showInFront` objects use an independent in-front opaque plus transparent depth map from both compositor routes. QPainter control cages and handles also test retained depth when available; regular ones hide behind in-front coverage, and X-Ray keeps them visible through geometry. | Theme wire/edit colors are configurable for supported stroke paths; Blender theme import, unsupported Smooth Wires geometry, transparency, and other fallback differences remain |
| Shadow controls | The Shift and Focus values feed Blender's normal/light falloff calculation in GPU and QPainter, including the `1 - focus * (1 - shift)` upper edge. Blender's pinned implementation is volume-only; classiCAD's GPU map fallback keeps a fixed depth bias and approximates volume occlusion for unsupported casters. Closed GPU shadow volumes extrude along Blender's light-ray direction and cap just inside the view far plane. QPainter uses a bounded CPU light-depth approximation for tessellated NURBS and polygon meshes, including binary transparent-material casters. | Partial; setting semantics are adapted, while the classiCAD-only depth-map fallback and QPainter shadow algorithm/coverage remain approximate |
| GPU fallback | QPainter has a separate shading implementation. Its committed scene uses increasing supersampling for non-Off AA choices, without temporal accumulation or SMAA. Smooth Wires are independent of that scene AA hint for the supported categories: Overlay controls committed curve strokes and wireframe surface lines; Edit Mode controls cage/control-polygon lines and categorized prepared fallback segments. If native GPU stroke drawing fails, the prepared-stroke fallback uses scene depth when available, sampled hidden/visible visibility, endpoint fades, and line patterns; this remains approximate, and without depth readback the visible pass is unoccluded. If the native renderer's main Solid surface draw fails, the viewport exits native painting and presents the same bounded QPainter surface compositor used by the ordinary fallback before resuming the normal native overlay sequence. Missing prepared geometry or a target over one million pixels retains the average-depth sorted painter path. The unjittered scene depth is then rebuilt, and selected-face highlights, scene strokes, grid, picture/tool previews, control points, and arc HUD draw in their regular order on top; prepared QPainter stroke fallback also runs after the fill. The fallback composites object-ID surface outlines from opaque-only IDs while selected-object contours continue to use visible-layer IDs, and draws selected NURBS surface/solid component edges after scene effects using retained depth when available. Those selected edges are unoccluded when depth was not retained. | Partial; renderer fallback differences are documented but not closed |

**Update (2026-10-12, corrected 2026-10-15):** trimmed NURBS use tessellation UVs for GPU Texture color, object-ID/outline alpha, and QPainter Texture color and cavity/outline alpha. Blender's filtered alpha cutoff is 0.1 when enabled; alpha-ignoring modes bypass it. QPainter effect sampling interpolates tessellated UVs perspective-correctly, normalized over each active knot domain. Trim boundaries remain tessellation approximations. Blender's shadow-volume pass does not sample texture alpha; classiCAD's GPU depth-map fallback now also casts the full geometry. This supersedes earlier limitations below that described trimmed NURBS texture or alpha coverage as unsupported. QPainter perspective correction also uses positive camera-forward reciprocal depth for mesh and NURBS rasterization; visual comparison is still open.

**Matched Studio Image Texture capture (2026-10-14):** Extended the offscreen matched-cube capture to render the same opaque active Image Texture in Studio lighting. With Blender 5.2.2's Default studio-light preset, AA off, zero dither, and constant UVs, 27 face-interior scene-linear probes match exactly (MAE/max 0); display RGB differs by MAE 0.1111/255 and max 1/255, with one right-face probe rounded by one display code value. This validates the active texture color reaching Studio lighting for this fixture. It does not cover varying UVs, filtering, alpha, alternate studio lights, OCIO float texture conversion, or edge coverage.

**Matched nonlinear OCIO Image Texture capture (2026-10-14):** Corrected `bake_image_source_transform` to use the RGB tuple returned by PyOpenColorIO's `applyRGB`; the previous call discarded it, and the prior expected-value assertion did the same. The Blender 5.2.2 texture-source contract now compares against the returned transformed RGB and asserts conversion differs from source values. GPU polygon-mesh ranges also treat a scene-linear float payload as an available texture when it has no byte path. A Blender-generated RGBA32F payload from the same opaque PNG, loaded as `Gamma 2.2 Encoded AdobeRGB`, was compared with a Blender Workbench Flat render (AA off, zero dither, constant UVs): 27 face probes have scene-linear MAE 0.0000102/max 0.0000305 and display MAE/max 0. This verifies nonlinear OCIO extraction through GPU texture sampling for one opaque texel; interpolation, alpha modes, other OCIO spaces, and edge coverage remain open.

**Native-renderer failure transparency fallback (2026-10-14):** When the bounded native-failure QPainter compositor receives no prepared depth mesh, it can now derive per-pixel depth from `drawShape`'s scene-linear fragment stream and use the existing bounded scene-linear transparency resolve. The opt-in is limited to the native-renderer failure path, a target of at most one million pixels, available fragment output, no deferred surface-layer request, and no cavity setting without prepared depth. Empty/unsupported geometry, four-million-fragment overflow, and other rejected cases retain the legacy painter fallback. Two overlapping transparent quads with missing prepared geometry produce the same display image as the prepared-depth reference under both Standard and AgX in the focused viewport contract. This still uses classiCAD's per-pixel sorted transparency approximation rather than Blender's weighted OIT; Blender 5.2.2 accumulates shaded transparency before display conversion in [`workbench_transparent_accum_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_transparent_accum_frag.glsl#L516-L524) and resolves it in [`workbench_transparent_resolve_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_transparent_resolve_frag.glsl#L277-L286).

**Shadow fallback audit (2026-10-12):** Blender 5.2.2's `workbench_shadow.cc` and `workbench_shadow.bsl.hh` implement stencil shadow volumes with pass/fail methods; the pinned Workbench code has no depth-map fallback. classiCAD's directional depth map is therefore an explicit renderer fallback for unsupported input geometry, not a parity port. The current shadow-volume path already adapts Blender's light-facing caps, silhouette side quads, and depth-pass/depth-fail stencil choice. No safe additional Blender-derived code change was identified in this bounded fallback audit. Sources: [workbench_shadow.cc](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_shadow.cc), [workbench_shadow.bsl.hh](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_shadow.bsl.hh).

### GPU transparent polygon-mesh shadow volumes (2026-10-14)

- Imported polygon meshes split into per-face draw ranges previously lost
  their shared stencil volume whenever any range was transparent. Those
  meshes fell back to the directional depth map, which samples image alpha.
  The renderer now retains validated shared topology when all face ranges
  have draw geometry, independent of alpha. If any face range is transparent,
  the volume uses forced depth-fail for the whole mesh, matching Blender's
  object-level `has_transp_mat` handling. Its binary stencil mask still does
  not sample image alpha; the depth-map fallback remains available for
  unsupported topology or incomplete ranges.
- Blender 5.2.2
  [`workbench_shadow.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_shadow.cc#L1720-L1790)
  sets `FORCED_FAIL` for transparent-material objects, then submits both
  surface caps and extruded sides. The GPU contract uses one imported mesh
  with opaque receiver faces and a raised, transparent textured component; it
  verifies that the component still changes the receiver through a binary
  stencil shadow even when its texture alpha is zero.
- Validation: `cmake --build build -j4` passed with the dependency audit;
  offscreen `viewport_render_contracts` and
  `workbench_raster_fragment_contracts` passed (2/2). Matched Blender image
  comparison, unsupported topology, and broader shadow geometry remain open.

### GPU transparent shadow lighting term (2026-10-15)

- Blender's transparent accumulation shader applies its normal-based shadow
  lighting term but does not sample opaque shadow-volume visibility. GPU
  transparent draws now keep the directional lighting factor enabled while
  disabling shadow-map and volume visibility for those faces. The same rule
  applies to in-front transparency. This follows Blender's shader structure;
  no matched image comparison has been run for the change.

**Transparency-domain plan correction (2026-10-15):** The QPainter Solid
compositor already keeps regular and `showInFront` opaque/transparent domains
separate in its weighted and sorted paths, and existing contracts exercise
their ordering. Only the direct painter fallback used when the bounded
compositor is unavailable lacks separate in-front domains. The no-shared-target
GPU fallback and broader visual comparisons remain open.

### Remaining parity gaps

- **Render result:** File → Render Workbench Result (F12) renders visible document geometry into viewport, HD, Full HD, QHD, or 4K output. Imported Blender scene resolution percentage accepts Blender's 1–32767 range and computes each output dimension with integer truncation, matching `BKE_render_resolution`; the app retains its 8192-pixel output cap. Imported render borders preserve `use_border`, `use_crop_to_border`, and normalized bottom-left border coordinates. Border endpoints truncate after scaling by the full frame dimensions, then convert Blender's bottom-left Y origin to the image's top-left rows. The renderer resolves the camera against the complete requested frame before applying the region: crop enabled crops Combined, depth, and scene-linear buffers to the selected rectangle; crop disabled retains full dimensions and clears pixels outside the rectangle, using Blender Workbench's `1e10` background depth sentinel. It uses the evaluated scene camera when available, renders fills through the shared configured resolve, applies the existing camera DOF pass, then draws CAD strokes and composites committed dimensions. It saves Combined plus view-space Depth as PFM; when OpenEXR is available, Combined EXR also contains optional `Z`, written as HALF or FLOAT to match the selected color depth. The imported pixel aspect informs camera sensor fit and shift and is recorded in image metadata. The Combined image also writes per-axis pixel-density metadata for Qt image writers that preserve it; image pixels are not resampled. The resolved active-scene Blender render filepath now seeds the Combined save dialog, with a generic `WorkbenchRender` filename when no path was imported; Blender frame tokens are retained literally and not expanded. The GPU Workbench render passes are represented by a standalone result object rather than Blender's host-registered `RenderResult` datablock. Blender 5.2.2 border mapping is now verified against the pinned render initialization in [`pipeline.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/render/intern/pipeline.cc#L1691-L1705), which scales normalized bounds by full-frame dimensions before storing integer rectangle endpoints. A nonblank Blender 5.2.2 CLI Workbench render at 8×6 with asymmetric fractional endpoints `(0.21, 0.31, 0.82, 0.72)` produces the expected `[1,6) × [2,5)` region: cropped output is 5×3 and matches those pixels from the full 8×6 render exactly; non-cropped output leaves alpha zero outside the region. The production contract checks the same rectangle across Combined, depth, and scene-linear buffers. Broader render-result camera and visual comparisons remain open. Remaining render differences are covered by the pass, camera-DOF, color-output, and visual-comparison gaps below.
- **Shading shortcut:** Z opens a hold-Z radial menu in idle Select mode with Solid, Wireframe, X-Ray, and settings directions. Blender's other shading modes are absent because classiCAD has no Material Preview or Rendered modes.
- **Color inputs and material assignment:** Blender scene mesh import preserves per-face material slots, color attributes, UV maps, and image references. A real Blender 5.2.2 material-slot `.blend` now passes through the production importer, Document records, viewport frame adapter, and QPainter renderer; its matched capture and measured residuals are recorded above. Material/Object/Random/Custom/Attribute mesh colors render through GPU and QPainter paths. GPU Texture mode samples the active Image Texture Node through interpolated active-UV coordinates and a per-fragment sampler; external raster files use repeat wrapping and linear filtering. Missing images and missing UV maps fall back to face Material color. QPainter Texture mode rasterizes each triangle per target sample, performs reciprocal-depth perspective correction, and bilinearly samples external and packed images with repeat wrapping. Both paths apply PREMUL handling and the 0.1 cutoff to the filtered alpha sample except for NONE (Ignore) and CHANNEL_PACKED modes. Remaining QPainter differences include display color management and raster edge coverage. Blender-style object-name/library-path Random identity is used by both renderer paths, with ObjectId fallback for legacy nameless objects.
- **Transparent sorting:** The primary GPU scene now uses weighted blended OIT, so intersecting/cyclic triangle overlap no longer depends on centroid draw order and transparent surfaces draw once per range. The auxiliary GPU path uses range-level far-to-near sorting when it has no shared depth attachment. The ordinary QPainter Solid path and native-renderer failure fallback use a bounded scene-linear weighted OIT compositor: opaque coverage uses nearest per-pixel tessellation depth, and every eligible transparent triangle sample from each object is accumulated after opaque-depth rejection. The fragment sink removes only coincident same-pixel samples with matching depth, color, and alpha to limit duplicate shared-edge coverage. Targets above one million pixels or missing prepared geometry use the existing average-depth painter fallback; fragment overflow also returns to that fallback. The QPainter implementation remains a CPU approximation and does not share the GPU's depth attachment or raster coverage.
- **QPainter transparent sorting update (2026-10-13):** The weighted OIT compositor now retains depth-separated transparent samples within one object, fixing the previous nearest-fragment-per-object collapse. A headless contract uses one mesh object with two overlapping translucent sheets at separate depths. The captured and accumulated fragment buffers each retain the four-million-sample limit; fallback behavior remains available when a limit is exceeded. The GPU path remains the source-matched implementation, while QPainter raster edge coverage and overflow fallback still differ.
- **Display transform:** imported scene display device, view transform, look, exposure, and gamma are retained for Workbench render results. The interactive Solid/Wireframe viewport keeps the default AgX settings; applying imported scene settings unconditionally there differed from Blender, where ordinary Workbench Solid uses the default view transform and only Rendered mode or View3D Scene Lights/Scene World selects scene render settings. classiCAD does not yet expose those View3D modes. Existing built-in sRGB AgX, Standard, Khronos PBR Neutral, and Filmic with the `None` look stay on their established transform paths. Other sRGB view/look combinations are baked to scene-specific 65³ RGB float LUTs by Blender's bundled PyOpenColorIO and applied in CPU/GPU rendering, with exposure before the table and gamma after it. This is an import-time snapshot of the machine's active OCIO configuration, not a runtime OCIO dependency; custom external config assets must be available to Blender during import. New LUTs sample a [0, 16] domain using an analytic `log2_1p` shaper (scale 64); older native documents preserve their linear [0, 2] metadata. Inputs outside a LUT's domain clamp to its endpoint. The document stores at most eight unique tables and 32 MiB decoded LUT data. A controlled Blender 5.2.2 PyOpenColorIO comparison of the AgX + Very High Contrast look against the 65³ log-shaped LUT sampled trilinearly at 41,000 deterministic scene-linear RGB inputs measured mean absolute channel error 0.000116, p95 0.000177, p99 0.00241, and maximum 0.0546 (0–1 display channel scale). For inputs in [0, 1]³, mean error was 0.000194 and p95 0.000371; the same probes with a linear [0, 16] LUT had 0.1758 mean error. This supports the current log-shaper as a bounded 65³ approximation, not exact transform parity. It compares OCIO evaluations to table interpolation, not Blender/classiCAD rendered pixels or final display quantization. Other display devices remain unsupported. Blender 5.2.2 selects the color-management context in [`draw_color_management.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/intern/draw_color_management.cc#L44-L57) and applies its default transform versus scene settings in [`viewport_settings_apply()`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/intern/draw_color_management.cc#L112-L140). Its display processor composes an explicit look before the DisplayViewTransform and applies exposure in scene-linear space and gamma afterward ([pinned source](https://github.com/blender/blender/blob/v5.2.2/source/blender/imbuf/opencolorio/intern/libocio/libocio_display_processor.cc)).
- **Camera depth of field:** the GPU viewport and offscreen render-result path share the Blender 5.2.2 `DofPass` shape: half-resolution RGBA16F color and RG8 near/far CoC pyramids, full-resolution CoC-aware preparation, quarter/eighth-resolution downsample levels, half-resolution 49-sample gather, 3x3 median cleanup, and a depth-derived CoC resolve before AA accumulation. The shader's sample perimeter and square-to-circle radius match `DofPass::setup_samples`; polygon blade shaping uses the algebraically equivalent sector formula, rotation, and reciprocal X aperture ratio. Its two-blade denominator no longer uses an extra epsilon floor. The gather-origin temporal jitter follows Blender's `sample / samples_len` offset and exact IGN/blue-noise vector equation; focused sequence contract coverage is added, but matched per-sample GPU output remains unverified. The aperture path remains unverified against Blender images, especially the visual two-blade slit. The textures and passes are kept separate from final presentation; the existing camera gate and per-temporal-sample placement remain. QPainter uses its separate full-resolution software approximation with Blender's generated 49-tap aperture kernel and now applies Blender's blue-noise/IGN gather-origin jitter with temporal offset zero; it has no CPU temporal accumulation, and matched QPainter image parity remains open. Scene-camera optics persist with project, session, and user preferences; imported Blender scene cameras populate the inputs; DOF runs in ordinary orbit and orthographic views whenever an active scene camera has DOF enabled and the Workbench DOF option is on.
- **Additional shadow-map behavior:** unsupported casters and nonmanifold/edited solids use the GPU 1024-pixel depth-map fallback with one bilinear depth comparison. This reduces softness toward Blender's binary stencil-volume mask, while the map remains a classiCAD-only approximation with narrower caster coverage. Blender's pinned [`workbench_composite.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_composite.bsl.hh#L635-L639) samples the current-pixel stencil value once and tests it as a boolean; Blender has no directional depth-map fallback. Both the GPU depth-map and QPainter software shadow fallbacks cast full geometry without Image Texture alpha, matching Blender's geometry-based shadow-volume input. The GPU code path is `renderShadowMap` in `src/ui/viewport/viewport_surface_renderer.cpp`; the software caster is assembled in `src/ui/viewport/viewport_raster_workbench_effects.cpp`. Blender's pinned [`workbench_shadow.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_shadow.cc#L1720-L1790) and [`workbench_shadow.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_shadow.bsl.hh) show that the dedicated shadow pass submits geometry to shaders without UV or image resources. Open NURBS surfaces and imported polygon meshes with manifold interiors and simple closed boundary loops use boundary-edge forced-fail stencil volumes. Polygon meshes with branched boundaries, loose vertices, inconsistent shared-edge winding, or nonmanifold edges remain on the depth-map fallback. In mixed-alpha scenes, opaque surfaces remain shadow receivers, and validated closed transparent solids plus supported open surfaces cast stencil shadow volumes; the depth-map fallback is limited to unsupported casters when volumes are active, and transparent triangles are drawn once without shadow sampling. Blender's handling of branched, nonmanifold casters and additional fallback fidelity remain unported. QPainter retains its separate bounded CPU light-depth shadow approximation for triangulated NURBS and polygon meshes. Transparent-material triangles cast binary shadows without material-alpha scaling; the software path still differs from Blender's stencil volumes. Retained-layer composition shades opaque receivers beneath transparency and the nearest visible transparent receiver contribution. Multiple transparent layers share an approximate aggregate factor. The GPU and QPainter outline silhouette opacity now uses Blender Workbench's four-cardinal-neighbor resolve. For classiCAD's separate selection-color extension, both renderers use four-cardinal-neighbor adjacency; Blender itself uses one outline color and has no selected-color choice.
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
  its transparent fragments against its own opaque depth. In-front transparent
  fragments therefore bypass regular depth but remain occluded by nearer
  in-front opaque surfaces. Retained cavity
  layers contain the composed opaque receiver and ordered transparent overlay.
- **2026-10-15 correction:** the bounded sorted fallback now applies that
  in-front opaque-depth test to `showInFront` transparent fragments as well as
  regular fragments. Previously it let front transparency show through front
  opaque geometry. Blender attaches `depth_in_front_tx` to its separate
  in-front transparent pass (see the pass setup and draw linked below). The
  C++ build passed; no matched Blender image comparison has been run for this
  fallback case.
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
- **Still different:** the QPainter fallback is a separate renderer. Its cavity composition, object-ID outline compositing, antialiasing, and transparency handling remain approximate compared with the GPU path. Polygon-mesh Texture cutouts honor active UVs, image interpolation/extension, and the supported filtered-alpha cutoff in GPU and QPainter color/effect coverage; QPainter software shadows use caster geometry without image alpha. QPainter samples texture color per target sample with reciprocal-depth correction. GPU and QPainter NURBS Texture use exact-domain tessellation UVs on trimmed and untrimmed surfaces, matching Blender 5.2.2's uniformly sampled NURBS grid mapping. GPU and QPainter effect alpha coverage also handles trimmed NURBS through the color, object-ID, shadow, cavity, and outline paths. Trim boundaries follow tessellation approximation; selected surface/solid component-edge overlays exist in the raster fallback, while other component-edge coverage and edge-depth fidelity remain incomplete. QPainter Cubic/Smart filtering may differ from Blender's optimized helper.
- **Still different:** QPainter cavity uses opaque-only samples and is omitted where a visible translucent surface overlays an opaque receiver; GPU cavity shades opaque ranges while leaving transparent ranges out of the cavity pass. Blender composes its effects through dedicated Workbench passes. GPU and QPainter outline passes run with partial-alpha scene content. QPainter texture cutout coverage includes polygon meshes and trimmed or untrimmed NURBS surfaces; its raster/effect composition remains approximate.

#### 2. Color, materials, and shading effects

- **Ported since the initial comparison:** Attribute and Texture color inputs now reach imported polygon meshes; material records and per-face material slots are retained and used by both renderers. These findings should not be described as missing mesh inputs. General shader-node evaluation is not a Workbench parity requirement: Workbench Material uses material display fields, while Texture uses the active Image Texture Node. GPU and QPainter NURBS Texture sample trimmed and untrimmed surfaces with exact-domain tessellation UVs matching Blender's uniformly sampled NURBS grid. GPU Cubic/Smart interpolation may differ from Blender's optimized helper.
- **Ported in this continuation:** Blender Image Texture `interpolation` and `extension` are extracted, persisted with backward-compatible defaults, and consumed by GPU and QPainter mesh/NURBS Texture sampling plus GPU/QPainter alpha coverage for effects and outlines. Closest, Linear, Cubic/Smart and Repeat, Extend, Clip, Mirror are supported. QPainter alpha sampling preserves Mirror for arbitrary out-of-range UVs. GPU UDIM color, effect-alpha, and object-ID-alpha taps apply the selected extension against each tile's actual dimensions; shadow casters intentionally ignore texture alpha. QPainter UDIM color and alpha samplers select the tile from the original UV before flipping only the tile-local V coordinate for top-down image rows; the float and byte color samplers now follow that rule. Missing UDIM tiles remain magenta for color and opaque for alpha coverage. Cubic filtering may still differ slightly from Blender's optimized helper, and filtering does not cross neighboring UDIM tile layers in Blender 5.2.2, so the per-tile behavior is intentional. Trimmed NURBS alpha coverage now follows their tessellated UV triangles. Rendered output has not been visually compared.
- **Random color hash corrected in this port:** when imported Blender object identity is available, both GPU and QPainter now hash the NUL-terminated `OB`-prefixed object ID name with Blender's Murmur2 helper, combine linked-library paths as `(objectHash * 13) ^ libraryPathHash`, and convert the result through Blender's `BLI_hash_int_01`. Native or older nameless objects retain the existing `ObjectId` color fallback. Source: Blender 5.2.2 [`workbench_materials.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_materials.cc#L19-L33), [`BLI_ghash_utils.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/blenlib/intern/BLI_ghash_utils.cc#L146-L151), [`hash_mm2a.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/blenlib/intern/hash_mm2a.cc#L96-L122), and [`BLI_hash.h`](https://github.com/blender/blender/blob/v5.2.2/source/blender/blenlib/BLI_hash.h#L50-L60). `BLI_hash_int_01`'s final normalization is at [lines 84–92](https://github.com/blender/blender/blob/v5.2.2/source/blender/blenlib/BLI_hash.h#L84-L92). Configurable theme wire/edit colors are also supported. Full-frame GPU cube comparisons cover Material, Object, Random, constant- and varying-UV Texture, Custom/Single, and per-corner Vertex Attribute; point/face domains, texture alpha/UDIM, other geometries, and QPainter output remain open.
- **Still different:** the GPU shadow path uses validated stencil volumes for supported closed `NurbsSolid` geometry and validated open `NurbsSurface` meshes; unsupported/nonmanifold casters retain the GPU depth-map fallback. QPainter uses a bounded CPU light-depth approximation for triangulated NURBS and polygon meshes, including transparent-material casters; it does not reproduce Blender's GPU stencil pass and does not scale transparent caster shadows by material alpha; retained translucent layers preserve shadows on opaque receivers underneath, and the nearest visible translucent receiver contribution is shaded separately. Multiple transparent layers share an approximate aggregate factor. Outline detection uses classiCAD's object-ID edge pass. QPainter effects remain approximate. GPU and QPainter depth of field follow Blender's active-scene-camera DOF gate in perspective and orthographic views. QPainter now has Blender-derived near/far CoC filtering, three color/CoC mip levels, adaptive mip sampling, and per-pixel gather-origin jitter; its temporal offset remains zero because it does not accumulate frames. A matched GPU cube capture has been compared with Blender's DOF output; per-sample GPU output and broader QPainter DOF output remain open for visual comparison.
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
  background opacity/blur are preserved as Eevee Lookdev metadata, not flagged
  as Workbench Solid gaps. Scene Lights/Scene World render modes also remain
  outside this interactive Solid settings mapping.
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
- `studiolight_background_alpha` and `studiolight_background_blur` remain in
  the extracted shading record for forward compatibility, but are no longer
  reported as unsupported Workbench features. The pinned Blender 5.2.2
  Workbench path uses Studio `.sl` data for its diffuse/specular lighting and
  takes its flat background from the selected Theme/Viewport/World color; it
  does not bind a Studio HDRI or consume these controls. Blender routes them
  through the Eevee Lookdev World shader instead: [`eevee_lookdev.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/eevee/eevee_lookdev.cc#L109-L172),
  [`eevee_world.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/eevee/eevee_world.cc#L117-L138),
  and [`eevee_surf_world.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/eevee/shaders/eevee_surf_world.bsl.hh#L67-L100).
  Import preserves the two raw values without claiming that classiCAD
  implements Eevee Lookdev. The Blender 5.2.2
  `blender_workbench_shading_contract.py` verifies non-default values survive
  extraction and do not enter the Workbench unsupported list.
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

The screenshot's Overlay and Edit Mode checkboxes are Smooth Wires preferences. They control antialiasing for the corresponding wire categories. They are separate from Workbench surface shading and from the grid's own line rendering. Blender 5.2.2's RNA source (`rna_userdef.cc`, `use_overlay_smooth_wire` and `use_edit_mode_smooth_wire`) confirms Overlay updates live while Edit Mode says “requires restart”; both properties request a GPU update. The Overlay wireframe pass (`overlay_wireframe.hh`) enables its custom depth bias only for the mesh-wire shader; its curve-wire shader has neither that mesh bias nor the view-dependent mesh NDC offset. classiCAD routes the two settings to separate stroke categories, applies the mesh-specific offset/resolve only to Blender mesh wires, leaves Overlay curve strokes unbiased, and keeps its CAD Edit Mode offset separate.

- Overlay smoothing routes to committed curve strokes and wireframe surface lines; Edit Mode smoothing routes to component controls/cages and Solid-mode `NurbsSolid` boundaries. The GPU categories stay independent of each other, the grid, and surface antialiasing.
- The offscreen render contract now captures both GPU stroke categories with all four Overlay/Edit Mode preference combinations. It checks that changing one preference changes only its matching category. The grid continues through its own MSAA framebuffer, while filled surface edges and Workbench silhouettes stay on the separate scene AA/outline path.
- Overlay Smooth Wires updates live in both Blender 5.2.2 and classiCAD. Edit Mode Smooth Wires takes effect after restart in both applications; classiCAD saves the requested value while keeping the active viewport value until relaunch.
- The grid renderer's independent MSAA coverage is tested both in isolation and in composed offscreen captures with a representative Overlay stroke. The composed contract toggles Overlay Smooth Wires, confirms the stroke changes, and compares partial-coverage grid pixels away from the stroke to ensure grid antialiasing remains unchanged.
- Opaque depth-writing GPU strokes now use a per-pixel line resolve adapted from Blender 5.2.2: record line color, signed perpendicular direction/distance, width, and depth, then resolve the center and four cardinal neighbors with depth-aware alpha-over/under. Neighboring wire samples alpha-over only when strictly nearer; equal-depth neighbors use Blender's alpha-under branch. The dense one-pixel-line correction now runs after the neighboring wire samples have been blended, matching Blender's pass order, and checks the original scene pixel's alpha. The resolve skips neighbors outside the target. Blender's pinned shader issues unguarded `texelFetch` calls for those coordinates; with no defined clamp or margin, out-of-range results are undefined, so classiCAD keeps the bounds guard rather than relying on driver behavior. It runs only for single-sample targets with a supported matching depth format, full color writes, full opacity, solid strokes, and depth-writing line geometry; other strokes retain the existing direct blend path. The fullscreen resolve writes the nearest accepted line depth while preserving scene depth where no line candidate contributes; it does not copy expanded-quad depth back wholesale. Private depth textures match the caller's depth attachment format for the scene-depth blits. Source: [`overlay_antialiasing.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/overlay/overlay_antialiasing.hh#L16-L24), [`overlay_antialiasing.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/overlay/shaders/overlay_antialiasing.bsl.hh#L22-L38) and [#L82-L180](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/overlay/shaders/overlay_antialiasing.bsl.hh#L82-L180). Runtime GPU captures and matched Blender crossing-wire comparisons remain unverified in this environment; transparent, patterned, non-depth-writing, and multisampled strokes still use the direct path.
- Remaining fallback gap: the prepared-stroke fallback used when native GPU stroke drawing fails now applies depth-aware visibility when scene depth can be read, endpoint fades, and line patterns. Its visibility is sampled and approximate; without depth readback, visible strokes are unoccluded. The direct QPainter drawing paths still do not share the GPU stroke pass's full depth-aware treatment.

## Implementation stages

### Stage 0: Baseline audit

Status: completed against Blender 5.2.2 and classiCAD source. Updated after implementation review.

- Pin source review and screenshots to Blender 5.2.2.
- Trace each preference from its UI/RNA definition through Workbench state and draw passes.
- Trace each classiCAD preference through serialization and into the renderer.
- Mark each option as implemented, partial, or missing in the baseline table above.

### Stage 1: Make anti-aliasing control the viewport

Status: implemented for the application's current interaction model. The GPU path uses Blender's first jittered 5-sample location for its one-sample TAA plus SMAA at preference value 1; 5+ modes accumulate jittered TAA samples plus SMAA while idle and reset to one unjittered sample during navigation. The viewport exposes `setViewportPlaybackActive(bool)`, but classiCAD has no scene timeline or playback owner; wiring that hook is deferred until playback exists. Scene strokes and selected-face overlays redraw after scene resolve using the unjittered camera and regenerated scene depth; the unjittered grid follows. Pixel-for-pixel equivalence has not been verified, and QPainter remains an approximation; those checks stay in Stage 5.

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

Status: partially implemented. GPU cavity/curvature and several existing effects are wired. Cavity settings now persist Blender 5.2.2 defaults/ranges, the renderer distributes SSAO samples across AA iterations with Blender's cap, and its jitter uses Blender's blue-noise-derived texture. In mixed-alpha scenes, the GPU effect prepass uses opaque ranges and supported texture-alpha cutouts; transparent ranges do not write cavity normals, IDs, or depth. The primary GPU scene uses weighted OIT with shared opaque depth: transparent lit RGB is composited over opaque scene-linear color, then depth of field and temporal AA run before exposure, active view transform/LUT, and gamma are applied once at presentation. Backgrounds, picture samples, and filled surfaces use the same scene-linear target; the offscreen result shares this final transform. This matches Blender's transform-after-AA order, while its sample weighting and SMAA output still need visual comparison. The auxiliary no-shared-depth path sorts ranges. QPainter's bounded sorted compositor captures individual transparent triangle fragments and orders them per pixel in scene-linear color. The interactive QPainter viewport caps its internal raster target at one million pixels, reducing its supersampling scale as needed so ordinary large viewports can still use retained OIT/depth layers. The four-million-fragment cap and unsupported geometry/offscreen targets above the cap retain the older average-depth painter fallback. QPainter's bounded sorted fallback now composes captured opaque and transparent fragments in scene-linear space, applies cavity before transparency, and transforms once. Its software cavity sampling remains approximate. Camera-gated GPU depth of field uses a shared viewport/offscreen pass with source-matched gather jitter, sample layout/weights, CoC-dependent LOD, and aperture shaping; the two-blade visual result, QPainter's separate full-resolution DOF approximation, and matched output comparison remain open; GPU temporal jitter follows the pinned sequence. Validated closed opaque `NurbsSolid` casters use depth-pass stencil volumes only under the camera near-plane safety gate; closed fallback cases, open `NurbsSurface` casters, and transparent casters use capped depth-fail stencil volumes. Unsupported topology retains the depth-map fallback. GPU `NurbsSolid` face boundaries draw in the Edit Mode wire pass. Runtime/pixel comparison remains open.

- Compare and port culling, X-Ray, shadow, outline, specular, cavity, curvature, and depth-of-field behavior.
- GPU OIT accumulates shaded scene-linear RGB and composites into the RGBA16F opaque target. Its resolve clamps summed weights to Blender's `[1e-4, 5e4]` range and directly divides the signed RGB sum, matching Blender's pinned [`workbench_transparent_resolve_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_transparent_resolve_frag.glsl#L269-L286); accumulation weights and revealage also match [`workbench_transparent_accum_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_transparent_accum_frag.glsl#L435-L470). GPU color management now runs after scene-linear DOF and temporal AA, at presentation, using the same imported custom LUT or built-in transform. Background and image-preview inputs are decoded into scene-linear values before that target. The bounded QPainter compositor now resolves scene-linear surfaces before display conversion; older painter fallbacks and later software effects still use display-space images. Blender's pass order is recorded in its pinned [`workbench_engine.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_engine.cc#L2618-L2645) and [`workbench_effect_taa_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_effect_taa_frag.glsl#L9-L24); visual output comparison remains open.
- The bounded QPainter compositor declines targets above 1,000,000 physical pixels and unsupported/prepared-depth or fragment-budget cases. Those cases fall back to display-space painter output before software effects, so effects can differ from Blender's scene-linear pass ordering there. Correcting this needs a separate bounded linear fallback layer; reverse-transforming the display image or disabling effects would not preserve supported behavior.
- Map Blender's World cavity action to SSAO and Screen to curvature (the pinned View3DShading RNA enum definition); Both enables both. Persist Blender's SSAO distance/attenuation/sample settings and cavity/curvature ridge/valley factors with the source defaults and RNA ranges; drive the shader's corresponding uniforms and distribute the selected SSAO sample count across AA iterations, capped at 512 total samples.
- Match effect enable flags, setting dependencies, and opaque pass composition to Blender's source. Keep transparent-scene differences explicit; the source-derived cavity jitter is implemented but still needs visual comparison.
- For closed `NurbsSolid` casters, derive indexed geometry from the cached base/trim tessellation and extrusion walls, or from validated planar materialized quads. Require every indexed edge to have exactly two oppositely directed uses and reject zero-volume or degenerate meshes. For open `NurbsSurface` casters, accept only tessellations with consistently wound two-manifold interiors and closed, unbranched boundary loops; add forced-fail side quads along boundary edges. Use depth-pass stencil counting for closed opaque solids only when every source and extruded volume vertex lies safely beyond the camera near plane; retain the capped depth-fail path for open, translucent, near-plane-risk, and otherwise uncertain volumes. Extrude vertices to the current view's far plane with Blender's signed-distance/light-plane-dot formula and `1e-3` inward epsilon; use the `1e5` distance when the light direction does not intersect that plane. Tessellation remains display data derived from the persisted NURBS geometry.
- Pass audit against Blender 5.2.2 `workbench_shadow.cc`: Blender submits manifold geometry to a depth-pass `PASS`, while `FAIL` and `FORCED_FAIL` use depth-fail counting; translucent casters and selected non-manifold cases skip `PASS` and use `FORCED_FAIL`. classiCAD now uses Blender-style depth-pass stencil updates for the conservative closed opaque `NurbsSolid` and validated `PolygonMesh3D` subsets when all volume vertices are in the camera-visible half-space with a safety margin. It draws silhouette sides only for this route; capped depth-fail volumes remain the fallback for open surfaces, transparent casters, and any near-plane or geometry uncertainty. Compare near-plane crossings, camera-inside views, and perspective/orthographic projections against Blender before widening eligibility.
- The GPU stencil-volume path now also accepts imported `PolygonMesh3D` objects only when their transformed triangulated surface is finite, nondegenerate, closed, consistently oriented (each indexed edge has exactly two oppositely directed uses), has one connected cycle in each vertex link, and every connected component has positive nonzero signed volume. Every face material range for that object must be opaque under the active Workbench alpha source. Loose vertices, open boundaries, duplicate/overused edges, pinched/disconnected vertex links, inconsistent winding, degenerate or inward/zero-volume components, mixed-alpha objects, and failed near-plane safety checks stay on the existing depth-map or depth-fail fallback. The volume topology is stored once on the first per-face draw range while every face range is marked as belonging to that caster, preventing fallback-map double drawing after a successful volume pass. This extends Blender's evaluated-mesh topology dispatch without making tessellation authoritative; runtime comparison remains open.
- Keep the GPU depth-map fallback for unsupported/nonmanifold topology, nonplanar materialized solid faces, missing stencil buffers, and ranges that cannot use the volume path. The fallback rasterizes selected triangle ranges into a fixed 1024×1024 light-depth map and casts complete triangles without Image Texture alpha, matching the geometry input to Blender's shadow-volume pass. This describes the GPU depth-map fallback; the GPU stencil shadow path also does not sample texture alpha. QPainter separately uses its bounded software depth shadow for opaque triangulated NURBS and polygon meshes; its caster and receiver coverage and depth-map algorithm remain distinct from Blender's GPU stencil pass. QPainter software shadow casters also use complete triangle coverage without material or Image Texture alpha, including in Texture mode; the color and effect passes retain their separate alpha-cutout behavior. Hard shadowing, map resolution, and remaining caster cases differ from Blender's ray-traced shadow option. In mixed-alpha scenes, opaque surfaces remain receivers and validated closed solids/open surfaces can cast volumes; transparent triangles draw afterward without shadow sampling. Branched/nonmanifold caster dispatch and Blender's ray-traced shadow option remain unsupported.
- Map the stored scene light direction through Blender's Y/Z swap and X/Z sign change. Clamp Shift to Blender's `[0,1]` range and compute `focus = clamp(shadow_focus, 0.0001, 0.99999)`, `shadow_focus_edge = 1 - focus * (1 - shadow_shift)`, then apply `smoothstep(shadow_shift, shadow_focus_edge, -dot(N, light_direction_view))`; shadow intensity mixes the result with the unshadowed contribution. Keep the fallback depth bias fixed so Shift no longer changes map bias.
- Keep GPU and QPainter depth of field camera-gated by an active scene camera with DOF enabled, regardless of view projection. The QPainter software path now uses camera-relative depth, applies the Blender aperture-radius factor, and has focused production-path tests for perspective and orthographic focus/defocus plus orthographic translation invariance. Validate GPU and QPainter aperture blade/rotation/ratio shaping against Blender. The GPU temporal sample lifecycle and DOF gather jitter now follow Blender's pinned sample-offset/vector equations; the focused contract is added, while matched per-sample output remains open.
- Keep GPU cavity/curvature behavior aligned with Blender's composition; its parameter controls, sample scheduling, and blue-noise-derived jitter texture are wired. In mixed-alpha scenes the effect prepass and shader apply cavity only to opaque ranges. The QPainter fallback uses separate visible and opaque-only samples: transparent surfaces do not occlude cavity samples or receive cavity shading, and post-composite cavity is skipped where a translucent surface is visible above an opaque receiver. Its cavity pass on NURBS and imported polygon meshes remains approximate and needs visual comparison.
- Derive all surface display data from the authoritative NURBS geometry. Do not convert committed surfaces into authoritative meshes.

Acceptance: each enabled control affects the expected objects and pixels, and disabling an effect removes only that effect. Verify saved settings survive reopen and test SSAO sample scheduling at the 512-sample cap. Unsupported effects stay disabled or are clearly identified as approximations. Pixel-level cavity jitter comparison, transparent-scene cavity behavior in the GPU path, visual comparison of QPainter cavity on imported polygon meshes, and visual comparison of the closed-solid shadow path remain open parity checks.

### Stage 4: Complete Smooth Wires and viewport composition

Status: partially implemented. The two preferences are independent and GPU stroke categories are separated. Overlay smoothing applies live; the Edit Mode checkbox is saved for the next launch, matching Blender's restart requirement. Selected-face overlays and GPU scene strokes now redraw after Workbench resolve with the unjittered camera and scene depth, including cached-history frames; selected faces do not write depth, and X-Ray back/front behavior remains intact. Committed Solid-mode scene strokes opt into depth writes per stroke; X-Ray's hidden/back pass, control guides/cages, selected component edges, custom outlines, and transform previews remain no-write. The grid renders against its own surface-depth framebuffer and composites without depth testing. Solid-mode `NurbsSolid` faces now draw evaluated face boundaries through the GPU Edit Mode wire pass, independently of the Workbench silhouette outline. QPainter maps Overlay to committed curve strokes and wireframe surface lines, and Edit Mode to control-cage/control-polygon lines. Opaque depth-writing GPU strokes now use a Blender-adapted per-pixel resolve for the supported single-sample case; regular Blender mesh wires also sample center and neighboring scene depth and adjust eligible smooth-wire depth before hardware depth testing. Mesh wires receive Blender's view-dependent NDC depth offset in the GPU and QPainter paths. Runtime GPU shader captures and matched crossing-wire images remain unverified; this resolve behavior does not cover multisample frames or the QPainter fallback. The native-GPU stroke failure fallback reads regenerated scene depth when available, separates hidden and visible strokes for X-Ray, applies endpoint fades, and resolves continuous, dashed, dotted, and custom line patterns; sampled visibility and edge coverage remain approximate. The ordinary raster QPainter path now paints selected NURBS surface/solid component edges after scene effects, honors the Edit Mode Smooth Wires antialiasing preference, and tests sampled line points against retained regular/in-front depth without writing depth. It is unoccluded if the raster compositor did not retain depth. QPainter committed curve visibility uses separate regular and `showInFront` opaque/transparent depth domains, matching the Workbench in-front pass ordering; the offscreen contract covers opaque blockers and nearer/farther transparent surfaces. QPainter and GPU now keep Blender's general object outline controlled separately from the saved selected-object outline flag; the software contour uses selected bits and a cardinal-neighbor coverage approximation. Polygon-mesh Texture mode masks IDs using active UVs, per-face images, repeat wrapping, and supported alpha cutouts. GPU NURBS texture alpha masking covers trimmed and untrimmed surfaces; QPainter cavity/outline effects apply texture-alpha coverage to trimmed and untrimmed tessellated NURBS. Category-to-Blender pass mapping and visual output still need verification.

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
| Volume data and rendering | Workbench reads the active evaluated scalar grid, uses Blender's volume texture transforms and display density, draws after transparent depth, and ray-marches with per-axis five samples per voxel, Bayer/Halton jitter, and closest/linear/cubic sampling. The pinned Workbench volume shader leaves shadow sampling as a TODO. | Blender import transfers one bounded single-channel `FLOAT` grid through evaluated Geometry Nodes mesh attributes; native JSON stores sparse voxel/tile records and display controls. `ViewportSurfaceRenderer` reconstructs a bounded dense texture and draws a volume pass in the matching pass slot. QPainter now supports bounded scalar slice and ray-marched modes with the existing transforms, density interpolation, scene-linear composition, and depth clipping; matched output remains open. | Preserve the bounded scalar grid and material contracts. Multiple/vector grids, animation, and oversized dense bounds remain unsupported; matched QPainter output and comprehensive occlusion comparisons remain open. |
| TAA/SMAA resolve | Blender uses a centered jitter sequence, a 3x3 Blackman-Harris filter in log color, accumulated weights, and an SMAA resolve that fades as temporal samples accumulate. | The GPU implementation follows those algorithms closely. The QPainter fallback instead supersamples and downsamples; it does not run the GPU TAA/SMAA sequence. | Controlled GPU renders match at AA Off and Single Pass. A source-derived AA-off camera-shift fixture now compares all eight raw samples; their silhouettes differ by at most two pixels per sample. The resolved scene-linear 8-sample silhouette masks now match exactly. Blender's background API still exposes only the final render, so per-sample comparison uses validated camera-shift renders rather than direct internal-buffer reads. Treat QPainter as a separate approximation. |
| Transparency/depth | Blender has separate transparent accumulation and transparent-depth passes following opaque drawing. `OBJECT_IN_FRONT` uses a second depth domain and merges sampled physical depth into scene depth before regular transparent depth excludes its pixels. After scene effects, the overlay-depth pass applies its opaque-object stencil domain and writes device depth zero at those in-front pixels before AA, giving final render-result Z its near-plane value. | The primary GPU shared-target path uses independent regular/front weighted-OIT targets and merges front depth/IDs before regular transparent depth. It keeps physical front depth through volume, outline, and DOF, then applies a late overlay-only near-plane override from a separately captured opaque-only front mask. Front transparent-only depth remains physical. The no-shared-target fallback retains sorted/regular behavior. QPainter keeps regular and in-front opaque/transparent compositor domains separate; the no-shared-target GPU fallback remains approximate. | A matched 640×480 GPU fixture verifies mid-pass merged physical front depth, front overlap color, opaque-only final near-plane Z, and physical transparent-only Z. Fallback and QPainter paths remain different; broader scenes and combinations still need comparison. |
| Outline and effects | Blender runs outline from its object-ID buffer, then DOF, then AA. Cavity/curvature uses Workbench-specific sample buffers and effect composition. | classiCAD generates normal/depth/object-ID inputs in its own prepass and uses custom outline/effect shaders. QPainter has separate software approximations. | Port and attribute the standalone shader/effect logic where inputs match. Preserve CAD-specific selection and NURBS tessellation handling. Validate each effect independently before combining them. |
| Geometry and materials | Workbench consumes Blender evaluated object batches and Workbench material records; its render engine is integrated with Blender scene, dependency-graph, draw-manager, GPU, and render systems. | classiCAD sends derived tessellation of exact NURBS surfaces/solids to its GPU renderer. The surface renderer has its own material packing and draw ranges. | Keep NURBS authoritative and pass only derived display data to the renderer. A future shader system should provide evaluated Workbench inputs; do not create a second Blender-shaped scene/geometry model. |

The previously supplied cube screenshots are not a controlled edge-quality comparison: the recorded views differ (`Ortho` versus `User Perspective`), the visible geometry differs, and the output dimensions differ. A same-geometry, same-camera, same-size GPU comparison confirms exact Off and Single Pass silhouettes. Per-sample references traced the 8-sample edge mismatch to the vertical projection-jitter sign: Blender adds positive Y to its orthographic projection, while classiCAD had negated it. After correcting that sign, the resolved scene-linear 8-sample silhouette masks match exactly.

**Multisample-count comparison (2026-10-13):** The Blender 5.2.2 cube fixture and classiCAD offscreen contract now cover 5, 8, 11, 16, and 32 samples. Each classiCAD capture can write both display PNG and resolved scene-linear PFM; Blender references use matching camera, AA mode, and scene-linear background. Face-interior probes match exactly in scene-linear RGB at 5/8/11/16, and 32-sample probes match scene-linear exactly with display MAE 0.1235/255 and max 1/255. Full-frame scene-linear PFM comparisons report foreground-mask IoU / XOR pixels of 0.99626 / 299 (5), 0.99717 / 226 (8), 0.99717 / 226 (11), 0.99999 / 1 (16), and 0.99997 / 2 (32). Two-pixel-band coverage MAE decreases from 0.01725 (5) to 0.00832 (32); per-channel whole-frame scene-linear MAE is at most 0.00018. This verifies the fixed orthographic cube, idle render path, and current background for all sample counts. Navigation image comparisons, non-cube scenes/effects, and QPainter AA remain open. Optional classiCAD capture variables are `CLASSICAD_VIEWPORT_AA_SAMPLES{5,8,11,16,32}_CAPTURE` and matching `_LINEAR_CAPTURE`; the Blender fixture accepts all five `--aa` sample-count values.

**Interrupted multisample history during navigation (2026-10-14):** A focused
GPU renderer contract covers Samples5, 8, 11, 16, and 32. For each mode it
advances to jittered sample 1, changes the camera while entering navigation,
and verifies a new history epoch with one centered sample and sample count 1.
On return to idle at the moved camera, it verifies another epoch reset,
centered sample 0 at the selected count, and a jittered sample 1 on the next
frame. Blender 5.2.2 likewise reduces navigation/playback to one unjittered
sample; the pinned state mapping is in
[`workbench_state.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_state.cc#L197-L225).
The C++ build and focused `viewport_render_contracts` test pass. This validates
renderer state and jitter placement, not navigation image equivalence;
application playback remains unwired because classiCAD has no playback owner.

Phase Two category audit: Blender 5.2.2 Workbench UDIM lookup in [`workbench_image_lib.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_image_lib.glsl#L348-L412) floors the UV to one tile, looks up that tile's array layer and coordinate scale/offset, and samples the resulting array coordinate. Filtering does not address neighboring layers. The classiCAD GPU samplers fetch taps from the selected layer and clamp/wrap/clip/mirror against that tile's dimensions; QPainter color/effect-alpha likewise resolve one tile before filtering. Therefore, cross-tile filter continuity is not a Blender 5.2.2 parity gap; retain the existing per-tile edge behavior and preserve magenta/opaque results for absent base tiles. Blender's outline shader [`workbench_effect_outline_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_effect_outline_frag.glsl#L10-L24) compares the center object ID with exactly four cardinal samples and applies one outline color. classiCAD's GPU and QPainter paths now use four-cardinal-neighbor adjacency for their additional selected-color behavior; this is backend consistency, not a Blender feature.

Phase Two Studio/MatCap/Flat comparison: the offscreen cube fixture selects the Workbench lighting mode and preset, rotates Studio lighting, supports world-space lighting, and disables output dithering for direct pixel comparisons. Its paired-capture comparator samples 27 face-interior points across the cube's top, front, and right faces and reports per-face errors. On reproduced Blender 5.2.2 captures using the corrected outward-wound cube fixture, Studio Default and Flat match exactly at all 27 probes; Studio Paint in world space at rotations 0 and 37 also matches exactly at all probes, and rotation 90 matches in scene-linear RGB with only 0.1111/255 display MAE and 1/255 maximum rounding difference. The earlier large Studio Paint rotation error cannot be attributed to winding alone: those captures were produced while other lighting experiments also changed, so a single-variable comparison remains open. The 2026-10-14 MatCap row-orientation correction makes `check_normal+y` match exactly at all 27 face probes in scene-linear and display RGB with specular disabled; `metal_bronze` matches scene-linear exactly and has 0.0988/255 display MAE and 1/255 maximum rounding difference with specular enabled. Earlier mismatch metrics described pre-fix captures. The earlier nine probes all fell on the central cube face and falsely suggested broader Studio-rotation parity; the earlier recorded MatCap pass (0.3333/255 MAE, 1/255 max) is not reproducible and is withdrawn. The corrected row orientation fixes these two presets but does not establish all MatCap presets, camera orientations, or full-frame edge parity. The MatCap work originally motivated preserving Blender 5.2.2 EXR diffuse/specular RGB channels in half-float resources; classiCAD packages all 27 presets as `.qmc` payloads. This capture set also exposed dark-range interpolation error in the uniform 65-cube AgX LUT. The LUT now uses a `log2(1 + 16x)` input shaper generated from Blender's bundled OCIO processor, applied on the CPU and GPU; the earlier bronze display comparison improved on its limited center-face probes before the present deferred MatCap path was enabled. Other preset combinations, full-frame edges, and QPainter parity remain open. Reproduce classiCAD captures with `CLASSICAD_VIEWPORT_LIGHTING_CAPTURE_DIR=/tmp/classicad-workbench-lighting QT_QPA_PLATFORM=offscreen ctest --test-dir build -R '^viewport_render_contracts$' --output-on-failure`, then run the Blender fixture and `tests/workbench_display_probe_compare.py` for each matching PNG/PFM pair.

**MatCap rendered row-orientation fix (2026-10-14):** The prior `check_normal+y` comparison still showed a scene-linear MAE/max of 0.1376717/0.6846046 after the no-specular isolation. Rechecking nonblank 640×480 Blender 5.2.2 and classiCAD captures showed that classiCAD's sampled diffuse and specular values corresponded to the opposite vertical QMC row: the QMC payload is top-down, the byte-image GL fallback reverses its rows, but the higher-precision QMC upload had not. The QMC GPU upload now reverses each layer's rows, and the CPU half-float sampler converts normalized GL V into the source's top-down row order. `check_normal+y` now matches Blender exactly across all 27 face probes in scene-linear and display RGB with specular disabled. `metal_bronze`, with specular enabled, also matches scene-linear exactly and has display MAE 0.0988/255 with 1/255 maximum rounding error. This corrects the previous row-order conclusion; the isolated texture readback had validated data and filtering but had not established rendered UV orientation. Other MatCap presets, cameras, and edge pixels still need comparison.

**Full-frame GPU MatCap cube comparison (2026-10-14):** Compared nonblank
640×480 polygon-cube output at the classiCAD lighting-capture camera, Blender
orthographic scale 5.0, AA Off, AgX, and matched neutral scene-linear
background (`0.021942` per channel). `metal_bronze` with specular enabled has
scene-linear foreground-mask IoU 0.997396 (83 XOR pixels), per-channel whole
frame scene-linear MAE below `0.000039`, and display whole-frame MAE
`0.0793/255`. `check_normal+y` with specular disabled has mask IoU 0.997991
(224 XOR pixels), per-channel scene-linear MAE `0.000505`, `0.000122`, and
`0.000547`, and display whole-frame MAE `0.1043/255`. These include edge
coverage; maximum display errors of 105/255 and 150/255 occur at a small number
of edge pixels. Existing 27 face-interior probes remain exact for the two
presets. Reproduce the classiCAD captures with
`CLASSICAD_VIEWPORT_LIGHTING_CAPTURE_DIR=/tmp/classicad-matcap-spec QT_QPA_PLATFORM=offscreen ./build/classicad_viewport_render_contracts_test`
and again with `CLASSICAD_VIEWPORT_LIGHTING_CAPTURE_NO_SPECULAR=1`; render
Blender with `tests/workbench_cube_reference.py --lighting MATCAP
--light-preset metal_bronze` or `--light-preset check_normal+y --no-specular`,
using `--ortho-scale 5.0 --match-classicad-ortho --aa OFF --zero-dither` and
the same background. Compare scene-linear PFM with
`tests/workbench_pfm_compare.py`. Other presets, camera orientations, Studio
rotation, and broader QPainter MatCap parity remain open.

**Ceramic Lightbulb MatCap comparison (2026-10-14):** Added the Blender 5.2.2
`ceramic_lightbulb` preset to the offscreen polygon-cube captures and compared
GPU and production QPainter output with specular enabled at the same 640x480
orthographic camera, AA Off, AgX, and neutral scene-linear background. GPU
output matches all 27 scene-linear face probes exactly; display probe MAE is
`0.1111/255` (max `1/255`). Its full-frame mask IoU is `0.997991` (224 XOR
pixels), per-channel scene-linear MAEs are `0.000215`, `0.000218`, and
`0.000245`, and whole-frame display MAE is `0.1136/255` (max `94/255`, at an
edge pixel). QPainter face probes have scene-linear MAE `0.0003986` (max
`0.0011610`) and display MAE `0.2222/255` (max `1/255`). Its full-frame mask
IoU is `0.994440` (622 XOR pixels); edge coverage and color residuals are
larger than GPU output, with whole-frame display MAE `0.6139/255` (max
`96/255`). This validates one more asymmetric preset at the fixed camera and
does not establish other MatCaps or camera orientations. Reproduce classiCAD
with `CLASSICAD_VIEWPORT_LIGHTING_CAPTURE_DIR=/tmp/classicad-ceramic-lightbulb
QT_QPA_PLATFORM=offscreen ctest --test-dir build -R
'^viewport_render_contracts$' --output-on-failure`; render Blender with
`tests/workbench_cube_reference.py --lighting MATCAP --light-preset
ceramic_lightbulb --aa OFF --zero-dither --ortho-scale 5.0
--match-classicad-ortho --world-color 0.0219421387 0.0219421387 0.0219421387
--linear-output /tmp/blender-ceramic-lightbulb.exr --linear-pfm-output
/tmp/blender-ceramic-lightbulb.pfm`. Compare probes with
`tests/workbench_display_probe_compare.py` and full-frame linear output with
`tests/workbench_pfm_compare.py`.

**Full-frame world-space Studio rotation comparison (2026-10-14):** Compared
Studio `Paint` at 90° world-space rotation on the same 640×480 cube with AA
Off, AgX, zero dither, and neutral scene-linear background. Blender 5.2.2 and
classiCAD have identical full-frame foreground masks (IoU 1.0, zero XOR),
per-channel scene-linear MAE below `0.000035`, and exact scene-linear RGB at
all 27 face-interior probes. Display output has `0.04304/255` whole-frame
MAE, max `46/255`, with `32,030` differing pixels; the PFM edge profile has
`0.001426` coverage MAE over its 6,012-pixel band. These residuals are
concentrated at cube edges; face lighting is matched. This confirms the
single-variable Paint rotation case, while other Studio presets/rotations,
cameras, and QPainter output remain open. The classiCAD offscreen
`viewport_render_contracts` capture passed (1/1), and the Blender 5.2.2
background render and PFM conversion completed.

**QPainter world-space Studio rotation comparison (2026-10-14):** Extended
the same opt-in cube capture to the production QPainter renderer for Studio
`Paint`, world-space lighting, 37° rotation, specular enabled, AA Off, AgX,
zero dither, and the matched 640×480 orthographic camera. All 27 interior
probes match Blender 5.2.2 display RGB exactly; scene-linear probe MAE is
`0.0001313` with max `0.0004581` (top/front/right MAE `0.000034`, `0.000237`,
`0.000123`). The GPU probes also match exactly; its foreground mask IoU is
`0.997991` (224 XOR pixels). QPainter's full-frame mask IoU is `0.994440`
(622 XOR pixels), with full-frame scene-linear channel MAE `0.004292` and
display channel MAE `0.6246/255`; its interior probes remain close despite
the larger full-frame/edge residuals. This verifies one non-cardinal CPU
lighting rotation and does not establish other Studio presets, camera
orientations, AA modes, or general QPainter edge parity. Blender 5.2.2's
world-space light transform is pinned in
[`workbench_world_light.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_world_light.bsl.hh).
Reproduce with `CLASSICAD_VIEWPORT_LIGHTING_CAPTURE_DIR=<output>
QT_QPA_PLATFORM=offscreen ./build/classicad_viewport_render_contracts_test`,
then render the Blender `Paint` 37° case using
`tests/workbench_cube_reference.py --lighting STUDIO --light-preset Paint
--world-space-lighting --rotation-degrees 37 --aa OFF --zero-dither
--ortho-scale 5.0 --match-classicad-ortho` and compare the matching PNG/PFM
files with `workbench_display_probe_compare.py`, `workbench_image_compare.py`,
and `workbench_pfm_compare.py`.

**QPainter Studio lighting matrix (2026-10-14):** Extended the opt-in
production QPainter captures to cover Studio Default plus world-space Studio
`Paint` rotations of 0°, 37°, and 90°. Each Blender 5.2.2 and classiCAD pair
uses the same 640×480 cube, orthographic view, Material color, specular on,
AA Off, AgX, zero dither, and neutral scene-linear background. All four
thresholded display masks have IoU `0.994440` with 622 XOR pixels, the same
AA-Off QPainter silhouette difference. Common-foreground display MAE/p95 are
`1.399/255` and `1/255` for Studio Default, `0.922/255` and `0/255` for Paint
0°, `1.436/255` and `0/255` for Paint 37°, and `1.292/255` and `1/255` for
Paint 90°. Full-frame scene-linear channel MAE (blue, green, red) is
`(0.001868, 0.001802, 0.001745)` for Default, `(0.004801, 0.004780,
0.004667)` for Paint 0°, `(0.004464, 0.004418, 0.003995)` for Paint 37°, and
`(0.002351, 0.002381, 0.001516)` for Paint 90°. The 4-pixel-eroded shared
foreground scene-linear p95 stays below `0.000667` per channel in all four
cases; the largest full-frame errors are candidate-only silhouette pixels.
This adds full-frame coverage for three Studio rotations and one preset, not
general Studio parity. Other lights, views, AA modes, effects, and QPainter
silhouette coverage remain open. The Blender world-space light transform is
pinned in
[`workbench_world_light.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_world_light.bsl.hh).
Validation: `cmake --build build -j4` passed with the dependency audit;
offscreen `viewport_render_contracts` passed (1/1) with the lighting capture
directory; Blender reported 5.2.2 LTS for all four references; and the
full-frame `workbench_image_compare.py` and `workbench_pfm_compare.py`
comparisons completed for each PNG/PFM pair.

**GPU deferred lighting-resolve comparison (2026-10-14):** Fresh matched
Blender 5.2.2 and classiCAD 640x480 captures exercised deferred-resolve-
eligible scenes at AA Off with zero dither, the same orthographic camera,
scene-linear background, AgX display transform, and default Workbench material.
Studio Default matches all 27 top/front/right face probes exactly with
specular both enabled and disabled; Flat also matches exactly at all probes.
`metal_bronze` MatCap matches every scene-linear probe with specular enabled
and disabled; display output is exact without specular and differs by at most
1/255 with specular enabled. Full-frame scene-linear per-channel MAE is at most
0.000403 for Studio Default, 0.0005672 for Flat, and 0.0000382 for
`metal_bronze` with specular. Full-frame masks retain the existing edge
residuals: Studio Default IoU 0.993817 (493 XOR pixels), Flat IoU 0.997991
(224 XOR), and bronze with specular IoU 0.997396 (83 XOR). The no-specular
bronze image falls below the comparison's 0.1 foreground threshold, so only
its stable face probes are used for color assessment. This validates these
resolve modes/preset with the default material; non-default roughness/metallic
payloads, other presets, and edge coverage remain open. Reproduce classiCAD
captures with `CLASSICAD_VIEWPORT_LIGHTING_CAPTURE_DIR=<output>
QT_QPA_PLATFORM=offscreen ./build/classicad_viewport_render_contracts_test`,
then render matching cases with `tests/workbench_cube_reference.py` and compare
face probes with `tests/workbench_display_probe_compare.py` and scene-linear
frames with `tests/workbench_pfm_compare.py`.

**GPU deferred Studio material-payload comparison (2026-10-14):** Added
`--material-roughness` and `--material-metallic` controls to the Blender 5.2.2
cube reference and an opt-in classiCAD production-path capture for six
single-material Studio Default scenes. The base color is fixed at scene-linear
0.42, specular is enabled, and AA/effects are disabled. Roughness 0.25 and
0.255 share Blender's post-`Material::pack_data` 5-bit bucket; 0.27 crosses
the next bucket. Metallic 0.10 and 0.14 share its 3-bit bucket; 0.15 crosses
the next one. The GPU contract now checks exact same-image output within both
roughness and metallic buckets and visible changes across both boundaries.
Compared Blender and classiCAD outputs match all 27 scene-linear face probes
exactly for all six values. Display probes are also exact except metallic 0.15,
which differs by at most 1/255. Within-bucket full images are pixel-identical
in both applications. Across the selected boundaries, roughness changes
31,784 Blender / 31,967 classiCAD display pixels (maximum 1 code); metallic
changes 111,256 / 111,480 pixels (maximum 10 codes). This validates one
material across a cube through the GPU write/resolve path; multiple material
slots, per-face variation, other Studio presets, and QPainter material
parity remain open. Reproduce classiCAD with
`CLASSICAD_VIEWPORT_STUDIO_MATERIAL_CAPTURE_DIR=/tmp/classicad-studio-material QT_QPA_PLATFORM=offscreen ctest --test-dir build -R '^viewport_render_contracts$' --output-on-failure`;
render Blender references with `tests/workbench_cube_reference.py` using
`--material-color 0.42 0.42 0.42` plus each `--material-roughness` and
`--material-metallic` value, then compare with
`tests/workbench_display_probe_compare.py`.

**GPU MatCap upload readback update (2026-10-13, corrected 2026-10-14):** The exact QMC half-float upload is shared by the production renderer and an offscreen OpenGL contract. With Mesa software GL, the contract samples two asymmetric texel centers and a four-texel midpoint in each layer for both `metal_bronze` and `check_normal+y`, comparing against the CPU sampler with the same top-down source-to-GL row mapping. Blender 5.2.2's [`workbench_resources.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_resources.cc#L18-L42) passes the MatCap ImBuf float data to texture creation; however, the earlier contract's raw coordinate checks did not prove that the production renderer matched Blender's effective image-row orientation at its MatCap UVs. All 27 bundled `.qmc` files regenerate byte-for-byte from Blender 5.2.2's bundled MatCap EXRs, and a sampled Blender image pixel matches its QMC half-float RGB exactly. Controlled rendered comparisons now verify the full texture orientation for three presets; other presets, camera orientations, and image edges remain open.

**CPU MatCap texture filtering update (2026-10-13, orientation corrected 2026-10-14):** Blender 5.2.2 evaluates MatCaps with `textureLod` on normalized textures (`source/blender/draw/engines/workbench/shaders/workbench_matcap.bsl.hh`). The CPU/QPainter samplers use normalized texel-center coordinates (`uv * dimension - 0.5`) with edge clamping, matching the OpenGL linear-filter footprint. QMC half-float rows are stored top-down, so the CPU sampler now converts normalized V to the corresponding source row; the sRGB image path keeps its equivalent image-to-GL orientation conversion. Focused contracts cover the center of a 2x2 texture, texel-center lookup, clamped edge lookups, both vertical endpoint mappings, and GPU sampling in both array layers. Matched rendered probes verify the corrected orientation for `check_normal+y`, `metal_bronze`, and `ceramic_lightbulb`; broader QPainter rendered parity remains open.

**QPainter MatCap horizontal flip (2026-10-14):** The CPU MatCap UV helper already accepts the Workbench horizontal-reflection control, and both the mesh-fragment and NURBS-surface shading call sites pass the viewport's persisted `matcapFlipX` setting. Headless production-path contracts render tilted NURBS and polygon-mesh fixtures with the asymmetric `metal_bronze` preset and verify that enabling Flip X changes each output. Blender 5.2.2 negates the computed MatCap UV X coordinate when flipped in [`workbench_matcap.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_matcap.bsl.hh#L11-L22). These contracts verify setting flow through both QPainter geometry paths, not matched Blender output; other presets, camera orientations, and full-frame parity remain open.

**QPainter MatCap rendered probes (2026-10-14):** The offscreen lighting
capture renders the polygon cube through the production QPainter
`ViewportRenderer` and writes a scene-linear PFM alongside the display PNG.
The PFM is reconstructed from the fragment sink, which intentionally omits
pixels in the fractional-edge coverage band; its foreground mask is not a
valid full-frame display-coverage comparison. Against matched Blender 5.2.2
captures, `metal_bronze` with specular enabled has 27-probe scene-linear MAE
`0.0003603` (max `0.0012085`) and display MAE `0.3333/255` (max `1/255`).
`check_normal+y` with specular disabled has scene-linear MAE `0.0004826` (max
`0.0012679`) and display MAE `0.1111/255` (max `1/255`). A fresh matched
`metal_bronze` comparison of the visible QPainter PNG gives foreground-mask
IoU `0.994440` (622 XOR pixels) against Blender AA Off; the matched GPU output
has IoU `0.997991` (224 XOR pixels). The image comparison isolates residual
silhouette/edge coverage; the face-interior probes continue to show close
MatCap shading. Other presets, cameras, and full-frame RGB parity remain open.
Reproduce the classiCAD captures with
`CLASSICAD_VIEWPORT_LIGHTING_CAPTURE_DIR=/tmp/classicad-workbench-lighting
QT_QPA_PLATFORM=offscreen ctest --test-dir build -R
'^viewport_render_contracts$' --output-on-failure`; the PFM uses bottom-up
rows, matching the Blender PFM comparator.

Phase Two color-source comparison: matched 640×480 Flat captures for Material, Object, and Random use the same cube, name, camera, and RGB inputs. All nine scene-linear and display probes match exactly for each source. Feature contracts verify interpolated Attribute RGB with material alpha, active-then-default color-attribute selection, and Texture RGB replacing material RGB while retaining material alpha. Blender import and native JSON now retain the default color attribute separately from the active attribute; both renderers use it when the active attribute is unset, and keep the material fallback when neither exists, matching Blender's active/default selection in [`workbench_state.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_state.cc). Texture extraction now selects Blender's separately marked active texture node (`show_texture`) even when `nodes.active` points elsewhere, following [`node_get_active_texture`](https://github.com/blender/blender/blob/v5.2.2/source/blender/editors/uvedit/uvedit_ops.cc#L98-L152). Meshes also retain Blender's default/render UV layer separately; GPU, QPainter, and raster paths use active UV first and default/render UV when active is unset, matching [`active_or_default_uv_map_name`](https://github.com/blender/blender/blob/v5.2.2/source/blender/blenkernel/intern/mesh.cc#L1300-L1324) and the Workbench mesh batch setup in [`draw_cache_impl_mesh.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/intern/draw_cache_impl_mesh.cc#L628-L635). Blender 5.2.2's active-texture selection contract and classiCAD's UV fallback/serialization contracts pass. The QPainter texture-alpha cutoff uses `< 0.1`, with a contract retaining exactly `0.1` and discarding the next float below, matching Blender's pinned [`workbench_image_lib.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_image_lib.glsl#L416-L429). Material alpha uses round-to-nearest UNorm8 packing; the CPU shadow contract now checks values immediately below, at, and above the 254.5/255 boundary against the opaque classification, following Blender's pinned [`math_base_inline.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/blenlib/intern/math_base_inline.cc#L2362-L2368). Existing records establish interior probes; the full-frame Flat comparisons below cover Material, Object, Random, constant- and varying-UV Texture, Custom/Single, and per-corner Vertex Attribute. Point-domain GPU output is compared below; Blender 5.2.2 does not recognize Face-domain `FLOAT_COLOR` as a color attribute. Other texture filters/alpha/UDIM output and broader QPainter comparisons remain open. The focused viewport contract passes; `core_contracts` still reports perspective/zoom, snapping, curve-tool, and dimension failures outside this color-source category. Reproduce the three exact color-source comparisons with `tests/workbench_cube_reference.py --lighting FLAT --color-source MATERIAL|OBJECT|RANDOM|TEXTURE|SINGLE|VERTEX` and compare each output pair with `tests/workbench_display_probe_compare.py`.

### Linked-object Workbench Random identity contract (2026-10-15)

- Added fixed vectors for the Blender 5.2.2 Random color hash with the same
  object name, `LinkedFixture`, and two distinct library paths:
  `/tmp/classicad-random-library-a.blend` maps to Rec.709 RGB
  `(0.64629560, 0.8, 0.4)`, and
  `/tmp/classicad-random-library-b.blend` maps to `(0.4, 0.8, 0.53649284)`.
  The vectors are calculated independently from the pinned Blender hash and
  HSV equations. The CPU helper contract checks both values and confirms the
  library path changes the result even when object names match.
- An offscreen GPU Flat/Random render contract supplies those same adapter
  name/path pairs and finds each expected color in scene-linear output. This
  exercises the GPU renderer's separate Random color resolver as well as the
  public QPainter helper. Blender 5.2.2 hashes `ob.id.name`, mixes the linked
  library filepath, converts the result to HSV `(h, 0.5, 0.8)`, and applies
  Rec.709-to-scene-linear conversion in pinned
  [`workbench_materials.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_materials.cc#L420-L438).
- This validates downstream rendering from the material adapter's library
  path field; it does not render a newly linked `.blend` fixture or re-test
  extraction of the library path itself. Other names, paths, and imported
  linked-object image comparisons remain open.
- Validation: `cmake --build build -j4` passed with the 318-source dependency
  audit; offscreen `viewport_render_contracts` passed with the fixed CPU
  vectors and GPU path captures enabled.

**Full-frame Flat color-source cube comparison (2026-10-14):** Compared
Material, Object, Random, and Texture inputs on the same nonblank polygon cube
at 640×480, Blender orthographic scale 5.0, AA Off, Flat lighting, AgX, and
the matched neutral scene-linear background. Material uses scene-linear
`(0.23, 0.47, 0.71)`; Object uses `(0.12, 0.62, 0.18)`; Random uses the shared
`ReferenceCube` object name; Texture uses the same one-pixel sRGB image
`(192, 128, 64)` on every face at UV `(0.5, 0.5)` with Closest interpolation.
All four modes match exactly at the 27 face-interior probes in scene-linear
and display RGB. Full-frame foreground-mask IoU is `0.997991` (224 XOR pixels)
for each mode. Per-channel scene-linear MAE is at most `0.000567`; whole-frame
display MAE ranges from `0.06514/255` (Texture) to `0.10354/255` (Random).
These captures validate constant face color/UV samples and imported Random
identity for this cube. Attribute output is validated separately below; other texture filters/alpha/UDIMs, additional surface geometries, and QPainter output remain open.
classiCAD captures use
`CLASSICAD_VIEWPORT_LIGHTING_CAPTURE_DIR=/tmp/classicad-workbench-colors`; the
Blender fixture uses `--lighting FLAT --color-source` with matching material,
object, or image arguments. `tests/workbench_pfm_compare.py` and
`tests/workbench_display_probe_compare.py` produce the recorded metrics.

**Full-frame Flat Custom/Single cube comparison (2026-10-14):** Added the
Blender 5.2.2 `SINGLE` option and `--single-color` control to the reference
fixture, plus a classiCAD Custom capture using scene-linear `(0.31, 0.18,
0.72)` converted to the sRGB `QColor` expected by `ViewportShadingSettings`.
At 640×480, scale 5.0, AA Off, Flat lighting, AgX, and the same neutral
background, all 27 face-interior probes match exactly in scene-linear and
display RGB. Full-frame foreground-mask IoU is `0.997991` (224 XOR pixels);
per-channel scene-linear MAE is `0.000210`, `0.000115`, and `0.000509`, and
whole-frame display MAE is `0.08847/255`. This validates the Custom/Single
source for the controlled GPU cube; Attribute, other geometries, and QPainter
output remain open. The classiCAD capture is `color_custom.png/.pfm` from the
lighting capture directory; Blender uses `--color-source SINGLE
--single-color 0.31 0.18 0.72`.

**Full-frame Flat Vertex Attribute cube comparison (2026-10-14):** Added a
matched per-corner `FLOAT_COLOR` layer to the classiCAD cube capture and the
Blender 5.2.2 reference fixture. Each corner's scene-linear RGB is derived
from its cube position, from `0.1` at coordinate `-1` to `0.8` at `+1`, so
the face interiors exercise attribute interpolation. Blender's `VERTEX`
color source uses the same active/render color layer. At 640×480, scale 5.0,
AA Off, Flat lighting, AgX, and the matched neutral background, 27
face-interior probes have scene-linear MAE `0.0000316` and max `0.0004883`;
display MAE is `0.0370/255`, max `1/255`. Full-frame foreground-mask IoU is
`0.997991` (224 XOR pixels), per-channel scene-linear MAE is at most
`0.000316`, and display whole-frame MAE is `0.07142/255`. This validates the
GPU interpolated corner-color path for this cube; other geometries remain
open. Point-domain output is compared below, while Blender has no active
Face-domain color attribute to compare. Reproduce with
`--color-source VERTEX` in `tests/workbench_cube_reference.py` and
`color_attribute.png/.pfm` from the classiCAD lighting capture directory.

**Full-frame Flat Point Attribute cube comparison (2026-10-14):** Extended
the fixture to create the active/render `FLOAT_COLOR` layer on Blender's
`POINT` domain and the matching per-vertex classiCAD attribute. At 640×480,
scale 5.0, AA Off, Flat lighting, AgX, and the matched neutral background,
the 27 face-interior probes have scene-linear MAE `0.0000316` and max
`0.0004883`; display MAE is `0.0370/255`, max `1/255`. Full-frame
foreground-mask IoU is `0.997991` (224 XOR pixels), and per-channel
scene-linear MAE is `0.0000633`, `0.0000619`, and `0.0003159`. This verifies
the GPU Point-domain interpolation path against Blender 5.2.2 for this cube.
Reproduce with `--color-source VERTEX --attribute-domain POINT` in
`tests/workbench_cube_reference.py` and
`color_attribute_point.png/.pfm` from the classiCAD lighting capture
directory.

**AA-Off QPainter Object-color cube comparison (2026-10-14):** Added a
production `ViewportRenderer` QPainter capture for Flat/Object color using the
same 640×480 cube, object RGB `(0.12, 0.62, 0.18)`, neutral scene-linear
background, and AA-Off framing as the Blender 5.2.2 reference. All common
foreground display pixels match exactly, including the four-pixel-eroded
interior. The mask has IoU `0.997991` with 224 XOR pixels, the same silhouette
coverage gap as the established AA-Off Material baseline. This isolates the
remaining difference to edge coverage for this fixture; it does not establish
Object-color parity for other lighting modes or geometry. Blender selects the
object color in pinned [`workbench_engine.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_engine.cc#L2026-L2037).
The classiCAD output is `color_object_qpainter.png/.pfm` under
`CLASSICAD_VIEWPORT_LIGHTING_CAPTURE_DIR`; compare the PNG and linear PFM with
`workbench_image_compare.py` and `workbench_pfm_compare.py`.

**AA-Off QPainter Random-color cube comparison (2026-10-14):** Extended the
same Flat cube capture to Workbench Random color with the shared object name
`ReferenceCube`, matching Blender's name-based seed input. The foreground
mask has IoU `0.997991` with 224 XOR pixels, the same AA-Off silhouette gap as
the Material and Object cases. Common foreground display MAE is `0.3333/255`
with maximum error `1/255`; common-foreground scene-linear MAE is `0.0001953`,
`0.0003757`, and `0.0000977` for red, green, and blue. Blender's Random
material selection is pinned in
[`workbench_engine.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_engine.cc#L2026-L2037).
The classiCAD output is `color_random_qpainter.png/.pfm` under
`CLASSICAD_VIEWPORT_LIGHTING_CAPTURE_DIR`.

**AA-Off QPainter Custom/Single-color cube comparison (2026-10-14):** Added
the same capture for classiCAD Custom color `(0.31, 0.18, 0.72)` and Blender
5.2.2 Single color with the same scene-linear input. Common foreground display
pixels match exactly. Common-foreground scene-linear MAE is `0.0001802`,
`0.0000687`, and `0.0002869` for red, green, and blue. The mask has IoU
`0.997991` with 224 XOR pixels, matching the established AA-Off silhouette
gap. Blender's Single color override is pinned in
[`workbench_engine.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_engine.cc#L2039-L2041).
The classiCAD output is `color_custom_qpainter.png/.pfm` under
`CLASSICAD_VIEWPORT_LIGHTING_CAPTURE_DIR`.

**AA-Off QPainter Vertex Attribute cube comparison (2026-10-14):** Added
matched Corner- and Point-domain `FLOAT_COLOR` captures using the existing
coordinate-derived cube colors. For both domains, common-foreground display
MAE is `0.0264/255` with maximum error `1/255`; common-foreground scene-linear
channel max error is below `0.000524`. Both masks have IoU `0.997991` with 224
XOR pixels, matching the same AA-Off silhouette gap. This validates the
QPainter interpolated attribute inputs for this cube; other geometry and
attribute layouts remain open. Blender selects the active color attribute in
pinned [`workbench_state.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_state.cc#L1415-L1442)
and supplies it as the vertex color material in
[`workbench_engine.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_engine.cc#L2043-L2045).
The classiCAD outputs are `color_attribute_qpainter.png/.pfm` and
`color_attribute_point_qpainter.png/.pfm` under
`CLASSICAD_VIEWPORT_LIGHTING_CAPTURE_DIR`.

**AA-Off QPainter Texture-color cube comparison (2026-10-14):** Added a
matched Workbench Texture capture using the existing 1×1 sRGB image
`(192, 128, 64)`, Closest filtering, and centered UVs across the cube's faces.
Common-foreground display RGB matches exactly; common-foreground scene-linear
channel max error is `0.0002286`, `0.0000402`, and below `0.0000001` for red,
green, and blue. The mask has IoU `0.997991` with 224 XOR pixels, matching the
AA-Off silhouette gap. This validates one constant-UV cube texture for the
QPainter fallback, not patterned UVs or other geometry. Blender's texture
material selection is pinned in
[`workbench_engine.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_engine.cc#L2047-L2055)
and its image sampling in
[`workbench_image_lib.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_image_lib.glsl#L34-L60).
The classiCAD output is `color_texture_qpainter.png/.pfm` under
`CLASSICAD_VIEWPORT_LIGHTING_CAPTURE_DIR`.

Blender 5.2.2 does not expose a `FLOAT_COLOR` attribute on the `FACE` domain
as a color attribute: creating it leaves `Mesh.color_attributes.find(name)`
at `-1`, and active/render color selection remains unset. The Workbench
reference therefore cannot render a matched Face-domain color layer. The
existing QPainter Face-domain contract verifies classiCAD's face-constant
extension; it is not Blender parity. Point and Corner are the renderable
color domains compared here. Blender's pinned `workbench_state.cc` checks
active then default color names and verifies the selected layer with
`bke::mesh::is_color_attribute` (lines 1415–1442).

**QPainter Face-domain color attribute contract (2026-10-14):** The software
renderer contract now builds a two-triangle mesh with an active `FLOAT_COLOR`
Face-domain attribute and samples well inside both faces. It verifies that
the first face resolves to its stored red value and the second to its stored
green value, rather than interpolating values across the shared diagonal. The
contract passes through the production `ViewportRenderer` path. Blender 5.2.2
selects the active color attribute, falling back to the default only when the
active name is absent, and validates it as a color attribute in pinned
[`workbench_state.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_state.cc#L244-L257).
This adds a regression check for QPainter's Face-domain index path. A matched
Blender rendered comparison is unavailable because Blender 5.2.2 does not
recognize Face-domain `FLOAT_COLOR` as a color attribute. The GPU Point path
is covered by the matched comparison above; a GPU Face-domain comparison has
no Blender reference equivalent.

**Full-frame varying-UV Texture cube comparison (2026-10-14):** Extended the
offscreen cube capture and Blender 5.2.2 fixture with a 2×2 sRGB image whose
red/green and blue/yellow texels exercise vertical orientation and bilinear
filtering. UVs vary per corner from cube X/Z coordinates over `[0, 1]`; both
renderers use Flat lighting, Linear interpolation, Repeat extension, AA Off,
AgX, and the matched neutral background. The 27 face-interior probes have
exact scene-linear RGB and display MAE `0.0247/255` (max `1/255`). Full-frame
foreground-mask IoU is `0.997991` (224 XOR pixels), per-channel scene-linear
MAE is at most `0.000330`, edge-coverage MAE is `0.03599`, and whole-frame
display MAE is `0.09705/255`. This covers GPU varying-UV interpolation and
Linear image sampling on this cube; Closest/Cubic/Smart with a patterned
image, alpha cutouts, UDIMs, additional surface geometries, and QPainter output remain open.
The fixture capture is `color_texture_uv.png/.pfm`; Blender uses
`--color-source TEXTURE --texture-interpolation Linear
--texture-uv-pattern` with the same generated pattern image.

**Full-frame Flat GPU Image Texture on a NURBS patch (2026-10-14):** Added a
production capture for classiCAD's degree-one planar NURBS surface with U knot
domain `[2, 6]` and V domain `[-3, 5]`, plus an equivalent Blender 5.2.2
display-proxy mesh carrying matching normalized corner UVs. Both use the same
asymmetric 2×2 sRGB image, Linear interpolation, Repeat extension, Flat
lighting, AA Off, AgX, and a matched scene-linear background. The Blender
5.2.2 scene-linear Combined comparison has foreground-mask IoU `0.999937`
(2 XOR pixels); per-channel full-frame MAE is at most `0.000010`, with the
maximum errors confined to edge coverage. Eroding the matching foreground by
two pixels leaves 29,718 samples with channel MAE `0.0000498` and maximum
`0.00684`; fixed probes at `(240,200)`, `(320,200)`, `(400,200)`,
`(240,240)`, `(320,240)`, `(400,240)`, `(240,280)`, `(320,280)`, and
`(400,280)` match exactly. This validates
GPU texture filtering and normalized parameter UVs for a regular planar NURBS
patch, not trimmed/nonplanar surfaces, other alpha modes, UDIMs, or QPainter
rendering. The classiCAD capture is `color_texture_nurbs.png/.pfm` from
`CLASSICAD_VIEWPORT_LIGHTING_CAPTURE_DIR`; Blender uses
`--scene NURBS_TEXTURE --match-classicad-ortho --lighting FLAT
--color-source TEXTURE --texture-interpolation Linear` with the captured
`workbench-texture-pattern.png`.

**Full-frame Straight-alpha Image Texture cutoff on a NURBS patch (2026-10-14):**
The same degree-one NURBS and Blender 5.2.2 proxy were rendered with a 2×2
sRGB texture whose left column has alpha zero and right column alpha one.
Both use the straight-alpha interpretation, Linear filtering, Repeat
extension, Flat lighting, AA Off, and matched scene-linear background. The
cutout mask IoU is `0.999930` (2 XOR pixels), with 28,630 Blender and 28,628
classiCAD foreground pixels. Per-channel full-frame scene-linear MAE is below
`0.000010`; the shared foreground eroded by two pixels has channel MAE
`0.0000492` and maximum `0.00342`. This verifies the enabled Blender
`alpha_cutoff` behavior for a regular planar NURBS patch and its filtered
boundary. Reproduce with the same capture environment and
`--texture-alpha-mode STRAIGHT`; the files are
`color_texture_nurbs_alpha.png/.pfm` and
`workbench-texture-alpha.png`.

**Full-frame Image Texture PREMUL, CHANNEL_PACKED, and no-cutoff alpha on a
NURBS patch (2026-10-14):** The same Blender 5.2.2 and classiCAD planar patch
fixture checked the remaining regular-image alpha interpretations. Blender's
image enum is `NONE` for the no-cutoff behavior; classiCAD's material adapter
uses `IGNORE` for that equivalent mode. A 2×2 image with zero-alpha colored
texels rendered as opaque coverage in both `CHANNEL_PACKED` and `NONE` modes;
the two Blender renders match each other and the classiCAD `CHANNEL_PACKED`
and `IGNORE` captures. Each mask has 31,794 Blender and 31,792 classiCAD
foreground pixels (2 XOR pixels, IoU `0.999937`); the full-frame channel MAE
is `0.0000073`, and the shared foreground eroded by two pixels has channel
MAE `0.0000499`, maximum `0.00684`.

A half-alpha PREMUL texel was compared with its equivalent straight-alpha
color after sRGB-space unpremultiplication. The Blender/classiCAD mask has the
same 2-pixel edge difference and IoU `0.999937`. On the shared foreground
eroded by two pixels, channel MAE is `0.00126` and maximum `0.00379`; classiCAD
also compares its PREMUL capture with a straight-alpha reference internally
(channel MAE `0.0000548`, maximum `0.00123`). These results cover regular
planar NURBS images for all four alpha interpretations. UDIM alpha, trimmed
surfaces, and QPainter full-frame comparisons remain open. The test helper
uses Blender's actual `--texture-alpha-mode NONE` enum while the classiCAD
contract tests `IGNORE`; Blender's pinned
[`workbench_materials.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_materials.cc#L536-L553)
sets PREMUL handling and bypasses the 0.1 alpha cutoff only for `IGNORE` and
`CHANNEL_PACKED`.

**Full-frame QPainter textured NURBS comparison (2026-10-14):** The QPainter
fallback now has a 640×480 capture of the same planar patch, sRGB 2×2 image,
Linear filtering, Flat lighting, AgX transform, and Blender-matched view. The
capture exposed an extra multiplication by the material base RGB in QPainter's
NURBS texture path. Blender 5.2.2's pinned
[`workbench_image_lib.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_image_lib.glsl#L385-L433)
returns the sampled image RGB directly; QPainter now does the same while
preserving material alpha and using material color when image data or UVs are
unavailable. A contract varies the material RGB and verifies that valid
textured NURBS samples do not change.

The comparison after this correction has mask IoU `0.987882` (390 XOR pixels;
31,794 Blender and 32,184 QPainter foreground pixels). On the shared
foreground eroded by two pixels, display RGB MAE is `8.82` with maximum 44;
the QPainter fragment capture compared in scene-linear space has channel MAE
`0.00191` and maximum `0.01266` on that same interior. The material-tint error
is fixed, but the remaining color and raster-coverage differences keep this
comparison approximate. Capture files are `color_texture_nurbs_qpainter.png`
and `.pfm` under `CLASSICAD_VIEWPORT_LIGHTING_CAPTURE_DIR`; Blender uses
`--scene NURBS_TEXTURE --match-classicad-ortho --aa OFF --lighting FLAT
--color-source TEXTURE --texture-interpolation Linear` and the same pattern
image.

**Full-frame QPainter textured polygon mesh comparison (2026-10-14):** The
QPainter fallback and Blender 5.2.2 render the same 640×480 planar mesh made
from two explicitly matching triangles, with per-corner normalized UVs, the
same sRGB 2×2 pattern, Linear filtering, Flat lighting, AgX, and matched
orthographic framing. Foreground mask IoU is `0.987882` (390 XOR pixels;
31,794 Blender and 32,184 QPainter foreground pixels). On their shared
foreground eroded by two pixels, display RGB MAE is `0.535` with maximum 42;
the CPU fragment capture compared in scene-linear RGB has channel MAE
`0.00157`, p95 `0.00374`, and maximum `0.00819`. Four interior quadrant probes
agree within 1 display code value per channel. This comparison found no
additional texture RGB or UV defect; the remaining difference is concentrated
in edge coverage and small interpolation/color errors. It validates one
opaque, regular-image, planar case and does not establish other mesh shapes,
texture extension/alpha modes, or perspective sampling. Capture files are
`color_texture_mesh_qpainter.png` and `.pfm` under
`CLASSICAD_VIEWPORT_LIGHTING_CAPTURE_DIR`; Blender uses
`--scene MESH_TEXTURE --match-classicad-ortho --aa OFF --lighting FLAT
--color-source TEXTURE --texture-interpolation Linear` and the same pattern
image.

**QPainter per-face mesh texture-slot comparison (2026-10-14):** The matched
mesh capture now assigns different sRGB 2×2 images to the two explicit
triangles through material slots 0 and 1, with identical per-corner UVs,
Linear filtering, Flat lighting, AgX, and orthographic framing. The Blender
5.2.2 fixture assigns the same images and material index to the same face.
The foreground mask remains at IoU `0.987882` (390 XOR pixels). On 25,833
shared foreground pixels after excluding a two-pixel silhouette border and a
six-pixel band around the internal diagonal, display RGB MAE is `0.209` with
maximum 3; scene-linear channel MAE is `0.00139`, p95 `0.00336`, and maximum
`0.00738`. The large differences before excluding the diagonal came from the
two renderers' different ownership of that shared edge. This verifies texture
selection for two opaque regular-image slots on this planar mesh; it does not
cover slot reassignment after topology edits, mixed alpha modes, or perspective.
Blender 5.2.2 resolves each material slot's active image through
[`workbench_materials.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_materials.cc#L451-L542)
and selects the texture pass for mesh draws in
[`workbench_mesh_passes.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_mesh_passes.cc#L1051-L1103).
The secondary image is passed with `--texture-image-secondary` to
`workbench_cube_reference.py --scene MESH_TEXTURE`.

**QPainter mesh texture alpha-mode comparison (2026-10-14):** A matched
640×480 two-triangle mesh uses a 2×2 sRGB image with zero, below-cutoff,
just-above-cutoff, and half-alpha texels. Blender 5.2.2 and QPainter were
captured for `STRAIGHT`, `PREMUL`, `CHANNEL_PACKED`, and Blender `NONE` alpha
modes, with Linear filtering, Flat lighting, AgX, and the same orthographic
view. Relative to the opaque mesh baseline, the `STRAIGHT` and `PREMUL` hole
masks have IoU `0.986872` (145 XOR pixels; 10,926 Blender and 11,019 QPainter
hole pixels). Their full foreground masks have IoU `0.983718` (345 XOR;
20,868 Blender and 21,165 QPainter foreground pixels). The two cutoff-disabled
modes retain every texel and match the opaque baseline mask; their remaining
mask difference is the same silhouette raster coverage recorded above.

On the shared foreground, excluding a two-pixel silhouette border and a
six-pixel band around the internal diagonal, `STRAIGHT` scene-linear channel
MAE is `0.00139` (p95 `0.00386`, max `0.00798`) and `PREMUL` MAE is `0.00454`
(p95 `0.00877`, max `0.01386`). `CHANNEL_PACKED` and `NONE` both measure MAE
`0.00140` (p95 `0.00377`, max `0.00798`). The PREMUL result has the largest
filtered-color difference near low-alpha texels; these captures establish the
alpha interpretation and cutoff behavior for one regular mesh image, not all
filters, image formats, or perspective cases. Blender 5.2.2 unpremultiplies
nonzero, nonopaque PREMUL samples and applies its fragment cutoff in pinned
[`workbench_image_lib.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_image_lib.glsl#L416-L433);
`workbench_materials.cc` disables that cutoff for `IGNORE` and
`CHANNEL_PACKED` ([lines 536–553](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_materials.cc#L536-L553)).
classiCAD's QPainter capture writes
`color_texture_mesh_alpha_<mode>.png/.pfm`; Blender uses
`--scene MESH_TEXTURE` and the same alpha-mode option. The fixture also writes
equivalent straight and premultiplied image assets to the capture directory.

**QPainter mesh UDIM texture comparison (2026-10-14):** A matched 640×480
two-triangle mesh spans UV tiles 1001 and 1002 with Closest filtering. Tile
1001 is opaque red; tile 1002 is green with one zero-alpha texel, using
STRAIGHT alpha and Flat lighting. Blender 5.2.2 and QPainter both show the two
tile colors and cutout. Foreground mask IoU is `0.986946` (368 XOR pixels;
27,822 Blender and 28,190 QPainter foreground pixels). On the shared
foreground eroded by two pixels, scene-linear channel MAE is `0.000197`, p95
`0.000815`, and maximum `0.000815` over 25,272 pixels. Red and green interior
probes match exactly in display RGB; their scene-linear MAEs are `0.000104` and
`0.000325`. The color capture confirms QPainter selects both horizontal tiles;
the mask comparison covers the single transparent texel. This is one external
PNG, two-tile, Closest-filter mesh case; float tiles, vertical tile numbering,
other filters/extensions, trimmed NURBS, and GPU parity remain open. Blender
5.2.2 performs tile lookup and alpha cutoff in pinned
[`workbench_image_lib.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_image_lib.glsl#L34-L60)
and lines 416–433. The offscreen production contract requires visible samples
from both tiles and a bounded sample count with the cutout enabled. Capture
files are `color_texture_mesh_udim_qpainter.png/.pfm` and
`workbench-udim.1001.png`, `workbench-udim.1002.png` under
`CLASSICAD_VIEWPORT_LIGHTING_CAPTURE_DIR`; Blender uses
`--scene MESH_TEXTURE --texture-udim --texture-alpha-mode STRAIGHT
--texture-interpolation Closest` with the matching `<UDIM>` pattern.

The production QPainter capture contract also exercises the same two-tile
STRAIGHT-alpha fixture with Linear filtering. On 2026-10-14 it produced 32,379
fragments, including 16,236 tile-1001 and 16,143 tile-1002 samples, while
retaining the zero-alpha texel cutout. Its capture files use the same
`color_texture_mesh_udim_qpainter.png/.pfm` names when
`CLASSICAD_VIEWPORT_LIGHTING_CAPTURE_DIR` is set. This verifies that both tile
lookups and alpha coverage remain active under Linear filtering; it does not
extend the Closest-filter Blender image metrics above. A matched Blender 5.2.2
Linear-filter image comparison remains open. Blender's pinned
[`workbench_image_lib.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_image_lib.glsl#L34-L60)
owns the UDIM lookup, filtering, and alpha cutoff behavior.

**QPainter perspective mesh texture comparison (2026-10-14):** The same
two-triangle texture fixture was rendered with matched perspective framing:
classiCAD zoom `17.28`, view direction `(4, -6, 4)`, and Blender's 25 mm lens
with 36 mm horizontal sensor, both at 640×480 with Flat lighting, AgX, Linear
filtering, and AA off. The foreground mask IoU is `0.988615` (419 XOR pixels;
36,384 Blender and 36,803 QPainter foreground pixels). On the shared foreground
eroded by two pixels, the scene-linear channel MAE is `0.001543`, p95
`0.003737`, and maximum `0.008007` over 34,206 pixels. Four interior probes
agree within one display code value per channel; their scene-linear MAEs are
`0.00120`, `0.00121`, `0.00152`, and `0.00169`. This supports the existing
perspective-correct UV interpolation for this planar mesh. The remaining mask
difference is raster coverage at the silhouette; other mesh shapes and
perspective texture extension modes remain open. Blender 5.2.2 marks the
prepass UV varying smooth in
[`workbench_prepass.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_prepass.bsl.hh#L100-L105)
and passes mesh UVs through at lines 141–150; the texture shader samples those
coordinates in
[`workbench_image_lib.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_image_lib.glsl#L385-L433).
Capture files are `color_texture_mesh_perspective_qpainter.png/.pfm` under
`CLASSICAD_VIEWPORT_LIGHTING_CAPTURE_DIR`; Blender uses
`--scene MESH_TEXTURE --match-classicad-perspective --aa OFF --lighting FLAT
--color-source TEXTURE --texture-interpolation Linear` and the same pattern
image.

**QPainter Image Texture interpolation contract (2026-10-14):** A patterned
2×2 image checks that QPainter produces identical Texture-mode pixels for the
Blender node interpolation labels Linear, Cubic, and Smart. A production mesh
probe also checks that `Closest` selects the expected yellow texel by matching
its output to a single-color reference image, and differs from the filtered
sample at that same probe. Blender 5.2.2
Workbench sets filtered sampling for every Image Texture interpolation except
Closest in pinned [`workbench_materials.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_materials.cc#L469-L476);
its shader uses that filtered sampler in
[`workbench_image_lib.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_image_lib.glsl#L385-L433).
These contracts protect both existing QPainter sampling branches from a future
mistaken per-label cubic reconstruction or accidental filtering of `Closest`.
They check QPainter production behavior only; matched Blender texture render
probes remain open.

**QPainter NURBS texture RGB contract (2026-10-14):** A production-path
contract renders a bilinear NURBS patch with U domain `[2, 6]` and V domain
`[-3, 5]` against an asymmetric 2×2 image, then checks four interior probes
for the expected texel quadrant and V orientation. This protects active-domain
normalization and UV transport through the separate QPainter NURBS raster
path; the pinned Blender 5.2.2
[`workbench_image_lib.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_image_lib.glsl#L385-L433)
samples the interpolated UV coordinate. The contract uses channel-dominance
margins rather than exact display RGB, and does not establish matched Blender
output, GPU sampling parity, or trim-boundary parity.
Validation: `cmake --build build -j4` passed with the 314-file dependency
audit, and `QT_QPA_PLATFORM=offscreen
./build/classicad_viewport_render_contracts_test` passed. The existing
`qfloat16`/`memcpy` compiler warning remains.

**Active Image Texture rendered probe (2026-10-14):** The controlled cube
fixture now supports Blender's `TEXTURE` color source with an active Image
Texture node. Its offscreen classiCAD counterpart assigns the same external
one-pixel sRGB PNG to every face corner at the same UV and uses Flat lighting.
Blender 5.2.2 and classiCAD captures matched exactly at all 27 top/front/right
face probes in scene-linear RGB and display RGB (MAE and maximum error both
zero). This verifies the active-node sample, material-color override, image
color conversion, and display path for opaque constant-UV input. It does not
verify filtered/nonconstant UVs, alpha/PREMUL cutoff behavior, OCIO float
payloads, UDIM sampling, or full-frame edge parity. Reproduce by capturing
classiCAD with
`CLASSICAD_VIEWPORT_LIGHTING_CAPTURE_DIR=/tmp/classicad-workbench-texture QT_QPA_PLATFORM=offscreen ./build/classicad_viewport_render_contracts_test`,
then run `workbench_cube_reference.py --aa OFF --zero-dither --lighting FLAT
--color-source TEXTURE --texture-image
/tmp/classicad-workbench-texture/workbench-texture.png --linear-output
/tmp/classicad-workbench-texture/blender_color_texture.exr --linear-pfm-output
/tmp/classicad-workbench-texture/blender_color_texture.pfm` and compare the
paired PNG/PFM files with `workbench_display_probe_compare.py`.

**Studio world-space direction update (2026-10-13):** Blender 5.2.2's
`workbench_resources.cc` builds a world-shading rotation matrix and remaps the
stored Studio Light direction axes. classiCAD's world-space Studio adapter now
applies the corresponding `(x, y, z) -> (x, -z, y)` direction mapping before
its camera-basis projection, while preserving the stored preset vectors and
the camera-following path. The offscreen Blender 5.2.2 Paint fixture was
captured at 0 and 90 degrees and compared at 27 interior probes across three
visible cube faces. Both classiCAD captures now match Blender's scene-linear
probes exactly; display probes have zero error at 0 degrees and at most
1/255 at 90 degrees. The focused C++ contract and full build pass. This
verifies the default orthographic fixture at those two rotations; other
presets and camera orientations remain open. The earlier 90-degree mismatch
metrics above describe the pre-fix baseline.

**Studio rotation precision (2026-10-15):** Blender stores
`studiolight_rotate_z` in radians and can save fractional-degree values.
classiCAD now retains the converted fractional degrees through lighting,
cache keys, and session files; older sessions with integer degree values
remain valid. The existing whole-degree slider rounds only its displayed
position and label, while changing it still sets an integer degree. No matched
Blender image comparison has been run specifically for fractional rotations.

**QPainter planar NURBS coverage resolve (2026-10-13):** The fallback used to
source-over each tessellation triangle after applying its own 4×4 subpixel
coverage alpha. A split-quad test showed that this made translucent output
depend on which diagonal the tessellator used. For untextured, orthographic,
coplanar NURBS surfaces, the fallback now sums premultiplied display samples
and scene-linear fragment contributions across the surface and composites one
resolved image/fragment per pixel. The bounded accumulation runs only up to
1,000,000 viewport pixels; perspective, textured, and non-coplanar surfaces
retain the existing path. A planar split-quad test passes with opaque output
identical, translucent display output within one 8-bit code value, and
scene-linear fragment probes identical for both diagonals. The focused raster
fragment contract passes. This does not reproduce Blender's multisample depth
resolve for general curved, textured, or perspective surfaces. Blender 5.2.2
resolves its Workbench antialiasing as a later scene effect in
[`workbench_effect_antialiasing.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_effect_antialiasing.cc#L1196-L1288).

Phase Two Category 8 source audit: the GPU TAA/SMAA weights, log-space accumulation, SMAA edge/area/search stages, temporal mix, and resolve follow Blender 5.2.2 closely, so no duplicate shader rewrite was made. QPainter still uses supersampling rather than Workbench temporal accumulation. Custom display LUTs carry their domain and shaper through extraction, import, native serialization, CPU rendering, GPU surface/transparency rendering, and AA presentation. A controlled PyOpenColorIO-to-trilinear-LUT comparison for Blender 5.2.2's AgX + Very High Contrast combination used 41,000 deterministic RGB probes over [0, 16]³: the 65³ log-shaped table measured 0.000116 mean absolute channel error, 0.000177 p95, 0.00241 p99, and 0.0546 maximum; the subset in [0, 1]³ measured 0.000194 mean and 0.000371 p95. A linear [0, 16]³ 65³ table measured 0.1758 mean error on [0, 1]³ probes, supporting the existing log shaper. This isolates interpolation error against OCIO and does not compare full Blender/classiCAD rendered output, GPU texture precision, or display quantization. Retain the QPainter and display-transform differences explicitly; worst-case LUT and matched-render comparisons remain open.

### Phase Two image source-space conversion category

The importer has a bounded RGBA32F texel payload path for Blender images whose ordinary QImage/GL sRGB route cannot represent Workbench's source behavior. Blender 5.2.2's byte-image float-texture helper converts each byte texel through the active OCIO source-to-scene-linear processor before filtering; extraction now supports every source space in the active OCIO config, including nonlinear spaces, and identifies transforms by config cache ID plus source and scene-linear names. Converted data is little-endian float RGBA per texel. Byte sRGB, Linear Rec.709, and Non-Color retain their existing routes; spaces absent from the active config are reported instead of passed through. Blender's float-image Workbench path directly copies its float buffer without OCIO, including float images labeled sRGB; those payloads are identified as `blender-float-buffer`, not as an identity color-space conversion. PREMUL follows Blender's per-texel multiply behavior and alpha is copied exactly.

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
- **Refreshed face-interior probes (2026-10-14):** The current Blender 5.2.2
  headless fixture and classiCAD offscreen capture both run successfully. The
  27-probe comparison found that its prior front-face probe at `(290, 190)` was
  only a few pixels inside the top/front silhouette. The comparator now moves
  that row to `y=220` and shifts the right-face group inward. Fresh 640×480
  captures match exactly in scene-linear RGB at all 27 probes; display RGB has
  MAE `0.2963/255` and max `1/255`. This verifies only face interiors. It does
  not establish full-frame edges, effects, or QPainter parity.
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
- **Per-sample shading-stage attribution (2026-10-14):** the opt-in capture now
  also writes the opaque material-data RGB and alpha payload, plus view-space
  normal XYZ, before `finishSceneSample()`. It checks each attachment's
  validity before reading; current-sample color is checked against the scene
  pass report because the AA target marks that attachment valid at finish.
  A sidecar manifest labels capture order, PFM orientation/color encoding,
  depth conversion, and fixture effects; the existing jitter file records the
  pixel offsets. Eight 640×480 samples produced the expected color, depth,
  material-base, material-payload, normal, log-history, and resolved files;
  selected buffers parsed with finite values. History remains weighted log2
  data, and the resolved image remains post-AA scene-linear output before
  display conversion. These are classiCAD diagnostic buffers, not direct
  Blender internal-buffer comparisons, since Blender's installed background
  API exposes only its final render result. Validation: full `cmake --build
  build -j4` passed with the 316-source dependency audit; capture-enabled
  offscreen `viewport_render_contracts` passed (1/1, 29.29 seconds).
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

### Controlled Blender 5.2.2 shadow and cavity deltas (2026-10-14)

- Re-rendered the controlled 640x480 orthographic cube in Blender 5.2.2 and
  classiCAD's offscreen GPU contract at AA Off, with the same camera direction,
  display transform, and no object outline. Compared each effect after
  subtracting its own matching baseline, so the one-code background offset
  does not dominate the effect metric. The classiCAD capture test also passed.
- Shadow delta coverage closely matches: at a 2/255 changed-pixel threshold,
  Blender has 111,256 effect pixels and classiCAD 111,480; their mask IoU is
  0.997991. The signed RGB-delta error is 0.01546/255 whole-image MAE. This
  validates the controlled cube case, not other caster topologies or combined
  effects.
- World cavity also has close coverage: Blender has 47,830 changed pixels and
  classiCAD 47,985; mask IoU is 0.969152. Signed RGB-delta error is
  0.03629/255 whole-image MAE and 0.16433/255 within the changed-pixel union.
  Differences remain concentrated in the cavity response; do not claim full
  cavity parity or infer curvature-mode parity from this world-cavity case.
- Captures were generated by `tests/workbench_cube_reference.py` using
  `--effect BASELINE`, `--effect SHADOWS`, and `--effect CAVITY`, and by the
  `CLASSICAD_VIEWPORT_POLYGON_CUBE_*_CAPTURE` outputs from
  `viewport_render_contracts_test`. `tests/workbench_effect_delta_compare.py`
  produced the metrics. Outline, culling, X-Ray, specular, curvature, and
  effect combinations remain in the comparison checklist.

### Shared render-pass color attachment reporting (2026-10-13)

- The shared named pass report now marks `CurrentSampleColor` when opaque
  ranges draw, when regular or in-front transparency resolves into the current
  sample (including sorted fallbacks), when in-front opaque color merges, and
  when the deferred opaque resolve succeeds. The isolated in-front color
  buffer is reported as `FrontColor`. This makes the attachment contract
  describe the actual color outputs consumed by the live and offscreen AA
  paths. Focused capture assertions cover opaque, deferred, transparent, and
  in-front color reporting.
  The C++ build succeeds, and `viewport_render_contracts` now passes these
  runtime assertions with Qt's offscreen OpenGL context.

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

### QPainter texture-alpha cutoff boundary contract (2026-10-13)

- Added exact-threshold and next-float-below assertions for QPainter textured
  mesh color and software shadow-caster alpha. Both consumers use the strict
  `< 0.1f` test: equality remains visible/casts, while the next representable
  float below is cut out. This matches Blender 5.2.2's
  `workbench_image.bsl.hh`, which discards only when sampled alpha is less than
  `image_transparency_cutoff`; `workbench_materials.cc` supplies the 0.1 cutoff.
- Added the isolated offscreen CTest entry
  `workbench_qpainter_texture_alpha_cutoff`, which returns before the later
  OpenGL-only checks. The contract uses CPU QPainter paths and does not claim
  GPU image comparison or Blender rendered-image parity. The full C++ build
  and focused CTest pass.
- Pinned references:
  [`workbench_image.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_image.bsl.hh#L408-L451),
  [`workbench_materials.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_materials.cc#L86-L96).

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

**Theme gradient fallback sampling (2026-10-15):** The QPainter Theme fallback
now evaluates Linear and Radial gradients at every physical pixel center using
the same display-space `gamma 2.2` interpolation as classiCAD's direct-display
background shader. Linear gradients no longer approximate the ramp with 32
stops. A high-contrast 101×101 contract checks every row against that equation
within one display code value; the existing full-frame CPU/GPU comparison
continues to cover Single, Linear, and Radial backgrounds at 1× DPR and Radial
at 2× DPR. This improves consistency when the native background shader cannot
draw; the scene-linear QPainter compositor continues to decode sRGB endpoints
with the exact transfer function described above. The direct-display formula
is in
[`viewport_background.frag`](src/ui/viewport/shaders/viewport_background.frag);
the scene-linear comparison remains tied to Blender's 5.2.2 Workbench
background behavior documented in the Theme integration entry above.

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
  The bounded sorted fallback now captures scene-linear opaque and transparent
  fragments, keeps per-pixel far-to-near ordering, applies cavity to opaque
  RGB, and applies the display transform once after composition. Its in-front
  transparent fragments resolve after in-front opaque content. A Standard to
  AgX round-trip contract covers the resolve with an 8-bit capture tolerance
  of 8 channel values. Both paths still use the QPainter software cavity
  sampling approximation and need matched Blender image comparison.
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
  the RGBA8 scene-linear test target). The older average-depth painter fallback
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

### Matched Workbench multisample modes (2026-10-14)

- Compared the nonblank 640x480 polygon-cube captures for Blender 5.2.2 and
  classiCAD at 5, 8, 11, 16, and 32 samples. Both used AA Off-compatible
  camera/view settings, AgX, no outlines/effects, and the same neutral
  scene-linear background (`0.021942` per channel). Matching the background is
  necessary because the existing comparison's foreground threshold would
  otherwise classify Blender's earlier brown World color as geometry.
- Scene-linear whole-frame RGB MAE per channel stayed between `0.000025` and
  `0.000149`. With a `0.1` foreground threshold, mask IoU / XOR pixels were:
  5 samples `0.996258 / 299`, 8 `0.997173 / 226`, 11 `0.997171 / 226`, 16
  `0.999987 / 1`, and 32 `0.999975 / 2`. The edge-coverage MAE decreased
  from `0.017231` at 5 samples to `0.008244` at 32 samples.
- All five modes matched exactly at the 27 face-interior probes in
  scene-linear RGB. Display RGB also matched exactly at 5, 8, 11, and 16
  samples; 32 samples had `0.1111/255` probe MAE and `1/255` maximum error.
  These results cover this controlled cube and do not close navigation-time
  accumulation resets, other geometries, effects, or QPainter AA parity.
- Blender references used `tests/workbench_cube_reference.py` with the
  corresponding `--aa` value and
  `--world-color 0.0219421387 0.0219421387 0.0219421387`. classiCAD captures
  came from the matching `CLASSICAD_VIEWPORT_AA_SAMPLES*_CAPTURE` and
  `*_LINEAR_CAPTURE` outputs of `viewport_render_contracts_test`. The offscreen
  contract passed; `tests/workbench_pfm_compare.py` and
  `tests/workbench_display_probe_compare.py` produced the metrics above.

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
  clear global opacity mismatch. Display-space ratios cannot fully attribute
  the residual because AgX is nonlinear; the later scene-linear comparison
  below confirms the standalone effect opacity.
- These initial metrics are display-PNG delta comparisons. The classiCAD
  capture now also exports scene-linear outline baseline/effect PFM data, as
  described below. No production renderer change is justified by these metrics
  alone. The classiCAD capture
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
- **Two-blade DOF comparison (2026-10-14):** The matched capture helpers now
  accept an aperture-blade count, retaining six as the default, so the
  Blender 5.2.2 and GPU classiCAD fixtures can measure the two-blade slit case.
  Blender's pinned [`DofPass::setup_samples`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_effect_dof.cc#L839-L897)
  applies the polygon aperture mapping for blade counts greater than one, and
  the pinned [`DOF gather shader`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_effect_dof_blur1_frag.glsl#L21-L57)
  consumes those aperture samples. In matched 640×480 renders at f/0.1,
  rotation 0.2 radians, ratio 1.2, and two blades, the changed-effect masks
  contain 9,963 Blender pixels and 11,383 classiCAD pixels at a 2/255
  max-channel threshold (IoU 0.7942; XOR 2,448). At 3/255, they contain
  8,492/9,996 pixels (IoU 0.7847; XOR 2,230). Mean effect magnitudes are
  0.6450/0.7981 per pixel. The no-DOF baselines already differ by
  1.4036/255 mean max-channel error (98,686 pixels exceed 2/255), and the
  final DOF images differ by 1.5622/255. This two-blade output comparison is
  weaker than the six-blade mask result and does not isolate aperture shaping
  from display quantization or the existing half-resolution gather
  approximation; it confirms that two-blade visual parity remains open and
  does not justify a production DOF change by itself. Captures used
  `CLASSICAD_VIEWPORT_GPU_DOF_APERTURE_BLADES=2` with the offscreen
  `viewport_render_contracts` test and Blender's
  `tests/workbench_dof_reference.py --aperture-blades 2`; metrics came from
  `tests/workbench_dof_compare.py` at thresholds 2 and 3.
- **QPainter DOF comparison (2026-10-14):** Added an opt-in baseline/effect
  capture for the CPU compositor using the existing native NURBS cube depth
  geometry, matched 640×480 perspective optics, six aperture blades, f/0.1,
  rotation 0.2 radians, ratio 1.2, and the same neutral scene-linear World
  background as Blender 5.2.2. The polygon-mesh fixture has no prepared CPU
  depth triangles and is correctly rejected by this compositor, so the
  QPainter capture uses the equivalent native cube fixture already supported
  by its prepared-depth path. At a 3/255 per-pixel effect threshold, Blender
  changes 11,647 pixels and QPainter changes 4,265; effect-mask IoU is
  `0.024466`. Across the mask union, signed RGB-delta MAE is `18.13681/255`,
  p95 `57/255`, and maximum `93/255`. This exposes a substantial CPU DOF
  approximation gap. It does not isolate whether the cause is the gather
  kernel, half-resolution gather, or source-depth reconstruction, so no
  production equation change is justified from the final-image metric alone.
  Blender's pinned DOF pass and gather shader remain the reference; broader
  camera/aperture settings and fallback parity remain open.
  Reproduce with `CLASSICAD_VIEWPORT_QPAINTER_DOF_CAPTURE` and
  `CLASSICAD_VIEWPORT_QPAINTER_DOF_BASELINE_CAPTURE` on the offscreen
  `viewport_render_contracts` test, Blender's
  `tests/workbench_dof_reference.py --aperture-blades 6`, and
  `tests/workbench_effect_delta_compare.py` at threshold 3.
- **GPU transparent-film DOF alpha (2026-10-15):** Blender 5.2.2 runs its
  transparent color resolve and transparent-depth pass before DOF, then runs
  DOF before AA. The GPU resolve now restores alpha from the exact source pixel
  with `texelFetch` after its linearly filtered RGB resolve. A matched
  640×480 pair uses two overlapping camera-facing translucent quads, a
  transparent film, AA Off, orthographic projection, and the same camera,
  focus distance (1000), focal length (2000 mm), f-stop (0.01), blade count,
  aperture rotation, and ratio in both applications. The first capture showed
  classiCAD changing 6,779 alpha pixels (maximum 90/255) while Blender changed
  none by more than 1/255; pinning resolve alpha to the source pixel removed
  those changes. The final alpha comparator reports zero changed pixels over
  1/255 in both renderers, maximum alpha deltas of 1/255 (Blender) and 0/255
  (classiCAD), identical alpha coverage masks, and maximum Blender/classiCAD
  effect-alpha error of 1/255. This supports the alpha behavior for this
  overlapping-quad fixture only. RGB effect coverage remains a separate open
  mismatch: at a 2/255 threshold the DOF RGB change masks contain 3,960 Blender
  pixels and 1,222 classiCAD pixels, with no shared mask pixels. The existing
  RGB DOF approximation and its broader optical/output parity remain open.
  Blender's pinned [`workbench_engine.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_engine.cc#L2632-L2645)
  orders transparent resolve/depth before DOF and AA;
  [`workbench_effect_dof.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_effect_dof.cc#L993-L1093)
  binds combined color and scene depth. The custom blend inputs are declared
  in [`workbench_effect_dof_infos.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/infos/workbench_effect_dof_infos.hh#L78-L85)
  and populated by
  [`workbench_effect_dof_resolve_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_effect_dof_resolve_frag.glsl#L21-L34).
  Reproduce classiCAD captures with
  `CLASSICAD_VIEWPORT_TRANSPARENCY_CAPTURE=<file>
  CLASSICAD_VIEWPORT_TRANSPARENT_DOF_CAPTURE_DIR=<dir>
  QT_QPA_PLATFORM=offscreen ctest --test-dir build -R
  '^viewport_render_contracts$' --output-on-failure`. Render Blender 5.2.2
  baseline/effect with `tests/workbench_cube_reference.py --scene
  TRANSPARENT_OVERLAP --match-classicad-ortho --view-direction 4 -6 4
  --ortho-scale 5 --transparent-film --aa OFF --zero-dither [--camera-dof]
  <output.png>`, then compare alpha using
  `tests/workbench_transparent_dof_alpha_compare.py`.
- **Scene-linear outline attribution (2026-10-14):** The controlled
  polygon-cube capture now saves the pre-display Blender Combined and
  classiCAD scene-color targets as RGB PFM alongside the existing PNG pair.
  `tests/workbench_outline_linear_compare.py` compares the effect deltas and
  estimates per-pixel outline opacity by projecting the baseline-to-effect
  color change onto the baseline RGB. At a linear delta threshold of 0.001,
  both 640×480 masks contain 2,316 pixels (XOR 0; IoU 1.0). Scene-linear
  baseline RGB MAE is 0.033844, while outline-delta RGB MAE is 0.00007965.
  Estimated mean opacity is 0.32477 for Blender and 0.32484 for classiCAD;
  medians are 0.25018/0.25035 and p95 values are 0.5 for both. Both opacity
  distributions lie close to the four-cardinal quarter steps. This attributes
  the earlier 19.1788 versus 13.9650/255 mean display-delta magnitudes to the
  different baseline shading/color under nonlinear AgX output; the measured
  standalone GPU outline blend and opacity agree for this fixture, so no
  production outline change is indicated. Broader geometry, cameras,
  transparency, combined effects, and QPainter remain open. The classiCAD
  capture uses `CLASSICAD_VIEWPORT_POLYGON_CUBE_OUTLINE_LINEAR_CAPTURE` and
  `CLASSICAD_VIEWPORT_POLYGON_CUBE_OUTLINE_BASELINE_LINEAR_CAPTURE`; Blender
  uses `--linear-output` and `--linear-pfm-output` with the controlled
  `BASELINE` and `OUTLINE` fixtures. PNG mask attribution and linear PFM
  metrics were both run after the full build and offscreen
  `viewport_render_contracts` test passed.
- **Matched QPainter world-cavity comparison (2026-10-14):** The offscreen
  contract can now capture QPainter baseline and WORLD-cavity output for the
  same 640×480 cube, camera, Studio light, AgX transform, and cavity controls
  used by the Blender 5.2.2 reference, including pre-display RGB PFM targets.
  The baseline silhouette masks contain 111,480 Blender pixels and 111,878
  QPainter pixels (XOR 398; IoU 0.9964), so the large cavity difference is
  not explained by the outer silhouette. At a 0.001 scene-linear effect
  threshold, the cavity masks contain 81,314 Blender pixels and 21,273
  QPainter pixels (XOR 69,211; IoU 0.1943). Mean max-channel effect is
  0.003154 for Blender and 0.001652 for QPainter; scene-linear effect-delta
  RGB MAE over the mask union is 0.01411 (p95 0.05684). The baseline RGB MAE
  is 0.05099, reflecting different CPU/GPU surface shading, while the
  within-renderer linear deltas still show QPainter's smaller cavity coverage
  and magnitude. Blender's pinned [`workbench_cavity.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_cavity.bsl.hh#L41-L100)
  and [`workbench_effect_cavity.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_effect_cavity.cc#L52-L68)
  define the screen-space offset, depth sampling, and Hammersley disk points;
  QPainter retains a discrete software-neighborhood approximation. These
  measurements confirm that the fallback cavity still needs parity work but
  do not isolate a single safe formula correction. Reproduce with the
  `CLASSICAD_VIEWPORT_QPAINTER_CAVITY_*` capture variables,
  `tests/workbench_cube_reference.py --effect BASELINE|CAVITY --linear-output
  <path.exr> --linear-pfm-output <path.pfm>`, and
  `tests/workbench_effect_linear_compare.py` at a 0.001 threshold. The full
  C++ build and offscreen `viewport_render_contracts` test passed.
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
- **Opaque-only in-front overlay-depth mask (2026-10-14):** Blender 5.2.2's
  late overlay-depth draw uses the opaque object's stencil value on the main
  depth attachment (`workbench_effect_antialiasing.cc`); the in-front
  transparent-depth merge does not populate that stencil. classiCAD previously
  used the merged front depth texture as its override mask, incorrectly moving
  transparent-only front pixels to the near plane. A separate R8 coverage
  attachment is now captured from the opaque front depth before the transparent
  front depth pass. The late override samples only this mask. The matched
  640×480 fixture keeps Combined exactly equal to Blender (all RGB samples
  equal); opaque in-front `(365,240)` is at classiCAD's 0.01 near clip versus
  Blender's `0.0100135`, while transparent-only `(450,240)` remains physical
  (`999.6`, within 0.4 of Blender's `999.9999`) instead of becoming 0.01. The
  focused pass test and `viewport_render_contracts` pass.
- Pinned Blender references: [`workbench_mesh_passes.cc` opaque/front
  routing and depth merge](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_mesh_passes.cc#L109-L201),
  [`workbench_mesh_passes.cc` transparent and depth passes](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_mesh_passes.cc#L232-L371),
  [`workbench_merge_depth_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_merge_depth_frag.glsl#L7-L13),
  [`workbench_effect_antialiasing.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_effect_antialiasing.cc#L133-L140) and its late invocation at [lines 243-253](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_effect_antialiasing.cc#L243-L253),
  [`workbench_overlay_depth_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_overlay_depth_frag.glsl#L7-L13),
  and [`workbench_engine.cc` pass order](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_engine.cc#L496-L504).
- Separate QPainter caveat: weighted OIT and the bounded sorted fallback apply
  cavity in scene-linear RGB before transparency. The sorted path keeps a
  per-pixel fragment list and alpha-over order rather than weighted OIT; its
  four-million-fragment limit can still route scenes to the older average-depth
  fallback. The GPU overlap fixture does not validate QPainter's software
  cavity sampling against Blender.
- **Full-frame Combined comparison (2026-10-14):** rendered the same
  `TRANSPARENT_IN_FRONT` fixture in Blender 5.2.2 and classiCAD at 640×480,
  top orthographic view, AA Off, and 0.01/2000 clip range. Matching the
  classiCAD fixture's scene-linear clear color (`0.0232` per channel) was
  required; the Blender fixture's default World color produces a nonmatching
  background. After matching it, the Blender and classiCAD scene-linear
  Combined PFMs are identical across all RGB samples (MAE and maximum error
  are zero for each channel). Their foreground masks each cover 176,100 pixels
  and have IoU 1.0 with zero XOR pixels. This establishes full-frame GPU color
  composition for this fixture; view-Z and front-domain depth remain covered
  by the existing isolated probes, and the saved classiCAD display PNG is the
  raw scene-linear capture rather than a matched display-transform output.
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
  the current sorted compositor uses scene-linear composition; only the older average-depth fallback remains display-space.

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
- GPU and QPainter slice opacity use the selected object-space dimension
  multiplied by object scale, without rotating the bounds into a world AABB.
  A direct offscreen GPU contract rotates an anisotropic 2×2×4 volume so the
  world-X AABB is twice its object-X dimension, then verifies the GPU opacity
  still corresponds to Blender's `0.05 * dimensions[axis]` step. The pinned
  [`VolumePass::draw_slice_ps`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_volume.cc#L1026-L1063)
  gets dimensions from `BKE_object_dimensions_get`; the pinned
  [`object.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/blenkernel/intern/object.cc#L3498-L3509)
  defines those dimensions without rotating the bounds into world axes.
- A direct `QOpenGLContext` + `QOffscreenSurface` + FBO contract renders a
  synthetic active volume, confirms the `Volume` pass is reported, and checks
  that more than 100 pixels change. This does not launch the classiCAD window.
- The port remains bounded to a single-channel FLOAT grid. Multiple/vector
  grids, animation, volumes above the dense GPU bounds, and matched QPainter
  output remain unsupported. Blender's Workbench volume shader itself
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

### Full-frame background and display comparison (2026-10-14)

- Repeated the five matched nonblank 640×480 cube captures with Blender 5.2.2
  (`d13f752e3b9c`) and classiCAD's offscreen GPU capture. The cases were World
  and Viewport backgrounds under AgX and Standard, plus Standard with exposure
  1 and gamma 2; AA and dither were off. The four corner scene-linear RGB
  values matched exactly for each background source, and the 27 face-interior
  probes matched exactly for every case. This is a render-result fixture, not
  Blender's interactive Theme background path.
- Full-frame display comparison found a one-channel, one-code-value rounding
  residual on the World background: 195,944 differing pixels for AgX
  (0.29938/255 MAE) and the same count for Standard (0.32612/255 MAE). Under
  Standard with exposure 1 and gamma 2, the World case differed at 224 cube
  edge pixels (0.06781/255 MAE, max 117/255). Viewport-background captures
  differed at 224 cube edge pixels under AgX (0.01872/255 MAE, max 47/255)
  and Standard (0.04035/255 MAE, max 94/255). The edge residuals are localized
  to cube coverage; sampled face colors and background scene-linear values
  remain equal. These numbers clarify the earlier probe-only comparison and do
  not sign off on all color-management or background options.
- Validation: the offscreen `viewport_render_contracts` capture passed (1/1);
  Blender 5.2.2 background renders and scene-linear conversions completed for
  all five cases. `workbench_display_probe_compare.py` and
  `workbench_pfm_compare.py` were used on the captures. Theme, interactive
  Solid/Wireframe background display, QPainter, non-sRGB display devices, and
  broader transforms remain open.

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
- **Use the existing Workbench material adapter seam.** The future material/shader evaluator should resolve only Workbench-supported material inputs: scene-linear base color, roughness, metallic, alpha, and the supported image-texture result plus its sampling/color-space/alpha settings. Blender 5.2.2's active image source includes Image Texture and Environment Texture nodes; classiCAD retains the active node's image record, using Extend when Environment Texture has no extension property and Repeat for the Image Texture fallback. This does not evaluate arbitrary shader-node graphs. Feed those values through `WorkbenchMaterialData` in `src/core/document/scene_object.h` and the per-slot conversion in `src/ui/viewport/viewport_render_frame.cpp`. Per-face material slot indices, active UVs, and color attributes stay in `PolygonMesh3D` (`src/core/geometry/polygon_mesh.h`) and travel through `ViewportRenderObject` (`src/ui/viewport/viewport_render_frame.h`) into `DrawRange` (`src/ui/viewport/viewport_surface_renderer.h`). Geometry normals remain sourced from evaluated/display geometry; they are not material-evaluator outputs. Blender UDIM tile numbers, external paths, and packed tile bytes survive import/native save. GPU color, effect-alpha, and object-ID-alpha paths sample the tiles; shadow casters do not sample image alpha. QPainter color and cavity/outline effect-alpha sampling support meshes, trimmed/untrimmed NURBS, and prepared-UV `NurbsSolid` effect triangles. GPU and QPainter shadow depth-map fallbacks cast complete geometry without Image Texture alpha, matching Blender's shadow-volume inputs; their caster/topology coverage remains narrower than the stencil path. QPainter's separate software shadow fallback casts binary shadows from tessellated geometry for opaque and transparent-material casters without sampling material or Image Texture alpha, while nearest visible transparent receiver shading uses a bounded aggregate approximation. GPU per-tile Repeat, Extend, Clip, and Mirror modes are implemented. Filtering across neighboring tile seams is not part of Blender 5.2.2 behavior. The GPU object-ID pass uses shared tessellation UV data for trimmed and untrimmed NURBS ranges, so UDIM alpha masking is supported there.
5. **Keep fallback scope honest.** Preserve the QPainter path for fallback, but compare and document it separately; do not claim that supersampling is equivalent to Blender's GPU pass graph. If CPU visual parity becomes a requirement, make it a separate rendering backend project after the GPU path is stable.

### Phase Two acceptance

- A repeatable, nonblank comparison uses identical geometry, camera, viewport size, color transform, AA setting, and overlay state.
- Intermediate outputs identify whether any edge/color difference appears in geometry shading, effect composition, AA resolve, or display conversion.
- Viewport and Workbench render-result GPU paths share the same named scene passes and attachment contract.
- Each newly ported pass has a pinned Blender 5.2.2 source reference, preserved attribution, and a comparison result; existing NURBS/surface contracts remain unchanged.
- The future shader system can provide Workbench's supported material values through `WorkbenchMaterialData` and existing mesh/draw-range data without changing the render-pass API or creating a second material model; arbitrary node graphs are out of scope. Imported UDIM metadata and packed tile bytes survive native save/reopen; GPU and QPainter mesh/trimmed-NURBS/untrimmed-NURBS color and effect-alpha paths consume tiles. GPU and QPainter shadow depth-map fallbacks cast complete geometry without Image Texture alpha, matching Blender's shadow-volume inputs. Per-face seed materials on `NurbsSolid` resolve through stable semantic IDs and classiCAD `.blend` proxy assignments round-trip by face ID; materialized-face lineage remains unsupported. The QPainter Solid fallback has an approximate software shadow pass for triangulated geometry, with opaque and transparent-material casters remaining binary and independent of material/Image Texture alpha; several geometry cases remain unsupported, while retained transparent layers preserve shadows on opaque receivers and the nearest visible transparent receiver contribution receives an approximate factor; stacked transparent layers share that factor; parity with the GPU shadow path remains open (see [QPainter opaque shadow fallback](#qpainter-opaque-shadow-fallback-2026-10-13)). GPU per-tile extension modes are implemented. Cross-tile filter continuity is not part of Blender 5.2.2 behavior.
- A bounded single-channel Blender `VOLUME` grid imports, survives native save/reopen, and renders through the GPU Volume pass after transparent depth; sampling, density scale, slice controls, and Blender's pass order are source-aligned. The QPainter fallback renders slice mode for at most 16M dense voxels and 1M covered target samples, with closest/linear/cubic filtering and scene-linear extinction/scattering. Its ray-marched mode now has a bounded CPU approximation with closest/linear/cubic filtering, scene-depth clipping, front-coverage rejection, and density integration; dense reconstruction is capped at 16M voxels, candidate target pixels at 1M per volume, and total ray integration at 4M samples per composition. Multiple/vector grids, animation, larger bounds, and matched Blender image parity remain open.
- QPainter differences remain separately described until that backend is explicitly brought into scope.

### Stage 5: Parity review

Status: in progress; the implementation and source reviews found the blockers listed above. The team has not signed off on full parity.

- The user clarified that classiCAD's current viewport should be treated as Edit Mode; its separate Object Mode is planned for later. Do not use the missing Object Mode as an explanation for base surface, background, or grid differences. The supplied screenshots still are not a controlled render comparison: classiCAD reports `Ortho`, Blender reports `User Perspective`, and the visible scene geometry differs. Blender's mesh edit edges and curve edit wires are separate Overlay-engine geometry; classiCAD's Solid-mode CAD curve strokes and `NurbsSolid` face boundaries use the configurable edit-wire theme setting, while control geometry and selection behavior remain a distinct implementation. Compare base rendering separately from mode-specific overlays, with matching camera and geometry.
- This is an adaptation of selected Blender shading equations and passes into classiCAD's renderer, not a verbatim port of Blender's complete Workbench and Overlay engines. The table above and stage notes remain the parity record.
- Review execution confirmed the installed Blender runtime is 5.2.2 LTS and the classiCAD C++ build passes. Background extraction smoke checks now pass for original and evaluated mesh modes, POLY/BEZIER/NURBS curves, NURBS surface patches, nested collection parents and visibility records, and reporting omitted loose mesh geometry. An offscreen classiCAD startup reached viewport construction, but Qt could not create an OpenGL context; this does not validate the GPU renderer. A direct native-GPU offscreen cube render is now compared against Blender's background Workbench render at 640×480 for AA Off, Single Pass, and 8 samples. The Blender fixture can now independently render baseline, shadows, cavity, and outline modes. Blender 5.2.2 reference runs changed 111,858 pixels for shadows, 45,995 for cavity, and 2,410 for outline at an RGB threshold of 2. classiCAD's offscreen GPU contracts now require shadows to darken a pixel and World cavity to change a pixel against matching unselected, outline-free captures; cavity uses the shared-target scene-pass coordinator. These assert effect activation, not pixel parity. Scene-camera GPU projection now consumes Blender's sensor fit and dimensions, pixel aspect, camera shifts, orthographic scale, and exact clip range; depth readback uses that same range. A transform contract checks framing center, shift direction/magnitude, pixel-aspect scaling, and clip values. Matched Blender camera renders, annotation alignment with non-square pixels, direct internal Blender per-sample buffer reads, QPainter, and navigation cases remain open. Full parity is not signed off.
- The AA capture comparison has a numerical edge-profile follow-up recorded in the [controlled cube baseline](#controlled-workbench-cube-baseline-2026-10-12). Orthographic matrix precision and vertical jitter sign are corrected; fresh captures confirm exact silhouette masks at Off, Single Pass, and 8 samples. The source-derived per-sample fixture isolates the previous 8-sample mismatch to projection jitter Y. Scene-linear foreground RGB MAE remains 0.00155 in the controlled cube comparison; broader QPainter AA and navigation image comparisons remain open.

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
- **AA history reset matrix (2026-10-14):** key contracts now isolate camera
  zoom, logical-size change at fixed DPR, DPR change at fixed logical size,
  specular-setting change, and AA-mode change. The offscreen renderer contract
  then verifies that each changed key advances the history epoch and begins at
  sample zero, while an unchanged key in a multisample mode advances to sample
  one in the same epoch. Existing cases continue to cover mesh revision,
  selection, object membership, material/texture data, visibility mode, and
  active background color. `cmake --build build -j4` and the focused offscreen
  `viewport_render_contracts` test pass. This validates the renderer key and
  sample lifecycle; app-level playback remains unwired until classiCAD has a
  playback owner. Blender source: [`workbench_state.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_state.cc#L1303-L1382)
  and [`workbench_effect_antialiasing.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_effect_antialiasing.cc#L145-L149).
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
  path. The QPainter outline pass now also uses the configured color without
  branching on the selected-ID bit. Its offscreen contract compares selected
  and unselected object images and requires identical outline output. The GPU
  contract compares selected and unselected silhouette pixels against matching
  outline-free baselines. Broader effect pixel comparisons remain open.
- **Workbench effect UI-scale sampling:** GPU outline and QPainter outline
  cardinal taps use an integer scale adapter (`max(1, qRound(DPR))`). Blender
  assigns `U.pixelsize` to Workbench's
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
  response, outline rejection, and background rejection. CPU curvature now
  retains fractional Qt DPR offsets until nearest-texel sampling for both the
  object-ID and normal buffers, matching Blender's shared normalized offset
  and default no-linear-filter sampler behavior. See
  [`GPU_texture.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/gpu/GPU_texture.hh#L2660-L2677).
  The DPR scale contract covers 1.25 and 1.5. The source sampler uses
  Blender's DPI-derived `ui_scale`; classiCAD currently uses DPR as its
  available approximation. The sorted fallback now retains transformed layers for later effects but
  computes opaque/transparent surface composition in scene-linear RGB. The
  average-depth painter fallback remains display-space.
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
  Studio Default, Flat, and selected Studio Paint rotations have 27-probe
  checks; full-frame GPU Paint comparison covers world-space 90° rotation.
  Full-frame GPU MatCap comparison covers `metal_bronze` with
  specular, `check_normal+y` without specular, and `ceramic_lightbulb` with
  specular. QPainter face-interior probes cover those three preset/specular
  combinations. A full-frame AA-Off QPainter comparison now covers
  `metal_bronze` with specular: display MAE is `0.1292/255`, and mask IoU is
  `0.997991` with 224 XOR pixels; an isolated shared-face-boundary outlier
  reaches `105/255`. QPainter full-frame comparisons also cover Studio Default
  and Paint world-space rotations 0°, 37°, and 90°. Exact edge parity remains
  open. Other presets, AA modes, camera orientations, and broader full-frame
  QPainter parity remain open (see [QPainter MatCap full-frame comparison](#qpainter-matcap-full-frame-comparison-2026-10-14)).
- Every color source, using objects with material values, object colors, color attributes, and UV-mapped textures. Full-frame GPU cube comparisons cover Material, Object, Random, constant- and varying-UV Texture, Custom/Single, per-corner Vertex Attribute, and Point-domain Attribute; matched regular planar NURBS patch comparisons cover filtered Texture UVs and STRAIGHT, PREMUL, NONE/IGNORE, and CHANNEL_PACKED alpha. Matched GPU and QPainter curved NURBS captures compare STRAIGHT cutout and NONE alpha bypass in orthographic and perspective views; a curved trimmed patch with an interior hole now also has matched STRAIGHT/NONE alpha captures. Matched QPainter captures cover regular planar mesh textures (per-face slots, alpha modes, and perspective UV interpolation), a constant-texture mesh shared-edge alpha case, a two-tile mesh UDIM Closest- and Linear-filter image comparison, Flat/Material three-slot and Flat/Object/Random/Custom-Single/Corner-Attribute/Point-Attribute/Texture cube comparisons. Saved Blender 5.2.2 `.blend` fixtures with polygon meshes and single-/multi-span NURBS Surfaces pass through classiCAD's production import, frame, GPU, and QPainter paths with paired material/texture captures plus imported STRAIGHT-alpha cutout checks. CPU contracts cover Point interpolation and constant Face-domain colors. Blender 5.2.2 does not recognize Face-domain `FLOAT_COLOR` as a color attribute, so that classiCAD extension has no Blender reference equivalent. Residual silhouette and display-edge differences remain. Broader trimmed-surface textures and alpha modes, curved-surface alpha modes/settings/camera coverage, other imported-surface knot/UV modes, alpha combinations and filters, broader QPainter parity, and other UDIM filters/extensions remain open.
- Background choices and color management.
- Culling, X-Ray, shadows, outlines, specular, and cavity/curvature, both individually and in combinations. Verify camera DOF now that classiCAD imports and persists camera optics, including aperture shape controls.
- No AA, the preference's “Single Pass Anti-Aliasing” value (Workbench one-sample TAA+SMAA), and 5/8/11/16/32-sample multisampling. Matched fixed-cube captures cover every value while idle; interruption/resume state contracts cover each 5/8/11/16/32-sample preference during navigation. A matched fixed-cube QPainter matrix now compares silhouettes and full-frame color for all seven values with Studio Default material shading and effects disabled; other color/effect combinations and navigation image comparisons remain open (see [QPainter fixed-cube AA comparison](#qpainter-fixed-cube-aa-comparison-2026-10-14)).
- Accumulation reset after camera movement, geometry edits, selection changes, setting changes, resize, and display-scale changes. Renderer contracts now cover camera zoom, logical resize, DPR, a shading setting, and AA-mode changes, in addition to mesh/selection/material/background key checks.
- Overlay Smooth Wires and Edit Mode Smooth Wires independently, including confirming the grid remains antialiased when overlay wire smoothing is off.
- Save/reopen behavior for all persistent settings and any new material or color data. The Workbench settings contract now round-trips every `ViewportShadingSettings` field, custom LUT bytes/metadata, and object Image Texture plus UDIM payload in one update-session file; QSettings coverage cycles all seven viewport AA modes and all four independent Overlay/Edit Mode Smooth Wires combinations. Camera preferences and the broader application settings matrix remain separate acceptance coverage.
- GPU renderer and QPainter fallback behavior.

### QPainter fixed-cube AA comparison (2026-10-14)

- Added opt-in production-path PNG captures to
  `viewport_render_contracts` through
  `CLASSICAD_VIEWPORT_QPAINTER_AA_CAPTURE_DIR`. The capture uses the same
  640×480 polygon cube, camera, QPainter AA flag, supersampling factors
  (1×/2×/3×/4×/6×), surface compositor/effect resolve, and DPR image
  composition as the viewport. The compositor's existing one-million-pixel
  work cap means 2× and larger captures follow the same direct-draw fallback
  used by the widget when that cap is exceeded. Studio Default material
  shading is captured with outline, shadows, DOF, and cavity disabled.
- Compared each QPainter output to its own Blender 5.2.2 reference at matching
  640×480 framing. Blender's `--aa FXAA` fixture argument sets the stored enum
  named FXAA, which Workbench consumes as one unjittered TAA sample, TAA color
  filtering, and SMAA resolve; it does not run an FXAA shader. At a 5/255
  foreground threshold, mask IoU / XOR pixels are: AA Off `.997991 / 224`,
  Single Pass 2× `.996456 / 396`, 5 samples `.992654 / 824`, 8 samples
  `.997266 / 306`, 11 samples `.996669 / 373`, 16 samples `.993762 / 699`, and
  32 samples `.993557 / 722`. The fresh AA-Off mask includes the pixel-center
  coverage change recorded below. QPainter supersampling remains approximate
  and non-monotonic; it does not reproduce Blender's temporal TAA/SMAA resolve.
- Full-frame display MAE / p95 per channel is: Off `.3699 / 1`, Single Pass
  `.5255 / 1`, 5 samples `.6096 / 1`, 8 samples `.6079 / 1`, 11 samples
  `.5270 / 1`, 16 samples `.4053 / 1`, and 32 samples `.4158 / 1` (values
  expressed per 255). The 4-pixel silhouette-band MAE ranges from
  `.8794/255` at Off to `8.2065/255` at 5 samples. Interior p95 is `2/255`
  for each mode. These captures verify a useful color baseline for one
  material/cube, not full color-source or effect parity.
- Reproduce QPainter captures with
  `QT_QPA_PLATFORM=offscreen CLASSICAD_VIEWPORT_QPAINTER_AA_CAPTURE_DIR=/tmp/classicad-qpainter-fullframe ctest --test-dir build -R '^viewport_render_contracts$' --output-on-failure`.
  Render each Blender reference through `tests/workbench_cube_reference.py`
  with `--aa OFF`, `--aa FXAA`, and each supported sample count, keeping
  `--lighting STUDIO --color-source MATERIAL --background-source WORLD
  --world-color 0.0219421387 0.0219421387 0.0219421387
  --match-classicad-ortho --zero-dither`; compare with
  `tests/workbench_image_compare.py`.
- Blender reference: [Workbench state and AA selection](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_state.cc), [Workbench AA resolve](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_effect_antialiasing.cc), and [Workbench TAA shader](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_fx_taa.bsl.hh). This is a measured fallback approximation, not a port of those GPU resolve passes.

### QPainter full-frame cube baseline (2026-10-14)

- Compared the actual QPainter AA-Off output with a Blender 5.2.2 Studio Default
  material cube at 640×480, matched orthographic framing, AgX, zero dither, and
  a neutral scene-linear world background that displays as RGB `(34,34,34)`.
  Both captures disable outline, shadows, DOF, and cavity. The 27 established
  face-interior display probes have MAE `0.8889/255` and maximum error
  `2/255`.
- After the AA-Off center-sample change below, full-frame display comparison
  reports per-channel MAE `0.3699/255`, p95 `1/255`, maximum `105/255`,
  foreground-mask IoU `0.997991`, and 224 XOR pixels at a 5/255 foreground
  threshold. The four-pixel-eroded reference object mask has MAE `0.8074/255`
  and p95 `2/255`; its maximum still includes internal face boundaries.
  Residual coverage differences remain around the silhouette; exact edge
  parity is still open.
- Reproduce the classiCAD capture with
  `CLASSICAD_VIEWPORT_QPAINTER_AA_CAPTURE_DIR=/tmp/classicad-qpainter-fullframe
  QT_QPA_PLATFORM=offscreen ctest --test-dir build -R
  '^viewport_render_contracts$' --output-on-failure`, render the Blender
  reference with `blender --background --python
  tests/workbench_cube_reference.py -- /tmp/blender-workbench-flat-aa-off.png
  --aa OFF --lighting STUDIO --color-source MATERIAL --background-source WORLD --world-color
  0.0219421387 0.0219421387 0.0219421387 --match-classicad-ortho
  --zero-dither`, and compare with `tests/workbench_image_compare.py`.
- Blender reference source remains pinned to
  [`workbench_state.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_state.cc).

### QPainter AA-Off center-sample coverage (2026-10-14)

- QPainter polygon-mesh and NURBS triangle rasterizers now select one
  pixel-center coverage sample when the viewport AA preference is Off. The
  fallback keeps its existing 4×4 estimate for every non-Off preference. The
  widget forwards its selected mode to the shared renderer; material,
  tessellation, and geometry contracts are unchanged.
- Against the same Blender 5.2.2 AA-Off cube, the foreground mask improved
  from IoU `0.994440` / 622 XOR pixels to `0.997991` / 224 XOR pixels. Full-frame
  display MAE fell from `0.8308/255` to `0.3699/255`, while the silhouette-band
  MAE fell from `5.6402/255` to `0.8794/255`. This is a closer coverage match,
  not exact pixel parity; the largest remaining color difference occurs at a
  silhouette pixel where the renderers choose different coverage.
- The opt-in shared-edge mesh contract also runs with AA Off and checks that
  the shared diagonal contributes one half-alpha fragment. The non-Off 4×4
  path remains covered by the AA capture matrix.
- Validation: `cmake --build build -j4`, then run `viewport_render_contracts`
  with `CLASSICAD_VIEWPORT_QPAINTER_AA_CAPTURE_DIR` and
  `CLASSICAD_VIEWPORT_QPAINTER_MESH_SHARED_EDGE_CAPTURE` set, plus
  `workbench_raster_fragment_contracts`. Blender source remains pinned to
  [`workbench_state.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_state.cc)
  and [`workbench_effect_antialiasing.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_effect_antialiasing.cc).

### QPainter widget AA preference propagation (2026-10-14)

- The viewport paint path now sets the shared `ViewportRenderer` raster AA
  flag from `viewportAaMode_` on every frame. AA Off therefore selects one
  physical-pixel-center sample in the production QPainter fallback; enabled
  modes retain the existing bounded 4×4 edge estimate. Reapplying the setting
  per frame also prevents a previous mode from persisting after a preference
  change. No geometry, material, or compositor contract changed.
- Blender 5.2.2 performs scene rasterization before its separate AA/TAA/SMAA
  resolve, as pinned above. This wiring activates classiCAD's previously
  implemented AA-Off coverage behavior in the normal widget fallback; it does
  not make the CPU estimate equivalent to Blender's resolve.
- Validation: `cmake --build build -j4` passed with the dependency audit;
  offscreen `viewport_render_contracts` passed with the AA capture directory
  enabled; and `workbench_raster_fragment_contracts` passed. The AA capture
  contract manually configures its test renderer, so it validates the raster
  modes rather than observing the widget preference itself. No new matched
  Blender image metric is claimed for this wiring-only change.

### QPainter mesh edge-coverage candidate re-audit (2026-10-14)

- Rechecked `workbenchTrianglePixelCoverage` in
  `src/ui/viewport/viewport_renderer.cpp` and the widget's AA preference
  wiring. The suspected center-only rasterization is limited to AA Off; every
  enabled QPainter raster-AA mode already estimates edge coverage with a
  bounded 4×4 subpixel grid. Interior pixels keep the center-sample fast path.
- Blender 5.2.2 resolves scene AA after the mesh passes. Its AA Off mode is a
  single raster sample, while FXAA/Single Pass and the multisample choices use
  the TAA/SMAA stages recorded in the pinned
  [`workbench_effect_antialiasing.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_effect_antialiasing.cc)
  and
  [`workbench_state.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_state.cc#L197-L225).
  Thus forcing supersampled coverage into AA Off would diverge from Blender;
  increasing the enabled-mode grid without an edge-profile comparison would
  be speculative. The existing 4×4 path remains the bounded CPU approximation.
- No renderer change was justified. Validation: the existing full-frame
  QPainter AA capture matrix documents all seven modes and the Blender
  comparison; `viewport_render_contracts` and
  `workbench_raster_fragment_contracts` both passed offscreen (2/2). The C++
  build and dependency audit passed immediately before this audit; no source
  code changed during the re-audit.

### QPainter MatCap full-frame comparison (2026-10-14)

- Compared a 640×480 QPainter AA-Off polygon cube using the `metal_bronze`
  MatCap with Blender 5.2.2 under matched orthographic framing, Material color,
  neutral `(34,34,34)` displayed world background, and zero dither. Specular is
  enabled in both. Outline, shadows, DOF, and cavity are disabled.
- Full-frame display MAE is `0.1292/255` with p95 `1/255`; common-foreground
  MAE is `0.3353/255` with p95 `1/255`. The foreground mask matches the AA-Off
  Studio baseline at IoU `0.997991` / 224 XOR pixels. Silhouette-band MAE is
  `0.3479/255` with p95 `1/255`. The maximum channel error is `105/255` at a
  shared triangulated face boundary; 95% of channels differ by at most one,
  so this isolated edge case remains visible despite the low aggregate error.
- The bounded surface compositor declined this MatCap fixture, so the capture
  intentionally exercises the widget's direct QPainter fallback. No renderer
  change is justified by these results. The opt-in
  `CLASSICAD_VIEWPORT_QPAINTER_MATCAP_CAPTURE` fixture reuses the controlled
  cube and display settings; compare its output with
  `tests/workbench_image_compare.py`.
- Validation: `cmake --build build -j4` and the offscreen
  `viewport_render_contracts` test with both
  `CLASSICAD_VIEWPORT_QPAINTER_AA_CAPTURE_DIR` and
  `CLASSICAD_VIEWPORT_QPAINTER_MATCAP_CAPTURE` set. Blender source is pinned to
  [`workbench_state.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_state.cc)
  and [`workbench_composite.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_composite.bsl.hh).

The port is complete when every enabled option has matching Blender 5.2.2 behavior, no enabled saved setting is inert, and the comparison checklist passes. Disabled unsupported features must remain clearly documented. Workbench shadows now use validated stencil volumes for closed `NurbsSolid` and imported `PolygonMesh3D` subsets, plus validated open surfaces; full parity still requires comparison and a matching path for the currently disclosed unsupported-geometry and transparency fallbacks.

## QPainter opaque shadow fallback (2026-10-13)

- Added a software light-depth pass to QPainter Workbench effects for opaque
  triangulated NURBS tessellation and polygon meshes. The pass uses the scene
  display shadow direction and intensity. In this initial implementation,
  transparent surfaces remained excluded as casters; when retained layers are available,
  an opaque receiver's shadow now darkens only its contribution beneath a
  visible translucent layer. The initial implementation sampled texture alpha
  in the light-depth pass; the 2026-10-14 correction below removes that
  behavior to match Blender's geometric shadow casters. This remains a
  bounded approximation; Blender Workbench uses stencil shadow volumes in
  its normal GPU path.
- Contracts cover shadow activation, direction-driven mask movement, zero
  intensity, monotonic intensity response, and a fully transparent textured
  caster. The full build passed. The offscreen viewport contract reported no
  failed assertions through the CPU shadow checks, then exited 1 at its later
  existing OpenGL-context requirement for the NURBS-solid display case; no
  GUI/OpenGL validation is claimed.

### QPainter software shadow caster alpha (2026-10-14)

- The QPainter light-depth fallback now casts from tessellated caster geometry
  without sampling material or Image Texture alpha. This matches Blender
  5.2.2's shadow-volume behavior: transparent materials still submit their
  geometry to the shadow pass, including materials with zero alpha. Zero-alpha
  triangles are retained as casters but skipped by visible color/effect and
  depth sampling. Color and object-ID/effect alpha cutouts continue to use
  their existing material adapter and sampling contracts. The CPU light-depth
  shadow remains an approximation of Blender's stencil volume.
- Blender 5.2.2 describes its Workbench shadows as a stencil mask over opaque
  surfaces in pinned [`workbench_shadow.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_shadow.cc#L1111-L1123);
  `ShadowPass::object_sync` selects a forced-fail material pass for transparent
  materials but still submits their caps and shadow sides in pinned
  [`workbench_shadow.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_shadow.cc#L1720-L1790).
  The shadow volume has no Image Texture alpha sampling stage.
- A production QPainter contract gives an opaque-material caster a fully
  transparent Image Texture. The caster remains invisible in the color pass,
  but its geometry still darkens the opaque receiver. The former alpha-cutout
  expectation and exact 0.1 shadow-alpha boundary assertions were removed;
  the Workbench texture cutoff continues to apply to color and effect coverage.
- The transparent receiver contract now checks material alpha 0.5, 0.25, and
  0.0. The shadow outside the caster's visible footprint is unchanged at all
  three values, including a zero-alpha caster. Validation: `cmake --build
  build -j4` passed with the 317-file dependency audit; offscreen
  `viewport_render_contracts` passed (1/1, 27.27 seconds).

### QPainter NURBS-solid seed-face texture alpha contract (2026-10-14)

- The supported seed-face material path already selects a NURBS-solid face's
  `WorkbenchMaterialData` and matching tessellation UV range in both QPainter
  color rendering and effect-alpha coverage. Added a production-path regression
  that keeps the object material texture-free, assigns a packed fully transparent
  Image Texture to only the displaced cap by its stable face ID, and checks that
  the resulting color/outline image matches the receiver-only image. An
  object-level `NONE` alpha control confirms the same caster is visible when
  the cutout is bypassed. Materialized boundary faces still lack seed-face
  lineage and remain outside this contract.
- Blender 5.2.2 selects image inputs per material batch in pinned
  [`workbench_materials.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_materials.cc#L451-L542)
  and samples the active image alpha in pinned
  [`workbench_image_lib.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_image_lib.glsl#L416-L433).
  Validation: `cmake --build build -j4` passed with the 317-file dependency
  audit; offscreen `viewport_render_contracts` passed (1/1, 28.12 seconds).

## QPainter Workbench shadow focus and shift (2026-10-14)

- The QPainter shadow resolve now applies Blender's normal/light `smoothstep`
  using `shadow_shift`, clamped `shadow_focus`, and the mapped light direction.
  Shadow intensity mixes that lit-normal response with the shadowed/unshadowed
  result. Shift no longer changes the software light-depth bias. The fallback
  now scales its light-depth comparison tolerance from the caster depth range
  but caps each receiver comparison to two ULP at the local caster/receiver
  depths. The numerical contract checks the range helper at 1/1000, 1, and
  1000x scene scales and rejects non-finite bounds. Blender 5.2.2 uses stencil
  shadow volumes in
  [`workbench_shadow.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_shadow.cc#L1111-L1123),
  so this tolerance is a QPainter fallback robustness rule, not Blender bias
  parity.
- The source formula is from Blender 5.2.2
  [`workbench_shadow.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_shadow.cc#L280-L300)
  and [`workbench_world_light.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_world_light.bsl.hh#L677-L689).
  Contracts check focus response, fully shadowed and zero-intensity limits,
  and a rendered QPainter focus change. Matched planar-receiver comparisons
  cover Blender 5.2.2 defaults (`shadow_shift=0.1`, `shadow_focus=0.0`) and a
  joint alternate (`0.25`, `0.4`), with measurements recorded below. Separate
  one-variable sweeps and wider values remain open. The CPU light-depth caster
  and receiver coverage remain approximations.

## QPainter shadow beneath transparency (2026-10-14)

- When the software depth map identifies an opaque receiver behind a visible
  translucent surface, QPainter now applies the shadow factor to the retained
  opaque color contribution and attenuates that adjustment by the transparent
  layer's remaining coverage. The transparent color and final alpha are left
  unchanged. If retained surface layers are unavailable, this case keeps the
  existing unsplit-pixel fallback.
- A CPU contract compares a known shadowed receiver through a half-alpha blue
  layer with shadows enabled and disabled. It verifies equal RGB deltas from
  the underlying white contribution and unchanged final alpha. The software
  light-depth map, source-over approximation, and caster/receiver coverage
  remain distinct from Blender's stencil-volume path.

## QPainter direct translucent shadow receiver (2026-10-14)

- The software resolve now retains the nearest visible surface alpha and uses
  that surface's light-space position and normal to evaluate a shadow factor.
  It applies the factor to the retained transparent contribution while
  preserving final alpha; any opaque receiver beneath it is still shaded by
  its separate factor. Blender 5.2.2's
  [`workbench_transparent_accum_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_transparent_accum_frag.glsl#L478-L526)
  calls `get_shadow` for transparent fragments before weighted accumulation.
- The CPU contract uses a half-alpha blue receiver over a white opaque surface
  and verifies that a shadow changes the transparent contribution while
  preserving the composed alpha. QPainter retains a weighted aggregate of
  transparent layers, so the nearest visible fragment's factor is applied to
  that aggregate when several layers overlap. Exact per-layer receiver factors
  remain open; this remains a bounded CPU
  approximation to Blender's GPU shadow path.

## QPainter per-fragment transparent shadow factors (2026-10-14)

- The QPainter compositor now applies each transparent receiver's normal-based
  shadow factor while accumulating scene-linear weighted transparency in
  bounded per-pixel buffers. Regular and `showInFront` fragments keep separate
  resolves so their layering stays consistent with the existing compositor.
  The final alpha comes from the unshadowed resolve, and opaque receiver
  shadowing remains a separate contribution beneath transparency. This avoids
  retaining a scene-wide fragment list and keeps overlapping transparent
  layers with different normals shaded independently, including dense scenes.
- If a fragment lacks valid shadow-receiver metadata, the effects pass retains
  its nearest-visible-layer fallback. Material and geometry inputs remain the
  existing renderer contracts; the per-pixel shadowed resolve is an
  effects-pass adapter.
- Blender 5.2.2's
  [`workbench_transparent_accum_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_transparent_accum_frag.glsl#L478-L526)
  evaluates `get_shadow` on each transparent fragment before accumulating its
  weighted color and alpha. A CPU contract checks a shadowed rear receiver
  under a lit front receiver and separately checks that opaque-underlay
  shadowing remains visible through the stack while final alpha is unchanged.
  The software light-depth map, receiver coverage, and weighted resolve remain
  approximations to Blender's stencil-volume and GPU accumulation paths;
  matched Blender image comparisons remain open.

## QPainter transparent-material shadow casters (2026-10-14)

- QPainter adds transparent-material geometry to the software light-depth
  map. Its shadow remains binary; neither material alpha nor Image Texture
  alpha changes caster coverage. Color and effect passes keep their separate
  image-alpha cutouts.
- Blender 5.2.2 routes objects with transparent materials through
  `FORCED_FAIL`, then still draws the shadow-volume sides and caps in pinned
  [`workbench_shadow.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_shadow.cc#L1759-L1790).
  The stencil result is a binary receiver mask, and its shadow shader does not
  consume material alpha in
  [`workbench_shadow.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_shadow.bsl.hh#L55-L68).
- The CPU contract checks that a semi-transparent caster shadows a retained
  translucent receiver beyond the caster's camera-space footprint and that
  two nonzero material alpha values produce the same shadow contribution.
  QPainter still uses a bounded depth map rather than Blender's stencil
  volumes; broader geometry coverage and matched image comparison remain open.

## QPainter outline blend space (2026-10-14)

- Blender 5.2.2 draws the outline into the Workbench scene-color target with
  alpha-premultiplied blending before display color management. The GPU path
  also converts the outline color to scene-linear before drawing. QPainter's
  weighted compositor now retains its pre-outline scene-linear result and
  applies outlines there when shadows and DOF are inactive, including X-Ray
  where those effects are suppressed. Its outline ID
  selects the nearest visible fragment, including partial-alpha surface
  samples. With shadows enabled, the compositor retains the opaque and
  transparent linear contributions plus the outline coverage mask; the later
  effect pass shades the opaque contribution beneath transparency, applies the
  outline, then display-transforms the result once. The viewport separates this
  surface effect target from later QPainter wires and previews so they remain
  crisp and are not erased by the scene resolve.
- The production-path contract compares fractional outline edges over a
  nonuniform scene-linear background under Standard and AgX, verifies the
  pre-display result, exercises a partial-alpha surface, and confirms the
  effect pass does not apply the outline twice. A combined shadow-plus-outline
  production contract verifies the retained linear resolve under Standard and
  AgX. The C++ build and focused viewport contract pass.
- The same scene-linear route now covers weighted-compositor X-Ray scenes. The
  transparent and in-front fragment IDs feed the outline before the single
  display transform, and X-Ray does not defer outlines to shadow or DOF work
  that is suppressed in that mode. A production contract checks overlapping
  partial-alpha surfaces, retained linear output, Standard and AgX transforms,
  and no duplicate outline during the later effects call. Weighted
  transparency and edge coverage remain CPU approximations.
- The weighted compositor now also retains the float target through outline
  and camera DOF when shadows and X-Ray are off. It composites the outline,
  performs the existing bounded 49-tap aperture gather and 3x3 median on straight
  scene-linear RGB, preserves the sharp alpha, then display-transforms once.
  A retained-layer contract checks the combined path and output under Standard
  and AgX. The gather kernel, CoC approximation, and far-plane spill behavior
  remain the QPainter fallback's existing approximations.
- The weighted compositor now retains its scene-linear result through the
  combined shadow, outline, and camera DOF path. CPU shadows update the opaque
  contribution beneath transparency first, the outline blends into that
  linear result, and the bounded DOF gather runs before a single display
  transform. Production-path contracts verify the retained linear result and
  Standard and AgX output for the combined path. The gather kernel, CoC
  approximation, and far-plane spill behavior remain the QPainter fallback's
  existing approximations.
- The sorted fallback now retains its scene-linear result and resolves
  four-cardinal object-ID outline coverage before display conversion when the
  destination is transparent or its background was supplied in scene-linear
  form. With shadows enabled and DOF off, it retains the opaque and transparent
  linear contributions, applies the software shadow, then resolves the outline
  before display conversion. With camera DOF enabled and shadows off, it
  composites the outline before the existing bounded scene-linear gather and
  display conversion. Its nearest visible ID includes partial-alpha
  transparent fragments and in-front geometry. Production-path CPU contracts
  verify outline-only, shadow-plus-outline, DOF-plus-outline, and combined
  shadow-plus-outline-plus-DOF resolves under Standard and AgX, including no
  duplicate outline application. The sorted path defers display conversion
  through shadow, outline, and its camera-gated DOF gather when the background
  is known in scene-linear form or the destination starts transparent. Legacy
  paths without a trustworthy scene-linear destination still use the
  display-space outline fallback; already display-transformed pixels cannot
  generally be inverted under AgX, Filmic, or a custom LUT. The sorted shadow
  map and DOF gather remain the bounded QPainter approximations documented
  above.
- Blender 5.2.2 reference: [`workbench_effect_outline.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_effect_outline.cc#L329-L368)
  binds the scene color target, and
  [`workbench_effect_outline_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_effect_outline_frag.glsl#L252-L271)
  writes the outline color times the four-neighbor edge opacity. The pinned
  [`workbench_engine.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_engine.cc#L2635-L2645)
  dispatches the outline pass before DOF and anti-aliasing.

## QPainter mesh triangle edge coverage (2026-10-13)

- The QPainter Workbench mesh rasterizer now estimates coverage only near
  triangle boundaries with a fixed 4x4 subpixel grid. Covered subpixels also
  provide the edge pixel's perspective-correct interpolation weights, and
  fractional coverage is applied to the final alpha. Interior pixels keep the
  existing center-sample path. Edge samples are evaluated at destination
  physical-pixel centers after applying `QPainter::deviceTransform`; temporary
  rasters are tagged and placed at that physical-pixel scale.
- This is a bounded edge-coverage approximation for the software fallback; it
  does not reproduce Blender's TAA/SMAA filters or temporal accumulation.
  Blender 5.2.2 performs AA after scene passes in
  [`workbench_effect_antialiasing.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_effect_antialiasing.cc),
  while opaque mesh geometry is depth-tested in
  [`workbench_mesh_passes.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_mesh_passes.cc).
  Half-open top-left ownership now assigns exact shared-edge subpixel samples
  to one mesh triangle. Its contract checks that isolated transparent
  triangle renders account for exactly one total contribution on a shared
  diagonal. This does not combine neighboring triangles' fractional pixel
  coverage before source-over composition; that remains an approximation.
  A production-path CPU contract compares the exterior edge profile with a
  matching NURBS quad at DPR 1, 1.25, 1.5, and 2. The C++ build and focused
  `workbench_raster_fragment_contracts` test pass. Exact vertex fill rules are
  outside the current assertions.
- **Matched Blender 5.2.2 mesh edge capture (2026-10-14):** An opt-in
  production `ViewportRenderer` capture now renders the same two-triangle
  planar quad as the Blender `MESH_TEXTURE` fixture, with a constant 2x2
  `(180, 90, 40, 255)` Non-Color image, Linear filtering, material alpha 0.5, Flat lighting,
  transparent film, and AA Off at 160x160. With the matched top orthographic
  view (`ortho_scale=2.6666666667`), both images cover 14,400 pixels (IoU
  `1.0`, XOR `0`). The projected shared diagonal and its 3x3 neighborhood
  have alpha `128/255` in both; RGB differs by at most two display codes in
  that neighborhood. Across shared pixels, alpha MAE is `3.18/255` and max
  error is `120/255`, localized to QPainter's fractional outer-edge estimate
  versus Blender's AA-Off coverage. QPainter's partial-edge RGB also differs,
  so the alpha silhouette and stable diagonal probe are the meaningful
  metrics for this capture. Reproduce classiCAD with
  `CLASSICAD_VIEWPORT_QPAINTER_MESH_SHARED_EDGE_CAPTURE=/tmp/classicad-mesh-shared-edge.png
  QT_QPA_PLATFORM=offscreen ctest --test-dir build -R
  '^viewport_render_contracts$' --output-on-failure`; render Blender 5.2.2
  using `tests/workbench_cube_reference.py --scene MESH_TEXTURE --resolution
  160 160 --ortho-scale 2.6666666667 --view-direction 0 0 1 --aa OFF
  --lighting FLAT --color-source TEXTURE --texture-image
  /tmp/mesh-edge-half-alpha.png --texture-color-space Non-Color
  --texture-interpolation Linear --transparent-film --zero-dither
  --material-alpha 0.5`, then compare with
  `tests/workbench_shared_alpha_edge_compare.py --center 80 80 --radius 1`.
  The fixture checks the half-alpha shared-diagonal contribution; outer-edge
  coverage remains an approximation.

## QPainter NURBS triangle edge coverage (2026-10-13)

- **DPR footprint correction (2026-10-14):** A call-site audit found that the
  mesh raster sampled target physical pixels while the NURBS raster still
  sampled logical pixels. At DPR above 1 this made NURBS edge coverage span
  multiple physical pixels. Both paths now transform projected vertices with
  `QPainter::deviceTransform`, rasterize and place their temporary images in
  physical-pixel space, and map fragment observations back to logical
  coordinates. The planar NURBS resolve uses the same target-pixel dimensions
  under its existing allocation bound.
- The QPainter tessellated NURBS surface and solid face raster paths now use
  the same bounded 4x4 physical-pixel edge coverage estimate as polygon meshes.
  The subpixel-averaged barycentric weights feed textured UV sampling and
  perspective shading, and fractional coverage reaches both the color alpha
  and the software fragment sink. Planar untextured coverage resolve also
  accumulates at physical-pixel resolution under the existing one-million
  pixel bound. The resolver also covers perspective views after verifying
  that all tessellation vertices remain on the shared plane; it keeps the
  orthographic depth-uniformity guard. Image alpha cutoff behavior is
  unchanged.
- This makes the NURBS software edges use the same approximation as mesh
  edges; it does not reproduce Blender's TAA/SMAA temporal and filter stages.
  Blender 5.2.2 AA remains a later pass in
  [`workbench_effect_antialiasing.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_effect_antialiasing.cc).
  The production-path edge-profile contract passes at DPR 1, 1.25, 1.5, and 2;
  the full C++ build passes. It excludes exact corners and the mesh's internal
  triangulation seam, whose shared-edge fill rules differ. A controlled
  perspective Blender comparison is recorded below; orthographic coverage
  comparison remains open.
- **Perspective planar NURBS shared-edge comparison (2026-10-14):** Enabled
  the bounded QPainter coverage resolver for perspective views after checking
  that all tessellation vertices are coplanar with the shared triangle
  normal. The 160×160 matched Blender 5.2.2 and classiCAD images use a
  perspective camera, Flat lighting, AA Off, a constant 2×2 Non-Color image,
  and material alpha 0.5. Coverage IoU is `0.952081` (114 XOR pixels; the
  classiCAD silhouette is two pixels wider overall). On shared covered pixels,
  RGBA MAE is `[0.989, 0.709, 0.706, 1.194]`; the 3×3 projected origin
  neighborhood matches Blender's alpha 128 exactly, with RGB at most one code
  value apart. The remaining alpha maximum of 64 is confined to silhouette
  edges. The focused `viewport_render_contracts` test and full C++ build pass.
  Reproduce the classiCAD capture with
  `CLASSICAD_VIEWPORT_QPAINTER_NURBS_PERSPECTIVE_CAPTURE=/tmp/classicad-nurbs-perspective-alpha.png`
  on the offscreen test, render Blender using
  `--scene NURBS_TEXTURE --resolution 160 160 --aa OFF --lighting FLAT
  --color-source TEXTURE --texture-image /tmp/nurbs-perspective-alpha.png
  --texture-color-space Non-Color --texture-interpolation Linear
  --match-classicad-perspective --view-direction 4 -6 4 --transparent-film
  --zero-dither --material-alpha 0.5`, then compare with
  `tests/workbench_shared_alpha_edge_compare.py`. This validates the
  planar perspective aggregation case only; other camera scales, curved or
  folded NURBS, and orthographic edge parity remain open.

## QPainter shadow material opacity classification (2026-10-14)

- The QPainter Solid fallback now classifies opaque Workbench material alpha
  after Blender's UNorm8 packing step. Alpha `254/255` stays out of the opaque
  receiver buffer, while `0.9985` rounds to `255/255` and uses the opaque
  receiver path. The later transparent-caster port adds transparent geometry
  to the binary shadow map independently of that receiver classification.
  The focused viewport contracts cover the opaque boundary and transparent
  caster path.
- Blender packs alpha with `unit_float_to_uchar_clamp` and considers material
  alpha opaque only when the packed byte is 255 in pinned
  [`workbench_private.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_private.hh#L1599-L1622)
  and [`math_base_inline.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/blenlib/intern/math_base_inline.cc#L2362-L2368).
  Material packing still determines whether receiver samples enter the opaque
  buffer; Blender's separate shadow-volume dispatch handles transparent
  casters without scaling their shadow by material alpha.

## QPainter transparent alpha classification (2026-10-14)

- The QPainter weighted-transparency compositor now puts only fully opaque
  byte-alpha pixels into its opaque depth/color layer. Byte alpha 254 remains
  in the transparent path, so a rear opaque surface contributes through a
  front material whose Blender-packed alpha is `254/255`. A focused production
  compositor contract covers this overlap.
- Polygon meshes now UNorm8-pack the Workbench material/source alpha before
  X-Ray modulation and the 4x4 software edge-coverage multiplier, matching the
  QPainter NURBS and GPU material paths. The fragment contract checks a
  non-byte-aligned alpha, values on both sides of a half-byte boundary, and
  that X-Ray and fractional edge coverage remain applied after packing.
  Texture image alpha keeps Blender's sampled-alpha cutoff behavior and is not
  quantized as material alpha.
- Blender 5.2.2 passes interpolated material alpha directly to transparent
  accumulation and revealage in
  [`workbench_transparent_accum_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_transparent_accum_frag.glsl#L518-L526),
  then resolves the accumulated color/revealage in
  [`workbench_transparent_resolve_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_transparent_resolve_frag.glsl#L269-L286).
  Blender's material datablock packs alpha using
  [`workbench_private.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_private.hh#L1599-L1622).
  The classiCAD compositor remains a CPU approximation of that pass.

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
  metadata. The saved Blender 5.2.2 fixture also exercises the imported
  surface through the production viewport frame and both rendering paths in
  Material mode. Blender's active image sampling and UDIM lookup are
  documented by pinned
  [`workbench_image_lib.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_image_lib.glsl#L348-L433).
- **NURBS Surface material scalar extraction (2026-10-14):** the Blender
  5.2.2 extractor contract now checks the assigned material record's name,
  scene-linear diffuse RGBA, roughness `0.27`, metallic `0.41`, and derived
  alpha `0.6`, in addition to its packed image metadata. The fixture now uses
  Blender's valid 4×4 NURBS-surface primitive; its previous empty spline did
  not satisfy the API's read-only control-count fields and stopped before
  exercising extraction. This verifies Blender-record extraction alongside
  the existing C++ `WorkbenchMaterialData` scalar projection contract. Blender's
  supported Workbench material resolution is pinned in
  [`workbench_materials.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_materials.cc#L19-L37)
  and
  [`workbench_materials.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_materials.cc#L538-L553).
- **End-to-end imported NURBS Surface material rendering (2026-10-14):** a
  saved ordinary Blender 5.2.2 `.blend` containing a valid 4×4 NURBS Surface,
  an assigned material, and a packed PREMUL image now passes through
  `importBlenderScene`, `Document`, `ViewportRenderFrame`, and both the
  QPainter and GPU render paths. The contract checks imported material
  scalars and packed image/sampling metadata. Captures use Studio/Material,
  AA Off, at 640×480; the image is verified as imported metadata, not sampled
  for this comparison. Blender's evaluated surface normal is `-Z`, while
  classiCAD's native U/V tessellation produces `+Z`; imported Blender surfaces
  now carry an explicit render-frame orientation flag so prepared depth and
  shading geometry match Blender's winding. GPU scene-linear RGB MAE is
  `[6.64e-8, 1.12e-7, 1.60e-7]`, p95 and p99 are zero, with a sparse maximum
  `0.01227`. Display foreground IoU is `0.999497` (4 XOR pixels); common
  foreground display MAE is `0.001248` on a 0–1 channel scale (p95 `1/255`).
  QPainter is included in the paired display comparison. Texture-mode sampling
  is compared separately below; broader camera/material coverage remains
  open. Blender behavior is pinned in
  [`workbench_materials.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_materials.cc#L538-L553),
  [`workbench_image_lib.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_image_lib.glsl#L348-L433),
  and
  [`workbench_prepass.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_prepass.bsl.hh#L100-L105).
- **End-to-end imported NURBS Surface Texture sampling (2026-10-14):** the
  same packed-image `.blend` fixture is rendered in Texture mode by Blender
  5.2.2 and imported through classiCAD's production frame into both GPU and
  QPainter paths. The fixture uses the assigned PREMUL image and generated
  active UV map; a headless Blender inspection confirmed the evaluated map
  spans 0–1, matching classiCAD's exact active-knot-domain normalization for
  this surface. The capture contract probes two different image regions in
  both backends so a flat material-color fallback fails. At 640×480, both
  classiCAD paths have foreground IoU `0.999497` (4 XOR pixels). GPU common
  foreground display MAE is `1.61/255` (p95 `8/255`, max `10/255`); QPainter
  MAE is `1.91/255` with the same p95 and maximum. GPU scene-linear full-frame
  RGB MAE is `[6.16e-5, 3.85e-5, 3.05e-5]`, with p95 `0`, p99 `0.001221`, and
  sparse max `0.010437`. This verifies one imported, uniform-domain surface
  and one packed PREMUL texture setup. Multi-span/custom UV coverage, other
  sampling filters and alpha modes, trimming, and broader camera/settings
  comparisons remain open. Blender's UV varying and image sampling are pinned
  in
  [`workbench_prepass.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_prepass.bsl.hh#L100-L105)
  and
  [`workbench_image_lib.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_image_lib.glsl#L348-L433).
- **Imported multi-span NURBS Surface texture UVs (2026-10-15):** the
  Blender fixture subdivides its valid 4×4 Surface control net to 7×7, keeping
  order 4 and producing four active uniform knot spans in each direction. The
  extractor and C++ viewport contract verify the multi-span knot data reaches
  the render frame. Blender 5.2.2's evaluated UVMap has 0–1 bounds on both
  axes; classiCAD's normalized active-domain parameter UVs match that generated
  mapping. Texture-mode captures compare the actual imported surface at
  640×480: GPU foreground IoU is `1.0` (no XOR pixels), QPainter IoU is
  `0.999937` (2 XOR pixels). GPU display MAE is `1.61/255` (p95 `8/255`, max
  `9/255`); QPainter MAE is `1.91/255` (p95 `8/255`, max `12/255`). GPU
  scene-linear full-frame RGB MAE is `[2.46e-4, 1.54e-4, 1.22e-4]`, with p95
  `0.000488`, p99 `0.005005`, and max `0.009277`. This verifies one ordinary
  four-span uniform Surface UV layout and the imported packed-texture path;
  other generated knot modes, alpha/filter settings, cyclic seams, and broader
  camera coverage remain open. Recreate the fixture with
  `CLASSICAD_SURFACE_MATERIAL_MULTI_SPAN=1 CLASSICAD_SURFACE_MATERIAL_COLOR_MODE=TEXTURE CLASSICAD_SURFACE_MATERIAL_BLEND_OUTPUT=<blend-path> blender --background --factory-startup --python tests/blender_surface_material_import_contract.py`; capture with matching `CLASSICAD_VIEWPORT_IMPORTED_SURFACE_MULTI_SPAN=1`, `..._COLOR_MODE=Texture`, and `..._BLEND=<blend-path>` environment values plus offscreen `ctest`. Blender's generated UV varying and sampling behavior is pinned in
  [`workbench_prepass.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_prepass.bsl.hh#L100-L105)
  and
  [`workbench_image_lib.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_image_lib.glsl#L348-L433).
- **Imported NURBS Surface image alpha cutoff (2026-10-15):** a separate
  Blender fixture now packs a real RGBA PNG with STRAIGHT alpha, zero alpha on
  one UV half and opaque texels on the other. The fixture verifies its packed
  PNG retains RGBA channels, then the saved `.blend` passes through the
  importer, viewport frame, GPU, and QPainter Texture paths. Both backends
  show the transparent probe as background and the opaque probe as textured
  surface. At 640×480 Blender has 3,976 foreground pixels; classiCAD has
  3,974 in each backend, with mask IoU `0.999497` (2 XOR edge pixels). GPU
  common-foreground display MAE is `0.283/255`, QPainter is `0.363/255`, each
  with p95 `1/255` and sparse max `8/255`. GPU scene-linear full-frame RGB MAE
  is `[6.57e-8, 2.25e-8, 3.13e-8]`, with p95 and p99 zero and sparse max
  `0.008255`. This confirms imported STRAIGHT alpha reaches the Blender-style
  `0.1` fragment cutoff in both backends; shadow-caster alpha is outside this
  image-color comparison. Other imported alpha modes and transparency
  combinations remain open. Blender's cutoff exception flags and shader
  discard are pinned in
  [`workbench_materials.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_materials.cc#L536-L553)
  and
  [`workbench_image_lib.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_image_lib.glsl#L416-L429).
- **End-to-end imported mesh material rendering (2026-10-14):** the reference
  fixture can save its configured material-slot scene as an ordinary Blender
  `.blend`. A headless acceptance path imports that file through
  `importBlenderScene`, stores its material records and polygon mesh in a
  `Document`, builds the production viewport frame, and captures the QPainter
  renderer. The integration exposed Blender LTS's source label `5.2.2 LTS`;
  the importer now accepts that label only with the exact `(5, 2, 2)` version
  tuple and continues to preserve the source label in imported metadata. The
  saved fixture verifies three imported slots' scene-linear colors, roughness,
  and metallic before image capture. At 640×480, the Studio/Material/AA-Off
  PNG comparison has foreground-mask IoU `0.997991` (224 XOR pixels), common
  foreground display MAE `0.09690/255` (p95 `1/255`); the top-down-corrected
  scene-linear PFM foreground IoU is `0.997164`, with full-frame RGB MAE
  `0.0000991`, `0.0000239`, and `0.0000288`. Sparse high-difference pixels and
  broader camera/material/alpha coverage remain open; this is a close matched
  capture, not exact pixel parity. Reproduce by adding `--save-blend
  /tmp/material-slots.blend` to the Blender fixture command, then setting
  `CLASSICAD_VIEWPORT_IMPORTED_MATERIAL_BLEND` to that file and
  `CLASSICAD_VIEWPORT_IMPORTED_MATERIAL_CAPTURE_DIR` to a capture directory
  when running `viewport_render_contracts` offscreen. Blender 5.2.2 material
  resolution remains pinned in
  [`workbench_materials.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_materials.cc#L538-L553).
- **End-to-end imported mesh material GPU rendering (2026-10-14):** the
  same saved three-slot Blender `.blend` now also passes through the production
  GPU scene-pass coordinator after import, Document insertion, and viewport
  frame construction. Studio/Material captures compare at 640×480. Foreground
  mask IoU is `0.997991` with 224 XOR edge pixels; common-foreground display
  MAE is `0.000607` (p95 `1/255`). Scene-linear PFM face probes for the top,
  front, and right slots match Blender exactly. Full-frame scene-linear RGB
  MAE is `0.000153`, `0.0000187`, and `0.0000203`, with p95 and p99 zero and
  sparse max error `0.24663`. This establishes that imported slot IDs and
  material payloads reach GPU `DrawRange` rendering; broader material values,
  views, and transparent slots remain open. Blender 5.2.2 material resolution
  and payload packing are pinned in
  [`workbench_materials.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_materials.cc#L538-L553).
- **Polygon-mesh frame material adapter convergence (2026-10-14):**
  ID-backed mesh slots in `buildViewportRenderFrame` now use
  `workbenchMaterialDataFromDocumentMaterial`, matching NURBS-solid seed
  materials and Blender import. This removes a second field/image-precedence
  projection while preserving the legacy inline `PolygonMeshMaterialSlot`
  path. The frame-builder contract verifies scene-linear base color,
  roughness, metallic, alpha, packed bytes, float payload metadata, alpha mode,
  interpolation, and extension survive the shared adapter. Geometry and
  `DrawRange` contracts are unchanged. Blender's supported material inputs
  remain pinned in
  [`workbench_materials.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_materials.cc#L538-L553).
- Validation: `cmake --build build -j4` passed with the dependency audit
  checking 317 source files, and offscreen `viewport_render_contracts` passed
  (1/1).
- **Canonical material adapter projection:** Blender import and immutable
  viewport-frame construction now share
  `workbenchMaterialDataFromDocumentMaterial` in
  `src/core/document/workbench_material_adapter.h`. This keeps scalar values,
  color-space identifiers, float payload dimensions, alpha/sampling settings,
  and first-usable image-source precedence in one projection. The material
  resolution contract covers scalar and metadata copying plus UDIM, packed,
  external-path, and unavailable-image ordering. This changes only transport
  into the existing `WorkbenchMaterialData` seam; it does not change geometry,
  material interpretation, or shader behavior.
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
  existing OpenGL-only NURBS-solid check. A later offscreen run of
  `viewport_render_contracts` creates an OpenGL context and passes; this
  validates its covered GPU paths but is not a broad GPU visual comparison.

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

### Render-result JPEG quality zero (2026-10-13)

- Blender image quality accepts the full 0–100 range. The Qt JPEG writer path
  previously treated zero as an unset value and substituted quality 75; it now
  clamps only out-of-range values and preserves zero. A format contract saves
  the same deterministic noisy image at qualities 0 and 75 and verifies the
  encoded outputs differ, guarding against the old fallback.
- `cmake --build build -j4` completed, the focused
  `workbench_render_result_format_contracts` CTest passed, and
  `git diff --check` passed. Blender documents the range in its pinned
  [ImageFormatSettings API](https://docs.blender.org/api/5.2/bpy.types.ImageFormatSettings.html#bpy.types.ImageFormatSettings.quality).

### TIFF render-result compression codecs (2026-10-13)

- Blender's TIFF `NONE`, `LZW`, `PACKBITS`, and `DEFLATE` codecs map to TIFF
  compression tags 1, 5, 32773, and 8. Qt's image writer exposes only NONE or
  LZW for TIFF, so an optional libtiff writer now preserves all four codecs,
  8/16-bit color channels, render metadata, and pixel-aspect resolution tags.
  Builds without libtiff retain exact NONE/LZW output and reject DEFLATE or
  PACKBITS with a clear error rather than writing a different codec.
- The format contract checks the exact TIFF compression tag and pixel-aspect
  tags with libtiff when available; the Qt-only contract checks that unsupported
  codecs fail without leaving an output file. The local `cmake --build build
  -j2` and focused format contract passed with the feature-off path. This
  environment has the libtiff runtime but lacks development headers, so the
  feature-on implementation could not be compiled or exercised locally.
- Blender's mapping is pinned in [`image_format.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/blenkernel/intern/image_format.cc#L782-L798)
  and its TIFF tags in [`format_tiff.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/imbuf/intern/format_tiff.cc#L55-L82).
  Qt 6.4.2's TIFF writer maps its limited compression option in
  [`qtiffhandler.cpp`](https://github.com/qt/qtimageformats/blob/v6.4.2/src/plugins/imageformats/tiff/qtiffhandler.cpp#L160-L168).

### CAD curve centerline pass classification (2026-10-13)

- classiCAD's current viewport represents editable NURBS curves as centerline
  wires, so they map to Blender's Edit Mode curve overlay path. They remain
  post-resolve and use the unjittered camera. Their current visible-wire depth
  behavior stays in the existing overlay draw; filled meshes and surfaces
  continue through scene AA/DOF.
- Blender's evaluated Curves scene batches are a distinct path. Workbench's
  `curves_sync` submits evaluated curve geometry through `draw_to_mesh_pass`,
  while Overlay's `edit_object_sync_legacy` submits editable curve edge wires
  to `edit_legacy_curve_ps_` and draws them in the overlay line pass. The
  classiCAD centerline renderer corresponds to the latter. It must not be moved
  into scene resolve unless classiCAD adds evaluated curve scene geometry with
  a separate representation.
- A tentative implementation that sampled every committed CAD centerline was
  reverted after this source distinction was confirmed. No GPU visual result
  was claimed for that experiment. The plan's final Smooth Wires and grid
  comparison remains open; current contracts validate grid AA in isolation.
- Pinned Blender sources: [Workbench evaluated curves](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_engine.cc#L429-L439), [Overlay edit curve wires](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/overlay/overlay_curve.hh#L221-L269).

### Blender proxy material active Image Texture export (2026-10-14)

- The `.blend` proxy adapter reconstructs captured active `TEX_IMAGE` and
  `TEX_ENVIRONMENT` nodes when their packed image payload is within the
  existing 32 MiB cap, or their bounded existing file is available. It
  preserves image alpha mode, color space, interpolation, and Image Texture
  extension; Environment Texture keeps Blender's default extension behavior.
  Packed source bytes are embedded so the proxy remains self-contained. The
  document model retains the active node type and defaults older snapshots to
  `TEX_IMAGE`. Repeated proxy synchronization reuses an unchanged managed node
  and image, avoiding duplicate datablocks and temporary image files.
- The proxy contract checks Workbench `show_texture` selection, regular and
  Environment Texture nodes, packed bytes, existing file paths, sampler
  metadata, repeated synchronization, and `.blend` save/reopen. It does not
  verify rendered pixel parity. Scene-linear float payload reconstruction and
  unsupported or missing image sources remain open.
  Arbitrary shader graphs and environment projection are not reconstructed;
  the exact imported source snapshot remains the authoritative classiCAD data.
- Validation: the Blender 5.2.2 proxy contract passed through `.blend`
  save/reopen and repeated sync; `cmake --build build -j4` and the dependency audit passed, as
  did `blender_surface_material_resolution_contracts`. The broader
  `core_contracts` CTest exits with viewport zoom/navigation and tool
  interaction failures outside this material-proxy change; its new native
  image-reference assertion emitted no failure diagnostic.
- Blender 5.2.2's Workbench material adapter resolves the active image node,
  handles both image-node types, reads Image Texture extension and both nodes'
  interpolation and image alpha mode in
  [`workbench_materials.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_materials.cc#L451-L542).

### Blender proxy parametric UV export (2026-10-14)

- Generated `.blend` display meshes for NURBS surfaces and solids now carry a
  `classiCAD.ParametricUV` layer. The C++ exporter derives per-corner UVs from
  each tessellated face's parameter values normalized by that face's active
  knot domain. It follows reversed mesh winding when emitting UV corners, and
  grouped proxies preserve each face's own UV island. The Python adapter also
  creates matching UVs in its untrimmed NURBS fallback tessellation.
- The Blender 5.2.2 proxy contract checks active/render UV selection, corner
  values, seams, and grouped surfaces with distinct parameter domains. This
  verifies UV transport through the adapter; it does not compare rendered
  texture pixels against Blender. Existing per-face seams remain, and no
  editable global unwrap is produced.
- Blender 5.2.2 Workbench passes mesh UV attributes through its prepass and
  samples those coordinates in the image shader:
  [`workbench_prepass.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_prepass.bsl.hh#L110-L149)
  and
  [`workbench_image_lib.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_image_lib.glsl#L34-L60).
- Validation: `blender_project_proxy_contract.py` passed under Blender 5.2.2;
  `cmake --build build -j4` and the dependency audit passed.

### Blender proxy UDIM Image Texture export (2026-10-14)

- The proxy adapter now reconstructs active `TEX_IMAGE` references with a
  `<UDIM>` path and unique tile numbers from 1001 through 1999. It accepts
  packed tile bytes or existing external tile files, checks the aggregate
  encoded payload against the 32 MiB per-image limit, and packs the rebuilt
  tile set into the `.blend` proxy. Blender 5.2.2's `Image.tiles` and
  `Image.packed_files` preserve each tile's number and bytes through save and
  reopen. Repeated material synchronization reuses the existing proxy node
  and image. Active Environment Texture UDIMs are covered in the following
  category.
- The Blender 5.2.2 contract checks two distinct packed tiles and two external
  tiles, Workbench-active node state, interpolation and extension, idempotent
  synchronization, and exact tile bytes after save/reopen after deleting the
  temporary source tiles. It does not render a visual comparison. Per-tile
  scene-linear float payload reconstruction is covered in the following
  category; malformed or unavailable tiles and sources outside the bounded
  path and size rules remain unsupported.
- Blender 5.2.2 Workbench resolves tiled images from mesh UVs in
  [`workbench_image_lib.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_image_lib.glsl#L34-L60).
- Validation: `blender_project_proxy_contract.py` passed under Blender 5.2.2;
  `cmake --build build -j4`, the dependency audit, and
  `blender_surface_material_resolution_contracts` passed.

### Blender proxy file-backed float Image Texture export (2026-10-14)

- The `.blend` proxy adapter now preserves active file-backed float image
  references even when extraction also supplies a non-identity scene-linear
  transform ID and RGBA32F payload. It loads the original packed or existing
  source image and preserves its Blender colorspace and alpha mode, letting
  Blender's own float image path provide the proxy texels. Packed OpenEXR is
  recognized by its file signature when the source path has no extension and
  remains embedded in the saved proxy.
- The Blender 5.2.2 proxy contract creates a packed RGBA OpenEXR with negative
  and above-one RGB, zero/fractional/unit alpha, a `blender-float-buffer`
  transform ID, and a scene-linear payload. It checks float pixels, sampler
  settings, colorspace, alpha mode, packed bytes, and `.blend` reopen after
  deleting the source EXR. This path uses original file bytes rather than
  reconstructing texels from the float payload. Source-less references can
  use the bounded payload materialization described below.
- Validation: `blender_project_proxy_contract.py` passed under Blender 5.2.2;
  `cmake --build build -j4` passed with the dependency audit checking 314
  source files. Rendered pixel parity was not compared.

### Blender proxy scene-linear float-payload materialization (2026-10-14)

- Active non-tiled image references with no usable packed or external source
  can now materialize their bounded scene-linear RGBA32F payload as a packed
  OpenEXR image. The adapter checks the declared dimensions, exact payload
  length, finite components, and the existing 32 MiB image limit. It writes
  the sampler-ready channels without another color transform or PREMUL
  multiply, restores the captured alpha mode after loading the EXR, and stores
  the source colorspace and alpha metadata on the managed image. Blender 5.2.2
  Workbench's image sampling remains the shader reference in
  [`workbench_image_lib.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_image_lib.glsl#L34-L60).
- The Blender proxy contract covers source-less 2×1 payloads for STRAIGHT,
  PREMUL, NONE, and CHANNEL_PACKED modes, including negative and above-one
  values and fractional alpha. It verifies exact float texels, restored mode,
  active node and sampler state, packed data, repeated synchronization, and
  save/reopen after removing the temporary EXR sources. This checks stored
  sampling data and mode transport; a matched rendered-pixel comparison is
  still open. Tiled per-tile float payloads are tracked in the following
  category; payloads that fail the declared bounds remain unsupported.
- Validation: `blender_project_proxy_contract.py` passed under Blender 5.2.2;
  `cmake --build build -j4` and the 314-file dependency audit passed.

### Blender proxy UDIM scene-linear float tiles (2026-10-14)

- Tiled Image Texture proxies now materialize per-tile scene-linear RGBA32F
  payloads as packed EXR tiles when every tile has a valid payload. All tiles
  are normalized into one scene-linear sampler domain, then share the source
  image's captured alpha mode; the original colorspace and alpha metadata are
  retained on the managed image. If payload coverage is partial, the adapter
  only proceeds when all tiles can instead be rebuilt from source files. It
  does not combine scene-linear and unconverted source tiles into one image,
  since Blender 5.2.2 assigns one colorspace and alpha mode per Image.
- The proxy contract checks two float tiles with distinct negative/HDR texels,
  zero/fractional/unit alpha, PREMUL mode, Non-Color proxy sampling, EXR
  signatures, tile-number associations, idempotent sync, and exact packed
  bytes after saving/reopening with temporary tile sources deleted. Blender's
  packed UDIM implementation serializes/restores each tile number and packed
  payload in pinned [`image.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/blenkernel/intern/image.cc#L2873-L2942).
  The contract verifies stored tile data and metadata, not rendered texture
  pixels. Active Environment Texture UDIMs are covered in the following
  category.
- Validation: `blender_project_proxy_contract.py` passed under Blender 5.2.2;
  `cmake --build build -j4` and the 314-file dependency audit passed.

### Blender proxy UDIM Environment Texture export (2026-10-14)

- Active `TEX_ENVIRONMENT` references now use the same bounded tiled-image
  reconstruction as `TEX_IMAGE`, including packed tile bytes, external tile
  sources, and complete scene-linear float payload sets. The proxy keeps the
  Environment Texture node type and interpolation setting; Image Texture
  extension settings are not applied to Environment Texture nodes. Per-image
  tile numbers and the 32 MiB aggregate limit remain enforced.
- Blender 5.2.2 Workbench resolves the active Environment Texture node and
  image in [`workbench_materials.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_materials.cc#L451-L542).
  Both node types use the captured interpolation for sampler filtering. The
  Workbench image shader looks up tiled images from mesh UV coordinates in
  [`workbench_image_lib.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_image_lib.glsl#L348-L414);
  it samples the resulting image UVs directly, so the Environment Texture
  node's general-purpose projection settings do not alter this Workbench path.
- The Blender 5.2.2 proxy contract now checks packed, external, and scene-linear
  float UDIM Environment Textures, active-node state, interpolation, tile
  numbers, packed bytes, idempotent sync, and save/reopen after deleting the
  temporary tile sources. These are storage and adapter contracts; rendered
  pixel parity remains open.
- Validation: `blender_project_proxy_contract.py` passed under Blender 5.2.2.

### Scene-linear CAD annotation overlap in HDR/EXR output (2026-10-14)

- The HDR/EXR render-result path now draws committed CAD dimensions into a
  second RGBA64 premultiplied layer tagged as linear sRGB. Dimension stroke,
  text, and label-background colors are converted from sRGB before rasterizing
  to that target, so antialias coverage and overlapping annotations blend in
  scene-linear RGB. The existing sRGB annotation layer still produces the
  display result; PNG/TIFF and other display-referred output behavior is
  unchanged. The material adapter and geometry contracts are unaffected.
- Blender 5.2.2 registers a four-channel Combined render pass and reads the
  Workbench render framebuffer into its float pass buffer in
  [`workbench_engine.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_engine.cc#L2888-L2919)
  and
  [`workbench_engine.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_engine.cc#L3097-L3109).
  The separate CAD annotation layer remains classiCAD-specific; this change
  preserves the Workbench scene-linear Combined buffer while compositing those
  extra annotations into it.
- The offscreen contract checks two crossing antialiased strokes with distinct
  colors against explicit premultiplied linear source-over results, preserves
  above-one scene-linear values and output alpha, and verifies that display
  output remains the existing sRGB composition. Validation: the full
  `cmake --build build -j4` passed (including the dependency audit), and
  `QT_QPA_PLATFORM=offscreen ./build/classicad_viewport_render_contracts_test`
  passed. Matched rendered comparisons of CAD annotation output against
  Blender remain outside the Blender Workbench scene pass.

### QPainter polygon-mesh opaque backface normals (2026-10-14)

- QPainter polygon-mesh fragments now determine front-facing state from the
  projected triangle winding, corrected for reflected mesh transforms. The
  Studio/MatCap lighting path flips interpolated normals for fully opaque
  backfaces. Partially transparent fragments keep their imported/interpolated
  normal orientation, matching the separate Blender prepass and transparent
  shader behavior. Mesh color/material data and geometry contracts are
  unchanged.
- Blender 5.2.2 passes `[[front_facing]]` into opaque normal encoding in
  [`workbench_prepass.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_prepass.bsl.hh#L306-L314),
  where [`workbench_common.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_common.bsl.hh#L35-L43)
  flips backface normals. The transparent shader uses interpolated normals
  directly in
  [`workbench_prepass.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_prepass.bsl.hh#L332-L345).
- Production-path QPainter contracts render a tilted mesh and its reversed
  winding with reversed corner normals: opaque center colors must match,
  transparent colors must retain the source-normal difference, and mean
  opacity must remain equal. Validation: `cmake --build build -j4` passed,
  including the source dependency audit, and
  `QT_QPA_PLATFORM=offscreen ./build/classicad_viewport_render_contracts_test`
  passed. The build reports the existing `qfloat16` memcpy warning in
  `workbench_lighting.h`; no matched Blender frame comparison was run.

### QPainter prepared NURBS smooth normals (2026-10-14)

- The untextured QPainter NURBS raster now interpolates its three prepared
  corner normals at each covered sample, using the same perspective-correct
  weights as position reconstruction, then normalizes before Studio/MatCap
  lighting. Missing or invalid prepared normals keep the flat geometric
  fallback. Textured NURBS and generated tessellation fallback behavior are
  unchanged by this category.
- Blender 5.2.2's Workbench prepass passes interpolated normal data to the
  fragment stage in
  [`workbench_prepass.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_prepass.bsl.hh#L306-L314).
- A production-path CPU contract supplies a planar NURBS triangle with
  varying smooth normals and confirms Studio shading varies across the face;
  the matching flat-normal fixture remains uniform. Validation:
  `cmake --build build -j4` passed with the dependency audit, and
  `QT_QPA_PLATFORM=offscreen ./build/classicad_viewport_render_contracts_test`
  passed. The existing `qfloat16` memcpy compiler warning remains; no matched
  Blender frame comparison was run.

### QPainter textured prepared NURBS smooth normals (2026-10-14)

- Textured prepared QPainter NURBS triangles now interpolate their three
  prepared corner normals using the same perspective-correct weights as UV and
  position reconstruction. The normalized result drives Studio/MatCap shading
  and the shadow receiver normal. Missing or invalid prepared normals fall
  back to the flat triangle normal; generated tessellation without prepared
  corner normals remains flat. Texture sampling, alpha, and geometry contracts
  are unchanged.
- Blender 5.2.2's pinned
  [`workbench_prepass.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_prepass.bsl.hh#L306-L314)
  passes interpolated normal data to the fragment stage.
- The production-path contract renders a textured prepared NURBS triangle in
  Studio mode with varying prepared normals and compares it with a flat-normal
  fixture: the former varies across the face while the latter stays uniform.
  Validation: `cmake --build build -j4` passed with the dependency audit, and
  offscreen `viewport_render_contracts` plus
  `workbench_raster_fragment_contracts` passed. Visual parity remains
  unverified.

### QPainter shared-edge subpixel ownership (2026-10-14)

- The shared triangle coverage helper now uses a winding-normalized half-open
  top-left edge rule for its 4x4 subpixel samples. Exact samples on an internal
  edge are assigned to one triangle, preventing duplicate per-triangle color
  and fragment coverage at that boundary. The helper serves mesh and NURBS
  raster paths.
- Blender 5.2.2 submits opaque and transparent mesh geometry through its GPU
  mesh passes in
  [`workbench_mesh_passes.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_mesh_passes.cc#L109-L201)
  and applies anti-aliasing afterward in
  [`workbench_effect_antialiasing.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_effect_antialiasing.cc).
  The half-open rule adapts shared-edge GPU raster ownership to classiCAD's
  CPU fallback; it is not a Workbench-specific shader rule.
- A production-path contract renders each half of a translucent quad
  separately and verifies their shared-diagonal sample coverage sums to one.
  Validation: `cmake --build build -j4` passed with the dependency audit;
  `QT_QPA_PLATFORM=offscreen ./build/classicad_workbench_raster_fragment_contracts_test`
  and
  `QT_QPA_PLATFORM=offscreen ./build/classicad_viewport_render_contracts_test`
  passed. The fallback still composites fractional coverage from adjacent
  triangles separately at this point in the implementation history; the
  per-surface transparent resolve is documented in the following section. No
  matched Blender frame comparison was run.

### QPainter transparent mesh fractional edge resolve (2026-10-14)

- The QPainter mesh fallback now collects fractional edge fragments by device
  pixel and resolves same-depth triangles together when their exact 4x4 sample
  masks are disjoint. It composites their coverage as one surface contribution
  and emits one matching software fragment, avoiding a doubled transparent
  seam along a triangulated surface. Overlapping masks and distinct depth
  layers keep their independent source-over contributions. The resolve is
  bounded by 16M target pixels, 8M triangle-bounds pixel tests, 250K distinct
  fractional pixels, and 1M fractional triangle-pixel entries; exceeding a
  bound disables the resolve for that object and uses the existing fallback.
- Blender 5.2.2 accumulates transparent mesh fragments and revealage in its
  transparent pass in
  [`workbench_transparent_accum_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_transparent_accum_frag.glsl#L474-L526)
  and resolves those buffers in
  [`workbench_transparent_resolve_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_transparent_resolve_frag.glsl#L265-L286).
  This classiCAD change adapts aggregation to the CPU fallback's estimated
  subpixel coverage; it does not reproduce Blender's GPU rasterization or
  weighted blended transparency for intersecting surfaces.
- Production-path contracts verify that the two halves of a transparent quad
  resolve to one shared-edge fragment and composite over an opaque receiver
  without a seam. Validation: `cmake --build build -j4` passed with the
  dependency audit, and the offscreen
  `classicad_workbench_raster_fragment_contracts_test` and
  `classicad_viewport_render_contracts_test` passed. The existing `qfloat16`
  memcpy warning remains; no matched Blender frame comparison was run.

**QPainter mesh edge-coverage re-audit (2026-10-14):** The previously noted
pixel-center-only mesh candidate is stale. `workbenchTrianglePixelCoverage`
uses 4×4 subpixel samples for fractional edge pixels, averages barycentric
weights over covered samples, and applies winding-normalized top-left edge
ownership. The mesh path retains sample masks and resolves bounded same-depth
transparent fragments; its pixel, bounds-test, and entry caps are recorded
above. `workbench_raster_fragment_contracts` passes. No second edge-coverage
algorithm is justified by the current source. This CPU approximation still
does not reproduce Blender 5.2.2's later GPU AA pass; matched full-frame mesh
edge comparison remains open.

**QPainter mesh AA mode re-audit (2026-10-15):** Rechecked the production
callers in `viewport_renderer.cpp` after the pixel-center-only fallback was
raised again. They pass one sample per pixel when AA is Off and four samples
per axis when a non-Off raster AA mode is active. The shared helper applies
those requested sample counts, while the mesh/NURBS coverage contract checks
their matching edge profiles at DPR 1, 1.25, 1.5, and 2 and verifies the
enabled-mode alpha remains in 1/16 coverage steps. Thus the fallback is
center-sampled in AA-Off by design and estimates fractional coverage for
enabled modes; replacing AA-Off with 4×4 coverage would conflict with Blender
5.2.2's centered single sample. No renderer change was justified by this
candidate. Validation: `QT_QPA_PLATFORM=offscreen
./build/classicad_workbench_raster_fragment_contracts_test` passed. Full-frame
comparisons of the enabled QPainter fallback against Blender's later GPU AA
resolve remain open.

### QPainter textured NURBS fractional-edge resolve (2026-10-14)

- The prepared QPainter NURBS coverage resolve now supports a single regular
  image, scene-linear float texture, or decoded tiled image/float texture when
  all triangles have UVs and the existing same-geometric-normal and same-plane
  checks pass. Orthographic views additionally require uniform depth.
  Perspective planar surfaces are covered by the matched comparison above;
  mixed-depth or mixed-normal surfaces, targets above 1M pixels, surfaces
  above 100K triangles, and tiled sets with no decoded tile use the existing
  direct per-triangle path.
- Blender 5.2.2 accumulates transparent fragments and revealage in its pinned
  [`workbench_transparent_accum_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_transparent_accum_frag.glsl#L474-L526)
  and resolves them in
  [`workbench_transparent_resolve_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_transparent_resolve_frag.glsl#L265-L286).
  This CPU path aggregates estimated coverage for one planar surface and does
  not claim to reproduce Blender's GPU rasterization or weighted OIT.
- The production contract renders a half-alpha textured NURBS surface split
  into tessellation triangles and verifies one half-alpha fragment at the
  shared diagonal pixel. An opt-in 160×160 PNG capture now records this exact
  fixture with `CLASSICAD_VIEWPORT_QPAINTER_NURBS_SHARED_EDGE_CAPTURE`. The
  Blender 5.2.2 reference helper accepts explicit resolution, material alpha,
  and transparent-film options; a matched transparent NURBS display proxy
  produced the same 14,400 covered pixels (IoU 1.0). At the central shared
  diagonal and its 3×3 neighborhood, Blender and QPainter both output alpha
  128/255 and differ by one red display code value. Across all shared pixels,
  alpha MAE is 3.18/255 (max 120/255), with zero covered/uncovered XOR pixels;
  this residual is concentrated in partially covered outer pixels and does
  not establish general CPU/GPU weighted-OIT parity. The reproducible report
  is produced by `workbench_shared_alpha_edge_compare.py`.
- Validation: `cmake --build build -j4` passed with the dependency audit and
  offscreen `viewport_render_contracts` passed. Blender 5.2.2 generated the
  matched 160×160 capture, and `workbench_shared_alpha_edge_compare.py`
  completed. The one planar patch/diagonal and its outer fractional edges are
  covered; other tessellations, view angles, overlap stacks, and broader
  QPainter transparency remain open.

### QPainter tiled NURBS fractional-edge resolve (2026-10-14)

- Decoded UDIM image and scene-linear float tile sets can now use the same
  bounded NURBS fractional-edge resolve as regular textures. UV completeness,
  same-normal/same-plane checks, the orthographic depth condition, the 1M
  pixel target cap, and the 100K triangle cap still apply. Tile selection and
  alpha cutoff happen per shaded sample before samples from adjacent
  tessellation triangles aggregate. The existing NURBS parameter-to-UV
  contract remains normalized over its knot domains; this comparison samples
  tile 1001 and does not claim cross-tile NURBS UV mapping.
- The production contract renders a half-alpha planar NURBS with a decoded
  1001/1002 image set. The normalized 0..1 parameter UVs select the opaque
  red 1001 image while the second tile has a distinct green payload. At the
  tessellation diagonal there is exactly one fragment with alpha 0.5, and its
  scene-linear RGB equals the regular-image control. The opt-in
  `CLASSICAD_VIEWPORT_QPAINTER_NURBS_UDIM_SHARED_EDGE_CAPTURE` writes the
  160×160 transparent PNG and both external tile files for the reference run.
- Blender 5.2.2 rendered the matching `NURBS_TEXTURE` display proxy with
  `--texture-udim-parameter-uvs`, preserving the same normalized UV range and
  selecting tile 1001. Both images contain 14,400 covered pixels (IoU 1.0).
  In the center 3×3 diagonal neighborhood both outputs have alpha 128/255;
  RGB differs by one red code value. Across shared pixels, RGBA MAE is
  `[1.066, 0.002, 0.002, 3.18]`, with maximum `[3, 8, 7, 120]`; the alpha
  maximum remains confined to partially covered outer pixels. This verifies
  tiled-source aggregation on the existing NURBS UV contract, not sampling
  another UDIM tile from the surface. Blender's pinned
  [`workbench_image_lib.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_image_lib.glsl#L34-L60)
  resolves tiled images from UVs and applies the image-alpha cutoff in the same
  shader file at
  [`workbench_image_lib.glsl#L416-L429`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_image_lib.glsl#L416-L429).
- Validation: `cmake --build build -j4` passed with the dependency audit,
  offscreen `viewport_render_contracts` passed with regular and UDIM captures,
  and `workbench_shared_alpha_edge_compare.py` completed against the
  Blender 5.2.2 PNG. Reproduce with
  `QT_QPA_PLATFORM=offscreen CLASSICAD_VIEWPORT_QPAINTER_NURBS_SHARED_EDGE_CAPTURE=/tmp/classicad-nurbs-udim/regular.png CLASSICAD_VIEWPORT_QPAINTER_NURBS_UDIM_SHARED_EDGE_CAPTURE=/tmp/classicad-nurbs-udim/udim.png ctest --test-dir build -R '^viewport_render_contracts$' --output-on-failure`, then render
  Blender with `--scene NURBS_TEXTURE --resolution 160 160 --aa OFF
  --lighting FLAT --color-source TEXTURE --texture-image
  /tmp/classicad-nurbs-udim/nurbs-translucent-udim.1001.png
  --texture-udim-parameter-uvs --texture-color-space Non-Color
  --texture-alpha-mode STRAIGHT --texture-interpolation Closest
  --view-direction 0 0 1 --ortho-scale 2.6666666667 --transparent-film
  --zero-dither --material-alpha 0.5`, and compare the two PNGs with
  `workbench_shared_alpha_edge_compare.py --center 80 80 --radius 1`.

### QPainter scalar-volume slice fallback (2026-10-14)

- The QPainter Solid compositor now accepts scalar-volume objects alongside
  its existing surface objects. A bounded CPU rasterizer reconstructs the
  active dense scalar grid from the existing sparse records, transforms the
  selected Blender slice plane through `gridToObject`, `objectToWorld`, and
  object placement, then samples the slice with CLOSEST, LINEAR, or CUBIC
  filtering. Slice opacity uses the selected object-space bound dimension,
  multiplied by the corresponding object scale and 0.05; object rotation does
  not expand that dimension into a world-axis-aligned bound. The density,
  density scale, extinction floor, signed scattering, and color source feed a
  scene-linear source-over resolve. Regular opaque and transparent depth can occlude the slice;
  `showInFront` coverage clips it, and in-front objects composite afterward.
- CPU work is capped at 16M dense voxels, 1M target pixels, and 1M covered
  slice samples. This slice rasterizer is separate from the bounded QPainter
  ray-march path documented below. The scalar volume data model and
  `WorkbenchMaterialData` adapter are unchanged.
- Perspective slice triangles are now clipped against the viewport camera's
  near plane before projection. The clipped vertices retain interpolated grid
  coordinates, so perspective-correct density sampling remains continuous
  across the clipped edge. The regression contract verifies a slice that
  straddles the configured near plane still produces its visible samples.
- Slice composition now follows the pinned Workbench order: resolve regular
  transparency and its depth, then composite the volume slice, then composite
  `showInFront` geometry. Linear camera-depth comparisons reject samples at or
  behind the regular opaque/transparent depth; in-front coverage rejects the
  slice. Contracts cover slices in front of and behind a transparent surface,
  plus rejection under `showInFront` coverage.
- Blender 5.2.2's pinned
  [`workbench_volume.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_volume.bsl.hh)
  defines the transformed slice plane, depth/stencil rejection, and volume
  sampling modes. Its [`workbench_volume.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_volume.cc)
  builds the selected slice face and derives its step from the selected axis of
  `BKE_object_dimensions_get`; the pinned [`object.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/blenkernel/intern/object.cc#L3498-L3509)
  defines those dimensions as object-space bounds multiplied by object scale,
  without rotating the bounds into world axes. The QPainter path follows that
  rule. A rotated anisotropic fixture confirms a 2-unit selected object axis
  keeps a 0.1 slice step even when rotation makes its world AABB extent 4
  units. The shader applies depth/stencil checks per fragment. Shadow sampling
  remains a shader TODO.
- The production viewport contract exercises a volume-only QPainter scene and
  verifies that a valid slice changes the scene image. Validation:
  `cmake --build build -j4` passed with the source dependency audit, and both
  offscreen `viewport_render_contracts` and
  `workbench_raster_fragment_contracts` passed. Matched Blender output,
  complex overlapping opaque-depth cases, and
  transformed slice-plane coverage remain open. The CPU ray marcher
  is a bounded approximation with a 16M dense-voxel cap, 1M candidate pixels
  per volume, and 4M aggregate integration samples per composition;
  multiple/vector grids, animation, and larger bounds remain unsupported.

### QPainter scalar-volume slice subpixel coverage (2026-10-15)

- Slice triangles now sample four quarter-pixel locations and retain each
  covered sample's density and camera depth. Each sample runs its own
  Beer–Lambert opacity and regular-scene depth check before the four resolved
  colors are averaged, avoiding a nonlinear density average at heterogeneous
  slice edges. A per-pixel candidate-work cap bounds large projected triangles
  at eight million subpixel tests; retained samples remain capped at one
  million. This reduces center-sample edge stair-stepping for oblique and thin
  slices, while remaining a four-sample approximation of Blender's
  per-fragment volume shader. `cmake --build build -j4` passed. Matched
  Blender slice captures remain open. Blender reference:
  [`workbench_volume.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_volume.bsl.hh#L1607-L1675).

### QPainter scalar-volume object composition order (2026-10-14)

- Per-pixel QPainter volume contributions now follow the incoming scalar-volume
  scene order for both slice objects and ray-marched objects. They are no longer
  reordered by camera depth. Blender 5.2.2 calls `VolumePass::object_sync_volume`
  from its object-sync loop, then draws the volume pass after regular transparent
  depth. Slice and ray-march draws use premultiplied alpha blending and write
  color without writing depth, so overlapping volume objects retain submission
  order.
- The production viewport contract uses two partial-opacity slice volumes at
  different depths and verifies reversing their scene order reverses the
  dominant output color. Existing slice contracts continue to cover opaque and
  transparent surface-depth clipping and `showInFront` rejection.
- Pinned Blender 5.2.2 sources:
  [`workbench_engine.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_engine.cc#L147-L203)
  syncs volume objects and draws the Volume pass after transparent depth at
  [lines 457–464](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_engine.cc#L457-L464).
  [`workbench_volume.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_volume.cc#L190-L231)
  sets premultiplied alpha blending and submits each slice or ray-march draw.

### QPainter scalar-volume ray-marched fallback (2026-10-14)

- The QPainter Solid compositor now integrates ray-marched scalar volumes in
  scene-linear space. It reconstructs the active dense grid, transforms the
  camera ray through the existing volume transforms, samples CLOSEST, LINEAR,
  or CUBIC density, and applies density scale, extinction, transmittance, and
  scattering. Rays stop at the nearest regular opaque/transparent scene depth;
  pixels covered by in-front geometry are skipped.
- This remains a bounded CPU approximation, not a replacement for Blender's
  GPU volume pass. Limits are 16M reconstructed voxels, 1M candidate target
  pixels per volume, and 4M aggregate ray integration samples per composition.
  Multiple/vector grids, animation, and larger bounds remain unsupported.
- Blender 5.2.2's pinned
  [`workbench_volume.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_volume.bsl.hh)
  is the source for ray step, density, extinction, scattering, and early
  transmittance behavior; [`workbench_volume.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_volume.cc)
  sets the volume sample count to five times each grid dimension and computes
  the world-space step as the length of the per-axis spacing vector.
  QPainter marcher now sets remaining transmittance to zero when it reaches
  Blender's 0.01 early-stop threshold, matching the shader's fully opaque
  result at termination. Orthographic rays also offset near/far clipping by
  the ray origin's camera-relative depth, matching Blender's depth endpoint
  when the ray starts on the view plane instead of at the camera.
  The production contract checks
  visible output and increased contribution at higher density. Matched Blender
  output and comprehensive depth/occlusion combinations remain open.
- Validation: full `cmake --build build -j4` passed with the source dependency
  audit. Offscreen `viewport_render_contracts` and
  `workbench_raster_fragment_contracts` passed.

### QPainter scalar-volume ray clipping and work caps (2026-10-14)

- The production CPU ray-sampler contract now exercises four cases: visible
  density integration, depth termination before the camera near plane,
  rejection of pixels covered by in-front geometry, and refusal to exceed a
  one-sample integration budget. The implementation retains the 4M aggregate
  integration cap used by the compositor.
- These checks exercise the existing nearest-scene-depth and in-front coverage
  inputs in `rasterWorkbenchVolumeRaySamples`; they do not establish matched
  Blender output or all opaque/transparent occlusion combinations.
- Validation: `cmake --build build -j4` passed with the dependency audit, and
  offscreen `viewport_render_contracts` plus
  `workbench_raster_fragment_contracts` passed.

### QPainter scalar-volume ray bounds across the perspective camera plane (2026-10-14)

- Ray-marched volume bounds can cross the perspective camera or near plane.
  Some box corners then cannot be projected. The QPainter fallback now uses
  the capped full viewport as a conservative candidate region when any corner
  projection fails, then relies on its per-pixel ray/box intersection and
  camera near-plane clipping to decide visibility. The existing 1M pixel and
  4M integration caps still bound this path.
- Production contracts verify that a volume box straddling the camera plane
  produces in-bounds samples within the integration budget, while a box fully
  behind the camera produces none. This follows Blender 5.2.2's
  [`workbench_volume.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_volume.bsl.hh)
  ray-box sampling model, which evaluates volume intersection per fragment
  rather than requiring every box corner to project.
- Validation: `cmake --build build -j4` passed with the dependency audit.
  Offscreen `viewport_render_contracts` passed. Matched Blender output and
  additional opaque/transparent volume-depth combinations remain open.

### QPainter scalar-volume Bayer row orientation (2026-10-15)

- The ray marcher now maps its top-left CPU image row to Blender's bottom-up
  fragment row before indexing the 4×4 volume dither matrix. Blender 5.2.2
  indexes the matrix with `int2(gl_FragCoord.xy) % 4`; the previous QPainter
  loop used its top-down array row directly. The matrix is asymmetric under
  vertical reflection, so the old code started rays at a different offset.
  The new `rasterWorkbenchVolumeDitherNoise` helper keeps that coordinate
  conversion explicit and covered independently.
- The focused contract checks the top and bottom row phases against the pinned
  Blender matrix. This corrects sample placement only; it does not establish
  matched volume output, density integration parity, or full camera/depth
  coverage. Those volume acceptance items remain open.
- Blender 5.2.2's Bayer matrix and bottom-up coordinate source are pinned in
  [`workbench_volume.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_volume.bsl.hh#L1627-L1645).
- Validation: `cmake --build build -j4` passed with the 320-source dependency
  audit; offscreen `viewport_render_contracts` and
  `workbench_raster_fragment_contracts` passed (2/2). No GUI was launched.

### Matched Blender volume ray-march fixture (2026-10-15)

- Added `tests/workbench_volume_reference.py`, a Blender 5.2.2-only background
  reference that creates and imports a native single-grid FLOAT VDB. Its eight
  active voxels occupy `[-1, 0]` on each axis and have distinct density values;
  Workbench uses Flat lighting, LINEAR interpolation, Standard color
  management, AA Off, and a 640×480 orthographic camera. The script checks the
  Blender version and the generated VDB bounds before rendering.
- Added an opt-in QPainter capture controlled by
  `CLASSICAD_VIEWPORT_QPAINTER_VOLUME_CAPTURE` to
  `viewport_render_contracts`. It uses the same sparse values, a +0.5
  grid-to-object translation, matching object color, background, and framing.
  This capture is diagnostic and does not run or write files by default.
- The first paired display PNG comparison has exact foreground coverage
  (181,476 foreground pixels in each image; IoU 1.0, XOR 0). Across their common
  foreground, channel MAE is 6.78/255, p95 16/255, and max 22/255. The mask
  agreement validates the fixture's camera framing and active support; the
  color residual is still too large to call matched ray-integration parity and
  is not yet localized to one sampling or density term.
- Pinned Blender 5.2.2 sources show Workbench uses five texture samples per
  axis, a Halton-3 sample offset, LINEAR hardware sampling, extinction and
  scattering integration, and early termination at transmittance 0.01 in
  [`workbench_volume.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_volume.cc#L830-L842)
  and
  [`workbench_volume.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_volume.bsl.hh#L1556-L1669).
  Matched volume color acceptance remains open pending attribution of the
  residual.
- Validation: `cmake --build build -j4` passed with the 320-source dependency
  audit; opt-in offscreen `viewport_render_contracts` passed (1/1); Blender
  5.2.2 generated the reference PNG and VDB; `workbench_image_compare.py`
  reported the metrics above. No GUI was launched.

### Matched GPU object-outline comparison (2026-10-14)

- Fresh 640×480 background captures from Blender 5.2.2 and classiCAD use the
  same nonselected polygon cube, matched orthographic framing and view
  direction, AA Off, and outline-free baseline/effect pairs. The existing
  `workbench_effect_delta_compare.py` compares only the effect deltas, which
  avoids treating small base-render edge differences as outline changes.
- At RGB threshold 2/255, Blender changes 2,312 pixels and classiCAD changes
  2,316; their outline masks have IoU 0.823483 with 448 XOR pixels. Signed
  RGB-delta error is 0.04164/255 whole-image MAE and 5.04058/255 over the
  effect-mask union (p95 16/255, max 30/255). Attribution with
  `workbench_effect_delta_compare.py --outline-mask-attribution` finds that
  the baseline object silhouettes differ by 224 pixels (IoU 0.997991), while
  each renderer's observed effect mask matches the four-cardinal boundary of
  its own baseline silhouette exactly (zero XOR). The outline-mask XOR is
  therefore inherited from silhouette raster coverage in this capture; it
  does not indicate a different outline neighbor rule.
- The Blender cube fixture now accepts `--outline-color` in scene-linear RGB.
  The GPU contract has an opt-in scene-linear capture sweep for black, white,
  and red outlines over linear backgrounds 0.02 and 0.18. The effect comparator
  also reports RGB-delta error and average effect magnitude on the shared
  changed-pixel mask. All six pairs retain the same 2,312 Blender / 2,316
  classiCAD outline pixels, 448 XOR pixels, and 0.823483 mask IoU. On shared
  changed pixels, scene-linear mean max-channel effect is 0.03390 vs 0.04161
  for dark-background black; 0.30027 vs 0.29283 for dark-background white;
  0.30002 vs 0.29260 for dark-background red; 0.06261 vs 0.06605 for
  light-background black; 0.27162 vs 0.26844 for light-background white; and
  0.27137 vs 0.26821 for light-background red (Blender vs classiCAD). Shared
  changed-pixel RGB-delta MAE ranges from 0.00425 to 0.00799 linear units.
  The response varies with outline color and background, so this sweep does
  not support one uniform magnitude correction; no production change is made.
- This comparison covers one unselected default cube only. Selected outlines,
  multiple or overlapping objects, smoothing, UI scaling, camera variation,
  and additional QPainter outline comparisons remain open. No implementation
  change was made;
  this is a measured attribution, not a parity sign-off.

### Matched QPainter object-outline comparison (2026-10-14)

- Added opt-in baseline/effect image captures to the existing 640×480 QPainter
  polygon-cube fixture. Set `CLASSICAD_VIEWPORT_QPAINTER_OUTLINE_CAPTURE` and
  `CLASSICAD_VIEWPORT_QPAINTER_OUTLINE_BASELINE_CAPTURE` to request them. The
  fixture uses the same camera, scene-linear world background, shading, and
  display transform for both captures; Blender 5.2.2 references use the cube
  script's `BASELINE` and `OUTLINE` effects, AA Off, zero dither, and matched
  orthographic framing.
- At RGB threshold 2/255, Blender changes 2,312 pixels and QPainter changes
  2,320; their effect masks have 1,244 XOR pixels and IoU 0.576583. Each
  renderer's observed effect mask equals the four-cardinal boundary of its own
  baseline silhouette. The baseline silhouettes differ by 622 pixels (IoU
  0.994440), so the outline-mask disagreement is attributable to the different
  base coverage in this capture, not evidence of a different neighbor rule.
  Mean maximum-channel effect is 19.1942/255 for Blender and 19.9328/255 for
  QPainter. On the 1,694 shared effect pixels, signed RGB-delta error has
  7.60055/255 max-channel MAE (p95 22, max 23); this still includes local edge
  coverage and shading differences and does not isolate outline blend color.
- Blender 5.2.2 compares the object ID against four cardinal neighbors in
  [`workbench_effect_outline_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_effect_outline_frag.glsl#L10-L24).
  QPainter's same four-neighbor boundary behavior is covered by
  `viewport_render_contracts`; no production change is justified from this
  single capture. Other colors, selected and overlapping objects, UI scales,
  camera views, AA modes, and scene-linear blend attribution remain open.
- Validation: `cmake --build build -j4` passed with the dependency audit, and
  the offscreen `viewport_render_contracts` test passed. A direct offscreen
  invocation with both capture variables saved the QPainter pair. The Blender
  reference script reported Blender 5.2.2 and the effect-delta comparison
  completed successfully. The GPU outline color/background sweep also passed
  in the offscreen viewport contract; six Blender 5.2.2 scene-linear capture
  pairs and `workbench_effect_linear_compare.py` completed successfully.

### Matched QPainter shadow comparison (2026-10-14)

- Added opt-in baseline and shadowed PNG captures to the existing 640×480
  QPainter polygon-cube fixture using its prepared depth geometry and camera.
  The captures can be requested with
  `CLASSICAD_VIEWPORT_QPAINTER_SHADOW_CAPTURE` and
  `CLASSICAD_VIEWPORT_QPAINTER_SHADOW_BASELINE_CAPTURE`.
- Compared with fresh Blender 5.2.2 CUBE renders using the same view direction,
  orthographic framing, AA Off, and zero dither. At RGB threshold 2/255, the
  shadow-effect masks contain 111,256 Blender pixels and 111,480 classiCAD
  pixels (224 XOR; IoU 0.997991). Mean maximum-channel effect is 12.2859/255
  for Blender and 32.5692/255 for QPainter; signed RGB-delta error is
  7.28147/255 whole-image MAE and 20.06520/255 over the union (p95 30/255,
  max 30/255). Coverage is close, but QPainter's measured effect is much
  stronger in this fixture.
- The comparison does not isolate shadow-map sampling, light matching, or
  receiver shading well enough to justify changing the software shadow
  factor. QPainter still uses a target-sized orthographic light-depth map and
  a single nearest-texel test in the capture described here (before the
  receiver-coverage update below) in
  `src/ui/viewport/viewport_raster_workbench_effects.cpp`; Blender 5.2.2 uses
  stencil shadow volumes and a nonzero-stencil resolve in pinned
  [`workbench_shadow.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_shadow.cc#L1759-L1790)
  and [`workbench_shadow.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_shadow.bsl.hh#L55-L68).
  Treat this as a measured approximation gap, not shadow parity sign-off.
- Validation: `cmake --build build -j4` and the offscreen
  `viewport_render_contracts` test passed. Blender capture and
  `workbench_effect_delta_compare.py` ran against Blender 5.2.2. This covers
  one unselected default cube only; other casters, receivers, directions,
  focus/shift settings, and QPainter shadow configurations remain open.

### QPainter shadow receiver coverage (2026-10-15)

- `shadowFactorAt` checks four quarter-pixel screen positions, projects each
  onto the receiver plane, maps those world positions to light-map space, and
  averages their visibility factors. This preserves the screen-pixel footprint
  on angled receivers instead of applying an axis-aligned light-map offset.
  Fully lit and fully shadowed interiors retain the prior binary factor; only
  receivers crossing a shadow edge can produce partial coverage.
- This is a bounded QPainter approximation of Blender 5.2.2's stencil-volume
  resolve, not an equivalent shadow algorithm. The existing matched cube
  capture predates this change; edge-coverage and effect-delta comparisons
  have not yet been rerun. Caster rasterization, depth bias, transparency, and
  shadow lighting remain unchanged.
- Validation: `cmake --build build -j4` passed for the receiver-plane sample
  correction. No matched image comparison was run for this change.

### QPainter fully shadowed factor contract (2026-10-15)

- Added a direct scalar regression contract for the fully shadowed branch of
  `rasterWorkbenchShadowFactor`, covering intensities 0, 0.2, 0.5, 0.8, and 1
  across three surface normals. Every result must equal `1 - intensity`.
- This follows Blender 5.2.2's pinned
  [`workbench_world_light.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_world_light.bsl.hh#L677-L688):
  `force_shadow` zeros `shadow_mix`, then `shadow_add` remains. The pinned
  [`workbench_shadow.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_shadow.cc#L1629-L1636)
  sets `shadow_mul` to intensity and `shadow_add` to its complement. This
  contract checks the factor equation only; it does not measure mask coverage,
  receiver shading, or close the QPainter shadow comparison gap above.
- Validation: `cmake --build build -j4` and the offscreen
  `viewport_render_contracts` test passed.

### QPainter closed-solid shadow caster contract (2026-10-14)

- A source audit confirmed the software effect rasterizer already obtains
  derived display triangles for `NurbsSolid` through
  `buildViewportDepthGeometry` and appends those triangles to its binary
  shadow-depth map. The missing coverage was a closed extrusion solid in the
  QPainter shadow contracts; existing caster contracts used open surface
  quads or polygon meshes.
- Added a production-path contract that creates and validates a closed NURBS
  extrusion solid, prepares its derived face geometry, and verifies that it
  casts a software shadow onto an opaque receiver outside the caster's screen
  footprint. Existing transparent-material and texture-alpha caster contracts
  continue to verify Blender Workbench's binary geometric shadow-volume
  semantics.
- Blender 5.2.2's shadow-volume pass submits mesh geometry and resolves
  nonzero stencil coverage in pinned
  [`workbench_shadow.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_shadow.cc#L1759-L1790).
  The QPainter light-depth map remains a hard-shadow approximation; this
  contract establishes supported solid coverage, not matched shadow quality.
- Validation: `cmake --build build -j4` passed with the dependency audit and
  the offscreen `viewport_render_contracts` test passed.

### Matched QPainter Linear UDIM texture comparison (2026-10-14)

- Compared the existing QPainter two-tile polygon-mesh capture with a fresh
  Blender 5.2.2 `MESH_TEXTURE` render using Flat lighting, Linear Image Texture
  filtering, STRAIGHT alpha, AA Off, and the same orthographic view. The Blender
  World background was set to the classiCAD fixture's scene-linear background.
- The QPainter fragment PFM stores rows in Qt top-down order, while Blender's
  PFM is bottom-up. `workbench_pfm_compare.py` now accepts
  `--candidate-top-down` to flip such captures before comparison and labels the
  row handling in its output. After alignment, foreground-mask IoU is 0.985560
  (464 XOR pixels; 31,711 Blender and 32,091 QPainter pixels). The two-pixel
  silhouette interior, excluding a six-pixel band around the shared triangle
  diagonal, contains 26,278 pixels; scene-linear RGB MAE is 0.000213, p95
  0.000815, and maximum 0.000815. Whole-frame channel MAEs are 0.000566 red,
  0.000630 green, and 0.000054 blue; larger maxima localize to cutout/edge
  coverage rather than the stable interior sample colors.
- This verifies Linear-filter sampling across two horizontal UDIM tiles for
  one external PNG and one mesh fixture; vertical-tile coverage is recorded
  below. Other extensions, float images, other texture filters, NURBS, and GPU
  parity remain open.
  Blender's pinned
  [`workbench_image_lib.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_image_lib.glsl#L34-L60)
  supplies the tile lookup and filtering behavior.
- Validation: the offscreen `viewport_render_contracts` capture passed, the
  reference was rendered with Blender 5.2.2, and
  `workbench_pfm_compare.py --candidate-top-down` completed on the paired
  captures. The 640×480 coverage edge differences remain recorded separately.

### Matched QPainter vertical UDIM texture comparison (2026-10-14)

- Extended the controlled QPainter mesh capture and Blender reference helper
  to render the vertical 1001/1011 tile pair with Linear filtering, Flat
  lighting, Texture color, STRAIGHT alpha, and AA Off. Each 2×2 external PNG
  uses distinct upper/lower colors and a transparent texel. Their UVs exercise
  both vertical tiles and the top-down image-row conversion. The new Blender
  flag is `--texture-udim-vertical`; QPainter capture artifacts are written
  under `CLASSICAD_VIEWPORT_LIGHTING_CAPTURE_DIR`.
- The scene-linear comparison has foreground-mask IoU 0.981909 (581 XOR
  pixels; 31,631 Blender and 32,020 QPainter foreground pixels). Whole-frame
  channel MAE is 0.002220 red, 0.001570 green, and 0.000146 blue. The stable
  red-dominant 1001 and green-dominant 1011 regions have matching spatial
  bright/dark row patterns and mean scene-linear colors within 0.002 per
  channel; the remaining image error is concentrated at coverage and alpha
  edges. Production contracts verify both tile colors are sampled and the two
  transparent texels reduce visible fragment coverage.
- Blender 5.2.2 chooses one UDIM tile from the integer UV coordinates and
  samples within that tile in pinned
  [`workbench_image_lib.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_image_lib.glsl#L34-L60).
  Filtering does not bridge neighboring tile layers. This comparison verifies
  vertical tile lookup and orientation for one mesh/PNG case; broader mesh
  UVs, NURBS, other filters, float tiles, and final edge coverage remain open.
- Validation: `cmake --build build -j4` passed with the dependency audit; the
  offscreen viewport contract with the lighting capture directory passed.
  Blender reported version 5.2.2, and
  `workbench_pfm_compare.py --candidate-top-down` completed on the matched
  scene-linear captures.

### Matched GPU per-face material slots (2026-10-14)

- Added an opt-in classiCAD GPU capture for a Flat-lit polygon cube whose
  visible top, front, and right faces select three distinct material slots.
  The fixture declares the matching mesh slot IDs as well as the resolved
  `WorkbenchMaterialData` values, preserving the geometry and material
  adapter contract. Its offscreen contract checks that the three face probes
  are present and visibly distinct, so a saved background-only frame cannot
  satisfy the capture.
- Compared with a Blender 5.2.2 Workbench `MATERIAL_SLOTS` cube using the same
  camera, Flat lighting, Material color source, AA Off, and zero dither. All
  27 face-interior probes across the three visible faces match exactly in both
  scene-linear RGB (MAE 0, max 0) and display RGB (MAE 0/255, max 0/255).
- The same fixture now captures QPainter through `ViewportRenderer::drawShape`
  using the same mesh face-slot indices and `WorkbenchMaterialData` payloads.
  Its 27 probes match Blender 5.2.2 display RGB exactly (MAE 0/255, max 0),
  with scene-linear MAE `0.0001237` and max `0.0002735`. Per-face scene-linear
  MAE is `0.000186` top, `0.000130` front, and `0.000055` right. The QPainter
  capture checks three distinct visible probes and saves scene-linear PFM and
  display PNG alongside the existing GPU capture.
- This verifies deferred GPU per-face slot selection and color resolve for
  three visible faces in one opaque mesh, plus QPainter's face-local material
  lookup for the same Flat fixture. It does not establish Studio, textured,
  transparent, additional slot-count, or general mesh parity; those
  comparisons remain open. No production renderer change was justified by
  this fixture.
- Validation: `cmake --build build -j4` passed with the dependency audit, the
  offscreen `viewport_render_contracts` test passed with the opt-in GPU and
  QPainter captures, and `workbench_display_probe_compare.py` passed against
  the Blender 5.2.2 reference for both renderers. Blender's pinned Workbench
  material selection uses the face slot in
  [`workbench_engine.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_engine.cc#L2026-L2067)
  and emits per-material mesh batches at
  [`workbench_engine.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_engine.cc#L2281-L2329).

#### Studio Default per-face material slots

- Extended the same three-slot cube to opt-in GPU and QPainter captures under
  Studio Default lighting, Material color, specular enabled, AA Off, and
  effects/outlines disabled. All three Blender and classiCAD slot materials
  carry distinct roughness/metallic pairs: `(0.2, 0.0)`, `(0.5, 0.5)`, and
  `(0.8, 1.0)`. The C++ fixture preserves the existing mesh face-slot IDs and
  `WorkbenchMaterialData` adapter and exercises the Studio material payload
  independently on each visible face.
- Compared 27 face-interior probes against Blender 5.2.2 Workbench with the
  matched 640×480 orthographic camera and zero dither. GPU scene-linear RGB is
  exact (MAE 0, max 0); display RGB MAE is 0.1111/255 with max 1/255. QPainter
  scene-linear MAE is `0.0000485` with max `0.0001308`; display MAE is
  `0.1111/255` with max `1/255`. QPainter per-face scene-linear MAE is
  `0.000044` top, `0.000075` front, and `0.000027` right. The offscreen
  contract confirms all three slot probes remain present and distinct in both
  captures.
- This comparison found that QPainter's polygon-mesh Studio path passed raw
  material roughness/metallic to its lighting function, while Blender's
  opaque path first packs those values and then truncates the lighting payload
  to 5 roughness bits and 3 metallic bits. The CPU mesh path now uses the same
  material packing and opaque quantization helpers as the GPU path; X-Ray and
  alpha-bearing draws retain the first packed payload without opaque-only
  quantization. The per-slot comparison now exercises this behavior for three
  distinct payloads.
- This verifies one opaque mesh with three visible face slots for Studio
  Default. Other Studio presets/rotations and roughness/metallic values,
  textured slots, slot counts beyond four, and broad mesh parity remain open.
  An offscreen QPainter fragment contract also verifies the three transparent
  Studio slots preserve alpha `0.5` and shade from the initial 8-bit packed
  material payload, without the opaque-only 5-bit roughness/3-bit metallic
  truncation. This checks per-fragment payload behavior; it does not establish
  final transparent OIT resolve parity across classiCAD and Blender. Blender
  5.2.2's transparent accumulation path is pinned in
  [`workbench_transparent_accum_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_transparent_accum_frag.glsl#L496-L522).
  Blender 5.2.2's
  deferred Workbench resolve and material
  payload are pinned in
  [`workbench_composite.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_composite.bsl.hh#L558-L620)
  and
  [`workbench_common.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_common.bsl.hh#L409-L456).
- Validation: `cmake --build build -j4` passed with the dependency audit; the
  offscreen `viewport_render_contracts` capture passed; Blender reported
  version 5.2.2; and `workbench_display_probe_compare.py` passed for both GPU
  and QPainter PNG/PFM pairs. The transparent payload and alpha fragment
  contract passed in the same offscreen `viewport_render_contracts` test.

#### `check_normal+y` MatCap per-face material slots

- Captured the same three visible face slots with the Blender `check_normal+y`
  MatCap, specular disabled, Material color, AA Off, no effects/outlines, and
  the same 640×480 orthographic framing. The fixture keeps its distinct
  per-slot roughness/metallic payloads, which MatCap shading does not use.
- All 27 GPU face-interior probes match Blender 5.2.2 exactly in scene-linear
  and display RGB. QPainter scene-linear MAE is `0.0001822`, max `0.0004926`;
  display MAE is `0.1111/255`, max `1/255`. QPainter per-face scene-linear
  MAE is `0.000284` top and `0.000131` on both front and right. Both
  offscreen captures verify three distinct visible slot probes.
- This verifies one MatCap preset on one opaque, three-slot mesh. Other
  presets, specular combinations, orientations, textured/transparent slots,
  additional slot counts, and broad mesh parity remain open. No production
  change was justified. Blender 5.2.2's resolve multiplies the sampled MatCap
  lighting by per-face base color in
  [`workbench_composite.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_composite.bsl.hh#L580-L600),
  and computes its orientation UVs in
  [`workbench_matcap.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_matcap.bsl.hh#L331-L355).
- Validation: `cmake --build build -j4` passed with the dependency audit; the
  offscreen `viewport_render_contracts` capture passed; Blender reported
  version 5.2.2; and `workbench_display_probe_compare.py` passed for both GPU
  and QPainter PNG/PFM pairs.

#### Four visible material slots in Flat and Studio

- Added a camera-facing 2×2 quad grid to the Blender 5.2.2 reference helper
  and the classiCAD GPU/QPainter contract captures. Each face selects its own
  opaque material slot, with scene-linear colors `(0.18, 0.28, 0.72)`,
  `(0.72, 0.20, 0.18)`, `(0.18, 0.65, 0.25)`, and `(0.78, 0.68, 0.12)` for
  slot IDs 0 through 3. The fixtures use Material color, Flat lighting, AA
  Off, no effects/outlines, and the matched 640×480 orthographic camera.
- The offscreen C++ contract verifies each GPU and QPainter face-center probe
  resolves to its expected scene-linear color and captures a nonblank PNG/PFM.
  With the scene-linear world background matched to `0.0219421387`, Blender
  5.2.2 and GPU have identical full-frame masks (IoU 1.0, zero XOR) and exact
  full-frame PFM RGB. QPainter has the same mask and exact display RGB;
  full-frame scene-linear channel MAE is at most `0.000036` with max
  `0.000313`. GPU display differs by at most `1/255` in a channel, from
  display rounding. This confirms slot ID 3 selection for this Flat
  Material-color mesh.
- Added Studio Default with specular enabled to the same four-face fixture.
  The slots use roughness/metallic pairs `(0.2, 0.0)`, `(0.4, 0.25)`,
  `(0.65, 0.5)`, and `(0.8, 1.0)`. GPU full-frame scene-linear RGB matches
  Blender 5.2.2 within `0.0000611` maximum error and has an exact foreground
  mask. QPainter maximum scene-linear error is `0.000545`; its full-frame
  display RGB is exact, while GPU display differs by at most `1/255` from
  rounding. Both backends match Blender's full-frame silhouette exactly.
  This confirms slot ID 3 selection and distinct Studio material payloads for
  this fixture; missing/invalid indices, slots beyond ID 3, MatCap four-slot
  shading, textures/alpha, and broad geometry parity remain open. Blender's
  per-face slot selection and per-material batching remain
  pinned in [`workbench_engine.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_engine.cc#L2026-L2067)
  and [`workbench_engine.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_engine.cc#L2281-L2329).
- Validation: Blender reported 5.2.2 LTS and rendered nonblank 640×480 PNG,
  EXR, and PFM output using `--world-color 0.0219421387 0.0219421387
  0.0219421387` for both Flat and Studio references;
  `cmake --build build -j4` passed with the dependency audit;
  offscreen `viewport_render_contracts` passed (1/1); and full-frame PNG/PFM
  comparisons passed with `workbench_image_compare.py` and
  `workbench_pfm_compare.py`.

#### Four visible material slots in MatCap

- Extended the same four-face fixture to the `metal_bronze` MatCap preset with
  specular enabled. The classiCAD offscreen contract now captures GPU and
  QPainter display images and scene-linear PFM buffers, and checks the expected
  face-center shading for each material slot.
- Blender 5.2.2 full-frame comparisons have exact foreground masks for both
  backends. GPU display RGB differs by at most `1/255` (channel MAE
  `0.0711/255`); QPainter has the same display error. Scene-linear PFM maximum
  error is `0.001953125` for GPU and `0.0019226` for QPainter. No partial edge
  coverage differs in this orthographic fixture.
- This verifies four opaque material slots for one MatCap preset and one view.
  Other MatCap presets and rotations, specular variations, invalid or higher
  slot indices, textured/transparent slots, and broader geometry parity remain
  open. Blender's per-face material selection is pinned in
  [`workbench_engine.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_engine.cc#L2026-L2067);
  MatCap color multiplication and orientation UV computation are pinned in
  [`workbench_composite.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_composite.bsl.hh#L580-L600)
  and [`workbench_matcap.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_matcap.bsl.hh#L331-L355).
- Validation: `cmake --build build -j4` passed with the dependency audit;
  offscreen `viewport_render_contracts` passed; Blender reported version
  5.2.2 LTS; and the full-frame PNG/PFM comparisons passed for GPU and
  QPainter captures.

#### Fifth visible material slot (index 4)

- Added a five-material Flat/Material fixture with five coplanar faces and a
  unique fifth color at slot index 4. The Blender 5.2.2 helper and classiCAD
  GPU/QPainter captures use matching 640×480 orthographic framing, AA Off,
  and the same neutral scene-linear world background. The offscreen C++
  contract checks all five face-center mappings in both backends.
- Blender and GPU have an exact full-frame foreground mask and scene-linear
  PFM RGB. GPU display differs by at most `1/255`; QPainter display RGB and
  silhouette are exact. QPainter scene-linear PFM maximum channel error is
  `0.0003711`, with exact edge-coverage classification and zero mask XOR.
- This confirms index 4 works without a production renderer change and that
  the current material adapter has no four-slot limit. Slot counts above five,
  invalid indices, per-face texture combinations, and broader geometry parity
  remain open. Blender's per-face slot selection remains pinned in
  [`workbench_engine.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_engine.cc#L2026-L2067)
  and its material batching in
  [`workbench_engine.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_engine.cc#L2281-L2329).
- Validation: `cmake --build build -j4` passed with the dependency audit;
  offscreen `viewport_render_contracts` passed; Blender reported version
  5.2.2 LTS; and `workbench_image_compare.py` plus
  `workbench_pfm_compare.py` passed for GPU and QPainter captures.

#### Fifth-slot Studio and MatCap shading

- Extended the five-face fixture to Studio Default with specular and the
  `metal_bronze` MatCap. Slot 4 keeps its own `(roughness, metallic)` values
  `(0.35, 0.15)`; all five faces retain distinct material colors. Blender
  5.2.2 references and classiCAD GPU/QPainter captures use the same camera,
  background, and AA-Off setup.
- All four backend/reference comparisons have exact full-frame masks.
  Studio GPU scene-linear max error is `0.0002442`, QPainter max is
  `0.0005449`; Studio display is exact for QPainter and differs by at most
  `1/255` for GPU. MatCap scene-linear max error is `0.001953125` for GPU and
  `0.0019226` for QPainter; both display captures differ by at most `1/255`.
- This confirms index-4 material roughness/metallic shading in Studio and
  material-color use in MatCap. Other presets, rotations, AA modes, higher slot
  counts, and textured/transparent combinations remain open. Blender's
  per-face slot selection and material batching remain pinned in
  [`workbench_engine.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_engine.cc#L2026-L2067)
  and [`workbench_engine.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_engine.cc#L2281-L2329).
- Validation: `cmake --build build -j4` passed with the dependency audit;
  offscreen `viewport_render_contracts` passed; Blender reported version
  5.2.2 LTS for both references; and the full-frame PNG/PFM comparisons
  passed for GPU and QPainter captures.

### Matched GPU planar-receiver shadow comparison (2026-10-14)

- The offscreen GPU contract now captures the same 640×480 blue-plane/red-cube
  scene used by the QPainter shadow comparison, in four cases: caster plus
  receiver with shadows off/on, and receiver only with shadows off/on. The
  receiver-only pair removes Blender Workbench's scene-wide lighting change
  when shadow rendering is enabled. The Blender reference uses the
  `SHADOW_RECEIVER` fixture, Object color, Studio Default lighting, AA Off,
  zero dither, orthographic scale 5, and the 5.2.2 defaults for shadow shift
  and focus (0.1/0.0). classiCAD captures the same scene through its GPU scene
  pass coordinator, with matching settings. The existing
  `workbench_shadow_receiver_compare.py` measures scene-linear caster-only
  deltas at a 0.001 threshold.
- Receiver-mask IoU is 0.998456 (145,090 Blender pixels and 144,866
  classiCAD pixels). The isolated cast-shadow masks contain 8,060 Blender
  pixels and 7,837 classiCAD pixels (IoU 0.972088). Mean maximum-channel
  attenuation over the affected pixels is 0.081299 and 0.080933 in scene-linear
  RGB. Mean absolute RGB delta error is `1.1881e-5` over the shared receiver
  and `2.1962e-4` over the union of the effect masks. This validates one
  opaque polygon caster/receiver topology and view; it does not close other
  light directions, geometry, fallback paths, or QPainter parity.
- Blender 5.2.2 builds shadow intensity into the per-sample shading factors
  and resolves stencil shadow-volume visibility in the pinned
  [`workbench_shadow.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_shadow.cc#L1620-L1636)
  and [`workbench_shadow.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_shadow.bsl.hh#L1023-L1038).
- Validation: full `cmake --build build -j4` passed with the 316-source
  dependency audit; offscreen `viewport_render_contracts` passed with all four
  captures enabled; Blender reported 5.2.2 LTS; and
  `workbench_shadow_receiver_compare.py` completed against the scene-linear
  PFM captures in `/tmp/classicad-shadow-receiver`.

### GPU opaque shadow-factor attachment contract (2026-10-15)

- Added an offscreen coordinated-render contract that reads the live opaque
  `R16F` shadow-mask texture and `R32UI` object-ID texture before the AA target
  is released. The 640×480 fixture has one flat opaque receiver and one opaque
  cube caster, with AA Off, shadows enabled, intensity 0.5, shift 0.1, and
  focus 0.0. It verifies an unshadowed receiver factor against Blender's
  `smoothstep(shift, focusEdge, -dot(normal, lightDirection))` equation, a
  fully shadowed receiver factor of 0.5, and neutral factor 1 on uncovered
  background pixels.
- The check exercises classiCAD's attachment population and scalar contents;
  it does not compare the full shadow mask's shape against Blender. The
  matched image comparison above still records its tested coverage and open
  limits. Blender's pinned [`workbench_world_light.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_world_light.bsl.hh#L677-L688)
  defines the factor equation, and pinned [`workbench_shadow.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_shadow.cc#L1629-L1636)
  defines the focus edge and intensity/add terms.
- Validation: `cmake --build build -j4` passed with the 318-source dependency
  audit, and offscreen `viewport_render_contracts` passed with the direct
  attachment readback enabled.

### GPU fallback shadow-map bounds (2026-10-15)

- When the GPU light-depth map runs in fallback-only mode, its orthographic
  bounds now include only ranges that the map will draw. Previously, supported
  shadow-volume casters could expand those bounds even though the fallback map
  skipped them, reducing the map resolution available to unsupported casters.
  The bounds pass now uses the same range eligibility filter as the draw pass.
- This does not change the full-scene map used when the shadow-volume pass is
  unavailable. The expected improvement is limited to mixed scenes where the
  remaining fallback casters occupy less space than the volume-supported
  geometry; a matched GPU capture of that case has not been run.
- Validation: `cmake --build build -j4` passed. No image comparison was run.

### Matched QPainter planar-receiver shadow comparison (2026-10-14)

- Added a controlled 640×480 plane-and-cube scene to the Blender 5.2.2
  reference helper and the QPainter capture harness. The 8×8 XY receiver is
  blue, the size-2 cube caster is red and rests on it, and a receiver-only
  scene provides a baseline/effect control. Both apps use Object color,
  Blender Studio Default lighting, AA Off, a `(4, -6, 4)` view direction,
  orthographic scale 5, scene-linear world background `(0.12, 0.07, 0.035)`,
  zero dither, and shadow intensity 0.5. Blender 5.2.2 defaults for shadow
  shift/focus are explicitly matched at 0.1/0.0. A second paired case sets
  `shadow_shift=0.25` and `shadow_focus=0.4` in both renderers; the Blender
  helper exposes these values as command-line parameters and the QPainter
  harness captures both caster and receiver-only scenes for each case.
- Blender changes the light response across the receiver when shadows are
  enabled, even without a caster. The new
  `workbench_shadow_receiver_compare.py` subtracts that receiver-only delta
  from each plane-and-caster delta before measuring the localized cast shadow.
  The baseline receiver masks contain 145,090 Blender and 145,437 classiCAD
  pixels (IoU 0.995419). At a 0.001 scene-linear threshold, the isolated cast
  shadow masks contain 8,060 Blender and 4,405 classiCAD pixels (IoU
  0.485697). Mean maximum-channel attenuation on each app's affected pixels
  is 0.08130 for Blender and 0.07691 for classiCAD; mean RGB delta error on
  the shared receiver is 0.001362.
- At shift/focus 0.25/0.4, the isolated cast-shadow mask remains 8,060 Blender
  and 4,405 classiCAD pixels (IoU 0.485697). Mean maximum-channel attenuation
  rises to 0.12195 and 0.11514, respectively, close to the same 50% increase
  in both renderers. Mean RGB delta error on the shared receiver rises from
  0.001362 to 0.002042. This supports the matched joint shift/focus response
  while showing the unchanged mask-shape gap is independent of those two
  values in this fixture; it does not establish either parameter's independent
  response.
- Comparing the uncorrected display effect pairs alone reports identical
  effect-mask coverage (IoU 1.0) but stronger whole-frame classiCAD darkening:
  14.662/255 mean maximum-channel effect in Blender versus 29.266/255 in
  classiCAD. The receiver-only control shows that this is not a direct measure
  of cast-shadow strength. In the original viewport-resolution map, the
  remaining localized mask-shape difference was consistent with classiCAD's
  orthographic depth map and nearest-texel test, while Blender 5.2.2 builds stencil shadow volumes and
  resolves their stencil mask. Blender's shadow source sets
  `shadow_mul`/`shadow_add` from the intensity and emits silhouette sides in
  pinned [`workbench_shadow.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_shadow.cc#L1620-L1636)
  and [`workbench_shadow.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_shadow.bsl.hh#L1023-L1038).
- No production shadow-factor change is justified by these comparisons: the
  per-hit attenuation is close, while most residual error is in shadow mask
  coverage and placement. The QPainter shadow map remains an approximation;
  additional caster topology, light direction, receiver, and camera cases are
  open.
- Validation: full `cmake --build build -j4` passed with the dependency audit;
  the offscreen `viewport_render_contracts` test passed with default and
  alternate shift/focus captures; Blender reported version 5.2.2; and
  `workbench_shadow_receiver_compare.py` plus
  `workbench_effect_delta_compare.py` completed on the paired captures.

### Matched QPainter DOF capture audit (2026-10-14)

- Captured a nonblank 640×480 cube baseline and DOF result from the QPainter
  production path, then rendered the matching Blender 5.2.2 reference. The
  Blender helper uses a 25 mm rendered camera at f/0.1. The classiCAD
  `ViewportTransform` capture uses 50 mm at f/0.4: its perspective viewport
  projection divides focal length by Blender's viewport projection zoom factor
  of two, so this matches the rendered framing; scaling f-stop by four keeps
  the `f²/N` circle-of-confusion scale. Both use six aperture blades, 0.2 rad
  rotation, 1.2 aperture ratio, and disabled shadows, cavity, specular, and
  outline.
- At an RGB channel threshold of 2, Blender changes 11,647 pixels and QPainter
  changes 4,265; their effect-mask IoU is 0.02447 (15,152 XOR pixels). At
  threshold 3, the counts are 10,059 and 3,163 and IoU is 0.01684. However,
  the baseline images already differ by a mean maximum-channel delta of
  27.75/255, with 113,804 pixels above 2. The DOF images differ by
  27.70/255. This baseline mismatch prevents attributing the mask disagreement
  to the DOF gather alone.
- **Fixture correction and follow-up (2026-10-15):** The QPainter capture now
  explicitly disables specular highlights, as the Blender helper already
  does. That setting alone did not improve the baseline. Matching the
  `ViewportTransform` projection with equivalent 50 mm/f/0.4 optics brought
  the Blender and QPainter foreground bounds to the same `(96,76)-(510,479)`.
  Baseline mean maximum-channel error fell from `27.781/255` to `19.826/255`,
  while threshold-2 effect-mask IoU rose from `0.023737` to `0.184709` (QPainter
  changed pixels: 4,181 to 8,172; Blender: 11,647). The remaining baseline
  error and mask disagreement show that face response and the DOF gather still
  differ. The comparison now has matched framing, but remains insufficient to
  justify a production DOF equation change.
- This confirms that matched QPainter DOF image acceptance remains open. The
  existing code uses a full-resolution approximation without Blender's
  temporal jitter and complete near/far pyramid. Align face response and
  baseline color before using this comparison to justify a renderer change.
  Blender's stage setup is pinned in
  [`workbench_effect_dof.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_effect_dof.cc#L53-L85);
  its gather is pinned in
  [`workbench_effect_dof_blur1_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_effect_dof_blur1_frag.glsl#L21-L57).
- Validation: Blender reported 5.2.2 LTS; `cmake --build build -j4` passed
  with the dependency audit; the capture-enabled offscreen
  `viewport_render_contracts` test passed (1/1, 27.37 seconds); and
  `workbench_dof_compare.py` completed on the refreshed baseline/effect pairs.
  The capture-only setting correction made no renderer code changes.

### QPainter supersampled light-depth shadow fallback (2026-10-14)

- The CPU light-depth shadow fallback now rasterizes and samples at 2x the
  viewport width and height when that map fits within a 16M-texel budget
  (64 MiB of float depth). Otherwise it retains the prior viewport-sized map.
  The light-space bounds, depth comparison, bias, receiver factors, and
  nearest-texel lookup are unchanged. A contract checks 2x sizing, budget
  fallback, and invalid dimensions.
- In the matched 640×480 planar receiver fixture, the isolated cast-shadow
  mask IoU against Blender 5.2.2 improves from 0.485697 to 0.494191 at both
  default shift/focus (0.1/0.0) and alternate values (0.25/0.4). classiCAD's
  thresholded shadow mask rises from 4,405 to 4,415 pixels against Blender's
  8,060. Mean RGB delta error on the shared receiver improves from 0.001362
  to 0.001333 at defaults and from 0.002042 to 0.001999 for the alternate
  case. This is a small coverage improvement, not stencil-volume parity; the
  map remains single-sample nearest-texel depth and caster geometry differs
  from Blender's silhouette-volume rasterization.
- Validation: `cmake --build build -j4` passed with the dependency audit, and
  the offscreen `viewport_render_contracts` test passed while capturing both
  shadow setting pairs. `workbench_shadow_receiver_compare.py` completed on
  the default and alternate captures against Blender 5.2.2.

### QPainter polygon shadow-ray experiment (2026-10-15)

- Evaluated a bounded CPU ray-to-light visibility path for opaque polygon
  scenes against the existing 640×480 plane-and-cube shadow fixture. The
  prototype used a closed opaque polygon mesh as an activation requirement,
  retained the current factor/shift/focus resolve, and left open-only scenes
  on the light-depth fallback. This was intended to test whether geometric
  visibility could close the mask gap between QPainter's depth map and
  Blender's stencil shadow volumes.
- The captured QPainter cast-shadow mask contained 72,080 pixels against
  Blender's 8,060 (IoU 0.054377). Mean attenuation on the QPainter mask was
  0.08079, close to Blender's 0.08130, but the much broader mask materially
  worsened coverage compared with the existing supersampled depth fallback's
  4,415 pixels and IoU 0.494191. The ray path was rejected and removed; the
  depth-map approximation remains active. A better bounded shadow-volume or
  receiver-coverage method is still open.
- Validation: Blender reported 5.2.2 LTS; the capture-enabled offscreen
  `viewport_render_contracts` test passed on the gated prototype, and
  `workbench_shadow_receiver_compare.py` completed on matched scene-linear
  captures. After removing the rejected prototype, `cmake --build build -j4`
  and the offscreen `viewport_render_contracts` test passed against the
  retained depth-map implementation.

### QPainter software shadow-volume experiment (2026-10-15)

- Tested a bounded CPU depth-pass shadow-volume rasterizer for closed,
  oriented, opaque imported polygon meshes against the matched 640×480
  plane-and-cube receiver fixture. It used the nearest opaque scene depth for
  volume testing and retained the existing light-depth path for casters that
  did not qualify.
- Against Blender 5.2.2, the prototype produced 4,234 cast-shadow pixels and
  mask IoU 0.485859, compared with 4,415 pixels and IoU 0.494191 for the
  established supersampled light-depth fallback. Mean scene-linear RGB delta
  error on the shared receiver was 0.001332 versus 0.001333, effectively
  unchanged. The prototype was removed because its software depth comparison
  did not improve the isolated shadow mask and introduced a second rasterizer
  plus per-caster path selection. The light-depth fallback remains active;
  software stencil/winding parity is still open.
- Validation of the prototype used Blender 5.2.2 LTS, paired captures from
  the offscreen `viewport_render_contracts` run, and
  `workbench_shadow_receiver_compare.py`. The prototype build and capture run
  completed, but its measured output was rejected. After removing it,
  `cmake --build build -j4` and the offscreen
  `viewport_render_contracts` test passed against the retained path.

### QPainter full-frame MatCap presets (2026-10-14)

- Compared the existing QPainter polygon-cube captures for `check_normal+y`
  and `ceramic_lightbulb` against fresh Blender 5.2.2 Workbench renders. Both
  use 640×480 orthographic framing, Material color, MatCap lighting, AA Off,
  and zero dither; `check_normal+y` uses the same enabled specular setting as
  the QPainter capture. PNG and scene-linear PFM comparisons cover the full
  frame.
- For both presets, the thresholded silhouette masks contain 111,256 Blender
  pixels and 111,878 QPainter pixels (622 XOR; IoU 0.994440), matching the
  previously measured QPainter AA-Off silhouette difference. For
  `check_normal+y`, common-foreground display channel MAE is 1.784/255 (p95
  1/255); scene-linear channel MAE over the full frame is 0.00258 blue,
  0.00467 green, and 0.00499 red. For `ceramic_lightbulb`, display channel
  MAE is 1.414/255 (p95 1/255); scene-linear channel MAE is 0.00380 blue,
  0.00304 green, and 0.00250 red. The larger maxima fall on the silhouette
  band; these captures do not justify changing the interior MatCap resolve.
- This measures two MatCap assets on the current opaque cube and one camera.
  Additional presets, orientations, specular variations, geometry, AA modes,
  and the residual AA-Off silhouette coverage remain open; this is not a
  MatCap parity sign-off.
- A second matched sweep disables specular for `metal_bronze`,
  `check_normal+y`, and `ceramic_lightbulb`. For `check_normal+y` and
  `ceramic_lightbulb`, the same 622-pixel mask XOR and 0.994440 IoU remain;
  common-foreground display MAE/p95 are 1.784/255 and 1/255, and 1.208/255
  and 1/255, respectively. Their full-frame scene-linear channel MAEs (blue,
  green, red) are `(0.002578, 0.004672, 0.004991)` and
  `(0.003328, 0.002610, 0.002121)`. With specular disabled,
  `metal_bronze` produces a black object in both applications; its display
  comparison has 536 mask XOR pixels and IoU 0.995205, but its scene-linear
  comparison has no foreground above the comparator's 0.1 threshold and does
  not provide a useful MatCap color measurement.
- Validation: Blender reported 5.2.2 LTS; `workbench_image_compare.py` and
  `workbench_pfm_compare.py` completed for the initial specular-on and second
  specular-off PNG/PFM pairs. The classiCAD captures came from offscreen
  `viewport_render_contracts` runs; the specular-off run passed (1/1). This
  comparison-only update made no C++ changes and required no rebuild.

### GPU `check_normal+y` MatCap with specular enabled (2026-10-14)

- Captured the GPU production path's `check_normal+y` MatCap with specular
  enabled, Material color, AA Off, and the matched 640×480 orthographic cube.
  Blender 5.2.2 used the same preset, view, world color, and enabled specular
  layer. This fills the specular-on cell alongside the existing specular-off
  result for this preset.
- The display PNG foreground masks contain 111,256 Blender pixels and 111,480
  classiCAD pixels (224 XOR; IoU 0.997991). Common-foreground display channel
  MAE is 0.146/255 with p95 0; silhouette-band MAE is 0.389/255 versus
  0.133/255 in the reference interior. The scene-linear full-frame channel
  MAEs (blue, green, red) are `(0.000547, 0.000122, 0.000505)`. Edge coverage
  remains the main measured difference.
- This verifies one MatCap preset, view, mesh, and specular setting; other
  presets, orientations, geometry, and AA modes remain open. Blender's
  two-layer diffuse/specular MatCap upload is pinned in
  [`workbench_resources.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_resources.cc#L21-L49),
  specular sampling/toggle in
  [`workbench_matcap.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_matcap.bsl.hh#L27-L60),
  and the additive composite in
  [`workbench_composite.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_composite.bsl.hh#L74-L86).
- Validation: Blender reported 5.2.2 LTS; the offscreen
  `viewport_render_contracts` test passed (1/1); and
  `workbench_image_compare.py` plus `workbench_pfm_compare.py` completed for
  the matched GPU and Blender captures. This comparison-only update made no
  C++ changes and required no rebuild.

### Independent weighted OIT equation contract (2026-10-14)

- Replaced the resolver test's circular depth-weight expectations with
  independent Blender 5.2.2 Eq. 7 float values at camera depths 2 and 8
  (`62.49608994` and `3.90623450`), plus signed-depth equivalence and both
  clamp endpoints. The weighted-color result now uses those fixed values
  instead of calling the production helper to compute its expected output.
- Added transparent-film checks for alpha 0, alpha 1, and two-fragment
  multiplicative revealage (`0.625` output alpha). Blender's weight equation
  and accumulation output are pinned in
  [`workbench_transparent_accum_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_transparent_accum_frag.glsl#L435-L470)
  and
  [`workbench_composite.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_composite.bsl.hh#L558-L620).
- This closes the independent scalar/resolver equation contract. It does not
  close the separate GPU/QPainter final-image comparison against Blender for
  transparent multi-surface scenes.
- Validation: `cmake --build build -j4` passed with the dependency audit, and
  `ctest --test-dir build -R '^workbench_scene_linear_resolve_contracts$'
  --output-on-failure` passed (1/1). No production renderer code changed.

### Matched Studio color-source captures (2026-10-14)

- Extended the offscreen lighting capture contract to compare Studio Default,
  specular-on output for Object, Random, Custom, Corner color attribute, Point
  color attribute, constant Image Texture, and patterned UV Image Texture
  sources. GPU and QPainter captures are paired with Blender 5.2.2 at
  640×480, orthographic framing, AA Off, and the same neutral background. The
  color-source selection and per-face Workbench material setup are implemented
  in pinned [`workbench_engine.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_engine.cc#L2020-L2055);
  active color-attribute selection is in pinned
  [`workbench_state.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_state.cc#L1415-L1442).
- Across the six constant-color/constant-UV cases, both renderers have the
  same display mask delta as the existing AA-Off fixture: 224 XOR pixels and
  IoU `0.997991`. The patterned UV case measures 564 XOR / IoU `0.994371`
  for GPU and 473 XOR / IoU `0.995278` for QPainter. The scene-linear PFM
  foreground masks measure 493 XOR / IoU `0.993817` for GPU in the ordinary
  cases (449 / `0.994076` for color attributes) and 226 / `0.997164` for
  QPainter (171 / `0.997742` for color attributes).
- Per-channel scene-linear mean absolute error stays below `0.000361` for GPU
  and `0.000286` for QPainter across the seven cases. Common-foreground display
  MAE stays below `0.35/255` for GPU and `0.30/255` for QPainter. The patterned
  UV comparison has greater perimeter coverage error than the constant-color
  cases; this sweep does not establish parity for additional texture
  projections, filtering/extension/alpha combinations, geometry, views, or
  lighting presets.
- Validation: `cmake --build build -j4` passed with the dependency audit;
  offscreen `viewport_render_contracts` passed (1/1) while writing GPU and
  QPainter PNG/PFM captures; all 14 PNG/PFM pairs passed through
  `workbench_image_compare.py` and `workbench_pfm_compare.py`. Blender
  reported version 5.2.2 LTS. These captures preserve the existing material
  adapter and geometry contracts.

### Matched Workbench texture interpolation modes (2026-10-14)

- Blender 5.2.2 Workbench maps Closest to nearest texture sampling and maps
  Linear, Cubic, and Smart to the same linear GPU sampler in pinned
  [`workbench_materials.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_materials.cc#L39-L73);
  its Workbench shader samples that prepared texture directly in
  [`workbench_image_lib.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_image_lib.glsl#L37-L64).
  A matched 640×480 Studio Default cube capture uses the same 2×2 patterned
  image and UVs, AA Off, zero dither, and neutral World background. Blender's
  Linear/Cubic/Smart scene-linear PFM files are byte-identical; their display
  PNG pixels also compare exactly. classiCAD GPU and QPainter likewise produce
  byte-identical PNG/PFM output for those three modes. Closest visibly differs
  in each renderer, as expected.
- The matched Closest comparison exposed a QPainter nearest-row discrepancy
  at exact V texel boundaries. Its byte-image sampler now uses the lower
  top-down row at the boundary, corresponding to Blender's bottom-up texture
  upload and nearest GPU sample; the GPU sampler and interpolation selection
  were unchanged. After this correction, QPainter Closest has 224 display-mask
  XOR pixels / IoU `0.997991`, common-foreground display MAE `0.146/255`
  (p95 `1/255`), and scene-linear channel MAE `(0.000137, 0.0000644,
  0.000129)` for blue/green/red. Its PFM mask has 226 XOR pixels / IoU
  `0.997164`. GPU Closest has 224 display-mask XOR / IoU `0.997991`, display
  MAE `0.290/255`, and PFM mask 493 XOR / IoU `0.993817`; its scene-linear
  channel MAE is `(0.000114, 0.000217, 0.000261)`. The remaining mask
  differences are raster coverage, not texture filtering.
- This verifies one patterned opaque cube and one camera for all four
  interpolation settings. Alpha filtering, additional texture sizes, geometry,
  extension modes, and views remain open. Validation: `cmake --build build
  -j4` passed with the dependency audit; offscreen `viewport_render_contracts`
  passed (1/1); Blender reported 5.2.2 LTS; and the eight matched GPU/QPainter
  PNG/PFM mode pairs completed with `workbench_image_compare.py` and
  `workbench_pfm_compare.py`.

### QPainter UDIM row selection (2026-10-14)

- Blender 5.2.2 selects a tiled-image layer by flooring the original UV in
  [`workbench_image_lib.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_image_lib.glsl#L348-L412).
  Source review found that QPainter byte and scene-linear float color sampling
  instead flipped V before calculating the row, causing upper UDIM rows such
  as tile 1011 to resolve as missing. The mesh raster adapter already stores
  V-flipped UVs for its top-down image path, so the tiled branch now recovers
  the original Blender UV for tile selection. The byte sampler then flips the
  tile-local V for QImage rows, while the float sampler passes local Blender V
  to its own row-flipping sampler. The alpha sampler already selected tiles
  from original UVs.
- Validation: `cmake --build build -j4` passed with the dependency audit.
  Offscreen CTest `viewport_render_contracts` passed (1/1, 38.80 seconds) with
  `CLASSICAD_VIEWPORT_LIGHTING_CAPTURE_DIR` set, which activates the vertical
  UDIM checks. Its byte tiles 1001/1011 use distinct colors and asymmetric
  alpha, and the float variant requires scene-linear fragments from both
  rows. No matched Blender render comparison was made for the UDIM fix itself.

### QPainter scene-linear float UDIM row contract (2026-10-14)

- Extended the offscreen vertical UDIM mesh fixture with distinct 1×1
  scene-linear float payloads for tiles 1001 and 1011. It counts scene-linear
  QPainter fragments by their dominant red/green tile color and requires
  samples from both rows. This directly covers the float color sampler's
  tile selection while preserving the existing image-byte and asymmetric
  alpha checks.
- Validation: the capture-enabled offscreen CTest `viewport_render_contracts`
  passed (1/1, 38.80 seconds), including this float-row assertion. This is a
  renderer contract. A matched Blender float-UDIM render comparison is
  recorded below.

### Matched scene-linear float UDIM comparison (2026-10-14)

- Added an opt-in Blender 5.2.2 reference that writes two 32-bit scene-linear
  EXR tiles (1001 and 1011), loads them as one tiled Image Texture, and renders
  the same planar two-triangle mesh with V UVs spanning `[0.05, 1.95]`. The
  classiCAD capture uses the same one-pixel-per-tile RGBA32F payloads in both
  GPU and QPainter Texture paths. Blender's pinned
  [`workbench_image_lib.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_image_lib.glsl#L34-L60)
  samples the selected UDIM layer with its normal image sampling path.
- The 640×480 scene-linear GPU comparison has foreground-mask IoU `0.999937`
  (2 XOR pixels) and per-channel MAE `(blue, green, red)` of
  `(0.00000126, 0.0000305, 0.0000302)`. QPainter has mask IoU `1.0` (0 XOR)
  and channel MAE `(0.00000036, 0.0000125, 0.0000125)`. Both display PNGs have
  zero channel error in the reference interior; the remaining errors are
  concentrated at raster edges. This validates the two vertical tiles for
  one mesh, camera, and Linear filter, not the full float image/alpha matrix.
- Validation: `cmake --build build -j4` passed with the dependency audit;
  capture-enabled offscreen CTest `viewport_render_contracts` passed (1/1,
  38.91 seconds); Blender reported 5.2.2 LTS; and
  `workbench_pfm_compare.py` / `workbench_image_compare.py` completed for GPU
  and QPainter output. The reference writes each tile's source pixels as
  Non-Color before saving, preserving the intended linear EXR values.

### Workbench Image Texture extension-mode comparison (2026-10-14)

- Added opt-in matched captures for Blender 5.2.2 and classiCAD GPU/QPainter
  using the existing nonblank polygon cube and asymmetric 2×2 sRGB texture.
  The cube's X/Z-derived UVs are scaled into `[-0.5, 1.5]` so Repeat, Extend,
  Clip, and Mirror are all exercised outside the image bounds. Blender's
  reference helper now accepts an Image Texture extension and patterned-UV
  scale/offset. Workbench 5.2.2 maps these sampler modes in pinned
  [`workbench_materials.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_materials.cc#L451-L540).
- For Repeat, Extend, and Mirror, GPU and QPainter display captures each have
  the same foreground-mask result as the existing cube edge fixture: 224 XOR
  pixels and IoU `0.997991`. In shared foreground regions, GPU channel MAE is
  `0.0157–0.0508/255` and QPainter channel MAE is `0.2598–0.3081/255` across
  those three modes. Clip yields GPU 5 XOR / IoU `0.999885` and QPainter 57
  XOR / IoU `0.998690`; common-foreground channel MAE is `0.0667/255` GPU and
  `0.2788/255` QPainter. The localized Clip mask difference is consistent with
  coverage at transparent image borders; no sampler or production code change
  is justified by this sweep.
- Validation: `cmake --build build -j4` passed with the dependency audit;
  capture-enabled offscreen CTest `viewport_render_contracts` passed (1/1,
  38.80 seconds). Blender reported 5.2.2 LTS for all four references, and
  `workbench_image_compare.py` completed for eight GPU/QPainter PNG pairs.
  The captures use Flat lighting, AA Off, zero dither, the matched orthographic
  cube camera, and a neutral World background. This does not establish alpha
  cutout parity for all extension/filter combinations or other geometries.

### Matched GPU cavity effect comparison (2026-10-14)

- Captured baseline and World Cavity output from the offscreen classiCAD GPU
  viewport contract and Blender 5.2.2 using the shared 640×480 orthographic
  cube fixture, AA Off, zero dither, and matched neutral background. Cavity
  toggles are measured against each application's own baseline so the
  comparison isolates the rendered effect rather than baseline shading.
- At a 3/255 RGB change threshold, the effect masks contain 27,109 Blender
  pixels and 27,212 classiCAD pixels, with 1,135 XOR pixels and IoU
  `0.959067`. On their union, the signed RGB-delta error is MAE `0.25031/255`,
  p95 `1/255`, and maximum `20/255`. This records a bounded cavity effect
  mismatch; it does not compare Blender's internal cavity buffers, and the
  mask result does not justify a shader adjustment by itself.
- Blender 5.2.2 builds a 64×64 cavity jitter texture in pinned
  [`workbench_resources.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_resources.cc#L74-L98)
  and samples it in
  [`workbench_cavity.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_cavity.bsl.hh#L41-L63).
  Validation: both background Blender captures reported 5.2.2 LTS; the
  offscreen classiCAD capture contract passed; and
  `workbench_effect_delta_compare.py` completed at threshold 3/255. No source
  code changed, so no rebuild was required.

### GPU cavity jitter texel contract (2026-10-15)

- Blender 5.2.2 constructs one 64×64 texture from EEVEE's two blue-noise
  channels. Its red/green channels become cosine/sine rotations; its blue
  channel is the clamped radius offset divided by the total cavity sample
  count. Workbench uploads the texture as floating-point RGBA16F with linear
  filtering and repeat wrapping, and its cavity shader samples one repeating
  texel per viewport pixel. References are pinned to
  [`workbench_resources.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_resources.cc#L74-L98),
  [`workbench_cavity.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_cavity.bsl.hh#L41-L63),
  and the
  [EEVEE blue-noise LUT](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/eevee/eevee_lut.cc#L6190-L6200).
- classiCAD stores the same row-major 64×64 RG LUT quantized to 1/256,
  converts every texel through the Workbench rotation/radius equations, and
  uploads the RGB channels as RGB16F with linear filtering and repeat
  wrapping. The pure construction helper is shared by the renderer and its
  focused contract. For sample counts 1, 16, and 512, the contract compares
  all 4,096 generated texels per channel against the pinned conversion and
  checks the full 8,192-byte LUT payload against SHA-256
  `fd9b4942a1124232212fc8226a62eb4f687532df24bcbc60db19ac9405164ec6`.
- The upload setup is shared by `ViewportSurfaceRenderer` and a focused
  offscreen GL contract. At sample counts 1, 16, and 512, that contract checks
  the uploaded 64×64 RGB16F internal format, linear min/mag filtering, repeat
  S/T wrapping, and reads every texel back as float after half-float storage.
  All returned RGB values match the generated values after binary16 rounding.
  Blender stores RGBA16F; its fourth channel is not consumed by the cavity
  shader, so classiCAD keeps the compared RGB payload in RGB16F.
- The CPU and GL contracts isolate LUT conversion and upload state. They do
  not constitute a full cavity image-parity claim; the earlier controlled GPU
  effect comparison remains the rendered-output check, with effect-mask IoU
  `0.959067` and union signed-RGB-delta MAE `0.25031/255`.
- Validation: `cmake --build build -j4`, focused
  `workbench_cavity_jitter_contracts` and
  `workbench_cavity_jitter_upload_contracts`, and the full offscreen
  `viewport_render_contracts` CTest all pass. No GUI was launched.

### Matched QPainter cavity effect comparison (2026-10-14)

- Repeated the same baseline-versus-World-cavity effect comparison through
  classiCAD's QPainter fallback, using the shared 640×480 orthographic cube,
  AA Off, zero dither, and matched cavity controls against Blender 5.2.2.
  The offscreen `viewport_render_contracts` test passed with QPainter PNG and
  PFM captures enabled.
- At a 3/255 RGB threshold, Blender changes 27,109 pixels and QPainter
  changes 15,732; the effect-mask IoU is `0.164347`. Across the mask union,
  signed RGB-delta MAE is `5.38365/255`, p95 `13/255`, and maximum `33/255`.
  The GPU path on this fixture measured IoU `0.959067` and union MAE
  `0.25031/255`, so the CPU fallback remains a substantially weaker cavity
  approximation.
- This confirms an open QPainter parity gap, rather than a solved category.
  The fallback evaluates a discrete software neighborhood against retained
  raster samples, while Blender 5.2.2 uses its screen-space depth/normal
  cavity shader and blue-noise sampling. Existing scene-linear captures and
  source audits do not isolate one bounded formula correction. Do not tune
  the production equation from this output comparison alone. Blender
  references remain pinned to
  [`workbench_cavity.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_cavity.bsl.hh#L41-L100)
  and
  [`workbench_effect_cavity.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_effect_cavity.cc#L52-L68).
- Reproduce with the `CLASSICAD_VIEWPORT_QPAINTER_CAVITY_*` capture variables,
  Blender's `tests/workbench_cube_reference.py --effect BASELINE|CAVITY`,
  and `tests/workbench_effect_delta_compare.py` at threshold 3. No C++ source
  changed, so no rebuild was required. Full QPainter cavity parity remains
  open.

### QPainter cavity factor mismatch diagnostic (2026-10-15)

- Added opt-in pre-composite factor capture to the QPainter compositor and
  saved the GPU cavity shader's combined factor channel beside its existing
  amount, encoded normal, and depth captures. The comparison uses the same
  640×480 orthographic cube and World cavity settings as the existing matched
  effect comparison. Captures are scalar PFM files with rows converted to the
  same top-left convention by
  `tests/workbench_qpainter_cavity_diagnostic.py`.
- The visible effect comparison reproduces its previous result: 27,109
  Blender effect pixels, 15,732 QPainter effect pixels, and IoU `0.164347` at
  3/255. Across common baseline foreground, GPU versus QPainter factor MAE is
  `0.049919`, p95 `0.212904`, and maximum `0.647132`. At the ten largest
  output-delta mismatch pixels, GPU factors exceed QPainter factors by
  `0.510864` to `0.580889`; the largest output error is 33/255 at `(355,289)`
  where the factors are `1.274414` and `0.725690`. This verifies that the
  mismatch includes a cavity-factor difference, while the captures do not
  yet attribute it to a particular sampled depth, normal, or visibility tap.
  The production equation remains unchanged pending that attribution.
- Validation: `cmake --build build -j4` passed; capture-enabled offscreen
  `viewport_render_contracts` passed (1/1, 28.55 seconds); and the diagnostic
  script reproduced the recorded mask and factor metrics. Reproduce the
  captures with `CLASSICAD_VIEWPORT_CAVITY_DIAGNOSTIC_PREFIX` and
  `CLASSICAD_VIEWPORT_QPAINTER_CAVITY_DIAGNOSTIC_PREFIX`, together with the
  `CLASSICAD_VIEWPORT_QPAINTER_CAVITY_CAPTURE` and
  `CLASSICAD_VIEWPORT_QPAINTER_CAVITY_BASELINE_CAPTURE` paths. This diagnostic
  does not establish full QPainter cavity parity.

### QPainter cavity projected-radius correction (2026-10-15)

- `rasterWorkbenchCavityStep()` received a cavity radius already converted to
  pixels, then divided that value by two a second time. Blender 5.2.2's cavity
  shader applies its half factor while converting a normalized UV offset; in
  the matched 640×480 orthographic view with `cavityDistance=0.2`, the CPU
  pixel-space base radius is already 25.6 px. The QPainter path now retains
  that full projected radius. A focused contract requires a 20 px input step
  to remain 20 px in orthographic views, while retaining the existing
  perspective receiver-depth scaling.
- The measured effect masks grew toward Blender's coverage, but did not
  establish factor parity. On the opaque cube at 3/255, Blender/QPainter
  changed-pixel counts moved from 27,109/15,732 to 27,109/31,875 and mask IoU
  from `0.164347` to `0.203362`. GPU/QPainter combined-factor MAE on common
  baseline foreground increased from `0.049919` to `0.067398` (p95 from
  `0.212904` to `0.242188`), so the wider footprint improves mask overlap but
  exposes remaining differences in sampling, visibility, or factor
  accumulation. On the transparent overlap fixture, QPainter changed pixels
  increased from 1,200 to 3,490 against Blender's 20,751; mask IoU improved
  from `0.057829` to `0.168185`. The scene-linear mask IoU improved from
  `0.051458` to `0.149657`, and full-frame RGB effect-delta MAE decreased from
  `0.00233283` to `0.00204065`. Cavity alpha remains unchanged by the effect.
- Validation: `cmake --build build -j4` passed; capture-enabled offscreen
  `viewport_render_contracts` passed (1/1, 30.00 seconds); the diagnostic
  capture and cube comparator completed; and both scene-linear and display
  effect-delta comparisons completed for the transparent overlap fixture.
  The changed radius is a verified correction to the CPU projection scale;
  QPainter cavity factor and full image parity remain open. Blender's matching
  shader conversion is pinned to
  [`workbench_cavity.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_cavity.bsl.hh#L463-L585)
  and GPU projection setup to
  [`workbench_effect_cavity.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_effect_cavity.cc#L416-L465).

### QPainter cavity projected radius above 64 px (2026-10-15)

- Removed the QPainter path's 64-pixel upper clamp from its projected cavity
  radius. Blender 5.2.2 computes the screen-space offset from the projection
  matrix, configured cavity distance, and receiver depth, then samples the
  resulting projected neighborhood without an equivalent 64-pixel limit.
  The CPU path preserves the resulting subpixel radius and its independent
  32-million sample-work ceiling, so removing the radius clamp does not
  increase the number of per-pixel taps.
- Added an opt-in shared cavity-distance override to the paired Blender and
  classiCAD diagnostic fixtures. At `cavityDistance=0.6`, the matched
  640×480 orthographic cube projects a radius of 76.8 px in classiCAD's
  `ViewportTransform`, exercising the former clamp. The capture-enabled
  offscreen `viewport_render_contracts` test passes; the per-tap comparator
  reports 48/64 exact GPU/QPainter texel matches and no error beyond one
  pixel. The paired Blender 5.2.2 capture confirms the remaining broad fallback
  gap: at a 3/255 threshold its cavity effect mask contains 34,505 pixels,
  compared with QPainter's 65,029 (IoU `0.258284`). This corrects the
  source-derived radius range but does not claim that the CPU approximation
  matches Blender's per-tap depth/normal contribution or final cavity result.
- Reproduce with
  `CLASSICAD_VIEWPORT_CAVITY_DIAGNOSTIC_DISTANCE=0.6`, the GPU and QPainter
  cavity diagnostic prefixes, and
  `blender -b --python tests/workbench_cube_reference.py -- <output.png>
  --effect CAVITY --cavity-distance 0.6 --lighting FLAT --no-specular
  --match-classicad-ortho --view-direction 4 -6 4 --ortho-scale 5.0 --aa OFF
  --zero-dither`. Blender's no-cap projection and depth scaling are pinned to
  [`workbench_cavity.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_cavity.bsl.hh#L482-L520).
  Validation: `cmake --build build -j4`, Python fixture syntax compilation,
  capture-enabled offscreen `viewport_render_contracts` (1/1), and the
  per-tap/effect-delta comparators passed. No GUI was launched.

### QPainter cavity subpixel projected radius (2026-10-15)

- The QPainter cavity path no longer forces the projected sample radius or its
  perspective-adjusted step to at least one pixel. Blender 5.2.2 projects the
  configured cavity distance directly through the view projection and samples
  at the resulting fractional screen offset; its source contains no one-pixel
  floor ([`workbench_cavity.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_cavity.bsl.hh#L48-L55), sample reconstruction and attenuation at lines 63-95).
- The focused offscreen contract now checks a 0.25-pixel radius remains 0.25
  in orthographic projection and scales to 1.25 pixels at the tested
  perspective receiver depth. The zero-distance radius is zero, and the
  existing 76.8-pixel case remains uncapped. `cmake --build build -j4` passed
  the 320-source dependency audit and `viewport_render_contracts` passed
  offscreen (1/1). A matched Blender/QPainter comparison at distance `0.002`
  found the CPU effect mask was empty before fractional-position
  reconstruction, while Blender changed 68,358 pixels at 3/255. The
  subsequent fractional-position comparison is recorded below. Broader
  QPainter cavity parity remains open. No GUI was launched.

### QPainter cavity blue-noise row orientation (2026-10-15)

- The GPU derives cavity jitter coordinates from `gl_FragCoord`, so its
  repeating blue-noise texture is addressed in bottom-up framebuffer rows.
  QPainter's retained raster samples are top-down. The CPU now maps row `y`
  to `height - 1 - y` before looking up the same 64×64 LUT, preserving the
  horizontal coordinate and the existing projected-radius and perspective
  depth scaling.
- The offscreen `viewport_render_contracts` run passed with GPU/QPainter
  factor and effect captures enabled. On common foreground, factor p95 moved
  from `0.242188` to `0.240885`, while MAE changed from `0.067398` to
  `0.067453`; cube mask IoU moved from `0.203362` to `0.201666`. On the
  transparent overlap fixture, mask IoU moved from `0.168185` to `0.167317`
  and full-frame scene-linear RGB effect-delta MAE from `0.00204065` to
  `0.00203833`. These aggregate results show no material parity gain, but the
  row conversion matches the GPU texture-coordinate convention. Cavity factor
  and effect parity remain open; the source-derived row correction does not
  justify any further change to the cavity accumulation equation.
- Reproduce the factor captures with
  `CLASSICAD_VIEWPORT_CAVITY_DIAGNOSTIC_PREFIX` and
  `CLASSICAD_VIEWPORT_QPAINTER_CAVITY_DIAGNOSTIC_PREFIX`, and run
  `tests/workbench_qpainter_cavity_diagnostic.py`. Blender 5.2.2 source
  references are pinned to
  [`workbench_cavity.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_cavity.bsl.hh#L463-L585)
  for `gl_FragCoord`-derived texture coordinates and the GPU resolve in
  [`viewport_surface_renderer.cpp`](src/ui/viewport/viewport_surface_renderer.cpp).

### QPainter cavity signed-radius clamp audit (2026-10-15)

- Blender 5.2.2 adds its per-pixel jitter directly to each sample radius,
  while QPainter wraps the same sum in `max(0, ...)`. The clamp looks
  different in isolation, but it is unreachable for the supported sequence:
  the smallest base radius is `0.5 / requestedSamples`, and the jitter is
  clamped above `-0.5 / requestedSamples` (specifically `-0.499 / N`). Every
  generated radius stays positive, including when the CPU work budget
  subsamples the requested sequence. Removing the clamp would therefore make
  no change to the rendered result; no production change was retained.
- The post-audit matched cube capture reproduced the existing QPainter factor
  MAE `0.067453`, p95 `0.240885`, and cavity-mask IoU `0.201666` against
  Blender 5.2.2 at 3/255. This confirms that the signed-radius syntax
  difference does not explain the open cavity gap. The remaining factor and
  effect mismatch still needs an attributable source-backed correction.
- Blender's sample-radius and jitter addition are pinned in
  [`workbench_cavity.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_cavity.bsl.hh#L503-L520);
  classiCAD's jitter bound and CPU sequence are in
  [`viewport_cavity_jitter.h`](src/ui/viewport/viewport_cavity_jitter.h)
  and [`viewport_raster_workbench_effects.cpp`](src/ui/viewport/viewport_raster_workbench_effects.cpp).
- Validation: capture-enabled offscreen `viewport_render_contracts` passed;
  `workbench_qpainter_cavity_diagnostic.py` compared the new capture against
  the matching Blender and GPU files. No renderer change remained to build;
  no GUI was launched.

### GPU per-face mesh texture-slot comparison (2026-10-14)

- Added an opt-in GPU capture of the existing two-triangle planar mesh with
  two distinct sRGB 2×2 Image Textures assigned by face material index. The
  capture shares geometry, UVs, Linear filtering, Repeat extension, Flat
  lighting, 640×480 orthographic framing, and the neutral scene-linear
  background with Blender 5.2.2's `MESH_TEXTURE` reference. The Blender
  helper already supports the second texture through
  `--texture-image-secondary`.
- The display foreground masks contain 31,794 Blender and 31,792 classiCAD
  pixels (2 XOR; IoU `0.999937`). On the shared foreground after excluding a
  two-pixel silhouette border, display channel MAE is `0.040/255`, p95 is
  `0/255`, and maximum is `2/255`. Scene-linear full-frame channel MAE is
  `6.4e-6` blue, `6.9e-6` green, and `9.3e-6` red. The PFM foreground mask
  also has 2 XOR pixels and IoU `0.999937`; its larger maximum channel errors
  occur at raster edges. This establishes GPU image selection for two opaque
  per-face slots in this planar case and gives no evidence for a renderer
  change.
- Blender source for per-material texture selection is pinned to
  [`workbench_materials.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_materials.cc#L451-L542)
  and mesh texture pass setup to
  [`workbench_mesh_passes.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_mesh_passes.cc#L1051-L1103).
  Validation: `cmake --build build -j4` passed with the dependency audit;
  capture-enabled offscreen CTest `viewport_render_contracts` passed (1/1,
  38.57 seconds); Blender reported 5.2.2 LTS; and the PNG/PFM comparisons
  completed. Reproduce with
  `CLASSICAD_VIEWPORT_LIGHTING_CAPTURE_DIR=<dir>` and Blender's
  `--scene MESH_TEXTURE --texture-image workbench-texture-pattern.png
  --texture-image-secondary workbench-texture-pattern-secondary.png`, setting
  `--world-color 0.0219421387 0.0219421387 0.0219421387` to match the captured
  background. This does not cover alpha-mixed slots, perspective, or other
  mesh geometries.

### Per-face mesh Image Texture alpha cutout (2026-10-14)

- Extended the same two-triangle mesh so slot 0 retains its opaque patterned
  image while slot 1 uses a separate 4×4 sRGB image with alpha 0 in the left
  half and alpha 1 in the right half. Flat lighting, Linear filtering, Repeat,
  AA Off, AgX, and the matched neutral World background are shared by Blender
  5.2.2, GPU, and QPainter. The offscreen contract compares the cutout against
  the opaque two-slot baseline, projects three interior UV probes, verifies
  that the primary slot is unchanged, the transparent secondary probe reveals
  the background, the retained secondary probe remains visible, and total
  foreground coverage decreases in both classiCAD backends.
- At a 5/255 PNG foreground threshold, the GPU baseline/cutout masks compare
  to Blender with IoU `0.999937` / `0.999757` (2 / 6 XOR pixels); QPainter
  compares with IoU `1.0` / `0.999594` (0 / 10 XOR). On common foreground
  pixels, display channel MAE is `0.0399/255` GPU and `0.2170/255` QPainter
  for the opaque baseline, and `0.0260/255` GPU and `0.1873/255` QPainter for
  the alpha cutout. Scene-linear full-frame channel MAE for the cutout is
  `(blue, green, red) = (3.23e-6, 1.48e-5, 5.30e-6)` GPU and
  `(7.85e-5, 1.03e-4, 1.05e-4)` QPainter. The remaining mask differences are
  localized to raster edges and the texture-alpha boundary; the result does
  not justify a renderer change for this fixture.
- Blender 5.2.2 selects the active Image Texture independently for each
  material batch in
  [`workbench_materials.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_materials.cc#L451-L542).
  Its pinned
  [`workbench_image_lib.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_image_lib.glsl#L416-L433)
  unpremultiplies PREMUL inputs and discards STRAIGHT/PREMUL samples below
  `image_transparency_cutoff`; NONE and CHANNEL_PACKED disable this cutoff.
  This fixture uses only 0/1 alpha so the strict `< 0.1` boundary is not
  ambiguous. It does not cover partial alpha, alternative filter/extension
  modes, topology changes, perspective, or broader mesh geometries.
- Validation: `cmake --build build -j4` passed with the 316-file dependency
  audit; capture-enabled offscreen CTest `viewport_render_contracts` passed
  (1/1, 38.69 seconds); Blender reported 5.2.2 LTS; and
  `workbench_image_compare.py` plus `workbench_pfm_compare.py` completed for
  the opaque baseline and cutout through GPU and QPainter. Reproduce the
  classiCAD captures with `CLASSICAD_VIEWPORT_LIGHTING_CAPTURE_DIR=<dir>` and
  Blender with `--scene MESH_TEXTURE --match-classicad-ortho --aa OFF
  --lighting FLAT --color-source TEXTURE --texture-interpolation Linear
  --texture-alpha-mode STRAIGHT`, `--texture-image` set to the opaque primary,
  and `--texture-image-secondary` set to the chosen opaque or cutout secondary.
  Match the World background with
  `--world-color 0.0219421387 0.0219421387 0.0219421387`.

### Per-face mesh Image Texture alpha cutoff boundary (2026-10-14)

- Extended the matched two-slot mesh fixture with uniform secondary images at
  alpha `25/255` and `26/255`, plus a `CHANNEL_PACKED` case at `25/255`.
  The offscreen contract captures GPU and QPainter output, verifies the
  straight-alpha sample below the cutoff reveals the World background, the
  sample just above it remains visible, `CHANNEL_PACKED` bypasses the cutoff,
  and the opaque primary face remains unchanged.
- Blender 5.2.2's pinned
  [`workbench_image_lib.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_image_lib.glsl#L385-L433)
  unpremultiplies PREMUL RGB, discards STRAIGHT/PREMUL samples only when alpha
  is strictly below the cutoff, then returns RGB without forwarding sampled
  alpha to transparent accumulation. Its
  [`workbench_materials.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_materials.cc#L536-L553)
  disables that cutoff for NONE/Ignore and CHANNEL_PACKED. Therefore fractional
  Image Texture alpha above the cutoff is opaque Workbench color, not an OIT
  opacity value; this category validates the cutoff boundary and bypass rather
  than claiming partial-alpha blending parity.
- At a 5/255 display foreground threshold, the GPU masks compare to Blender
  with IoU `0.999937` and 1 XOR pixel below the cutoff, and IoU `0.999937` and
  2 XOR pixels above it or with CHANNEL_PACKED. QPainter masks match exactly
  (IoU `1.0`, 0 XOR) for all three cases. On the displayed foreground interior,
  GPU channel MAE is `0.0398/255` below and `0.0201/255` above; QPainter is
  `0.2841/255` below and `0.1452/255` above. Full-frame scene-linear RGB MAE
  for the retained STRAIGHT and CHANNEL_PACKED captures is `(blue, green,
  red) = (3.14e-6, 5.77e-6, 5.13e-6)` GPU and `(7.86e-5, 9.99e-5,
  1.05e-4)` QPainter. The remaining differences are raster-edge coverage and
  QPainter's small scene-linear color error; no renderer adjustment is indicated
  by this fixture.
- Validation: `cmake --build build -j4` passed with the dependency audit;
  capture-enabled offscreen CTest `viewport_render_contracts` passed (1/1,
  40.04 seconds); Blender references reported 5.2.2 LTS; and the paired PNG/PFM
  comparisons completed for both backends. Reproduce the classiCAD captures
  with `CLASSICAD_VIEWPORT_LIGHTING_CAPTURE_DIR=<dir>` and Blender with
  `--scene MESH_TEXTURE --match-classicad-ortho --aa OFF --lighting FLAT
  --color-source TEXTURE --texture-interpolation Linear
  --texture-image` set to the opaque primary, `--texture-image-secondary` set
  to a generated boundary image, and `--texture-alpha-mode STRAIGHT` or
  `CHANNEL_PACKED`. Match the World with
  `--world-color 0.0219421387 0.0219421387 0.0219421387`.

### Matched GPU mesh texture alpha-mode comparison (2026-10-14)

- Extended the existing QPainter mesh alpha-mode capture to the GPU path for
  `STRAIGHT`, `PREMUL`, `CHANNEL_PACKED`, and `NONE`, using the same 2×2 sRGB
  image, Linear filtering, Repeat extension, Flat lighting, AA Off, and
  640×480 orthographic camera as Blender 5.2.2. The offscreen contract checks
  that GPU STRAIGHT/PREMUL masks contain the expected cutoff holes and that
  CHANNEL_PACKED/NONE retain those samples.
- Against Blender, GPU display-mask IoU is `0.999856` (3 XOR pixels) for both
  STRAIGHT and PREMUL, and `0.999937` (2 XOR pixels) for CHANNEL_PACKED and
  NONE. On the shared PNG foreground away from the silhouette, display-channel
  MAE is `0.0404/255` STRAIGHT, `1.275/255` PREMUL, and `0.0424/255` for each
  cutoff-disabled mode. Scene-linear error was measured over the common
  foreground after excluding a two-pixel silhouette border and six pixels
  around the shared triangle diagonal. Per-channel MAE `(blue, green, red)` is
  `(7.14e-5, 4.42e-5, 4.56e-5)` for STRAIGHT, `(0.00464, 0.00310, 0.00122)`
  for PREMUL, and `(7.05e-5, 4.43e-5, 3.56e-5)` for CHANNEL_PACKED and NONE.
  The larger PREMUL difference is consistent with the QPainter comparison and
  is concentrated in filtered low-alpha texture regions; this fixture does not
  isolate whether image decoding or filtering accounts for it, so it does not
  justify a production sampling change.
- Blender 5.2.2 unpremultiplies PREMUL samples before returning texture RGB and
  applies the fragment cutoff for STRAIGHT/PREMUL in pinned
  [`workbench_image_lib.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_image_lib.glsl#L416-L433).
  Its
  [`workbench_materials.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_materials.cc#L536-L553)
  disables cutoff for CHANNEL_PACKED and NONE/Ignore. The matched Blender helper
  applies the selected alpha mode to both mesh texture materials so both
  triangles share the same controlled image interpretation.
- Validation: `cmake --build build -j4` passed with the dependency audit;
  capture-enabled offscreen CTest `viewport_render_contracts` passed (1/1,
  39.61 seconds); all four Blender references reported 5.2.2 LTS; and matched
  PNG/PFM comparisons completed for GPU and QPainter. Reproduce the classiCAD
  matrix with `CLASSICAD_VIEWPORT_LIGHTING_CAPTURE_DIR=<dir>` and Blender with
  `--scene MESH_TEXTURE --match-classicad-ortho --aa OFF --lighting FLAT
  --color-source TEXTURE --texture-interpolation Linear
  --texture-extension REPEAT --texture-image` set to the selected straight or
  premultiplied capture asset, pass that same file as
  `--texture-image-secondary`, and select each `--texture-alpha-mode`. Match
  World color with `--world-color 0.0219421387 0.0219421387 0.0219421387`.

### Matched GPU perspective mesh texture comparison (2026-10-14)

- Extended the varying-UV two-triangle perspective fixture with a GPU capture,
  using the same mesh, texture, camera transform, 640×480 target, Flat lighting,
  AA Off, and World color as its QPainter capture and Blender 5.2.2 reference.
  The GPU capture's PFM scene-linear RGB matches Blender exactly across the
  frame (zero error in all channels), and its display foreground mask has IoU
  `1.0` with zero XOR pixels. Display-channel MAE is `0.03395/255` across the
  shared foreground. The QPainter capture remains at IoU `0.988615` with 419
  XOR pixels and scene-linear per-channel MAE `(blue, green, red) =
  (0.000462, 0.000615, 0.000807)`; this reiterates its known perspective
  raster-edge and interpolation differences without isolating a production
  correction.
- Blender 5.2.2's pinned
  [`workbench_prepass.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_prepass.bsl.hh#L100-L105)
  declares the mesh UV varying smooth, and
  [`workbench_image_lib.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_image_lib.glsl#L385-L433)
  samples the interpolated coordinates. The GPU result validates classiCAD's
  perspective-correct mesh texture path for this planar two-triangle fixture;
  it does not cover curved or non-coplanar meshes, alternate filters/extensions,
  or broader perspective camera ranges.
- Validation: `cmake --build build -j4` passed with the dependency audit;
  capture-enabled offscreen CTest `viewport_render_contracts` passed (1/1,
  39.06 seconds); the Blender reference reported 5.2.2 LTS; and paired PNG/PFM
  comparisons completed for GPU and QPainter. Reproduce the classiCAD captures
  with `CLASSICAD_VIEWPORT_LIGHTING_CAPTURE_DIR=<dir>` and Blender with
  `--scene MESH_TEXTURE --match-classicad-perspective --view-direction 4 -6 4
  --aa OFF --lighting FLAT --color-source TEXTURE --texture-interpolation
  Linear --texture-image <fixture> --world-color 0.0219421387 0.0219421387
  0.0219421387 --zero-dither`; the scene-linear capture uses
  `--linear-output` and `--linear-pfm-output`.

### QPainter committed-curve scene-depth visibility (2026-10-14)

- The QPainter surface compositor retains regular scene depth per physical
  target pixel by combining the nearest opaque depth with accepted regular
  transparent fragments. Committed NURBS curve strokes use that map to split
  prepared tessellation segments into visible runs; when prepared segments are
  unavailable, the renderer creates bounded sampled curve segments and applies
  the same test. The depth check uses orthographic linear depth or
  perspective-correct reciprocal depth and is capped at one million samples
  per curve. Disconnected curve components start separate runs. The retained
  map excludes `showInFront` geometry and does not replace the separate opaque
  and transparent depth data used by other effects.
- A production-renderer contract composes an opaque surface blocker, then draws
  a valid degree-one NURBS line with a portion behind the blocker and portions
  outside it. It verifies that the prepared and direct sampled paths leave the
  blocked center pixel identical to the blocker-only image, preserve the
  exposed stroke, and that the no-depth baseline still draws through the
  blocker. The test also confirms the compositor retains finite blocker depth
  and marks an uncovered pixel as empty.
- In-front objects now retain their own physical-target scene-depth map, built
  from in-front opaque depth plus accepted in-front transparent fragments in
  both the weighted and sorted compositor routes. The viewport passes the
  regular map to regular objects and the in-front map to `showInFront` objects,
  so those strokes ignore regular geometry while respecting nearer in-front
  geometry. Contracts cover regular-depth bypass, in-front opaque rejection,
  and nearer/farther in-front transparent cases in both compositor routes.
- Blender 5.2.2's Workbench draws the regular transparent-depth pass after
  transparent accumulation, and Overlay wireframe depth-tests strokes against
  the scene depth texture in pinned
  [`workbench_engine.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_engine.cc#L243-L252)
  and
  [`overlay_wireframe.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/overlay/overlay_wireframe.hh#L948-L970).
  The production contract keeps the opaque blocker regression and no-depth
  baseline, then verifies a translucent plane hides the curve only when nearer,
  while the exposed stroke remains visible. The offscreen renderer contract
  verifies that `showInFront` strokes bypass regular blockers, are hidden by
  nearer in-front opaque and transparent surfaces, and remain visible ahead of
  farther in-front transparent surfaces. Separate X-Ray depth domains,
  and compositor-unavailable cases remain open.
- **Bounded large-curve depth sampling (2026-10-15):** when a committed curve
  would exceed the one-million depth-sample budget, the QPainter path now
  reduces sampling density uniformly and, for exceptionally large component
  counts, samples a bounded subset of segments. It keeps the depth-tested
  visible-run path instead of falling back to an unconditional full NURBS
  stroke. This prevents over-budget curves from leaking through opaque
  blockers, with reduced occlusion-edge precision as the tradeoff. Matched
  large-curve GPU/QPainter captures remain open.
- Validation: `cmake --build build -j4` passed with the 317-file dependency
  audit; offscreen `viewport_render_contracts` passed (1/1, 28.43 seconds).
  This verifies the software visibility contract, not a matched full-frame
  Blender image comparison.

### Matched GPU perspective mesh texture extension modes (2026-10-14)

- Extended the established perspective two-triangle mesh fixture to capture
  GPU output for Image Texture `EXTEND`, `CLIP`, and `MIRROR` in addition to
  its existing `REPEAT` baseline. The test scales and offsets each corner UV
  from `[0, 1]` to `[-0.5, 1.5]`, so all modes exercise coordinates beyond the
  image bounds under perspective-correct interpolation. The Blender reference
  helper now applies the same optional UV scale/offset to the mesh fixture.
- Against Blender 5.2.2, all three GPU PFM captures have zero scene-linear RGB
  error in every channel, with zero edge-coverage error. The PNG foreground
  masks each have IoU `1.0` and zero XOR pixels. Display-channel MAE over the
  shared foreground is `0.0157/255` for EXTEND, `0.0485/255` for CLIP, and
  `0.0290/255` for MIRROR. CLIP retains fewer foreground samples because its
  out-of-range UVs return the configured border color. No GPU sampler change
  is indicated by these captures.
- Blender 5.2.2 maps Image Texture EXTEND to sampler extend, REPEAT to repeat,
  MIRROR to mirrored repeat, and CLIP to clamp-to-border in pinned
  [`workbench_materials.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_materials.cc#L451-L513).
  The matched fixture tests opaque STRAIGHT texture color with Linear filtering;
  other filtering modes and perspective NURBS texture cases beyond the curved
  Repeat fixture remain open.
- Validation: `cmake --build build -j4` passed with the dependency audit;
  capture-enabled offscreen CTest `viewport_render_contracts` passed (1/1,
  39.82 seconds); the three Blender references reported 5.2.2 LTS; and paired
  PNG/PFM comparisons completed for each extension. Reproduce classiCAD output
  with `CLASSICAD_VIEWPORT_LIGHTING_CAPTURE_DIR=<dir>` and Blender with
  `--scene MESH_TEXTURE --match-classicad-perspective --view-direction 4 -6 4
  --aa OFF --lighting FLAT --color-source TEXTURE --texture-interpolation
  Linear --texture-uv-pattern --texture-uv-scale 2 --texture-uv-offset -0.5
  --texture-extension <EXTEND|CLIP|MIRROR> --texture-image <fixture>
  --world-color 0.0219421387 0.0219421387 0.0219421387 --zero-dither` plus
  `--linear-output` and `--linear-pfm-output`.

### Matched QPainter perspective mesh texture extensions (2026-10-15)

- Extended the existing perspective two-triangle mesh fixture to write
  opt-in QPainter PNG/PFM captures for Image Texture `EXTEND`, `CLIP`, and
  `MIRROR`. The capture helper accepts the fixture's perspective transform;
  the mesh uses the same asymmetric 2×2 sRGB texture, UV range `[-0.5, 1.5]`,
  Linear filtering, 640×480 target, and neutral World as the GPU and Blender
  captures. Blender references use the pinned 5.2.2 `MESH_TEXTURE` helper
  scene and matching perspective view.
- Against Blender, QPainter foreground-mask IoU is `0.999973` (one XOR
  pixel) for EXTEND and MIRROR, and `0.998891` (17 XOR pixels) for CLIP.
  Shared-foreground display channel MAE is `0.1857/255`, `0.2217/255`, and
  `0.2711/255`, respectively. Scene-linear RGB MAE `(blue, green, red)` is
  `(4.03e-5, 1.14e-4, 8.29e-5)` for EXTEND,
  `(8.29e-5, 1.16e-4, 1.45e-4)` for MIRROR, and
  `(3.18e-5, 3.87e-5, 5.59e-5)` for CLIP. PFM maximum errors are isolated
  samples; the captures do not show a broad extension-sampling mismatch, so
  no production sampler change is justified. Perspective raster edges and
  other camera/mesh cases remain approximate.
- Validation: `cmake --build build -j4` passed with the dependency audit;
  capture-enabled offscreen `viewport_render_contracts` passed (1/1,
  42.34 seconds); Blender reported 5.2.2 LTS for all three references; and
  `workbench_image_compare.py` plus `workbench_pfm_compare.py` completed for
  each QPainter extension capture.

### Matched GPU Image Texture on a nonplanar NURBS patch (2026-10-14)

- Added a degree-2 by degree-2, non-rational 3×3 NURBS patch with asymmetric
  control-net heights, shifted U/V knot domains, unit weights, and the existing
  asymmetric sRGB texture. The production GPU capture remains Flat, Linear,
  Repeat, AA Off, and 640×480. Its test export writes the renderer's exact
  derived triangle positions and normalized parameter UVs as an OBJ proxy;
  Blender consumes those same triangles so this comparison does not conflate
  texture behavior with another surface tessellator.
- The Blender 5.2.2 comparison has foreground-mask IoU `0.999971` (one XOR
  pixel; 34,437 reference and 34,438 classiCAD foreground pixels). Scene-linear
  RGB MAE on the shared foreground is `0.0000563`, with maximum `0.00671`;
  nine fixed interior probes match exactly. Display-channel MAE is
  `0.0423/255` with maximum `4/255`. The remaining mismatch is concentrated
  at display/raster coverage boundaries; these results validate this curved
  GPU texture/material path, not trimmed surfaces, alternate filters/extensions,
  perspective views beyond the matched configuration below, or the QPainter
  curved-surface fallback (measured separately below).
- Blender 5.2.2 interpolates the per-vertex UV varying in pinned
  [`workbench_prepass.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_prepass.bsl.hh#L100-L105)
  and samples Image Texture color in
  [`workbench_image_lib.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_image_lib.glsl#L385-L433).
  classiCAD's material adapter and exact `NurbsSurface3D` source contract are
  unchanged; the exported mesh is comparison/display data only.
- Validation: `cmake --build build -j4` passed with the 316-file dependency
  audit; capture-enabled offscreen CTest `viewport_render_contracts` passed
  (1/1, 43.96 seconds); Blender reported 5.2.2 LTS; and paired PNG/PFM
  comparison completed. Reproduce classiCAD captures and the proxy with
  `CLASSICAD_VIEWPORT_LIGHTING_CAPTURE_DIR=<dir> QT_QPA_PLATFORM=offscreen
  ctest --test-dir build -R '^viewport_render_contracts$' --output-on-failure`,
  then render Blender with `--scene NURBS_TEXTURE_CURVED
  --nurbs-proxy-obj <dir>/color_texture_nurbs_curved_proxy.obj
  --match-classicad-ortho --aa OFF --lighting FLAT --color-source TEXTURE
  --texture-interpolation Linear --texture-extension REPEAT
  --texture-image <dir>/workbench-texture-pattern.png
  --world-color 0.0219421387 0.0219421387 0.0219421387 --zero-dither`,
  plus `--linear-output` and `--linear-pfm-output` for scene-linear capture.

### Matched GPU perspective Image Texture on a nonplanar NURBS patch (2026-10-14)

- Reused the asymmetric degree-2 NURBS patch and its exported exact display
  triangles/normalized UVs, switching both renderers to a matched perspective
  camera: view `(4, -6, 4)`, classiCAD zoom `17.28`, Blender's corresponding
  25 mm lens and 36 mm sensor, 640×480, Flat lighting, Linear/Repeat texture,
  and AA Off. This exercises perspective-correct texture coordinates on a
  genuinely curved, non-coplanar surface without introducing tessellation as
  another comparison variable.
- Blender 5.2.2 and GPU classiCAD foreground masks match exactly (IoU `1.0`,
  zero XOR pixels; 44,879 foreground pixels each). Scene-linear RGB MAE on the
  shared foreground eroded by four pixels is `0.00000061`, maximum `0.00342`;
  all nine fixed interior probes match exactly. Display-channel MAE is
  `0.0350/255` on the shared foreground, maximum `2/255`. This validates the
  perspective curved-texture configuration; other camera poses, texture modes,
  trimming, and QPainter perspective behavior remain open.
- Blender's Workbench prepass passes smoothly interpolated UVs to the fragment
  shader in pinned
  [`workbench_prepass.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_prepass.bsl.hh#L100-L105),
  where pinned
  [`workbench_image_lib.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_image_lib.glsl#L385-L433)
  samples Image Texture color. classiCAD retains its source NURBS surface and
  material adapter; the shared OBJ exists only as test comparison geometry.
- Validation: `cmake --build build -j4` passed with the 316-file dependency
  audit; capture-enabled offscreen CTest `viewport_render_contracts` passed
  (1/1, 39.66 seconds); Blender reported 5.2.2 LTS; and paired PNG/PFM
  comparisons completed. Reproduce classiCAD captures and the proxy with
  `CLASSICAD_VIEWPORT_LIGHTING_CAPTURE_DIR=<dir> QT_QPA_PLATFORM=offscreen
  ctest --test-dir build -R '^viewport_render_contracts$' --output-on-failure`,
  then render Blender using `--scene NURBS_TEXTURE_CURVED
  --nurbs-proxy-obj <dir>/color_texture_nurbs_curved_proxy.obj
  --match-classicad-perspective --view-direction 4 -6 4 --aa OFF
  --lighting FLAT --color-source TEXTURE --texture-interpolation Linear
  --texture-extension REPEAT --texture-image <dir>/workbench-texture-pattern.png
  --world-color 0.0219421387 0.0219421387 0.0219421387 --zero-dither`, plus
  `--linear-output` and `--linear-pfm-output` for scene-linear capture.

### QPainter curved NURBS Image Texture comparison (2026-10-14)

- Extended the same degree-2 curved surface fixture to the production QPainter
  renderer in the matched orthographic and perspective camera setups. Both
  captures use the exact exported Blender proxy triangles and normalized UVs,
  Flat Texture shading, Linear/Repeat filtering, AA Off, and the same texture
  and background. The QPainter capture records its scene-linear fragment
  samples and final PNG for comparison.
- Against Blender 5.2.2, the orthographic foreground mask has IoU `0.999971`
  (one XOR pixel); perspective has IoU `0.999978` (one XOR pixel). Display
  channel MAE on shared foreground is `0.3347/255` orthographic and
  `0.3034/255` perspective, with p95 `1/255` in both. On the common foreground
  eroded by four pixels, scene-linear fragment RGB MAE is `0.00156` (p95
  `0.00373`, max `0.00771`) orthographic and `0.00154` (p95 `0.00371`, max
  `0.00815`) perspective. Eight of nine interior probes are exact in each view;
  the remaining probe differs by at most `0.00172` orthographic and `0.00148`
  perspective. The one-pixel silhouette difference and small interior residual
  do not identify a sampling or shading correction, so no production renderer
  change was justified. This is one smooth curved patch, not general QPainter
  parity for trims, other camera poses, filters, or alpha modes.
- Blender's pinned
  [`workbench_prepass.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_prepass.bsl.hh#L100-L105)
  interpolates the UV varying, then
  [`workbench_image_lib.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_image_lib.glsl#L385-L433)
  samples texture RGB. The QPainter path retains classiCAD's surface/material
  contracts; this comparison uses the shared mesh solely to match display
  tessellation.
- Validation: `cmake --build build -j4` passed with the 316-file dependency
  audit; capture-enabled offscreen CTest `viewport_render_contracts` passed
  (1/1, 40.87 seconds); Blender reported 5.2.2 LTS; and matched PNG/PFM
  comparisons completed in both views. For the classiCAD images and proxy run
  the capture-enabled CTest described above; render the Blender references
  using `--scene NURBS_TEXTURE_CURVED --nurbs-proxy-obj
  <dir>/color_texture_nurbs_curved_proxy.obj` with the same orthographic or
  perspective arguments in the preceding categories, plus `--linear-output`
  and `--linear-pfm-output`.

### Matched textured trimmed NURBS surface with an interior hole (2026-10-14)

- Added a curved degree-2 NURBS patch with an outer UV boundary and a square
  interior hole, both represented by closed degree-1 NURBS trim loops. The
  offscreen contract checks that the projected hole center remains the World
  background and a projected annulus point remains textured in both GPU and
  QPainter captures. It exports the production tessellator's exact triangles
  and normalized UVs for the Blender 5.2.2 mesh proxy; the authoritative surface
  and trim curves remain unchanged.
- The Blender comparison uses the same 640×480 orthographic camera, Flat
  Texture, sRGB Linear/Repeat image, AA Off, and neutral World background. GPU
  mask IoU is `0.999580` (12 XOR pixels; 28,565 foreground pixels in each
  image); QPainter mask IoU is `0.999755` (7 XOR pixels; 28,565 Blender and
  28,568 QPainter foreground pixels). On the common foreground eroded by four
  pixels, scene-linear RGB MAE is `0.0000575` GPU (max `0.00671`) and `0.00157`
  QPainter (p95 `0.00375`, max `0.00771`). The tested hole and textured annulus
  are correct; remaining mask differences are at the trim/raster boundary.
  This does not establish arbitrary trim topology, finer boundary accuracy,
  alternate texture modes, or broad QPainter parity.
- Blender's Workbench prepass interpolates the proxy's UV varying in pinned
  [`workbench_prepass.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_prepass.bsl.hh#L100-L105)
  and samples texture RGB in pinned
  [`workbench_image_lib.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_image_lib.glsl#L385-L433).
  The paired capture measures classiCAD's tessellated trim result and texture
  path; it does not claim Blender Workbench natively renders NURBS trims.
- Validation: `cmake --build build -j4` passed with the 316-file dependency
  audit; capture-enabled offscreen CTest `viewport_render_contracts` passed
  (1/1, 45.58 seconds); Blender reported 5.2.2 LTS; GPU and QPainter PNG/PFM
  comparisons completed. Reproduce captures and the OBJ with
  `CLASSICAD_VIEWPORT_LIGHTING_CAPTURE_DIR=<dir> QT_QPA_PLATFORM=offscreen
  ctest --test-dir build -R '^viewport_render_contracts$' --output-on-failure`,
  then run Blender with `--scene NURBS_TEXTURE_CURVED --nurbs-proxy-obj
  <dir>/color_texture_nurbs_trimmed_proxy.obj --match-classicad-ortho
  --aa OFF --lighting FLAT --color-source TEXTURE --texture-interpolation
  Linear --texture-extension REPEAT --texture-image
  <dir>/workbench-texture-pattern.png --world-color 0.0219421387 0.0219421387
  0.0219421387 --zero-dither`, plus `--linear-output` and
  `--linear-pfm-output` for scene-linear output.

### Curved NURBS Image Texture alpha cutoff comparison (2026-10-14)

- Extended the nonplanar degree-2 NURBS fixture with an 8×8 sRGB texture whose
  left half has zero alpha and right half has unit alpha. Captures cover GPU
  and QPainter rendering, orthographic and perspective cameras, and STRAIGHT
  versus NONE alpha interpretation. The same production tessellation triangles
  and normalized UVs are exported as the Blender 5.2.2 comparison proxy.
- STRAIGHT removes about half the patch while NONE retains the full surface in
  all four classiCAD renderer/camera combinations. Against Blender 5.2.2,
  orthographic masks have IoU `0.999894` (2 XOR pixels) for STRAIGHT and
  `0.999971` (1 XOR) for NONE; perspective masks have IoU `1.0` (zero XOR) for
  STRAIGHT and `0.999978` (1 XOR) for NONE. GPU and QPainter produce matching
  display-mask metrics. Perspective GPU scene-linear output matches exactly in
  both alpha modes. QPainter perspective scene-linear channel MAE stays below
  `0.000196`; orthographic GPU/QPainter channel MAE stays below `0.000122`.
  Display comparisons are exact over common foreground at p95; isolated edge
  samples remain visible in channel maxima, so this does not claim broad
  curved-surface or general alpha-filter parity.
- Blender 5.2.2's pinned
  [`workbench_image_lib.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_image_lib.glsl#L416-L429)
  applies the Image Texture alpha cutoff and bypasses it for NONE/Ignore;
  [`workbench_prepass.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_prepass.bsl.hh#L100-L105)
  supplies interpolated UVs. The comparison keeps classiCAD's NURBS surface
  and `WorkbenchMaterialData` contracts intact; only the test proxy is a mesh.
- Validation: `cmake --build build -j4` passed with the 316-file dependency
  audit; capture-enabled offscreen CTest `viewport_render_contracts` passed
  (1/1, 52.60 seconds); all four Blender references reported 5.2.2 LTS; and
  PNG/PFM comparisons completed for both renderers, alpha modes, and cameras.
  No production renderer change was justified by these results. Reproduce
  classiCAD captures with `CLASSICAD_VIEWPORT_LIGHTING_CAPTURE_DIR=<dir>
  QT_QPA_PLATFORM=offscreen ctest --test-dir build -R
  '^viewport_render_contracts$' --output-on-failure`. Render each Blender
  reference with `--scene NURBS_TEXTURE_CURVED --nurbs-proxy-obj
  <dir>/color_texture_nurbs_curved_proxy.obj --texture-image
  <dir>/workbench-texture-nurbs-curved-alpha.png --texture-interpolation
  Closest --texture-extension REPEAT --texture-alpha-mode STRAIGHT|NONE
  --lighting FLAT --color-source TEXTURE --aa OFF --zero-dither`, using
  `--match-classicad-ortho` or `--match-classicad-perspective --view-direction
  4 -6 4`, plus `--linear-output` and `--linear-pfm-output` for PFM comparison.

### Curved NURBS filtered Image Texture alpha cutoff (2026-10-14)

- Extended the curved-patch alpha fixture to Linear texture interpolation,
  retaining Closest as the nearest-sample baseline. The same binary 8×8 alpha
  transition exercises interpolated alpha values around Blender's `0.1`
  fragment cutoff. GPU and QPainter captures cover STRAIGHT cutout and NONE
  bypass, each with orthographic and perspective cameras.
- Against Blender 5.2.2, GPU mask IoU is `0.999862` (3 XOR pixels) for
  orthographic STRAIGHT, `0.999971` (1 XOR) for orthographic NONE, and `1.0`
  (zero XOR) for both perspective modes. QPainter mask IoU is `0.999817` (4
  XOR), `0.999971` (1 XOR), `0.999884` (4 XOR), and `0.999978` (1 XOR) in the
  same order. On common foreground, GPU display MAE ranges from `0.0123/255`
  to `0.0246/255` with p95 `0/255`; QPainter ranges from `0.0924/255` to
  `0.1247/255` with p95 `1/255`. Scene-linear max channel MAE is at most
  `0.000012` for GPU and `0.000246` for QPainter. The remaining QPainter
  differences are limited to filtered cutout/raster coverage edges; these
  captures do not establish other texture filters or all alpha modes.
- Blender 5.2.2 linearly samples Image Texture RGBA before applying the alpha
  cutoff in pinned
  [`workbench_image_lib.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_image_lib.glsl#L385-L429);
  the interpolated surface UV is supplied by
  [`workbench_prepass.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_prepass.bsl.hh#L100-L105).
  No material or geometry contract changed, and these measurements do not
  justify a production renderer adjustment.
- Validation: `cmake --build build -j4` passed with the 316-file dependency
  audit; capture-enabled offscreen CTest `viewport_render_contracts` passed
  (1/1, 53.24 seconds); all four Blender references reported 5.2.2 LTS; and
  eight PNG/PFM pairs were compared. Reproduce classiCAD captures with
  `CLASSICAD_VIEWPORT_LIGHTING_CAPTURE_DIR=<dir> QT_QPA_PLATFORM=offscreen
  ctest --test-dir build -R '^viewport_render_contracts$' --output-on-failure`.
  Use the Blender command in the preceding curved-alpha section, changing
  `--texture-interpolation Closest` to `--texture-interpolation Linear`, and
  render both `--texture-alpha-mode STRAIGHT` and `NONE` for each camera.

### Curved NURBS Image Texture alpha interpretation modes (2026-10-14)

- Extended the curved degree-2 NURBS alpha fixture with a smooth 8×8 alpha
  ramp and separate straight and scene-linear-premultiplied RGB encodings.
  Captures cover STRAIGHT, PREMUL, CHANNEL_PACKED, and NONE on GPU and QPainter
  renderers in orthographic and perspective views. The production NURBS
  surface, `WorkbenchMaterialData` adapter, and UV contracts are unchanged.
- Internal image comparisons verify PREMUL matches its equivalent STRAIGHT
  encoding within 2 display levels, CHANNEL_PACKED matches NONE exactly, and
  CHANNEL_PACKED preserves substantially more coverage than STRAIGHT by
  bypassing the image alpha cutoff. These checks pass across both renderers
  and camera views.
- Against Blender 5.2.2, the CHANNEL_PACKED and NONE images have exact display
  RGB on common foreground; their orthographic masks differ by one pixel
  (IoU `0.999971`) and perspective masks by one pixel (IoU `0.999978`) for
  both renderers. PREMUL scene-linear RGB MAE is at most `0.000088` against
  Blender's float EXR reference in GPU captures; display MAE on common
  foreground is at most `0.0563/255`, with maximum `1/255`. QPainter
  scene-linear RGB MAE is at most `0.000104` for CHANNEL_PACKED/NONE captures;
  single boundary pixels account for the remaining silhouette XORs. This
  measures one alpha ramp and one smooth patch; it does not establish all
  image encodings, alpha-filter combinations, trim boundaries, or general
  renderer parity.
- Blender 5.2.2's pinned
  [`workbench_image_lib.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_image_lib.glsl#L416-L429)
  unpremultiplies RGB for PREMUL samples with nonzero alpha and conditionally
  applies the alpha cutoff. Pinned
  [`workbench_materials.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_materials.cc#L538-L553)
  sets the premultiplied flag only for PREMUL and disables cutoff for Ignore
  and Channel Packed images. Blender's pinned
  [`colormanagement.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/imbuf/intern/colormanagement.cc#L2070-L2194)
  converts byte and float image inputs through different paths, so the
  PREMUL comparison uses scene-linear float EXR data as the reference input.
  The results did not justify a production renderer change.
- Validation: `cmake --build build` passed with the 316-file dependency
  audit; capture-enabled offscreen CTest `viewport_render_contracts` passed
  (1/1); Blender references reported 5.2.2 LTS; and paired PNG/PFM
  comparisons completed for all four modes, renderers, and cameras. Reproduce
  classiCAD captures with `CLASSICAD_VIEWPORT_LIGHTING_CAPTURE_DIR=<dir>
  QT_QPA_PLATFORM=offscreen ctest --test-dir build -R
  '^viewport_render_contracts$' --output-on-failure`. Render Blender with the
  curved proxy and `--texture-interpolation Linear --texture-alpha-mode
  STRAIGHT|PREMUL|CHANNEL_PACKED|NONE`, using the matched orthographic or
  perspective camera arguments from the preceding alpha categories; use
  `--linear-output` and `--linear-pfm-output` for scene-linear comparison.

### Curved trimmed NURBS Image Texture alpha comparison (2026-10-14)

- Extended the existing curved degree-2 NURBS surface fixture, with its outer
  trim and square interior hole, to capture a linearly filtered binary alpha
  texture in STRAIGHT and NONE modes. Both the production GPU and QPainter
  renderers write display PNG and scene-linear PFM captures; their foreground
  checks confirm STRAIGHT cuts out substantial surface coverage while NONE
  retains the trimmed surface. The authoritative NURBS surface, trim loops,
  UVs, and material adapter remain unchanged.
- Compared with Blender 5.2.2 using the production tessellation exported as
  the exact OBJ triangle/UV proxy, GPU foreground-mask IoU is `0.999309`
  (12 XOR pixels) for STRAIGHT and `0.999580` (12 XOR) for NONE. QPainter
  mask IoU is `0.999540` (8 XOR) and `0.999755` (7 XOR), respectively.
  Display MAE on common foreground is `0.0206/255` and `0.0160/255` for GPU,
  and `0.0987/255` and `0.0791/255` for QPainter; p95 is at most `1/255`.
  Scene-linear channel MAE is at most `0.000019` GPU and `0.000135` QPainter.
  This validates alpha cutoff/bypass across one trimmed patch and one texture;
  it does not establish other trim topology, texture modes, camera poses, or
  general QPainter parity. Blender renders the matching mesh proxy, not native
  NURBS trims.
- Blender 5.2.2 interpolates Image Texture RGBA and applies its alpha cutoff in
  pinned
  [`workbench_image_lib.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_image_lib.glsl#L385-L429);
  the prepass supplies interpolated UVs in pinned
  [`workbench_prepass.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_prepass.bsl.hh#L100-L105).
  The remaining differences are concentrated at alpha/trim raster boundaries;
  these measurements did not justify a production renderer change.
- Validation: `cmake --build build` passed with the 316-file dependency
  audit; capture-enabled offscreen CTest `viewport_render_contracts` passed
  (1/1, 43.96 seconds); both Blender references reported 5.2.2 LTS; and four
  PNG plus four PFM comparisons completed. Reproduce classiCAD captures with
  `CLASSICAD_VIEWPORT_LIGHTING_CAPTURE_DIR=<dir> QT_QPA_PLATFORM=offscreen
  ctest --test-dir build -R '^viewport_render_contracts$' --output-on-failure`.
  Render each Blender reference with `--scene NURBS_TEXTURE_CURVED
  --nurbs-proxy-obj <dir>/color_texture_nurbs_trimmed_proxy.obj
  --texture-image <dir>/workbench-texture-nurbs-curved-alpha.png
  --texture-interpolation Linear --texture-extension REPEAT
  --texture-alpha-mode STRAIGHT|NONE --lighting FLAT --color-source TEXTURE
  --aa OFF --match-classicad-ortho --world-color 0.0219421387 0.0219421387
  0.0219421387 --zero-dither`, with `--linear-output` and
  `--linear-pfm-output` for scene-linear captures.

### Workbench scene-pass coordinator rejection contract (2026-10-14)

- Extended the offscreen GPU render-pass contract to submit packets with an
  empty viewport size, target dimensions that do not match the viewport, and
  a non-finite device-pixel ratio. Each packet is rejected before drawing and
  reports no completed scene passes or populated attachments. Existing valid
  opaque and transparent fixtures continue to verify successful sample-color
  and depth behavior.
- The live viewport and offscreen Workbench render-result path now use the
  shared `renderFrameSceneColor` submission helper. It builds the frame packet
  from the caller's presentation-adjusted draw list, active sample camera,
  device-pixel ratio, settings, and AA targets, then forwards the returned
  attachment mask to the active AA sample. Named pass sequencing remains in
  `ViewportSurfaceRenderer`.
- The offscreen contract compares a `fromScene` route against this same
  production helper on separate AA targets and requires identical pass masks,
  attachment masks, resolved scene-linear Combined, depth, and display pixels.
  It deliberately gives `ViewportRenderFrame` a different stored camera to
  prove the explicit active sample camera controls the frame submission. This
  exercises the helper used by both production routes but does not execute the
  widget call sites or verify their argument selection independently.
  Allocation-failure injection and direct widget-call-site integration remain
  open, so the broader pass-ownership acceptance item remains open.
- Validation: `cmake --build build -j4` passed with the dependency audit
  checking 317 source files; offscreen `workbench_render_pass_capture` passed
  (1/1) under software GL.

### GPU transparent-film weighted OIT comparison (2026-10-14)

- Captured the production GPU pass for two overlapping translucent quads at
  640×480 with AA Off and transparent film, then compared its scene-linear
  Combined RGB/alpha and display PNG against Blender 5.2.2's matching
  `TRANSPARENT_OVERLAP` Workbench render. The offscreen contract passed (1/1).
- Scene-linear alpha is exact across the frame (maximum error 0); both images
  have center-overlap alpha `0.7646484375` and zero corner alpha. On 104,592
  common-coverage pixels, scene-linear RGB mean maximum-channel error is
  `0.00015772` and p95 is `0.00024414`. Display alpha is exact; display RGB
  differs by at most `1/255`, with 0.0106/255 mean channel error and p95 0.
- This validates final GPU OIT output for one overlapping-quad fixture and
  view. Other transparent geometry, lighting modes, and AA remain open.
  QPainter's separate compositor has a corresponding matched capture below.
  Blender 5.2.2 accumulation and revealage
  are pinned in
  [`workbench_transparent_accum_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_transparent_accum_frag.glsl#L435-L524)
  and
  [`workbench_composite.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_composite.bsl.hh#L558-L620).
- Validation: Blender reported 5.2.2 LTS; software-GL CTest
  `workbench_render_pass_capture` passed (1/1); and
  `workbench_transparent_film_compare.py` completed within its scene-linear,
  alpha, and display tolerances.

### Matched QPainter transparent-film weighted OIT comparison (2026-10-15)

- Added an opt-in scene-linear RGBA capture from the QPainter weighted-OIT
  compositor and compared the final transparent-film output with Blender
  5.2.2. Both captures use the same 640×480 Top view, two overlapping
  camera-facing quads, Standard transform, Flat Material shading, AA Off,
  transparent film, and zero dither. Blender's direction and orthographic
  scale are explicit (`0, 0, 1` and `5.0`) to match the classiCAD fixture.
- At an alpha threshold of `0.05`, both renders have 104,592 covered pixels,
  zero mask XOR, and IoU `1.0`. Center-overlap alpha is `0.7646484` in Blender
  and `0.7647828` in QPainter. On common coverage, scene-linear RGB maximum-
  channel p95 is `0.0005726`; alpha p95 is `0.0001915`. Full-frame mean
  absolute alpha error is `0.001040`. Display RGB p95 is `1/255` and display
  alpha p95 is zero. The maximum color/alpha errors are localized to a small
  number of fractional edge pixels: display RGB max is `28/255` and scene-
  linear alpha max is `0.4116`, consistent with the fallback's estimated
  4×4 coverage versus Blender's GPU raster coverage. The QPainter comparator
  therefore checks mask equality, common-coverage percentile error, and bounded
  whole-frame alpha error separately; the GPU comparator retains its stricter
  exact-alpha and one-code display thresholds.
- This validates one two-quad camera/view and AA-Off setting in the QPainter
  compositor. Other transparent geometry, camera views, lighting, AA, and
  combinations with cavity, shadows, outlines, X-Ray, or DOF remain open.
  Blender 5.2.2 accumulation and revealage are pinned in
  [`workbench_transparent_accum_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_transparent_accum_frag.glsl#L435-L524)
  and
  [`workbench_composite.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_composite.bsl.hh#L558-L620).
- Validation: `cmake --build build -j4` passed with the dependency audit;
  offscreen `viewport_render_contracts` passed (1/1) while writing QPainter
  scene-linear RGB/alpha and display captures. Blender reported 5.2.2 LTS,
  and `workbench_transparent_film_compare.py --q-painter` passed on the paired
  files.

### QPainter mesh edge-coverage audit (2026-10-15)

- Rechecked the production fallback in `workbenchTrianglePixelCoverage` and
  its mesh callers. AA Off uses a pixel-center sample and a half-open top-left
  edge rule; enabled fallback AA uses its bounded 4×4 sample estimate. Shared
  edge ownership, transparent-quad fragment resolution, and mesh/NURBS edge
  profiles across device-pixel ratios already have contracts in
  `tests/workbench_raster_fragment_contracts.cpp`.
- Blender 5.2.2 uses GPU triangle rasterization and its transparent OIT pass;
  the pinned renderer does not define a CPU sample grid. The center sample is
  a defensible single-sample fallback rule, while the 4×4 grid remains an
  approximation. No coverage change is justified without a matched Blender
  edge-profile comparison, so this audit makes no renderer change.

### QPainter World cavity with transparent film (2026-10-15)

- Added a matching `TRANSPARENT_CAVITY_OVERLAP` Blender 5.2.2 fixture and an
  opt-in classiCAD capture for the existing opaque-receiver plus offset
  translucent-pane contract. The capture compares each renderer's baseline
  and World-cavity result at 640×480, Top orthographic, Standard, Flat Material,
  AA Off, transparent film, and zero dither. The offscreen contract also
  requires cavity to change the opaque contribution while retaining the
  transparent accumulation unchanged. It caught an incorrectly ordered test
  pane; its prepared-depth offset now places it in front of the opaque
  receiver, matching the reference fixture.
- Blender's alpha buffer is unchanged by enabling cavity, and QPainter's alpha
  buffer is likewise unchanged. The paired alpha buffers differ between
  renderers only at raster-coverage boundaries (p95 absolute difference 0;
  maximum `0.75`). Cavity RGB effect deltas remain substantially different:
  at 3/255, Blender changes 20,751 display pixels and QPainter changes 1,200
  (IoU `0.057829`; union signed-delta MAE `7.94354/255`, p95 `17/255`, max
  `30/255`). At a `0.001` scene-linear threshold, mask IoU is `0.051458`, and
  full-frame RGB effect-delta MAE is `0.00233283`. Thus the pass-order
  contract holds, while QPainter cavity factor coverage remains an open parity
  gap; no production shading equation changed.
- Blender 5.2.2 source references for the interaction are pinned to
  [`workbench_cavity.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_cavity.bsl.hh#L41-L100),
  [`workbench_effect_cavity.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_effect_cavity.cc#L52-L68),
  [`workbench_transparent_accum_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_transparent_accum_frag.glsl#L435-L524),
  and [`workbench_composite.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_composite.bsl.hh#L558-L620).
- Validation: `cmake --build build -j4` passed; capture-enabled offscreen
  `viewport_render_contracts` passed (1/1); Blender reported 5.2.2 LTS; and
  `workbench_effect_linear_compare.py` plus
  `workbench_effect_delta_compare.py --threshold 3` completed. Reproduce the
  classiCAD capture with
  `CLASSICAD_VIEWPORT_QPAINTER_CAVITY_TRANSPARENCY_CAPTURE_DIR`; render Blender
  with `--scene TRANSPARENT_CAVITY_OVERLAP --effect BASELINE|CAVITY
  --transparent-film --aa OFF --zero-dither --view-direction 0 0 1
  --ortho-scale 5.0` and paired linear PFM outputs. This checks one opaque
  receiver and one overlapping pane only.

### Workbench settings and material persistence matrix (2026-10-14)

- Expanded `workbench_persistence_contracts` to write and reopen one update
  session containing every `ViewportShadingSettings` field, including a
  custom LUT's bytes, hash, and metadata, together with object Image Texture
  data and a packed UDIM tile. The test compares the reopened values against
  the source settings rather than only checking selected defaults.
- The isolated QSettings contract now round-trips all seven Workbench
  viewport AA preferences (`Off`, `Single Pass`, and 5/8/11/16/32 samples)
  and all four Overlay/Edit Mode Smooth Wires combinations independently.
  The update-session contract also round-trips all eleven camera preference
  fields. The isolated QSettings test exposed and fixed a mapping error where
  optical sensor size was saved from projection sensor width; distinct values
  now survive both stores. This closes the Workbench shading/material and
  camera-preference portions of persistence coverage. Native project camera
  settings and unrelated application preferences remain outside this matrix.
- Validation: `cmake --build build` passed with the 316-file dependency
  audit; `ctest --test-dir build -R '^workbench_persistence_contracts$'
  --output-on-failure` passed (1/1).

### Native project camera settings round-trip (2026-10-14)

- Extended the `.vignola` save/load contract to persist distinct values for all
  eleven `ProjectViewportCameraSettings` optical fields and all twenty stored
  viewport `viewState` fields, including projection sensor width separately
  from optical sensor size. An out-of-range optical sensor value is rejected
  before the project save starts. The document geometry/ID/material round-trip
  assertions remain in the same end-to-end fixture.
- Corrected the existing ordinary Rhino import fixture to use a mesh for its
  unsupported-geometry report. The importer supports spatial NURBS curves, so
  the earlier test's expectation that a nonplanar curve be skipped was stale.
- Validation: `cmake --build build -j4` passed with the 316-source dependency
  audit; `ctest --test-dir build -R '^vignola_file$' --output-on-failure`
  passed (1/1, 6.32 seconds); the fixture runtime reported Blender 5.2.2 LTS.

### Linked Blender object Random identity import (2026-10-15)

- Added a Blender 5.2.2 fixture generator with two separate source libraries,
  each containing a mesh object named `LinkedFixture`, and a host `.blend`
  linking both. The host passes through classiCAD's production
  `importBlenderScene` path, a `DocumentTransaction`, and
  `buildViewportRenderFrame`; the contract verifies the base object name,
  distinct library paths and IDs, and matching Random color samples from both
  QPainter and GPU render paths.
- The end-to-end contract exposed an import identity mismatch: Blender
  Workbench hashes the object's `ID.name` and separately mixes the linked
  library filepath, while Blender Python's `name_full` adds a library suffix.
  Scene extraction now supplies `obj.name` as the viewport display/hash name;
  its stable import ID continues to use `name_full` and the library path so
  separate linked objects remain distinct. Blender 5.2.2's hash inputs are
  pinned in [`workbench_materials.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_materials.cc#L19-L33).
- Validation: Blender reported 5.2.2 LTS and created the linked fixture;
  `cmake --build build -j4` passed with the 318-source dependency audit; the
  offscreen `classicad_viewport_render_contracts_test` passed with
  `CLASSICAD_VIEWPORT_LINKED_RANDOM_BLEND` set to the generated host file.
  Reproduce fixture generation with
  `blender --background --python tests/blender_linked_random_import_fixture.py -- OUTPUT_DIRECTORY`,
  then run the viewport contract with
  `QT_QPA_PLATFORM=offscreen CLASSICAD_BLENDER_EXECUTABLE=blender
  CLASSICAD_VIEWPORT_LINKED_RANDOM_BLEND=OUTPUT_DIRECTORY/linked-random-host.blend
  ./build/classicad_viewport_render_contracts_test`. This validates identity
  transport and each renderer's color input, not a paired full-frame Blender
  render comparison.

### Linked Blender Random identity persistence and shared resolver (2026-10-15)

- Consolidated the Blender-compatible Random color hash and legacy ObjectId
  fallback into `workbench_random_color.cpp`, shared by QPainter and GPU
  rendering. Both paths now call the same implementation for the Blender
  `OB`-prefixed object name and linked-library filepath hash.
- Extended the linked-object fixture contract through `.vignola` save and
  reopen. It verifies both base names, raw library paths, and object IDs match
  their imported render-frame values, then checks that the reopened objects
  still produce their expected QPainter and GPU Random color samples. Blender
  5.2.2's source identity equation remains pinned in
  [`workbench_materials.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_materials.cc#L19-L33).
- Validation: `cmake --build build -j4` passed with the 320-source dependency
  audit; offscreen `viewport_render_contracts` passed (1/1, 41.38 seconds)
  with `CLASSICAD_VIEWPORT_LINKED_RANDOM_BLEND` set to the generated Blender
  5.2.2 host fixture. This covers linked identity persistence and both
  renderer paths, not a paired full-frame render comparison.

### Linked Blender Random paired render probes (2026-10-15)

- Extended the Blender 5.2.2 linked-object fixture to render two same-named
  cubes from separate libraries with Flat Random shading, AgX, AA Off, and a
  matched 640x480 orthographic camera. The contract compares 9x9 interior
  probes at both linked-object centers in the Blender PNG, classiCAD GPU
  output, and QPainter output; maximum RGB error is limited to 3/255 per
  renderer and probe.
- The paired render exposed that Blender's runtime Workbench Random hash uses
  the resolved linked-library filepath, even when the imported source record
  retains a relative `//` path. Scene extraction now carries that resolved
  hash input separately from the original library path, and classiCAD's
  document serializer and viewport render frame preserve it. Local objects
  and older projects without the field continue to use the existing path
  fallback. Blender 5.2.2's name/path hash equation is pinned in
  [`workbench_materials.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_materials.cc#L19-L33).
- The QPainter compositor requires depth triangles for prepared geometry. The
  imported polygon mesh has no NURBS depth cache, so this fixture exercises
  the existing fragment-generated-depth fallback rather than treating an
  early compositor rejection as a rendered image. Both renderer probes now
  match the Blender reference within the stated tolerance. This is a two-object
  interior color check, not full-frame coverage or edge parity; other linked
  object types and file relocation behavior remain open.
- Validation: Blender reported 5.2.2 LTS; `cmake --build build -j4` passed
  with the source dependency audit; the linked-fixture
  `classicad_viewport_render_contracts_test` passed with
  `CLASSICAD_VIEWPORT_LINKED_RANDOM_BLEND` set to the generated host file; and
  offscreen CTest `workbench_persistence_contracts`,
  `viewport_render_contracts`, and `vignola_file` passed (3/3). No GUI was
  launched.

### QPainter cavity sample range and bounded work (2026-10-15)

- The QPainter World-cavity loop previously clamped `cavitySamples` to 32,
  although Blender 5.2.2 and classiCAD expose requests through 500. It now
  honors 1..500 when the covered opaque target fits a deterministic 32-million
  pixel-sample budget. On denser targets it evenly subsamples the requested
  Hammersley-like sequence and normalizes by the samples actually evaluated;
  this keeps the fallback's worst-case work bounded without taking only the
  sequence's smallest-radius prefix.
- A production effect contract verifies that the same QPainter fixture changes
  between 32 and 64 cavity samples. The raster-fragment contract checks the
  1/500 request limits and the 32-million work-budget results at 100,000 and
  1,000,000 covered pixels. This corrects setting truncation; it does not close
  the documented QPainter cavity-factor or full-image parity gap.
- Blender 5.2.2 accepts 1..500 SSAO samples in
  [`rna_scene.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/makesrna/intern/rna_scene.cc#L8106-L8108).
  Its cavity effect schedules `samples_len * ssao_samples` and caps the shared
  GPU sample buffer at 512 in
  [`workbench_effect_cavity.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_effect_cavity.cc#L416-L463)
  and
  [`workbench_private.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_private.hh#L218-L223).
  QPainter remains a separate single-frame CPU approximation, so its 32-million
  sample-work ceiling is a backend resource bound rather than Blender behavior.
- Validation: `cmake --build build -j4` passed with the dependency audit
  (320 source files); `QT_QPA_PLATFORM=offscreen
  ./build/classicad_workbench_raster_fragment_contracts_test` and
  `QT_QPA_PLATFORM=offscreen
  ./build/classicad_viewport_render_contracts_test` both passed. No GUI was
  launched.

### QPainter cavity viewport-boundary audit (2026-10-15)

- Verified the proposed viewport-edge clamp against Blender 5.2.2's pinned
  `workbench_cavity.bsl.hh`: `cavity_compute` skips a sample when its UV lies
  outside `[0, 1]` before reading the depth texture. QPainter's bounds check
  likewise skips that tap. The GPU texture's `GL_CLAMP_TO_EDGE` setting does
  not affect these cavity taps because the shader rejects them first, so
  clamping the CPU coordinates would be a parity regression.
- No renderer change was justified. This verifies viewport-boundary sampling
  only; the separately documented QPainter cavity-factor and full-image
  parity gaps remain open. Blender source comparison is pinned to
  [`workbench_cavity.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_cavity.bsl.hh#L520-L541).
- Validation: confirmed the source behavior in Blender's v5.2.2 shader and
  classiCAD's production fallback. No C++ change remained to build or test.
  No GUI was launched.

### QPainter cavity tap orientation and attribution (2026-10-15)

- Added opt-in per-tap GPU output for the fixed 640×480 cube and a bounded
  QPainter CSV trace for four selected receiver pixels. Each GPU tap pass
  captures its raw signed cavity/edge contributions and normalized sample
  coordinate; each CPU row records requested/effective counts, the raster
  sample coordinate and object ID, distance, dot products, bias, attenuation,
  and accepted contributions. The normal renderer keeps aggregate output and
  does not enable this diagnostic unless the existing capture prefix is set.
- The per-tap captures exposed two CPU coordinate-convention differences.
  Blender 5.2.2 forms its jittered direction with dot products against
  `(cos(angle), sin(angle))` and `(-sin(angle), cos(angle))`, which rotates by
  the negative noise angle. QPainter had applied the inverse rotation. GPU
  sample Y also grows upward while retained QPainter rows grow downward. The
  CPU fallback now uses Blender's rotation and negates the projected Y offset
  when mapping to its top-down raster.
- The tap comparator found 50 of 64 selected receiver/tap coordinates equal
  at the raster-texel level; the other 14 differed by one pixel, with no
  sample farther than one pixel. On the 111,480 common foreground pixels, the
  aggregate GPU/QPainter factor MAE changed only from `0.067453` to `0.067385`
  and p95 from `0.240885` to `0.240255`; tap contribution differences remain
  substantial. This fixes the source-backed sample orientation mismatch but
  does not close the QPainter cavity-factor or full-image parity gap.
- Reproduce the opt-in captures with
  `CLASSICAD_VIEWPORT_CAVITY_DIAGNOSTIC_PREFIX` and
  `CLASSICAD_VIEWPORT_QPAINTER_CAVITY_DIAGNOSTIC_PREFIX`, then run
  `python3 tests/workbench_cavity_tap_compare.py <gpu-prefix> <cpu-taps.csv>`.
  Blender's pinned formula is in
  [`workbench_cavity.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_cavity.bsl.hh#L503-L541).
  Validation: `cmake --build build -j4`, the capture-enabled offscreen
  `viewport_render_contracts` test (1/1), and the tap comparator passed. No
  GUI was launched.

### QPainter cavity nearest-texel coordinate mapping (2026-10-15)

- Blender 5.2.2 samples the cavity depth texture with nearest filtering and
  clamp-to-edge, after rejecting UV coordinates outside `[0, 1]`. The CPU
  fallback now derives its texel from the full fractional pixel coordinate:
  `floor(x)` for columns and `ceil(y) - 1` for top-down rows, clamping exact
  viewport-edge coordinates to the edge texel. This matches GL's
  `floor(uv * dimension)` selection and row orientation, including exact
  half-pixel ties where rounding an offset is not equivalent.
- A focused contract covers pixel centers, exact row/column boundaries,
  viewport-edge clamp behavior, and rejection outside the viewport. The
  capture comparator still reports 50/64 exact selected texel matches and no
  error larger than one pixel. Aggregate GPU/QPainter factor MAE is `0.067381`
  with p95 `0.240240` across 111,480 foreground pixels, effectively unchanged
  from the preceding `0.067385` / `0.240255`. This is a sampling-contract
  correction, not a measured closure of the broader cavity contribution or
  image-parity gap.
- The earlier fractional-position experiment was removed after its factor
  MAE rose on the larger-radius fixture. Blender's pinned cavity shader
  reconstructs the sampled view-space point from both UV and depth, so the CPU
  fallback now moves each retained depth sample from its pixel center to the
  actual fractional sample coordinate on the constant camera-depth plane.
  At distance `0.002`, GPU/QPainter factor MAE fell from `0.051837` to
  `0.023112` (p95 `0.047223`); the QPainter effect mask grew from zero pixels
  to 68,039 against Blender's 68,358 (IoU `0.371182` at 3/255). At the usual
  distance `0.2`, QPainter changes 36,834 pixels versus Blender's 27,109
  (IoU `0.214307`); factor MAE is `0.074704`, p95 `0.245981`. The small-radius
  comparison confirms the source-aligned reconstruction matters, while the
  default-radius mismatch and full-image cavity parity remain open. Validation:
  `cmake --build build -j4`, capture-enabled offscreen
  `viewport_render_contracts` at distances `0.002` and `0.2` (1/1 each), paired
  Blender 5.2.2 background renders, and
  `workbench_qpainter_cavity_diagnostic.py` passed. No GUI was launched.
- Blender's pinned sampler and UV bounds are in
  [`workbench_cavity.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_cavity.bsl.hh#L520-L541);
  the classiCAD depth texture uses `GL_NEAREST` and `GL_CLAMP_TO_EDGE` in
  `viewport_surface_renderer.cpp`. Validation: `cmake --build build -j4`, the
  capture-enabled offscreen `viewport_render_contracts` test (1/1), and the
  tap comparator passed. No GUI was launched.

### GPU deferred Studio local-space rotation comparison (2026-10-15)

- Added `studio_paint_local_37` to the existing offscreen lighting capture
  matrix: Studio `Paint`, local-space lighting, 37-degree rotation, specular
  enabled, AA Off, and the controlled 640x480 cube camera. The paired Blender
  5.2.2 fixture uses the same material color `(0.8, 0.8, 0.8)`, roughness
  `0.4`, metallic `0`, view direction `(4, -6, 4)`, and neutral World color.
  An initial reference with different material inputs and a shared capture path
  was discarded; the final captures use separate renderer/reference folders.
- All 27 GPU face-interior scene-linear and display probes match exactly.
  Full-frame scene-linear RGB MAE is `0.000397` per channel, with maximum
  boundary error `0.453`; the foreground mask IoU is `0.997991` (224 XOR
  pixels). Full-frame display RGB MAE on common foreground is `0.022/255`
  (p95 `0/255`, maximum `20/255` at boundary pixels). This validates the
  deferred Studio lighting result for this additional local-space rotation.
- QPainter matches the 27 display probes exactly; scene-linear probe MAE is
  `0.0003687` with maximum `0.0007267`. Its full-frame mask IoU is `0.994440`
  (622 XOR pixels), and full-frame scene-linear MAE is about `0.049` per
  channel. This remains one preset, rotation, and camera and does not close
  general Studio display parity or QPainter edge parity.
- Validation: `cmake --build build -j4` passed with the dependency audit
  (320 source files); the offscreen `classicad_viewport_render_contracts_test`
  passed with `CLASSICAD_VIEWPORT_LIGHTING_CAPTURE_DIR` set; Blender reported
  5.2.2 LTS; and `workbench_display_probe_compare.py`,
  `workbench_image_compare.py`, and `workbench_pfm_compare.py` completed on the
  separated matching captures. No GUI was launched.

### QPainter oversized mesh color transform fallback (2026-10-15)

- Oversized mesh triangles now use a bounded coarse raster (at most four
  million samples, with a 16,384-pixel side limit) instead of falling back to
  one flat polygon color. The raster keeps perspective-correct UV and corner
  color interpolation, scene-linear shading and display conversion, texture
  alpha cutouts, and configured X-Ray opacity. Fine detail and edges are less
  accurate at this size. No matched Blender image comparison has been run for
  this fallback.

### QPainter oversized textured NURBS fallback (2026-10-15)

- Oversized textured NURBS triangles now use a bounded coarse raster instead
  of disappearing when their device-pixel bounds exceed four million pixels.
  The sampled image is stretched across the original bounds, retaining the
  perspective-correct UV and alpha-cutout sampling path. Fine texture detail
  and edge coverage are less accurate at this size. No matched Blender image
  comparison has been run for this fallback.

### QPainter DOF pyramid and odd-size audit (2026-10-15)

- The QPainter renderer already builds the half-, quarter-, and
  eighth-resolution CoC-aware color pyramid and uses adaptive mip sampling.
  Earlier status text that said these stages were missing is now corrected;
  the `2026-10-14` entry records the state at that time. QPainter still has no
  temporal accumulation, so its per-pixel jitter uses temporal offset zero.
- An odd-size audit found no CPU/GPU mismatch to fix: both classiCAD paths
  allocate floor-sized mip levels, gather fixed 2x2 blocks, and clamp taps at
  the source edge. This follows Blender 5.2.2's pinned
  [`workbench_effect_dof_prepare_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_effect_dof_prepare_frag.glsl#L24-L58)
  and
  [`workbench_effect_dof_downsample_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_effect_dof_downsample_frag.glsl#L22-L53)
  sampling pattern. QPainter's tap budget, already-composited transparency,
  and lack of temporal accumulation remain documented approximations.
- The QPainter half-resolution median cleanup now gets its radius from the
  corresponding half-resolution near/far CoC values, taking the larger value
  and applying Blender's `clamp(CoC - 9, 0, 1)`. It previously used one
  full-resolution center-depth sample, which could miss blur from another
  sample in the same 2x2 block. This follows Blender's pinned
  [`workbench_effect_dof_blur2_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_effect_dof_blur2_frag.glsl#L430-L443)
  and half-resolution CoC preparation. C++ build passed; no matched DOF image
  comparison has been run for this correction.
- Validation for this audit was source inspection only; no matched image
  comparison or GUI run is claimed.

### QPainter prepared PolyCurve Smooth Wires (2026-10-15)

- Prepared PolyCurve strokes now set and restore the Overlay Smooth Wires
  preference, matching direct curve evaluation and the other committed curve
  paths. Previously, the prepared path inherited the painter's prior
  antialiasing state. The C++ build passed; no matched image comparison is
  claimed.

### QPainter average-depth in-front ordering (2026-10-15)

- Both ordinary and native-renderer-failure QPainter fallbacks now draw
  regular surface objects before `showInFront` objects when the bounded
  per-pixel compositor cannot run. Average view-depth order is retained within
  each group. This follows Blender's regular-then-in-front pass ordering, but
  still cannot reproduce per-pixel depth ordering in the average-depth path.
  The C++ build passed; no matched image comparison is claimed.
- The shared fallback depth estimate now weights each tessellated triangle by
  its surface area. This prevents densely tessellated regions from steering
  painter order more than equally sized sparse regions; it remains an
  object-level approximation and does not replace Blender's per-pixel sorting.

### QPainter camera-independent shadow casters (2026-10-15)

- The software shadow pass now submits caster triangles from their world
  geometry before the camera-projection validity check. A triangle behind or
  crossing the camera can still cast onto visible receivers, matching
  Workbench's separate shadow pass. It remains a bounded light-depth
  approximation; no matched Blender image comparison has been run.

### QPainter nearest effect receiver depth (2026-10-15)

- The bounded software effects pass now keeps the smallest camera depth when
  selecting its nearest opaque receiver. Camera depth increases farther into
  the scene, matching the GPU and transparent-fragment depth comparisons; the
  old comparison could let a farther overlapping surface replace the nearer
  receiver. No matched Blender image comparison has been run for this fix.

### QPainter perspective selected-curve depth (2026-10-15)

- Selected curve visibility now uses reciprocal interpolation for perspective
  depth with the renderer's signed negative camera-distance convention. The
  previous positive-depth gate skipped reciprocal interpolation for visible
  curve points, which could shift the apparent edge against an occluding
  surface. Orthographic depth interpolation and the existing visibility test
  are unchanged. The C++ build passed; no matched Blender image comparison has
  been run for this fix.

### GPU depth-map shadow caster alpha (2026-10-15)

- The GPU depth-map fallback now casts the complete triangle geometry without
  sampling Image Texture alpha. Blender 5.2.2's shadow pass submits geometry
  to its stencil shadow-volume shader and does not pass UV or image resources;
  material transparency can select a different volume technique but does not
  spatially cut out the caster. The QPainter software fallback already follows
  this rule. Texture alpha remains active in color, depth, and object-ID passes.
  The C++ build passed; no matched Blender shadow comparison has been run for
  this correction.

### QPainter in-front shadow receiver (2026-10-15)

- When shadows are enabled, the nearest opaque `showInFront` sample now
  supplies its own position and normal to the software shadow receiver pass,
  even when cavity is disabled. Previously, the front sample replaced the
  regular opaque receiver only for cavity, so an in-front object could receive
  the shadow factor computed for geometry behind it. Blender's pinned
  `OpaquePass` resolves the in-front depth/normal domain and applies the same
  shadow stencil in its composite pass. Transparent in-front samples still
  leave the regular opaque receiver available. The C++ build passed; no
  matched Blender shadow comparison has been run for this fix.

### QPainter transparent shadow visibility (2026-10-15)

- Transparent QPainter fragments now retain Blender's normal-based shadow
  lighting response without being darkened by the software caster map.
  Opaque receivers continue to use the bounded light-depth visibility test.
  This matches Blender's transparent accumulation path, which has no opaque
  stencil or shadow-map visibility input. No matched image comparison has
  been run for this change.

### Imported Blender mesh triangulation (2026-10-15)

- Blender mesh extraction now carries each evaluated face's loop-triangle
  corner indices into the imported mesh record. classiCAD keeps the original
  polygon loops authoritative and stores these indices as an optional derived
  display cache; older native files and meshes built inside classiCAD continue
  to use the existing triangulator. GPU/QPainter surface draws, shadow
  fallbacks, software effects, snapping, and native save/load use the retained
  indices when available. This preserves Blender's diagonal choices, which
  affect non-planar face shading and per-corner UV/color interpolation. The
  C++ build passed. Matched Blender mesh captures have not been run, so this
  closes the source-level topology mismatch without claiming visual parity.

### QPainter DOF transparency (2026-10-15)

- The QPainter blur gather now averages alpha along with RGB, and its resolve
  mixes all four channels. Scene-linear output keeps premultiplied RGB while
  allowing a blurred object to contribute color and alpha into nearby clear
  pixels. Blender 5.2.2's pinned DOF resolve writes both add and multiply
  terms as RGBA, and its blur passes sample and accumulate RGBA. This fixes
  the previous sharp-alpha clamp in the CPU fallback; its bounded gather and
  already-composited transparency still differ from Blender's full-resolution
  pass setup. The C++ build passed; no matched transparent-film DOF comparison
  has been run. References:
  [`workbench_effect_dof_resolve_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_effect_dof_resolve_frag.glsl#L304-L320),
  [`workbench_effect_dof_blur1_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_effect_dof_blur1_frag.glsl).

### X-Ray hidden-stroke fade (2026-10-15)

- GPU and QPainter scene strokes behind geometry now fade by
  `1 - xrayAlpha`, matching Blender 5.2.2's separate overlay X-Ray fade
  multiplier. The previous fixed 0.5 matched only the default setting. Visible
  strokes retain their configured color alpha. Blender's pinned
  [`overlay_xray_fade.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/overlay/overlay_xray_fade.hh#L25-L43)
  sets the multiplier and its
  [`overlay_xray_fade.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/overlay/shaders/overlay_xray_fade.bsl.hh#L55-L87)
  applies it to overlay pixels behind scene geometry. The C++ build passed;
  matched Blender captures at non-default X-Ray opacity remain open.

### QPainter committed-curve X-Ray visibility (2026-10-15)

- The ordinary QPainter prepared-curve path now separates depth-visible and
  hidden curve runs. Visible strokes keep their configured alpha; hidden runs
  are drawn first with the Blender X-Ray fade `1 - xrayAlpha`. Previously this
  path dimmed the whole curve by `xrayAlpha` and discarded hidden samples. The
  GPU-stroke fallback's separate hidden/visible passes remain independently
  handled. When the raster surface compositor retains depth, the curve path
  uses its regular or in-front depth map, including accepted transparent
  fragments. If surface composition cannot retain depth, curve strokes remain
  unoccluded. A matched Blender comparison remains open.

### QPainter control-cage depth visibility (2026-10-15)

- Curve control cages and handles now use the retained regular or in-front
  raster depth when available, so normal Solid overlays do not draw through
  nearer surfaces. Regular cages also hide behind in-front surfaces. X-Ray
  keeps cages visible through geometry and fades the hidden runs by
  `1 - xrayAlpha`; visible runs retain their normal opacity. Ordinary cages
  remain covered by the later in-front layer, and in-front cages use their own
  depth. The depth sampling is bounded and has no effect when the fallback did
  not retain scene depth. Blender's pinned Overlay wire pass depth-tests against the scene in
  [`overlay_wireframe.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/overlay/overlay_wireframe.hh#L60-L74).
  The C++ build passed; no matched Blender image comparison has been run.

### Imported Blender object selection (2026-10-15)

- The extractor records selected and active source object IDs for each Blender
  view layer and identifies the active window's scene and layer (falling back
  to the first scene and layer when no active window is available). Ordinary
  `.blend` open and scene import map that active scene/layer state to classiCAD
  object IDs after import has assigned those IDs, then restore it through the
  existing selection model. Ordinary open reapplies the selection after camera
  settings reset the viewport. Appending activates the imported Blender layer
  so its selection and visibility rules agree. When one
  Blender curve or surface becomes several imported objects, all pieces
  follow the source object's selection and its first imported piece becomes
  primary. Unsupported objects without imported geometry are omitted. This
  preserves initial selection state; no matched visual comparison has been
  run.

### Blender mesh wire-over-solid import (2026-10-15)

- Blender scene extraction now saves the active 3D View's
  `show_wireframes`, `wireframe_color_type`, `wireframe_opacity`, and
  `wireframe_threshold` values. The color type is read from the shading
  settings, where Blender 5.2.2 defines it; if an older Blender lacks that
  property, extraction defaults it to `THEME` instead of failing. Imported
  viewport sessions now preserve these three wire overlay controls, while
  older session files retain their default settings. Imported objects also
  retain `show_wire`,
  `show_all_edges`, and whether `display_type` is `WIRE`; native classiCAD
  documents preserve those object flags. The viewport draws deduplicated
  polygon-loop edges over Solid mesh fills when the saved global overlay or
  an object's wire flag enables them. Theme, Object (half-brightness), and
  Blender Random colors are supported. GPU strokes and the ordinary QPainter
  path depth-test against their retained scene layers. Blender's WIRE display
  hides mesh fills while keeping the edge overlay visible in both paths.
  Random mesh and NURBS wires now use Blender's separate HSV palette
  (saturation 0.75, value 0.8) while preserving the same object hue used by
  Random surface shading (saturation 0.5). Their generated colors also bypass
  the Workbench scene color transform, matching Blender's overlay wire shader;
  Object wire colors retain their existing transform. No matched Blender
  wire-color image comparison has been run.
- Object wire colors now use the imported Standard, AgX, Filmic, Neutral, or
  custom-LUT display transform, including exposure and gamma, after Blender's
  half-brightness adjustment. Imported Blender preferences now supply the
  Theme wire, background, active-object, and selected-object colors. Theme
  mesh wires and Object/Random mesh wires use the view-facing endpoint
  gradient. Selected Theme wires use the active/selected rim color; selected
  Object/Random wires use Blender's normalized wire and brightened rim values.
  The imported Blender `BOUNDS` display type is preserved in native documents
  and replaces the source fill with generated Box, Sphere, Cylinder, Cone, or
  Capsule wire proxies. Polygon meshes derive their bounds from the current
  imported mesh vertices when older records lack bounds metadata. New Blender
  imports retain local evaluated bounds and the object's world transform, so
  rotated curves and surfaces use object-aligned proxies instead of a world
  axis-aligned box. Capsule placement follows Blender's local X-size formula.
  These proxies have not been matched visually against Blender.
  The ordinary QPainter mesh-wire overlay interpolates Blender's per-endpoint
  colors for each visible edge sample and preserves the configured wire
  opacity, matching the GPU stroke color treatment. Matched Blender wire-color
  image comparison remains open.
  These theme colors come from the Blender installation used for import, not
  the `.blend` file, so another system can render a different theme. Polygon
  edge filtering uses Blender's face-angle
  factor and square-root threshold conversion; boundary and nonmanifold edges
  remain visible, while `show_all_edges` bypasses the threshold. Blender's
  Selected NURBS surface wires and their silhouette strokes now use Blender's active-object or selected-object theme color and the fixed-facing wire blend used by its curve shader; Object and Random modes use the same transformed and selected-color treatment as mesh wires at a fixed facing value of 0.5. Matched Blender image comparison remains open.
  Blender's Optimal Display flag for Subdivision Surface and Multires hides
  generated subdivision edges, but its per-edge draw mask is built in the
  internal subdivision GPU cache and is not exposed by the public evaluated
  `bpy.types.Mesh` API. Reading only the modifier toggle cannot identify the
  effective mask through arbitrary modifier stacks; exact import needs a
  Blender-side export hook or reliable edge provenance. See the pinned
  [`subdiv_mesh.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/blenkernel/intern/subdiv_mesh.cc)
  implementation and the public [Mesh API](https://docs.blender.org/api/5.2/bpy.types.Mesh.html).
  Mesh endpoint gradients
  use averaged imported loop normals at shared vertices and the shader's 0.35
  front-color mix. No matched Blender wire capture has been run. Loose mesh
  edges remain represented by their existing companion curves. NURBS surfaces now hide
  their fill for Blender's WIRE display and reuse the cached surface-wire
  tessellation for global, per-object, and WIRE display overlays. Blender's
  legacy surface control-net wire layout may differ from this generated
  surface tessellation when importing older files, native surfaces, or Blender
  surfaces with modifiers. The WIRE fill suppression also covers the ordinary
  QPainter batch list, and GPU surface wire strokes now invalidate with the
  object's geometry revision.
  The C++ build passed; matched Blender image comparison remains open.

### QPainter DOF scene-linear retention (2026-10-15)

- The weighted QPainter surface compositor now keeps its scene-linear result
  for DOF when the background is also available in scene-linear form or the
  destination is transparent. Eligible scenes therefore blur before the
  Standard, AgX, or custom display transform, matching Blender's color-space
  order. If the QPainter target already contains a display-referred background,
  this path still uses its existing display-color gather because that background
  cannot be recovered accurately. The C++ build passed; matched Blender DOF
  comparison remains open.

### Bounds overlay selection colors (2026-10-15)

- Blender's overlay bounds pass uses theme colors for all objects, regardless
  of Workbench's Theme/Object/Random mesh-wire color mode. classiCAD bounds now
  use the imported active-object theme color for the primary selected object,
  the selected-object theme color for other selected objects, and the wire
  theme color otherwise. Requested Bounds display geometry now also remains
  visible when optional wire overlays are hidden; Blender's bounds pass
  explicitly ignores `show_extras()` for this display type so the object does
  not disappear. This follows Blender 5.2.2's `overlay_bounds.hh` and
  `overlay_private.hh`. The C++ build passed; matched bounds comparison
  remains open.

### Active Blender scene and selection restore (2026-10-15)

- Blender extraction format 18 records the active window scene and view layer,
  with a deterministic first-scene/first-layer fallback when no active window
  is available. Import restores selected and active objects from that exact
  pair, uses its view-layer key for the document, and applies shading from the
  same scene. Ordinary `.blend` open reapplies selection after camera setup,
  which resets viewport selection; importing into an existing document makes
  the imported active layer visible alongside its selection. Older extraction
  versions keep their first-scene/layer behavior. The C++ build passed;
  multi-scene Blender comparison remains open.

### Edit Mode wire depth offset (2026-10-15)

- GPU Edit Mode scene strokes keep their small depth offset regardless of the
  Edit Mode Smooth Wires preference. The preference changes their edge
  coverage only. Blender's custom Smooth Wires depth-bias switch applies to
  the regular Overlay wire pass; edit-mesh edges use a separate fixed screen
  offset. The C++ build passed; matched depth-crossing captures remain open.

### Blender-evaluated NURBS surface wires (2026-10-15)

- Blender imports now carry evaluated SURFACE mesh edge paths as optional
  display-only world-space line segments. Extraction is bounded to 200,000
  edges and skips objects with modifiers, whose evaluated wire geometry would
  not match the preserved exact NURBS patches. Multi-spline objects are
  evaluated one patch at a time so their display edges remain attached to the
  matching imported surface. A saved geometry fingerprint disables these
  cached paths after control-net or trim edits; current geometry then uses the
  generated wire fallback. Native classiCAD save/reopen preserves both the
  paths and their fingerprint separately from the authoritative NURBS
  geometry. QPainter and GPU wire overlays consume the same valid imported
  paths. `evaluated.to_mesh().edges` is being used as a display proxy for
  Blender's legacy SURFACE overlay, but edge-set equivalence has not been
  confirmed. The C++ build passed; no matched Blender image comparison has
  been run.

### Blender collection geometry instances (2026-10-15)

- Blender extraction now expands visible collection instances of mesh, curve,
  NURBS surface, font, metaball, scalar Volume, and static file-backed Image Empty objects
  into separate scene items, keeping mesh vertices
  local with the instance transform and applying that transform to exact curve
  and surface control points. Mesh material slots, surface patch materials,
  bounds transforms, and Blender's source-object Random color identity are
  retained. Per-scene synthetic IDs keep repeated copies distinct; extraction
  is capped at 50,000 instances per scene. The source collection hierarchy,
  instance selection, per-instance Random variation, curve material slots, and
  image-texture UV evaluation for exact NURBS instances are not represented.
  Font glyphs and metaball surfaces are baked from evaluated geometry; their
  source editing settings are not retained. Image instances use the source
  object's per-instance world matrix and are capped at 256 instances and 16M
  decoded pixels per scene, in addition to the packed-byte limit. Other
  generated instance types remain unsupported. Scalar Volume instances use
  the instance matrix for object-to-world placement and are capped at four
  instances, 32 MiB aggregate payload, 524,288 active records, and 16M dense
  voxels per scene. The C++ build passed;
  no matched Blender instance comparison has been run.

### Transform theme color on selected overlays (2026-10-15)

- Blender changes selected object wire and Bounds display colors to the
  viewport theme's Transform color while an object transform is active.
  Blender extraction now retains that theme color, and classiCAD applies it
  to selected mesh/surface wires, surface outlines, and Bounds while scale or
  rotate previews are active. Ordinary selection continues to use the active
  or selected theme colors. The C++ build passed; a matched transform-preview
  comparison remains open.

### QPainter Workbench effect and overlay ordering (2026-10-15)

- When the bounded raster surface compositor succeeds, QPainter now keeps the
  scene and editor overlays in separate images whenever outline, shadows, or
  depth of field is enabled. Effects resolve on the scene image before the
  overlay image is composited, including when shadows and depth of field are
  both enabled. This remains limited to compositor-supported frames and is
  disabled for X-Ray. The C++ build passed; no matched Blender comparison has
  been run.

### QPainter Workbench effect memory bound (2026-10-15)

- The software effects pass previously allocated multiple full-resolution
  depth, normal, and object buffers without a practical pixel limit. It now
  skips those effects above one million target pixels and displays retained
  scene-linear surfaces without effects when available. This bounds CPU
  memory use but leaves shadows, cavity, outlines, and depth of field absent
  in oversized QPainter fallback frames. Blender's GPU path does not share
  this CPU allocation limit. The C++ build passed; matched output remains
  open.

### Blender font visibility in original mesh mode (2026-10-15)

- Blender text objects now import as their currently evaluated glyph mesh even
  when the user chose original mesh mode, so visible text is not omitted based
  on that mesh-only choice. The imported glyphs remain baked geometry; editable
  text, font settings, and layout remain unsupported and reported. The C++
  build passed; no matched text comparison has been run.

### Blender Image Empty display proxies (2026-10-15)

- Static file-backed Image Empties now import as classiCAD Picture objects.
  The image quad follows Blender's normalized image aspect, Empty display size,
  object transform, and image offset; readable packed images and external
  files are supported with 32 MiB packed-byte and 16M decoded-pixel bounds.
  Projection-specific visibility, image side, depth modes, alpha blending,
  and scene-linear object tint are retained. QPainter bakes tint only for
  source images up to 16M pixels; larger sources keep their original RGB.
  Opaque cutouts are thresholded after filtering only when the viewport raster
  fits within 16M pixels. Blender's image alpha mode is retained: GPU drawing
  converts decoded RGB to scene-linear, premultiplies when Blender requests
  PREMUL, then unpremultiplies the filtered sample before tinting. QPainter
  works from Qt's straight-format conversion. Generated or
  animated images and matched comparison remain unsupported. GPU Image Empty
  textures now generate mip levels and use trilinear minification; the
  QPainter fallback still relies on Qt's smooth image transform.
  The C++ build passed;
  no matched Blender image-empty comparison has been run.

### Blender metaball visibility in original mesh mode (2026-10-15)

- Blender METABALL objects now import as their current evaluated polygon mesh
  in either mesh import mode, so the original-mesh choice does not omit their
  visible surface. Editable metaball data, threshold settings, and alternate
  animation states remain unsupported and reported. The C++ build passed; no
  matched metaball comparison has been run.

### Evaluated FONT/META extraction with original mesh mode selected (2026-10-15)

- FONT glyphs and METABALL surfaces use evaluated geometry even when the
  selected global mesh mode is original. The extractor now obtains the scene's
  dependency graph for those conversions instead of passing the absent
  original-mode graph into Blender's evaluated mesh API. Updated the import
  status summary to describe the currently supported image-empty proxies and
  the remaining unsupported object kinds.

### Blender curve visible-surface proxies (2026-10-15)

- Curves with bevel, extrusion, bevel-object, 2D fill, or modifiers now retain
  their exact supported source splines and add a uniquely identified companion
  mesh from Blender's current evaluated curve. The proxy carries the source
  object identity for selection and collection-instance metadata. Surface
  settings and alternate evaluated states are not editable or retained; no
  matched Blender curve comparison has been run. Evaluated mesh acquisition is
  now inside the cleanup scope, so `to_mesh_clear()` runs if mesh conversion
  fails or returns no mesh.

### Collection-instanced Blender text and metaballs (2026-10-15)

- Collection instances containing FONT or META objects now use the same
  evaluated mesh extraction as their direct scene-object counterparts. Each
  keeps the collection instance transform and parent visibility; text and
  metaball editing data remain unsupported. No matched instance comparison
  has been run.

### Collection-instanced Blender Image Empties (2026-10-15)

- Static file-backed Image Empties inside collection instances now import as
  Picture items using each instance's world matrix. To bound repeated image
  decoding, extraction limits these instances to 256 items and 16M decoded
  pixels per scene, while sharing the existing 32 MiB packed-byte budget.
  Projection modes and other Image Empty display rules remain unsupported;
  no matched comparison has been run.

### Collection-instanced Blender scalar Volumes (2026-10-15)

- Supported scalar Volume grids inside collection instances now retain their
  grid-to-object transform and use the dependency-graph instance matrix for
  object-to-world placement. Per-scene instance, payload, record, and dense
  voxel caps bound duplicated grids. Other Volume grid types remain unsupported;
  no matched instance comparison has been run.

### Blender Grease Pencil centerline import (2026-10-15)

- Blender 5.2 Grease Pencil objects now import their visible, multi-point
  strokes from each layer's current frame as classiCAD POLY curves. The
  evaluated object transform and each unparented layer's local transform are
  applied before import; cyclic strokes are closed. Collection instances use
  their instance transform. Per-scene caps limit extraction to 10,000
  strokes and 100,000 points. Stroke width, opacity, color, fills, layer
  effects, modifiers, animation, parented layers, and one-point dots remain
  unsupported and are reported. This retains line paths as ordinary curves,
  not Blender's thick Grease Pencil appearance. The C++ build passed; Blender
  fixture and matched viewport comparisons have not been run.

### Blender Hair Curves centerline import (2026-10-15)

- Blender Hair Curves objects and collection instances now import their
  multi-point strand control paths as classiCAD POLY curves. Source point
  offsets are validated; evaluated object and instance transforms are applied;
  per-scene limits cap extraction at 10,000 strands and 100,000 points. This
  preserves path locations, but turns curved interpolation into straight
  segments and omits strand radius, profile, color, materials, and evaluated
  modifier output. One-point strands remain unsupported. The C++ build passed;
  Blender fixture and matched viewport comparisons have not been run.

### Blender Point Cloud import (2026-10-15)

- Blender `POINTCLOUD` objects and collection instances import as one grouped
  classiCAD scene object, retaining evaluated world positions, per-point
  radii, supported point-domain colors, and source indices. Import is capped at
  100,000 points per scene. Point radii follow Blender 5.2.2 Workbench's
  `length(to_scale(object_to_world)) / sqrt(3)` transform rule; Gaussian splats
  are reported as unsupported.
- The grouped data is serialized natively and participates in scene rendering,
  radius-aware click picking and box selection, snapping, transforms,
  and duplication.
  GPU markers and the QPainter fallback now project each world radius into a
  screen-space disk; both paths use a one-pixel minimum diameter. Point colors
  still use object color, and the raster path uses flat disks rather than
  Workbench's shaded point surfaces.
  Box selection uses projected point footprints for crossing and full
  containment. This follows the visible geometry, but Blender's exact
  Workbench box-selection behavior has not been source-verified. Blender
  fixtures and matched viewport comparisons have not been run.

- Non-image Blender Empty display icons are reported as viewport overlays with
  no matching classiCAD scene-object representation. Still Image Empties retain
  their existing Picture import path.

### Blender mesh extraction payload bounds (2026-10-15)

- Mesh extraction now estimates the expanded vertex, loop, polygon, and edge
  payload before building its JSON arrays. The estimate also accounts for UV
  layers, mesh attributes, and material slots. A mesh is capped at 32 MiB
  estimated payload; direct meshes, generated curve/font/metaball meshes, and
  repeated collection instances share a 128 MiB per-scene budget. Oversized
  items are skipped with an unsupported-item reason and a scene-level summary.
- The extractor rejects JSON handoffs over 256 MiB, and the C++ importer checks
  the file size before reading it into memory. These bounds are transport
  safeguards; they do not change Blender geometry or the supported mesh
  representation. Evaluated modifiers must still produce a Blender mesh
  before its final size can be checked, so these limits bound Python payload
  construction and classiCAD JSON parsing, not Blender's internal evaluation
  allocation. The byte estimate is approximate, and the final JSON cap is
  checked after writing the temporary file. C++ build passed; Blender fixture
  loading was not run.

### Blender mesh isolated vertex import (2026-10-15)

- Vertices that belong to no face or edge now import as individually
  selectable classiCAD Point items. Each derived item uses the source mesh or
  collection-instance identity plus its source vertex index, and carries the
  transformed world position, display color, collection membership, and
  per-view-layer visibility. The scene allows at most 10,000 derived points
  per source mesh and 50,000 per scene; over-limit point sets are reported and
  omitted. This is a bounded bridge using classiCAD's existing one-point
  object type; Blender POINTCLOUD remains a separate unsupported batched type.
  C++ build passed; no Blender fixture comparison was run.

### Blender evaluated object transforms in original geometry mode (2026-10-15)

- Original mesh mode now preserves source mesh vertices while applying each
  object's current dependency-graph world transform, matching Blender's
  displayed placement when constraints or animation affect the transform.
  The same transform handling now covers source spline curves, NURBS surfaces,
  still Image Empties, Grease Pencil paths, Hair Curves, and scalar Volumes;
  collection instances continue to use their captured instance matrices.
  Image Empty type, size, offset, opacity, depth, and
  perspective/orthographic visibility now come from the evaluated object as
  well. Blender's Object RNA leaves `empty_display_type` animatable.
  Geometry that is explicitly evaluated, such as text and metaballs, remains
  evaluated as before. C++ build passed; no matched Blender comparison was run.

### Blender mesh wire offset with Smooth Wires disabled (2026-10-15)

- Regular Blender mesh wire endpoints now receive the view-facing normal
  offset used when Smooth Wires is disabled, capped at half a screen pixel,
  plus Blender's tiny forward depth nudge. The GPU path applies both in clip
  space; the ordinary QPainter fallback converts the clip depth nudge through
  the viewport near/far range before its visibility check. Blender's optional
  wire curvature weight is reconstructed from adjacent polygon normals for
  each edge; Blender's separate NDC offset is implemented in the following
  category. Blender's neighboring scene-depth adjustment for
  Smooth Wires is also implemented in the following category. The C++ build passed; no matched
  runtime comparison has been run.

### Blender mesh Smooth Wires neighboring depth (2026-10-15)

- In the opaque, single-sample GPU stroke resolve, regular Blender mesh wires
  now compare their depth with the center scene pixel and the two neighboring
  pixels perpendicular to the edge. Eligible fragments use center scene depth
  before the hardware depth test, so they are not discarded before line
  composition. The resolver retains the same bounded correction for candidate
  arbitration. Multisample rendering, the QPainter fallback, Blender's mesh-wire
  runtime shader validation, and matched visual
  comparison remain open. The C++ build passed.

### Blender mesh wire NDC depth offset (2026-10-15)

- Regular mesh wire strokes now apply Blender's view-dependent NDC depth
  offset in both GPU and QPainter paths, with the perspective and orthographic
  factor derived from the active projection and clip range. The QPainter path
  converts the normalized depth shift back to camera depth for visibility
  testing. Runtime shader and matched image comparison remain open; the C++
  build passed.

### Blender mesh wire curvature offset factor (2026-10-15)

- Smooth Wires-off mesh-wire offsets now reconstruct Blender's per-edge `wd`
  from the adjacent face normals and scale the screen and forward-depth offset
  by `(1 - wd * 0.75)`. Boundary and non-manifold edges use Blender's zero
  default. The same per-endpoint offset feeds GPU strokes and QPainter's
  software visibility path. Blender's transient subdivision edge data is not
  available in imported meshes. C++ build passed; matched image comparison
  remains open.

### Blender mesh wire endpoint corner normals (2026-10-15)

- Mesh-wire offset directions now use the transformed source corner normal at
  each emitted edge endpoint, matching Blender's loop-domain wire normal input.
  The old vertex-averaged normal remains only as a fallback when corner data is
  absent. This keeps hard-edge offsets separate on adjoining faces; shared
  edges still use classiCAD's selected representative face. C++ build passed;
  matched image comparison remains open.

### Blender curve Overlay depth offset (2026-10-15)

- Overlay curve strokes no longer receive classiCAD's generic forward-depth
  nudge when Smooth Wires is enabled. Blender 5.2.2 applies its optional
  smooth-wire depth correction and NDC offset only in the mesh branch of
  [`overlay_wireframe_vert.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/overlay/shaders/overlay_wireframe_vert.glsl#L116-L134);
  the curve branch explicitly skips both.
  Blender mesh wires retain their existing NDC offset or smooth resolve, and
  CAD Edit Mode controls keep their separate fixed overlay offset. The C++
  build passed; a matched curve-wire depth comparison remains open.

### QPainter shadow samples outside caster bounds (2026-10-15)

- Four-sample QPainter shadow coverage now treats light-map positions outside
  the caster-bounded projection as unshadowed instead of clamping to the edge
  texel. Directional shadow volumes preserve their position in the plane
  perpendicular to the light, so caster shadows cannot extend beyond those
  projected bounds. C++ build passed; matched image comparison remains open.

### Shadow caster texture alpha status correction (2026-10-15)

- Blender's shadow pass casts the full triangle geometry without sampling
  Image Texture alpha. A previous QPainter change incorrectly applied the
  color pass's `0.1` cutout to its software shadow map; that alpha test has now
  been removed. GPU stencil, GPU depth-map, and QPainter software shadow paths
  all cast the full geometry. Texture cutouts remain active in the color and
  other applicable effect passes.

### Restore Blender shadow caster coverage in QPainter (2026-10-15)

- The CPU light-depth shadow map now rasterizes every in-bounds caster triangle
  without consulting its active Image Texture alpha, matching Blender's
  geometry-based shadow pass. The UV/image alpha sampling used by color,
  outline, and cavity paths is unchanged. C++ build passed; matched shadow
  comparison remains open.

### Blender mesh loose-edge overlay visibility (2026-10-15)

- Imported loose mesh edge companions now retain an explicit provenance flag
  through Blender extraction, native documents, and viewport render objects.
  In Solid mode, classiCAD hides those curve companions when Blender's saved
  overlays are hidden; Wireframe mode keeps them visible. Ordinary CAD curves
  are unaffected. C++ build passed; a matched view comparison remains open.

### Blender mesh wire handling for loose edges (2026-10-15)

- Visible Solid-mode loose-edge companions now use the mesh-wire stroke path in
  both GPU and QPainter rendering. This applies Blender's mesh-wire depth/NDC
  offsets and neighboring scene-depth correction while keeping them in the
  Overlay category. They have no imported face-corner normals or adjacent-face
  curvature data, so those offsets use the renderer's existing zero/default
  behavior. C++ build passed; matched image comparison remains open.

### Blender loose mesh wire color and opacity (2026-10-15)

- Loose-edge companions now use the imported viewport wire color mode instead
  of the generic Solid curve color. Theme mode follows Blender's selected or
  unselected rim color; Object and Random modes use Blender's selected-color
  adjustment and no-normal facing mix. The wireframe opacity setting controls
  their alpha. The same prepared stroke carries these values through the GPU
  and QPainter paths. C++ build passed; matched image comparison remains open.

### Loose mesh wire follows source selection (2026-10-15)

- Native scene objects now retain the source Blender object identity for
  derived loose-edge companions. The viewport resolves the source mesh's live
  selected, active, and transform-preview state and uses it when choosing the
  companion wire color. This keeps the edges separate selectable scene objects
  while matching the source mesh's selected Theme/Object/Random wire treatment.
  C++ build passed; matched image comparison remains open.

### Per-edge selection for loose mesh companions (2026-10-15)

- Each companion curve now keeps the Blender source edge index for every
  loose-edge component through Blender import and native save/load. In Edge
  selection mode, selected companion components get their own wire overlay;
  the remaining edges keep the unselected wire color. Source-edge selections,
  when present, map by the saved indices to the matching curve components.
  Source-object lookup now prefers the original mesh over its derived
  companion, preserving active/selected state when both share a source
  identity. The C++ build passed; matched selection-image comparison remains
  open.

### QPainter Workbench outline composition (2026-10-15)

- The QPainter outline fallback now reconstructs bounded scene-linear color
  from display RGB for the Standard transform, blends Blender's outline color
  and coverage in scene-linear space, then applies exposure, sRGB conversion,
  and gamma again. This applies where the retained linear compositor is not
  available; AgX, Filmic, Neutral, and imported custom-LUT outline fallback
  blending remain display-space approximations. Alpha is preserved. C++ build
  passed; matched image comparison remains open. A follow-up flow audit found
  that whenever retained scene-linear color is available, the weighted and
  sorted compositors already apply the outline before the display transform
  (or retain its coverage for the later effects resolve) for every transform.
  The remaining approximation is limited to fallback routes without that
  source-linear backing; reverse-transforming AgX/custom display pixels cannot
  recover the original scene color.

### QPainter software shadow texture-alpha audit (2026-10-15)

- The software shadow map submits full geometric caster triangles even when
  the visible receiver raster skips texture-alpha cutouts. This matches
  Blender 5.2.2 Workbench shadow volumes, which are built from geometry and do
  not sample image UV alpha. No alpha-mask sampling was added to the shadow
  map; that would make its shadows differ from Blender. The relevant Blender
  implementation is
  [`workbench_shadow.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_shadow.cc#L348-L390).

### Blender Image Empty view visibility (2026-10-15)

- Imported Image Empties now retain their perspective/orthographic display
  flags, front/back/both side, axis-aligned-only setting, and Default/Front/Back
  depth preference in native shape data. Both QPainter and GPU picture paths
  apply the projection, side, and axis-alignment visibility rules from Blender's
  [empty-image visibility checks](https://github.com/blender/blender/blob/v5.2.2/source/blender/blenkernel/intern/object.cc#L3577-L3629).
  GPU drawing now places Back images before scene surfaces, depth-tests Default
  images against the scene, and draws Front or `showInFront` images in the
  overlay pass. If the GPU surface pass fails, Default images are redrawn
  against the rebuilt scene depth after the software surface fallback. Blender's
  overlay behavior is documented in the [pinned image Empty path](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/overlay/overlay_empty.hh#L247-L354).
  QPainter applies these visibility rules and uses retained scene depth when
  available. Its bounded surface compositor now blends Back images beneath
  transparent surfaces; direct depth masking still treats retained transparent
  surfaces as opaque, and failed picture-texture previews remain unmasked.
  C++ build passed; matched image comparison remains open.

### Blender Image Empty alpha blending and tint (2026-10-15)

- Imported Image Empties now retain Blender's object RGBA color and
  `use_empty_image_alpha` setting in native shape data. The GPU shader
  applies the tint in scene-linear color and reproduces Blender's 0.05 source
  alpha cutout when alpha blending is off. QPainter applies the tint and
  cutout for images up to 16M pixels and encodes their linear image colors to
  sRGB without applying the Workbench view transform, matching Blender's
  overlay image shader. Under the bounded raster surface path, translucent
  and non-default-depth images are deferred until after Workbench effects and
  blended in linear light. Direct QPainter fallbacks still blend in encoded
  display space.
  Blender's shader behavior is pinned in
  [`overlay_image_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/overlay/shaders/overlay_image_frag.glsl#L13-L30).
  C++ build passed; matched comparison remains open.

### Blender Image Empty depth writes (2026-10-15)

- Opaque Default-depth GPU images now use Blender's LESS comparison and write
  depth after the shader's alpha cutoff. Blended Default and Back images use
  LEQUAL without depth writes; Front images use no depth test or writes. This
  follows Blender's separate passes. Default images also use its view-dependent
  projection depth bias; Back and Front remain unbiased. See the pass states in
  [`overlay_empty.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/overlay/overlay_empty.hh#L78-L87)
  and the depth-bias matrix setup in
  [`draw_view.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/intern/draw_view.hh#L182-L193).
  QPainter keeps its retained-depth masking behavior. C++ build passed; no
  matched image comparison has been run.

### Blender Image Empty premultiplied alpha (2026-10-15)

- Image Empty import now retains Blender's PREMUL setting as an image semantic,
  independent of Qt's decoded pixel format. The GPU byte path now premultiplies
  source RGB bytes before upload, uses an sRGB texture for sRGB inputs, and
  unpremultiplies the decoded filtered sample before tinting. Known linear and
  Non-Color byte sources use a linear texture; other OCIO spaces normally use
  the imported scene-linear float payload. If that payload is absent, the
  bounded byte fallback assumes sRGB. This follows Blender's
  byte/float upload branches and `texture_read_as_linearrgb`, pinned in
  [`overlay_empty.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/overlay/overlay_empty.hh#L301-L307)
  [`image_gpu.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/blenkernel/intern/image_gpu.cc#L755-L786),
  [`colormanagement.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/imbuf/intern/colormanagement.cc#L1924-L1965),
  and [`draw_colormanagement_lib.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/intern/shaders/draw_colormanagement_lib.glsl#L16-L25).
  Matched comparison remains open.

### Blender Image Empty mipmapped minification (2026-10-15)

- The GPU Image Empty texture builds the mip chain from the Blender-compatible
  byte/float texture upload and uses trilinear minification with linear
  magnification. sRGB byte inputs use `GL_SRGB8_ALPHA8`; linear/data byte
  inputs use `GL_RGBA8`; float payloads use `GL_RGBA32F`. PREMUL byte images
  are premultiplied before mip generation, and the texture cache refreshes when
  that image setting changes. Blender builds and enables
  image mipmaps in
  [`image_gpu.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/blenkernel/intern/image_gpu.cc#L2917-L2928).
  For source images and viewports within the 16-million-pixel CPU budget, the
  QPainter path now builds scene-linear mip levels with premultiplied-alpha
  filtering and blends the two adjacent levels by the fractional part of the
  projected quad's screen footprint before Qt's final smooth transform.
  Blender uses implicit per-fragment LOD and trilinear mip blending, while
  QPainter uses one bounded LOD for the whole quad; strong perspective
  minification can still differ.
  Oversized inputs retain the existing fallback. No matched image comparison
  has been run.

### QPainter Image Empty filtered alpha cutout (2026-10-15)

- When Blender Image Empty alpha blending is disabled, QPainter now filters
  the image into a bounded device-pixel raster before applying Blender's 5%
  alpha cutoff. The existing retained scene-depth mask is applied after that
  cutoff, and the raster is composited without another scaling pass. If the
  viewport raster exceeds 16M pixels, QPainter keeps the source-texel cutoff
  fallback. Oversized sources also skip RGB tint preprocessing, even when this
  bounded raster applies the cutoff. Filtering through premultiplied QImage
  pixels can still differ when fully transparent texels contain nonblack RGB.
  No matched image
  comparison has been run.

### Image Empty alpha mode and depth writes (2026-10-15)

- The retained scene-depth geometry now gives Image Empties depth only for
  Default images with alpha blending disabled and `In Front` off. Its bounded
  conservative mask keeps cells whose source pixels all pass Blender's 10%
  alpha cutoff; blended, Back, and Front images no longer occlude later
  scene-depth queries. Blender separates the
  depth-writing opaque image pass from blended passes in
  [`overlay_empty.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/overlay/overlay_empty.hh#L1132-L1145), and applies the alpha cutoff in
  [`overlay_image_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/overlay/shaders/overlay_image_frag.glsl#L13-L30).
  The 256×256 source mask remains conservative at cutout edges. C++ build
  passed; no matched image comparison has been run.

### QPainter Image Empty scene-depth masking (2026-10-15)

- The direct QPainter picture fallback uses retained scene depth when available.
  Default images are masked where a nearer surface is present; Back and Front
  images bypass the mask, matching Blender's final color-only output target.
  The bounded deferred overlay route also draws Back and Front without
  scene-depth masking. The original image alpha and filtering are preserved.
  The bounded raster mask is skipped above 16 million depth pixels or when depth is
  unavailable; retained transparent surfaces are treated as occluding, and
  failed picture-texture previews still use the unmasked draw path. C++ build
  passed; matched image comparison remains open.

### QPainter Back Image blending with transparent surfaces (2026-10-15)

- Back images use Blender's late color-only overlay output pass, so they draw
  over the scene without an opaque-depth mask. QPainter's bounded surface
  compositor already draws them in a separate overlay after scene effects.
  When that compositor cannot run, QPainter now keeps Back and Front images
  in a far-to-near deferred overlay when the existing image-work budget allows;
  otherwise the direct painter path can still draw them inline in scene order.
  The selected image frame is drawn with its image. Blender's
  Back-image pass uses premultiplied-alpha blending with depth LEQUAL, while
  the final output framebuffer has no depth attachment; see pinned
  [`overlay_empty.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/overlay/overlay_empty.hh)
  and the final output setup and ordering in
  [`overlay_instance.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/overlay/overlay_instance.cc).
  QPainter blends this late image layer over display-transformed scene pixels,
  while Blender composites its scene-linear image sample before final display
  conversion. Semi-transparent image pixels can therefore differ under AgX or
  custom transforms. Transparent viewport film remains unsupported; the
  native-GPU renderer-failure fallback and matched image comparison remain
  open. C++ build passed.

### QPainter curvature independent of World cavity taps (2026-10-15)

- QPainter now preserves its Screen-curvature response when every World-cavity
  tap is outside the viewport or has no usable depth sample. This matches
  Blender's resolve, which computes curvature independently and multiplies it
  with the World-cavity factor. The change affects Screen and Both cavity
  modes; matched edge-pixel comparison remains open.

### QPainter effects on large viewports (2026-10-15)

- The software effect pass now reduces its internal depth/normal/object-ID
  buffer resolution when the viewport exceeds one million samples, then maps
  its effects over the full-size scene image. This keeps QPainter shadows,
  outlines, and other enabled effects active on large displays while retaining
  the existing one-million-sample memory bound. Large-view effect detail is
  coarser than Blender's full-resolution GPU result; matched image comparison
  remains open.

### QPainter Workbench outlines use opaque surface IDs (2026-10-15)

- The software Workbench outline now reads a separate object-ID/depth buffer
  populated only by Blender-opaque surface fragments, with `showInFront`
  fragments retaining their separate precedence. Semi-transparent surfaces
  still contribute to visible color and selection contours, but no longer
  replace the IDs that define Workbench silhouettes. The bounded weighted and
  sorted scene-linear compositors now derive Workbench outlines from their
  opaque object buffers too. Blender 5.2.2 attaches
  the object-ID target to its opaque pass, renders transparent passes
  separately, then runs the outline pass over that target in
  [`workbench_engine.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_engine.cc#L2603-L2645)
  and [`workbench_effect_outline.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_effect_outline.cc#L329-L367).
  C++ build passed; matched outline comparison remains open.

### QPainter deferred outline fallback ownership (2026-10-15)

- When the weighted or sorted compositor has retained a full-size
  scene-linear outline mask for the later effects resolve, the generic CPU
  outline fallback now skips drawing a second outline from its own ID buffer.
  The fallback remains enabled if the retained scene-linear color or coverage
  mask is missing or has the wrong size, and selected-object outlines remain
  separate. C++ build passed; matched outline comparison remains open.

### QPainter shadow comparison bias cap (2026-10-15)

- The software shadow depth-map fallback now caps its scene-range tolerance to
  two ULP at the compared caster and receiver depths. This keeps the small
  float-precision guard for its raster map without allowing distant geometry
  to inflate the receiver comparison threshold and hide nearby blockers.
  Blender uses stencil shadow volumes without a receiver bias; the QPainter
  depth-map path remains an approximation. C++ build passed; matched shadow
  comparison remains open.

### QPainter NURBS surface wire depth testing (2026-10-15)

- Committed QPainter NURBS surface and `NurbsSolid` wire segments now reuse
  the bounded camera-depth sampling used by committed curve strokes. Hidden
  segments are split against retained opaque scene depth, while the existing
  depth tolerance keeps coplanar face wires visible. Preview wires retain
  their existing draw path. Blender's Overlay wireframe uses depth-tested
  `LESS_EQUAL`; matched edge visibility comparison remains open.

### QPainter opaque prepass coplanar depth ties (2026-10-15)

- QPainter per-pixel surface samples now let a later triangle replace an
  earlier one at equal camera depth, matching Blender's GPU opaque prepass
  `GL_LEQUAL` depth test. This applies to the visible, in-front, cavity/shadow,
  and opaque outline-ID samples, including the bounded weighted and sorted
  compositors, so coplanar scene order consistently selects the same surface
  ID and normal. C++ build passed; matched image comparison remains open.

### GPU smooth-wire equal-depth neighbor blending (2026-10-15)

- The per-pixel wire resolve now alpha-overs neighboring wires only when they
  are strictly nearer than the current line sample. Equal-depth neighbors use
  Blender's alpha-under branch and do not replace the tracked depth, matching
  `neighbor_blend` in pinned
  [`overlay_antialiasing.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/overlay/shaders/overlay_antialiasing.bsl.hh#L769-L794).
  The final resolved-line/scene comparison retains its separate rule. Build
  passed; runtime GPU and matched image comparisons remain unverified.

### QPainter weighted transparency fragment budget (2026-10-15)

- The weighted QPainter compositor now accumulates transparent fragments into
  regular and `showInFront` per-pixel OIT buffers as each object's bounded
  fragment list is processed. Previously, the combined scene-wide fragment
  list stopped at four million samples and sent the whole viewport to the
  depth-sorted painter fallback, changing transparency ordering and blending.
  The weighted result and retained transparent depth now scale with the
  already bounded pixel target instead of total scene fragment count.
- Per-object fragment capture remains capped at four million samples, and
  that limit can still select painter fallback for exceptionally dense single
  objects. Transparent receiver shadowing now uses bounded per-pixel shadowed
  OIT accumulation rather than a one-million-fragment retained list. No
  matched Blender comparison was run; `cmake --build build -j4` passed.

### QPainter committed curve depth writes (2026-10-15)

- The prepared-stroke fallback now updates its retained depth after drawing
  each visible committed Solid curve segment whose GPU stroke would write
  depth. Later fallback strokes test against that updated depth, so a farther
  curve cannot paint over a nearer curve simply because it was drawn later.
  The CPU update stamps the segment's visible width into covered target pixels;
  it remains a bounded approximation of Blender's per-fragment line coverage.
- X-Ray hidden strokes and non-depth-writing overlays do not update the depth
  buffer. No matched Blender comparison was run; `cmake --build build -j4`
  passed.

### Image Texture extension-mode status (2026-10-15)

- Blender Image Texture extension settings `REPEAT`, `EXTEND`, `CLIP`, and
  `MIRROR` are imported, saved in native documents, and applied by both the GPU
  and QPainter samplers for meshes and NURBS surfaces. Matched perspective
  mesh captures cover all four modes in the GPU path and all four modes in the
  QPainter path; the QPainter comparisons retain small raster-edge differences
  (up to 17 foreground-mask pixels in the recorded CLIP fixture). These
  results supersede older summary text that described QPainter extension
  handling as repeat-only. Other filter settings, geometries, and views remain
  open as recorded in the individual comparison entries above.

### QPainter dense transparent shadow receivers (2026-10-15)

- The weighted software compositor now accumulates a second regular/in-front
  transparency result with Blender's normal-based transparent shadow factor
  applied per fragment. The effects pass uses that per-pixel result instead of
  retaining up to one million fragments and applying the nearest layer's
  factor to a dense overlapping stack. Opaque receiver shadowing and the
  unshadowed transparent alpha remain separate. If a fragment has no valid
  receiver normal, the existing nearest-layer approximation remains active.
- Blender 5.2.2 applies `get_shadow` to each transparent fragment before its
  weighted color accumulation in
  [`workbench_transparent_accum_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_transparent_accum_frag.glsl#L478-L526).
  The QPainter light-depth map and weighted rasterization remain approximate;
  no matched Blender comparison was run.
- This update supersedes the broader shadow summaries above that said all
  transparent receiver layers share one aggregate factor. That remains true
  only when the compositor uses a fallback or a fragment lacks valid receiver
  metadata.

### GPU smooth-wire dense-scene alpha gate (2026-10-15)

- The fullscreen wire resolve now gates Blender's dense one-pixel-line
  correction on the original rasterized center wire's alpha. It previously
  checked the scene pixel alpha; Blender's `color_tx` is its transparent
  overlay-color target, separate from the scene, so this was the wrong input.
  The check now matches `original_center_has_alpha` in pinned
  [`overlay_antialiasing.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/overlay/shaders/overlay_antialiasing.bsl.hh#L140-L193).
  Build and matched wire-image comparisons remain open.

### GPU smooth-wire depth-ordered overlay resolve (2026-10-15)

- The fullscreen wire resolve now starts from the rasterized center wire,
  alpha-composites each neighboring wire in Blender's fixed cardinal order,
  and compares each candidate against the currently retained depth before
  compositing the resulting overlay over the scene. Previously it combined
  every line color first, kept only the nearest candidate depth, and compared
  that aggregate once against the scene; a farther wire could therefore ride
  in front of the surface because another candidate was closer. Blender's
  resolve uses a separate transparent overlay target and updates depth as it
  blends each neighbor in
  [`overlay_antialiasing.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/overlay/shaders/overlay_antialiasing.bsl.hh#L763-L904).
  Neighbor depth is local to this resolve; only the center raster sample can
  update scene depth, since Blender's final AA target has no depth attachment.
  C++ build and matched GPU image comparison remain open.

### Blender mesh color attribute selection (2026-10-15)

- Attribute shading now prefers the mesh's default color attribute, which is
  Blender Workbench's render choice, and falls back to the active editing
  attribute when no default is available. Both GPU and QPainter mesh paths use
  the same preference. This fixes files where Blender's active and default
  attributes differ. No matched Blender image comparison was run.

### QPainter NURBS scene clipping (2026-10-15)

- The QPainter Solid path now clips each NURBS surface triangle against the
  viewport near and far depth planes before projection. It interpolates UVs
  and smooth normals at generated boundary vertices, and applies the same
  path to NURBS solid faces. A follow-up audit found that imported polygon
  meshes had still been dropping whole triangles when a vertex crossed either
  plane. They now clip their triangles before projection as well, interpolating
  UVs, corner normals, and color attributes at new vertices. This prevents
  out-of-range geometry from drawing and preserves the visible portion of
  crossing triangles. Build passed; no matched Blender image comparison was
  run.
- Committed curve strokes, prepared curve segments, Edit Mode control-cage
  edges, control-point markers, subdivision markers, and closing cage edges
  also use the viewport's near/far range. Crossing line segments are trimmed
  in world space before screen projection. The closing rectangle/polygon cage
  edge also uses the retained scene-depth test used by the other cage edges;
  when QPainter has no retained depth, the documented unoccluded fallback
  still applies. The NURBS surface wireframe's direct-painter fallback and
  legacy QPainter line, rectangle, Bezier, and NURBS fallback strokes now trim
  crossing segments too; Bezier fallback strokes are sampled before clipping.
  Blender's curve overlay binds the
  clipping planes in its edit-curve line and handle passes, and its screen
  projection helper clips line segments crossing near/far planes; see pinned
  [`overlay_curve.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/overlay/overlay_curve.hh#L850-L899)
  and [`view3d_iterators.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/editors/space_view3d/view3d_iterators.cc#L2150-L2257).

### Route NURBS surface wires through Edit Mode smoothing (2026-10-15)

- GPU strokes for NURBS surfaces and solids already identified themselves as
  Edit Mode wires, but their smooth-wire category defaulted to Overlay. They
  now use the Edit Mode Smooth Wires preference, leaving ordinary curve
  strokes on the independent Overlay preference. C++ build passed; matched
  wire-image comparison remains open.

### Image Empty alpha-channel blending (2026-10-15)

- The GPU Image Empty shader emits straight RGB, so its blend state now uses
  source alpha for RGB while accumulating alpha with `ONE,
  ONE_MINUS_SRC_ALPHA`. This avoids multiplying source alpha twice in the
  render target and matches Blender's premultiplied image output/blend
  contract in
  [`overlay_image_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/overlay/shaders/overlay_image_frag.glsl#L29-L30)
  and [`overlay_empty.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/overlay/overlay_empty.hh#L80-L87).
  C++ build passed; matched alpha-composite comparison remains open.

### Keep Image Empty frames visible when image data is filtered (2026-10-15)

- Visibility now separates the frame's perspective/orthographic flags from
  image-data side and axis-alignment checks. A disallowed view still hides
  both; when image data is filtered or missing, the QPainter frame remains
  visible, including for unselected objects, while image pixels are skipped.
  This follows Blender's
  separate `show_frame` and `show_image` checks in
  [`overlay_empty.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/overlay/overlay_empty.hh)
  and [`object.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/blenkernel/intern/object.cc).
  C++ build passed; no matched view comparison has been run.

### QPainter Back Image linear-light blending (2026-10-15)

- In the bounded surface-raster path, Back images are now drawn one at a time
  over the assembled final viewport output. Their premultiplied pixels are
  decoded from display sRGB, composited in linear light, then encoded back to
  display sRGB. This follows Blender's final overlay placement and
  premultiplied blend in
  [`overlay_image_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/overlay/shaders/overlay_image_frag.glsl)
  and the Back image pass setup in
  [`overlay_empty.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/overlay/overlay_empty.hh).
- Each deferred image also uses the retained opaque scene depth, so opaque
  objects still hide the portions of a Back image behind them.
- The deferred pass keeps its existing 16-million target-pixel budget across
  all Back images. The CPU blend receives display-sRGB pixels after applying
  the image's scene-linear source conversion and sRGB encoding, without the
  Workbench display transform. Its linear-light overlay blend happens after
  the scene display transform, matching Blender's separate Image Empty pass.
  Failed or over-budget conversions retain the sRGB fallback. Direct
  QPainter fallbacks continue using their existing composition. Build passed;
  no matched Blender comparison has been run.

### Blender Image Empty source color management (2026-10-15)

- Image Empty imports now retain the Blender source color-space name and a
  stable source-transform identity. Byte sRGB sources decode to scene-linear;
  known Linear and Non-Color byte sources are treated as linear data. Other
  supported source spaces and float image buffers carry a bounded
  scene-linear RGBA32F payload from Blender, including its PREMUL semantics.
- Payload transport is limited to 32 MiB per image (2,097,152 RGBA pixels)
  and 64 MiB across a scene. Invalid payloads are ignored with an import
  warning, and unsupported conversions keep the image visible with an sRGB
  fallback. Native JSON preserves the metadata and bounded payload.
- The GPU Image Empty texture uploads supported float payloads directly as
  RGBA32F, and includes source-space/transform/payload identity in its texture
  cache key. The bounded QPainter overlay path encodes converted scene-linear
  source pixels to sRGB without applying Workbench's view transform, then
  blends them in linear display-referred space after the scene transform. Its
  16-million-pixel processing cap retains the existing encoded-image fallback
  above the limit. Direct QPainter fallbacks still use their older
  composition. C++ build passed; GPU runtime and matched Blender comparisons
  remain open.

### GPU depth-of-field alpha gather (2026-10-15)

- The GPU DOF gather now carries alpha through its CoC-aware 2×2 downsample
  and 12-tap blur, then mixes the gathered alpha with the original pixel using
  the same CoC amount as color. The median cleanup now also filters alpha
  alongside RGB. Previously RGB blurred while alpha stayed sharp, leaving
  inconsistent transparent edges. Blender's DOF resolve, blur, and median
  passes process RGBA in
  [`workbench_effect_dof_resolve_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_effect_dof_resolve_frag.glsl)
  and
  [`workbench_effect_dof_blur1_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_effect_dof_blur1_frag.glsl).
  Runtime GPU and matched image comparisons remain open.

### GPU depth-of-field final alpha resolve (2026-10-15)

- The final GPU DOF resolve now keeps the blurred alpha produced by its
  CoC-aware gather and median stages. It no longer overwrites the result with
  the sharp source alpha. Blender 5.2.2 computes its final add/multiply resolve
  over RGBA, so transparent film now uses the same source/blur blend amount for
  alpha as for color. `cmake --build build -j4` passed; matched
  transparent-film captures remain open.
  Reference:
  [`workbench_effect_dof_resolve_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_effect_dof_resolve_frag.glsl#L304-L319).

### GPU depth-of-field CoC tap weights (2026-10-15)

- DOF taps now use each sample's circle-of-confusion radius and ring coverage
  weight. CoC uses Blender's half-aperture factor and is measured in viewport
  pixels; inverse viewport size is applied only when moving a tap in UV space.
  The gather no longer hard-rejects every sample with nearer depth, which
  suppressed foreground bokeh at depth edges. Blender derives each tap's weight
  from its CoC and sample-ring radius in
  [`workbench_effect_dof_blur1_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_effect_dof_blur1_frag.glsl).
  classiCAD keeps its bounded 12-tap gather; Blender's signed near/far CoC,
  mip selection, and full sample pattern are still more detailed. Runtime GPU
  and matched foreground-edge comparisons remain open; C++ build passed.

### QPainter Back Image depth ordering (2026-10-15)

- The deferred QPainter Back-image list now sorts by image-plane center depth
  before alpha compositing, from farther to nearer; equal-depth images retain
  scene order. Previously it used scene order for every image, which could
  change the result for overlapping translucent images. Blender assigns the
  same camera-space key in
  [`overlay_empty.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/overlay/overlay_empty.hh)
  and sorts `PassSortable` keys ascending in
  [`draw_pass.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/intern/draw_pass.hh).
  Build passed; no matched overlapping-image comparison has been run.

### QPainter oversized-mesh color fallback (2026-10-15)

- The rare raw-polygon fallback now runs Workbench scene-linear face colors
  through the selected Standard, AgX, PBR Neutral, Filmic, or imported LUT
  display transform before drawing. It preserves the existing flat face
  shading and alpha. Random and Custom colors reuse the scene-linear values
  already prepared for the normal Workbench triangle path. Build passed;
  matched Blender image comparison remains open.

### Validate imported polygon triangulations (2026-10-15)

- Blender's evaluated loop triangles are already preserved and used by both
  renderers. Blender import and native JSON loading now reject incomplete or
  malformed per-face triangle lists instead of drawing only part of a face.
  Validation checks the expected triangle count, loop indices, boundary-edge
  coverage, and paired internal edges. Meshes without stored Blender
  triangulation still use classiCAD's local tessellator. C++ build passed;
  malformed-input and matched color-image comparisons remain open.

### QPainter Image Empty PREMUL fallback audit correction (2026-10-15)

- Removed a byte-image fallback divide that was based on an incorrect
  assumption. Blender's PREMUL setting controls its converted GPU texture
  used for filtering; it does not establish that the original byte image read
  by QPainter is already premultiplied. The optional scene-linear float
  payload keeps its existing premultiply-on-import and unpremultiply-on-use
  handling. Matched Image Empty comparisons remain open.

### QPainter Image Empty linear-light filtering (2026-10-15)

- When both the source image and viewport fit the existing 16-million-pixel
  processing budgets, QPainter filters Image Empty source pixels as
  scene-linear values into the viewport raster, then applies the selected
  display transform to the filtered pixels. Before this change it transformed
  each source texel to display sRGB before Qt's smooth image resampling, which
  differs from Blender's shader order. Failed raster setup falls back to a
  display-transformed source image; over-budget source images keep their
  existing bounded fallback. Retained depth masking is used only when its
  raster matches the viewport target. Matched Blender comparisons remain open.

### GPU scalar-volume slice bounds (2026-10-15)

- GPU slice opacity now uses the selected axis extent of the transformed
  volume bounds in world space. The prior calculation scaled each local AABB
  axis independently and missed the way rotation mixes the other axes into
  Blender's world-space object dimensions. This matches Workbench's
  `BKE_object_dimensions_get()` input to the 0.05 slice step factor; matched
  rotated-volume images remain open.

### QPainter depth-of-field mip edge sampling (2026-10-15)

- QPainter now clamps each DOF mip coordinate before deriving bilinear texel
  indices and interpolation fractions. Previously, a gather just outside an
  image edge could blend the edge texel with the next interior texel, while
  Blender's filtered DOF texture lookup clamps at the edge. Matched edge-bokeh
  images remain open. Blender source: `workbench_effect_dof_blur1_frag.glsl`.

### Blender mesh-wire endpoint coverage (2026-10-15)

- Native GPU Smooth Wires now use perpendicular-distance coverage for
  imported Blender mesh edges and keep the expanded line quad flush with each
  endpoint. The prior finite-segment distance created rounded caps; Blender's
  Overlay resolve filters the stored perpendicular distance and leaves CAD
  curve end caps on their existing path. This follows `line_coverage()` in
  [`overlay_antialiasing.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/overlay/shaders/overlay_antialiasing.bsl.hh#L703-L727).
  Matched crossing-wire comparisons remain open.

### GPU DOF blue-noise channel (2026-10-15)

- The gather angle now reads alpha from the 64×64 blue-noise LUT, matching
  Blender's `get_random_vector()` in
  [`workbench_effect_dof_blur1_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_effect_dof_blur1_frag.glsl#L24-L32).
  The radial offset remains driven by interleaved gradient noise. C++ build
  passed; matched DOF output comparison remains open.

### Workbench active color attribute precedence (2026-10-15)

- GPU and QPainter Attribute mode now use the mesh's active color attribute
  first and fall back to the default attribute only when no active attribute
  is available, matching Blender 5.2.2. Previously both paths preferred the
  default attribute when both existed. C++ build passed; matched image
  comparison remains open.

### QPainter textured shadow-caster audit correction (2026-10-15)

- An attempted software shadow-map texture-alpha cutoff was removed after
  checking Blender 5.2.2's active shadow pass. Blender builds its stencil
  shadow volume from object edge and surface geometry and does not sample the
  image alpha there, even though the visible color/depth shaders discard low
  alpha fragments. The QPainter shadow caster therefore remains geometric to
  match Blender. See [`workbench_shadow.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_shadow.cc#L348-L389)
  and [`workbench_image_lib.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_image_lib.glsl#L34-L59).

### QPainter Image Empty overlay composition (2026-10-15)

- When the bounded software surface batch succeeds, QPainter now defers
  alpha-blended and non-default-depth Image Empties to the post-Workbench
  overlay stage, sorts them by camera depth, and composites their sRGB overlay
  pixels with linear-light alpha arithmetic.
  Their source color uses Blender's sRGB display encoding without the
  Workbench view transform. Default images keep scene-depth masking; Back and
  Front bypass it because Blender composites them in a depthless color-only
  target. Direct QPainter fallback scenes still use encoded-space blending
  for ordinary direct draws. Back and Front images use a late, depthless
  overlay when their bounded deferral is available; otherwise their inline
  order remains scene-dependent. If scene-linear image allocation or conversion
  fails, QPainter switches that draw back to its encoded-image path instead of
  treating source bytes as linear color. Front-depth and object-level In Front
  images bypass scene depth, matching Blender's no-depth image pass.
  See Blender's image shader,
  [`overlay_empty.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/overlay/overlay_empty.hh#L1121-L1145), and [`gpu_shader_image_overlays_merge_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/gpu/shaders/gpu_shader_image_overlays_merge_frag.glsl). C++ build and matched Image Empty comparisons remain open.

### QPainter Image Empty fractional mip blend (2026-10-15)

- QPainter now blends the two adjacent scene-linear mip levels according to
  the fractional quad-wide LOD, with alpha-aware premultiplied interpolation.
  This removes whole-level jumps at fractional minification while retaining
  the bounded whole-image footprint estimate. Mip levels are generated once
  through the selected upper level, and the maximum LOD follows the actual
  source dimensions, including long images. Blender still selects LOD per
  fragment, so perspective-skewed quads can differ; levels are regenerated
  per draw. C++ build and matched image comparison remain open.

### QPainter SSAO attenuation lower bound (2026-10-15)

- The QPainter World/Both cavity path now preserves Blender's full
  nonnegative attenuation range instead of forcing values below `0.01` up to
  that floor. Blender 5.2.2 exposes `matcap_ssao_attenuation` with a hard
  minimum of zero; its cavity shader separately protects the attenuation
  denominator with `1e-8`. Screen-only curvature is unaffected. See
  [`workbench_cavity.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_cavity.bsl.hh#L41-L100)
  and [`rna_scene.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/makesrna/intern/rna_scene.cc).
  C++ build passed; matched cavity comparison remains open.

### QPainter DOF prefilter color clamp (2026-10-15)

- The first half-resolution scene-linear DOF prefilter now clamps RGB to
  Blender's `[0, 3]` range before storing the image pyramid; later levels are
  built from those bounded pixels. Without this clamp, negative or HDR RGB
  values could bleed into the QPainter blur even though Blender clips them at
  the prepare pass. Alpha remains in its existing `[0, 1]` range. See
  [`workbench_effect_dof_prepare_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_effect_dof_prepare_frag.glsl#L405-L415).
  C++ build passed; matched DOF output comparison remains open.

### QPainter scene-linear DOF alpha (2026-10-15)

- Scene-linear QPainter DOF now keeps premultiplied RGB with alpha through its
  prefilter, mip pyramid, gather, median, and resolve. Blender's DOF shaders
  accumulate RGBA directly; the previous CPU path unpremultiplied before blur
  and multiplied by alpha again afterward, changing color around translucent
  edges. The display-referred fallback keeps its separate QImage path. Matched
  Blender DOF output remains open.

### QPainter DOF target-pixel radius (2026-10-15)

- The software DOF CoC already uses the physical color target width to produce
  a radius in target pixels. Removed the second multiplication by the image
  device-pixel ratio from its gather offsets, CoC weights, mip selection,
  resolve blend, and 100-pixel coverage dilation. The ratio remains in the
  mapping from color pixels to the separately bounded depth buffer. This keeps
  blur width consistent at high DPI and during supersampling. The Blender
  5.2.2 blur shader also applies CoC directly in target-pixel units in
  [`workbench_effect_dof_blur1_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_effect_dof_blur1_frag.glsl#L395-L425),
  with the 100-pixel cap in
  [`workbench_effect_dof_lib.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_effect_dof_lib.glsl#L291-L316).
  The C++ build passed; matched high-DPI and supersampled Blender DOF
  comparisons remain open.

### QPainter bounded viewport raster target (2026-10-15)

- The interactive QPainter viewport now reduces its internal supersampling
  scale as needed to keep the committed scene target at or below one million
  pixels, checking the rounded physical dimensions so the cap is guaranteed.
  The native-renderer failure fallback uses the same bounded target and
  smoothly scales its result back to the viewport. Prepared curve depth
  sampling maps viewport device coordinates into the retained depth map when
  the target runs below display resolution. These paths let large viewports
  retain the bounded scene-linear transparency compositor and depth maps
  instead of switching immediately to average-depth painter composition.
  Large views can render below display pixel density, and unsupported/offscreen
  targets can still take the legacy fallback. Matched large-viewport
  comparisons remain open.

### QPainter control-point X-Ray in-front depth (2026-10-15)

- QPainter control cages, curve handles, and selected surface/solid component
  points now bypass regular scene-depth occlusion under X-Ray while continuing
  to respect the separate `showInFront` depth domain. Outside X-Ray, regular
  control points still test both regular and in-front depth; in-front objects
  test only their own in-front domain. This keeps X-Ray visibility separate
  from Blender's in-front pass ordering, which routes X-Ray objects into
  distinct regular and in-front accumulation/depth passes in
  [`workbench_engine.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_engine.cc#L238-L250).
  Blender's Overlay X-Ray fade pass also binds regular and in-front depth
  textures separately in
  [`overlay_xray_fade.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/overlay/overlay_xray_fade.hh#L361-L374).
  Matched control-point and cage captures remain open.

### QPainter selected component-edge X-Ray depth (2026-10-15)

- Selected NURBS surface/solid component edges now bypass ordinary scene-depth
  occlusion while X-Ray is enabled. They still remain covered by a finite
  `showInFront` layer, which Blender composites later; selected edges belonging
  to an in-front object test only that layer's depth. Each retained depth map
  is sampled using its own dimensions, including bounded raster targets.
  Blender's separate in-front Workbench passes are described in
  [`workbench_engine.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_engine.cc#L238-L250).
  Matched component-edge captures remain open.

### QPainter selected component-edge X-Ray fade (2026-10-15)

- Selected NURBS surface/solid component edges remain visible through ordinary
  geometry in X-Ray. Blender's Edit Mode edge pass first applies a 0.5 base
  opacity to all edges whenever the X-Ray checkbox is enabled, including when
  its alpha is 1.0; its separate overlay fade then multiplies portions behind
  scene depth by `1 - xrayAlpha` when transparency is active. The QPainter
  fallback now applies both factors using the applicable regular or
  `showInFront` depth layer. Regular
  selected edges remain covered by a later in-front layer. Blender sets the
  base wire opacity in
  [`overlay_mesh.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/overlay/overlay_mesh.hh#L2370-L2375)
  and binds it to the Edit Mode edge pass in
  [`overlay_mesh.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/overlay/overlay_mesh.hh#L2566-L2584),
  and applies the hidden overlay multiplier in
  [`overlay_xray_fade.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/overlay/overlay_xray_fade.hh#L344-L376)
  and
  [`overlay_xray_fade.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/overlay/shaders/overlay_xray_fade.bsl.hh#L500-L535).
  Matched edge captures remain open.

### NURBS surface derivative normals (2026-10-15)

- Smooth NURBS surface and solid shading normals are now evaluated from the
  exact rational surface derivatives at tessellation UV samples (`du × dv`),
  with reversed face orientation applied consistently. Ruled degree-one
  extrusion panels keep their existing hard profile edges, and facet-averaged
  normals remain the fallback when parameter samples or derivatives are
  unavailable. This removes the general curved-surface dependence on triangle
  density and the fixed crease-angle threshold. `cmake --build build -j4`
  passed; matched Blender surface-shading comparison remains open.

### GPU Image Empty overlay composition (2026-10-15)

- Committed GPU Image Empties now draw after the scene's AA/DOF and display
  transform, matching Blender's separate overlay framebuffer. Default-depth
  images retain scene-depth testing and opaque depth writes; Front and Back
  images bypass scene depth; Back images draw after the other viewport
  overlays, matching Blender's final output pass. Alpha-blended images use a
  viewport snapshot and an explicit
  linear-light composite into the display-sRGB target. Blender 5.2.2 routes
  ordinary Image Empties to `overlay_fb`, in-front images to
  `overlay_color_only_fb`, and Back images to
  `overlay_output_color_only_fb`; its image pass uses premultiplied alpha and
  sorts transparent groups by camera depth. See pinned
  [`overlay_empty.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/overlay/overlay_empty.hh#L1037-L1145)
  and [`overlay_instance.cc`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/overlay/overlay_instance.cc#L888-L995).
  `cmake --build build -j4` passed after moving Back images to the final
  overlay stage. Matched Blender Image Empty comparisons remain open.

### Blender generated Image Empty import (2026-10-15)

- Still Image Empties backed by Blender-generated pixel buffers now use the
  existing Picture import and viewport path. The extractor packages their
  pixels as bounded PNG data and also preserves the scene-linear float payload
  used by the image color pipeline. Generated images share the existing 32 MiB
  packed-payload budget and 16M pixel cap. Image Empties with Blender alpha
  mode `NONE` are normalized to opaque pixels in both transport paths, matching
  Blender's rule to ignore their stored alpha channel. Sequence, movie, and
  tiled Image Empties remain unsupported; no matched Blender comparison has
  been run.

### QPainter Image Empty alpha cutoff (2026-10-15)

- The QPainter Image Empty cutout and conservative depth-mask paths now share
  Blender's 0.05 alpha cutoff. This restores the earlier threshold for
  source-texel masking, filtered-raster masking, and retained picture depth;
  the previous 0.1 threshold was for Workbench material Image Textures, not
  Image Empties. The GPU Image Empty shader already uses 0.05. See pinned
  [`overlay_image_frag.glsl`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/overlay/shaders/overlay_image_frag.glsl#L293-L309).
  C++ build passed; no matched Blender image comparison has been run.

### GPU shadow-volume direction audit correction (2026-10-15)

- The preceding audit incorrectly treated Blender's negative-light-direction
  shadow culling frustum as the caster extrusion direction. Blender's actual
  shadow-volume vertex shader extrudes caster vertices along `+L`, computes
  the far-plane intersection from that same direction, and subtracts `1e-3`
  to keep the cap inside the far plane. classiCAD's existing GPU direction was
  already aligned, so the temporary sign flip was removed. See
  [`workbench_shadow.bsl.hh`](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_shadow.bsl.hh#L1142-L1165).
  Matched GPU shadow comparisons remain open; QPainter uses a separate
  approximate depth-map algorithm.

### Blender saved viewport view on Open (2026-10-15)

- Ordinary `.blend` Open now reads a saved free `VIEW_3D` region's focus point,
  orientation, distance, and perspective/orthographic mode and applies them to
  classiCAD's initial viewport. It uses the same deterministic screen and area
  selection policy as the saved viewport shading import. Blender camera-locked
  views are skipped because they need the separate viewport-camera framing
  contract; if no saved free 3D view is available, classiCAD keeps its default
  view. Blender stores these values on `SpaceView3D.region_3d`; see
  [`RegionView3D`](https://docs.blender.org/api/5.2/bpy.types.RegionView3D.html).
  The scene render camera remains a separate record for Workbench render
  results. The scene extractor record is now version 20, and the reader accepts
  versions 10 through 20. C++ build passed; a matched saved-view comparison
  has not been run.
