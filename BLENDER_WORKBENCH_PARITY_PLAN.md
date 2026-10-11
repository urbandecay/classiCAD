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

### Background regression correction

Compared with GitHub `origin/feature/3d-workplanes` commit `6722914`, the theme
background retains its original radial gradient and RGB 61/48 endpoints.
The AA port changed the Qt widget to single-sample presentation but retained an
sRGB backing texture; Qt decoded the already display-encoded viewport during
composition, darkening the background, grid, and objects. The presentation
texture now uses RGBA8 while the private AA scene texture retains sRGB sampling.
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
| Color management | An AgX default-transform lookup from Blender 5.2.2 is bundled. | Foundation ported; complete display-pipeline parity remains unverified |
| Surface drawing | NURBS surfaces and solids are tessellated for display and shaded by classiCAD's custom forward GLSL renderer. Curves use the stroke path. | Different rendering architecture; visual parity remains unverified |
| Color source | Object, Random, Material, and Custom color plus their Blender-compatible alpha sources reach filled NURBS surfaces/solids in GPU and QPainter paths. Blender mesh import preserves object display colors, shared materials, per-face slots, active UV maps, and image references. GPU mesh Texture mode samples the active Image Texture Node per fragment through interpolated active-UV coordinates, with repeat wrapping and linear filtering; supported external raster files are loaded with size limits and cached by path/color space. QPainter mesh Texture mode samples external images with an affine map per triangle and repeat wrapping; perspective, alpha behavior, and filtering can differ. Packed Image Texture bytes up to 32 MiB are transported, persisted, and sampled by the GPU path. Missing images or UVs fall back to Material. The GPU shader unpremultiplies PREMUL images except at alpha 0 or 1, and discards sampled alpha below 0.1 except for NONE (Ignore) and CHANNEL_PACKED modes. Shader-node evaluation remains unsupported. | Partial; QPainter texture alpha behavior, node graph evaluation, filtering parity, and NURBS Texture remain gaps |
| Blender scene import | File > Import > Blender Scene invokes the pinned Blender 5.2.2 extractor and offers original or evaluated mesh mode. Imported polygon meshes keep local vertices, ordered face loops, object transform, object/library identity, display color, shared material records, per-face slot indices, UV/color attributes, active selectors, image-texture references including packed bytes up to 32 MiB per image, and source mode/version. Native document version 9 persists this data and reads versions 1–8. Mesh objects render through GPU or QPainter paths and support object-level hit testing. Unsupported object types and omitted scene features are reported. | Partial; only polygon meshes import; QPainter packed-image rendering, collection/layer and visibility state, custom split normals, and shader-node graphs are not represented/evaluated |
| Material values | Per-object base color, roughness, metallic, and alpha are serialized and consumed by both renderers. Imported Blender meshes retain shared material records and per-face slot references; GPU and QPainter mesh paths select face material values. Attribute colors also reach both mesh renderers. classiCAD currently has no UI for authoring appearance values. | Partial; material packing matches Blender's `Material::pack_data`; Texture supports external raster files on the GPU path only; material authoring remains a gap |
| Culling and X-Ray | GPU surface culling works; X-Ray uses translucent fill plus depth/stroke passes. QPainter applies backface culling to polygon-mesh triangles using their transformed world-space winding and the active view direction. QPainter still sorts mesh triangles by projected centroid and has no scene-wide depth buffer or separate X-Ray depth/stroke passes. | Partial; mesh backface culling is supported in the fallback, while pass behavior, transparency ordering, and remaining fallback behavior differ |
| Shadows and outlines | Validated closed `NurbsSolid` objects use depth-fail stencil volumes built from their indexed cap/trim tessellation and extrusion walls, or from planar materialized boundary quads. Open NURBS surfaces, unsupported/nonmanifold geometry, nonplanar edited solid faces, and partial-alpha scenes use the 1024-pixel directional depth-map fallback with 3x3 PCF. Focus/shift and light-direction mapping follow Blender's Workbench formulas. Outlines use a custom object-ID buffer and four-neighbor edge test and remain enabled when any surface range has partial alpha. The default outline, shadow, and cavity flags are off as in Blender 5.2.2; specular highlights remain on. | Closed-solid stencil path is implemented; fallback and Blender's full pass/fail/nonmanifold handling remain partial; outline edge detection and compositing still differ |
| Cavity and depth of field | GPU Solid maps Blender's World type to SSAO and Screen type to normal-based curvature; Both uses both. The persisted settings expose Blender 5.2.2 defaults and ranges for SSAO distance/attenuation/sample count (`SceneDisplay`) and cavity/curvature ridge/valley factors (`View3DShading`); the renderer uses these values and distributes SSAO samples across AA iterations with Blender's 512-sample cap. Cavity jitter now recreates Blender's 64x64 blue-noise-derived RGB texture, including its clamped, total-sample-scaled radius offset. Composition follows Blender's opaque resolve order. The prepass/effects are skipped for the whole draw when any surface has partial alpha. QPainter rasterizes committed NURBS and imported polygon-mesh display triangles into scene-wide depth, normal, and object-ID buffers at viewport device-pixel resolution (independent of the supersampled color target), then applies an opaque-only cavity/curvature composite; its fixed disk taps, nearest depth resolve, and screen-linear depth interpolation approximate Blender's GPU sampling. A perspective scene-camera mode stores focal length, sensor width, focus distance, and f-stop. Before AA accumulation, GPU presentation derives CoC from depth and optics, samples a CoC-aware 2x2 color footprint, gathers 12 taps, applies a three-value median cleanup, and composites by CoC into a linear intermediate. | GPU cavity sampling is source-derived; QPainter cavity uses approximate software-buffer neighborhood sampling; DOF is camera-gated and runs before temporal accumulation, but its one-pass full-resolution sampling approximates Blender's dedicated low-resolution passes; QPainter DOF remains unavailable |
| Anti-aliasing preference | The UI offers Off, “Single Pass Anti-Aliasing,” and 5/8/11/16/32 samples; the RNA description identifies the single-pass method as FXAA. In Blender 5.2.2 Workbench, stored value 1 means one unjittered sample with TAA color filtering and SMAA resolve; Workbench does not invoke an FXAA shader. Values 5+ accumulate jittered frames and use SMAA while settling. During navigation or playback, Workbench resets to one unjittered sample and SMAA. classiCAD implements navigation reset and exposes a playback-state setter, but has no scene timeline, playback clock, or playback frame scheduler to call it. The grid is drawn afterward with its own unjittered MSAA path. QPainter renders committed geometry into a supersampled target for non-Off choices, without temporal accumulation or SMAA resolve; its wire categories use the separate Smooth Wires preferences. | Partial; GPU path follows Workbench's AA passes, but app playback integration, output equivalence, and fallback parity remain open |
| Smooth Wires | Overlay and Edit Mode preferences are independently stored and GPU strokes have separate categories. Committed curve strokes and wireframe surface lines use Overlay; component controls/cages and Solid-mode `NurbsSolid` face-boundary strokes use Edit Mode. The separate Workbench Outline effect draws object silhouettes. QPainter applies the preferences to supported curve, surface, and control-cage categories; prepared fallback segments still lack depth testing, per-vertex fades, and patterned lines. | Partial; the GPU `NurbsSolid` boundary pass is present, while QPainter depth handling and some component-edge fallback geometry remain unavailable |
| Transparency | GPU surfaces are separated into opaque and partial-alpha ranges. Opaque ranges establish depth first; transparent display triangles are then blended back-to-front using each triangle's projected centroid depth, with opaque depth testing retained and depth writes disabled. This improves ordering within spatially extended tessellated NURBS surfaces; triangle draws still use the existing display tessellation and leave the stored NURBS authoritative. The surface outline pass also runs for partial-alpha content. QPainter has no depth-aware transparent pass. Blender uses dedicated transparent and transparent-depth passes. | Partial; centroid sorting cannot correctly order intersecting triangles or cyclic overlap, transparent scenes issue one draw per triangle, and Blender's dedicated pass behavior, shadow/effect behavior, and QPainter differ |
| Stroke and overlay composition | Workbench surface silhouettes are drawn by its outline pass before AA resolve. CAD curve and wire strokes, like Blender's Overlay wireframe pass (`overlay_wireframe.hh`), draw afterward using the unjittered camera and regenerated scene depth; selected-face overlays also remain post-resolve, and the grid follows. Selected faces depth-test without depth writes. X-Ray's hidden/visible wire passes and the two Smooth Wires categories are preserved. GPU CAD strokes use analytic coverage, with depth bias enabled only for depth-tested strokes whose Smooth Wires category is enabled, matching Blender's `do_smooth_lines` gating. Solid Edit Mode strokes and Theme-mode wire strokes use separately configurable theme colors, saved with viewport shading state; selection, previews, and non-Theme wire colors retain their separate paths. QPainter maps the two smoothing preferences for curve strokes, wireframe surface lines, and control cages, but lacks Solid-mode surface silhouettes and some component-edge geometry; its prepared-segment fallback also loses depth and stroke-style effects. | Theme wire/edit colors are configurable for supported stroke paths; Blender theme import, unsupported Smooth Wires geometry, transparency, and other fallback differences remain |
| Shadow controls | The Shift and Focus values feed Blender's normal/light falloff calculation, including the `1 - focus * (1 - shift)` upper edge. The map fallback keeps a fixed depth bias and remains an approximation of volume occlusion. | Partial; setting semantics are adapted, while unsupported casters still use depth maps |
| GPU fallback | QPainter has a separate shading implementation. Its committed scene uses increasing supersampling for non-Off AA choices, without temporal accumulation or SMAA. Smooth Wires are independent of that scene AA hint for the supported categories: Overlay controls committed curve strokes and wireframe surface lines; Edit Mode controls cage/control-polygon lines and categorized prepared fallback segments. Direct prepared-segment fallback does not preserve depth testing, fades, or line patterns. Whole-shape fallback calls cannot reproduce selected component edges or Solid-mode surface silhouettes. | Partial; renderer fallback differences are documented but not closed |

### Remaining parity gaps

- **Render result:** File → Render Workbench Result (F12) renders the visible document geometry into an offscreen target at viewport, HD, Full HD, QHD, or 4K dimensions. It uses the current viewport pose and projection, renders fills and CAD strokes without editor overlays/grid, displays the Combined result, and saves Combined as PNG with camera/output metadata plus linear camera-space Depth as PFM (zero means background). This is classiCAD's reusable offscreen scene-result path; Blender's independent scene-camera data-block evaluation, exact optical projection/DOF output pipeline, registered pass lifecycle, and render-engine parity remain unimplemented.
- **Shading shortcut:** Z opens a hold-Z radial menu in idle Select mode with Solid, Wireframe, X-Ray, and settings directions. Blender's other shading modes are absent because classiCAD has no Material Preview or Rendered modes.
- **Color inputs and material assignment:** Blender scene mesh import preserves per-face material slots, color attributes, UV maps, and image references. Material/Object/Random/Custom/Attribute mesh colors render through GPU and QPainter paths. GPU Texture mode samples the active Image Texture Node through interpolated active-UV coordinates and a per-fragment sampler; external raster files use repeat wrapping and linear filtering. Missing images and missing UV maps fall back to face Material color. QPainter Texture mode uses affine per-triangle mapping with repeat wrapping; perspective, alpha behavior, and filtering can differ. GPU Texture mode transports alpha mode, unpremultiplies PREMUL samples except at alpha 0 or 1, and applies Blender's 0.1 fragment cutoff except for NONE (Ignore) and CHANNEL_PACKED modes. Blender-style object-name/library-path Random identity is used by both renderer paths, with ObjectId fallback for legacy nameless objects. The `.blend` adapter still round-trips classiCAD's embedded document snapshot instead of arbitrary Blender datablocks.
- **Transparent sorting:** GPU transparent display triangles now sort by projected centroid rather than one average per object. Intersecting or cyclically overlapping triangles can still composite in the wrong order, each transparent triangle is a draw call, and QPainter has no depth-aware transparent pass.
- **Display transform:** the bundled AgX lookup covers the default transform, but classiCAD has no user-configurable Blender display device, view transform, look, exposure, or gamma configuration path.
- **Camera depth of field:** the viewport uses a fused full-resolution approximation before AA accumulation: physical CoC, a 2x2 CoC-aware source footprint, 12-tap gather, three-value median cleanup, and CoC-weighted composite into a linear intermediate. Blender v5.2.2 uses dedicated low-resolution intermediate textures and different gather/median behavior. Scene-camera optics persist with project, session, and user preferences; DOF is gated off for ordinary orbit/orthographic views.
- **Additional shadow-map behavior:** unsupported casters, open surfaces, nonmanifold/edited solids, and any partial-alpha scene use the current 1024-pixel depth-map fallback with 3x3 PCF; Blender's complete shadow-volume/pass behavior and additional fallback fidelity remain unported. The renderer also still has a custom four-neighbor object-ID outline resolve rather than Blender's outline shader and full pass composition.

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

## Blender behavior to reproduce

### Workbench shading

- Studio lighting uses Blender's Workbench studio-light behavior and presets. MatCap uses the selected MatCap and its orientation. Flat uses an unlit flat result. Workbench does not shade objects with scene lights.
- The color modes have distinct data sources: Material reads Workbench material display values; Object reads the object's display color; Random derives a stable color from object identity; Attribute reads the chosen color attribute; Texture reads the supported image texture and UV mapping; Custom uses the user-selected color.
- Background choices, color management, object alpha, and material settings must feed the same final result as the chosen shading mode.
- Culling, X-Ray, shadows, outlines, specular highlights, cavity, and curvature must match Blender's enabled/disabled behavior and be composed in the correct order. Keep depth of field unavailable until classiCAD has an active scene camera and its optical settings.
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

The screenshot's Overlay and Edit Mode checkboxes are Smooth Wires preferences. They control antialiasing for the corresponding wire categories. They are separate from Workbench surface shading and from the grid's own line rendering. Blender's Edit Mode Smooth Wires preference is marked as requiring a restart in its preference definition.

- Route overlay-wire smoothing to Blender-equivalent overlay strokes.
- Route edit-mode wire smoothing to edit-mode edges.
- Keep these switches independent. Turning off overlay wire smoothing must not turn off grid antialiasing, surface antialiasing, or edit-mode smoothing.
- Confirm which classiCAD strokes correspond to Blender overlays and edit-mode edges before wiring the preferences. Do not use a generic scene stroke flag if it groups unlike Blender draw passes.
- Preserve the restart requirement for Edit Mode Smooth Wires. Verify the v5.2.2 UI and RNA definition for the Overlay setting before deciding whether it can update live.

## Implementation stages

### Stage 0: Baseline audit

Status: completed against Blender 5.2.2 and classiCAD source. Updated after implementation review.

- Pin source review and screenshots to Blender 5.2.2.
- Trace each preference from its UI/RNA definition through Workbench state and draw passes.
- Trace each classiCAD preference through serialization and into the renderer.
- Mark each option as implemented, partial, or missing in the baseline table above.

### Stage 1: Make anti-aliasing control the viewport

Status: partially implemented. The GPU path uses Blender's unjittered one-sample TAA filter plus SMAA for preference value 1 and during navigation, and jittered TAA accumulation plus SMAA for the 5+ modes. The viewport exposes `setViewportPlaybackActive(bool)`, but no application playback owner calls it; an external timeline owner must set it during playback and drive viewport frames for the one-sample reset to take effect. Scene strokes and selected-face overlays redraw after scene resolve using the unjittered camera and regenerated scene depth; the unjittered grid follows. Pixel-for-pixel equivalence has not been verified, and QPainter remains an approximation.

- Define one classiCAD AA setting model that matches Blender's UI choices and exact sample mapping.
- Route the setting through the complete Workbench scene render and resolve path, not only the grid framebuffer.
- Implement idle accumulation, responsive navigation/playback behavior, and invalidation on the changes listed above.
- Keep the Workbench scene and its supported scene strokes coherent. Draw the grid and other viewport overlays afterward with their own unjittered camera, as Blender does. Ensure resizing and device-pixel-ratio changes rebuild the Workbench targets correctly.
- Compare GPU output and fallback behavior, and make the fallback's supported behavior explicit.

Acceptance: changing each AA option visibly changes the expected full viewport output; no fixed independent sample setting overrides it; camera movement and scene edits do not leave stale accumulation.

### Stage 2: Complete Workbench color and material inputs

Status: partially implemented. Object, Random, Material, and Custom/Single inputs reach filled NURBS surfaces/solids in both renderers and persist in document version 9. Native objects persist display name and library path; both renderers use these for Blender-style Random identity, with an ObjectId fallback for older nameless objects. The Blender scene importer reads mesh identity, object color, shared materials, face material assignments, UV maps, color attributes, active selectors, and active Image Texture Node references. Material image reference metadata persists through document save/load, including node/image names, source paths, color-space and alpha modes, and packed bytes up to 32 MiB per image. GPU and QPainter mesh display support Material, Object, Random, Custom, and Attribute. GPU mesh Texture mode samples external and packed images per fragment; QPainter mesh Texture mode samples external images with an affine per-triangle mapping. QPainter does not yet sample packed bytes or apply texture alpha behavior. Missing images or UV maps fall back to Material. GPU applies Blender's PREMUL unpremultiplication and conditional 0.1 alpha cutoff; QPainter perspective correction and filtering may differ. Solid Edit Mode strokes and Theme-mode wire strokes have distinct configurable theme colors in the shading popover, persisted with viewport update-session state; classiCAD does not import Blender user themes. Surface roughness/metallic/source-alpha values follow Blender 5.2.2 `Material::pack_data`: clamp to [0,1], square-root remap roughness, round all three to 8-bit UNorm, then apply X-Ray opacity to the packed source alpha. Preview fills also pack their default roughness 0.4 through this path, while retaining fixed 0.5 alpha. Blender's Custom source supplies RGB to `Material(float3)`, which packs alpha 1; classiCAD also forces the source opaque. classiCAD has no object/material appearance editor. Transparency ordering remains approximate.

- Keep the implemented Material, Object, Random, and Custom sources connected to filled surfaces/solids in both renderers. Keep Edit Mode strokes on their separate configurable `TH_WIRE_EDIT`-style setting and Theme-mode wire strokes on a separate configurable `TH_WIRE`-style setting, distinct from Workbench fill colors. Preserve selection, previews, and non-Theme wire colors. Preserve appearance values loaded from document data through the existing scene-object fields.
- Preserve the implemented Attribute source and Material fallback for missing attributes. Keep Texture's Material fallback when no active image, active UV map, or supported external image is available. Shader-node graph evaluation remains outside this import path.
- Preserve Blender object-name/library identity for Random colors; classiCAD stores those names and paths, with ObjectId fallback for older objects.
- Imported polygon meshes preserve material slots and per-face assignments. NURBS curves and surfaces still use one Workbench material payload per object; define an explicit parameter-space mapping before adding mesh-style color attributes or textures to NURBS.
- Verify color identity and appearance remain stable after save/reopen and object reorder.
- Keep custom shading inputs in the C++ scene/material model and serialization.

Acceptance: supported sources produce source-correct results; Attribute uses point, face, or corner data and falls back to Material only when absent. Mesh Texture uses the active UV and supported external image, with the documented QPainter approximation and Material fallback when an input is unavailable. Verify supported inputs after save/reopen. Unsupported inputs are reported rather than silently becoming gray.

### Stage 3: Complete Workbench effects

Status: partially implemented. GPU cavity/curvature and several existing effects are wired. Cavity settings now persist the Blender 5.2.2 values: Scene Display SSAO distance 0.2 (RNA [0, 100000]), attenuation 1.0 ([0, 100000]), and samples 16 ([1, 500]); View3D cavity ridge/valley 1.0 ([0, 250], UI range [0, 2.5]) and curvature ridge/valley 1.0 ([0, 2]). The menu maps Blender's World type to SSAO, Screen type to curvature, and Both to both, following the pinned shading RNA; its actions are exclusive and the button label/check mark refresh from the active setting, including restored settings. The renderer applies the factors and sample count using Blender's per-AA-iteration sample ranges and 512-sample cap. Cavity jitter recreates Blender's 64x64 blue-noise-derived RGB texture and scales its radius offset by the total cavity sample count. Pixel-level output has not been visually compared. Partial-alpha scenes still skip the cavity prepass/effect for the whole draw; per-pass effects in transparent scenes and camera DOF remain open. QPainter cavity now includes imported mesh triangles but needs visual comparison. Validated closed `NurbsSolid` geometry uses a depth-fail stencil-volume pass, with documented depth-map fallback for unsupported casters. GPU Solid-mode `NurbsSolid` face boundaries now draw in the Edit Mode wire pass, separately from Workbench's silhouette outline; selection colors and device-scale rounding remain visually unverified. Shadow output has not had visual comparison yet.

- Compare and port culling, X-Ray, shadow, outline, specular, cavity, curvature, and depth-of-field behavior.
- Map Blender's World cavity action to SSAO and Screen to curvature (the pinned View3DShading RNA enum definition); Both enables both. Persist Blender's SSAO distance/attenuation/sample settings and cavity/curvature ridge/valley factors with the source defaults and RNA ranges; drive the shader's corresponding uniforms and distribute the selected SSAO sample count across AA iterations, capped at 512 total samples.
- Match effect enable flags, setting dependencies, and opaque pass composition to Blender's source. Keep transparent-scene differences explicit; the source-derived cavity jitter is implemented but still needs visual comparison.
- For opaque primary-scene draws, derive indexed shadow casters only from validated closed `NurbsSolid` data: use the cached base/trim tessellation's shared boundary vertices plus extrusion walls, or validated planar materialized quads. Require every indexed edge to have exactly two oppositely directed uses and reject zero-volume or degenerate meshes. Draw light-facing caps, reversed extruded caps, and silhouette-edge quads with depth-fail stencil counting. Extrude each vertex to the current view's far plane with Blender's signed-distance/light-plane-dot formula and `1e-3` inward epsilon; use the `1e5` distance when the light direction does not intersect that plane. Tessellation remains display data derived from the persisted NURBS solid.
- Keep the depth-map fallback for open `NurbsSurface` objects, unsupported/nonmanifold topology, nonplanar materialized solid faces, missing stencil buffers, and partial-alpha scenes. A mixed opaque scene uses stencil volumes for valid solids and the map only for unsupported casters. This is a supported closed-solid subset, not Blender's full manifold/nonmanifold PASS/FAIL dispatch or ray-traced shadow option.
- Map the stored scene light direction through Blender's Y/Z swap and X/Z sign change. Clamp Shift to Blender's `[0,1]` range and compute `focus = clamp(shadow_focus, 0.0001, 0.99999)`, `shadow_focus_edge = 1 - focus * (1 - shadow_shift)`, then apply `smoothstep(shadow_shift, shadow_focus_edge, -dot(N, light_direction_view))`; shadow intensity mixes the result with the unshadowed contribution. Keep the fallback depth bias fixed so Shift no longer changes map bias.
- Keep the depth-of-field control disabled until a scene camera and Blender-equivalent focus/aperture inputs exist. Then implement the camera-gated DOF pass and enable it only when a camera has DOF active.
- Keep GPU cavity/curvature behavior aligned with Blender's composition; its parameter controls, sample scheduling, and blue-noise-derived jitter texture are wired, but whole-draw partial-alpha skip remains. The QPainter fallback applies an approximate cavity pass to NURBS and imported polygon meshes; it still needs visual comparison.
- Derive all surface display data from the authoritative NURBS geometry. Do not convert committed surfaces into authoritative meshes.

Acceptance: each enabled control affects the expected objects and pixels, and disabling an effect removes only that effect. Verify saved settings survive reopen and test SSAO sample scheduling at the 512-sample cap. Unsupported effects stay disabled or are clearly identified as approximations. Pixel-level cavity jitter comparison, transparent-scene cavity behavior, visual comparison of QPainter cavity on imported polygon meshes, and visual comparison of the closed-solid shadow path remain open parity checks.

### Stage 4: Complete Smooth Wires and viewport composition

Status: partially implemented. The two preferences are independent and GPU stroke categories are separated. Selected-face overlays and GPU scene strokes now redraw after Workbench resolve with the unjittered camera and scene depth, including cached-history frames; selected faces do not write depth, and X-Ray back/front behavior remains intact. Solid-mode `NurbsSolid` faces now draw evaluated face boundaries through the GPU Edit Mode wire pass, independently of the Workbench silhouette outline. QPainter maps Overlay to committed curve strokes and wireframe surface lines, and Edit Mode to control-cage/control-polygon lines; prepared categorized stroke segments use the matching preference when the GPU stroke draw falls back. That direct fallback does not retain depth testing, fades, or line patterns. QPainter still cannot reproduce selected component edges or Solid-mode surface silhouettes from whole-shape fallback calls. Category-to-Blender pass mapping and visual output still need verification.

- Map overlay and edit-mode wire categories to their actual renderer passes.
- Implement the two Smooth Wires preferences independently.
- Match Blender's composition order: resolve Workbench scene content first, regenerate unjittered scene depth, then draw selected-face overlays and scene strokes before the grid. Both post-resolve redraws are implemented; verify stroke-category mapping and grid depth behavior.
- Check that changing wire smoothing does not change the grid or solid surface edges.

Acceptance: each checkbox affects its matching wire category only, while other viewport layers retain their expected antialiasing.

### Stage 5: Parity review

Status: in progress; the implementation and source reviews found the blockers listed above. The team has not signed off on full parity.

- The user clarified that classiCAD's current viewport should be treated as Edit Mode; its separate Object Mode is planned for later. Do not use the missing Object Mode as an explanation for base surface, background, or grid differences. The supplied screenshots still are not a controlled render comparison: classiCAD reports `Ortho`, Blender reports `User Perspective`, and the visible scene geometry differs. Blender's mesh edit edges and curve edit wires are separate Overlay-engine geometry; classiCAD's Solid-mode CAD curve strokes and `NurbsSolid` face boundaries use the configurable edit-wire theme setting, while control geometry and selection behavior remain a distinct implementation. Compare base rendering separately from mode-specific overlays, with matching camera and geometry.
- This is an adaptation of selected Blender shading equations and passes into classiCAD's renderer, not a verbatim port of Blender's complete Workbench and Overlay engines. The table above and stage notes remain the parity record.

- Run the comparison matrix below against Blender 5.2.2.
- Record remaining visual differences with a screenshot and a source or implementation explanation.
- Update the baseline table and this plan as each stage is completed.

## Comparison and completion checklist

Use the same machine and display where possible. Match viewport pixel dimensions, view and projection, clipping range, scene contents, selection/edit state, color management, and preference values. Compare Blender and classiCAD side by side or with aligned screenshots.

- Studio, MatCap, and Flat modes, including light/MatCap choice and rotation.
- Every color source, using objects with material values, object colors, color attributes, and UV-mapped textures.
- Background choices and color management.
- Culling, X-Ray, shadows, outlines, specular, and cavity/curvature, both individually and in combinations. Verify camera DOF only after classiCAD has camera/optical inputs; until then confirm the control is disabled.
- No AA, the preference's “Single Pass Anti-Aliasing” value (Workbench one-sample TAA+SMAA), and 5/8/11/16/32-sample multisampling, both idle and during navigation.
- Accumulation reset after camera movement, geometry edits, selection changes, setting changes, resize, and display-scale changes.
- Overlay Smooth Wires and Edit Mode Smooth Wires independently, including confirming the grid remains antialiased when overlay wire smoothing is off.
- Save/reopen behavior for all persistent settings and any new material or color data.
- GPU renderer and QPainter fallback behavior.

The port is complete when every enabled option has matching Blender 5.2.2 behavior, no enabled saved setting is inert, and the comparison checklist passes. Disabled unsupported features must remain clearly documented. Workbench shadows now use validated stencil volumes for the closed `NurbsSolid` subset; full parity still requires comparison and a matching path for the currently disclosed unsupported-geometry and transparency fallbacks.

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
- [Workbench cavity jitter texture generation](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/workbench_resources.cc#L74-L98)
- [Workbench cavity jitter sampling shader](https://github.com/blender/blender/blob/v5.2.2/source/blender/draw/engines/workbench/shaders/workbench_cavity.bsl.hh#L41-L63)
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
