# classiCAD runtime performance baseline

Captured on 2026-10-04 for the R0 baseline and R10/R12 comparison. The runtime
numbers below come from opt-in synthetic fixtures and must not be read as
production-scene frame guarantees.

## Recorded startup check

Command:
QT_QPA_PLATFORM=offscreen CLASSICAD_LOG_PATH=/tmp/classicad-r0-startup.log timeout 5s ./build/classiCAD

The process logged application start at 01:44:20.093 UTC and viewport
constructed at 01:44:20.131 UTC. It remained in its event loop until timeout
at five seconds. The 5.01 second process duration is the timeout duration,
not the measured startup latency.

## R10/R12 runtime measurements

`classicad_viewport_runtime_benchmark` uses 64, 256, and 1,024 degree-3
Bezier curves, a 1280x720 top orthographic view at zoom 12, three warm-up
iterations, and 21 timed iterations. It reports median and p95. Snap and hit
queries use a fixed cursor. CPU timings include actual `QPainter` drawing into
an image; grid/origin and curve-only work are reported separately. The CPU
curve path and native GL path both consume the prepared world-geometry cache.
The CPU path retains double-precision world samples and applies 0.3-pixel
screen-space simplification in bounded chunks; clipped or unsupported curves
fall back to the existing adaptive renderer.
Native GL uses an offscreen OpenGL 3.3 context on the available NVIDIA GTX
1050 Ti / OpenGL 4.6 display and excludes the Qt grid/overlay painter. These
are synthetic service/render timings, not full interactive application frames.

| Measurement | Fixture and timing boundary | R10/R12 results |
|---|---|---|
| Surface evaluation | Rational surfaces at 4x4, 12x12, and 24x24 CVs; median of five rounds, 1,200 points per round | R5b: checked path 0.90/2.92/6.77 ms; prepared evaluator 0.44/2.02/4.24 ms; prepared reuse measured 1.45–2.06x faster. Checked path reconstructs prepared state for every point. |
| Snap and pick latency | Fixed curves and camera; candidate construction, full snap resolution, and hit test are timed separately | Current medians at 64/256/1,024 curves: candidate generation 0.052/0.195/0.871 ms; snap resolution 0.691/1.339/4.268 ms; hit test 0.624/0.801/1.351 ms. At 1,024 curves, the projected control-hull near-snap filter reduced snap resolution from 18.991 ms to 4.268 ms; clipped/invalid hulls still fall back to the existing narrow phase. |
| Surface tessellation/cache | Same rational surfaces; median of three cold tessellations and five batches of 1,200 warm cache lookups | R5b: cold tessellation 2.53/8.93/17.79 ms; warm lookup batches 0.040/0.039/0.039 ms. |
| Camera-only redraw | Fixed visible scene; compare CPU fallback and native GL, with world samples warm | CPU full fallback medians at 64/256/1,024 curves: 26.38/38.84/96.42 ms; p95 28.01/41.16/104.05 ms. Curve-only CPU medians: 5.51/19.63/71.82 ms; grid+origin: 20.37/20.10/20.18 ms. Native GL cached-curve medians: 0.558/2.078/7.035 ms; p95 1.264/3.998/7.621 ms. At 1,024 curves, the prior CPU path that reevaluated committed curves measured 196.84 ms for grid+origin+curves; the cached path measured 96.42 ms. CPU fallback remains slower than native GL at this scene size. |
| Single-object edit invalidation | Edit one object in increasing scene sizes; time frame/cache refresh and count regenerated world samples | Current medians at 64/256/1,024 curves: 0.053/0.111/0.328 ms; p95 0.075/0.199/0.434 ms. Exactly one object's prepared world geometry changed per edit in all three fixtures. |
| Undo memory and latency | Five geometry edits with retained snapshots; measure commit, undo, redo, process RSS before/after, and peak RSS | At 100/1,000/5,000 objects, commit medians 0.029/0.273/1.425 ms; undo 0.031/0.842/9.177 ms; redo 0.023/0.749/8.651 ms. Current RSS grew by 0.6/3.1/14.6 MiB after commits and undo/redo; whole-process peak RSS was 19,712/21,244/39,004 KiB. Five undo snapshots were retained. This fixture does not represent documents with large images or dense surfaces. |

## NURBS surface slowdown diagnosis and fix

The surface path had three concrete costs. `makeViewportSceneStrokes()` did not
accept `NurbsSurface`, so each surface bypassed the batched OpenGL renderer and
`ViewportRenderer::drawNurbsSurface()` issued one `QPainter::drawLine()` per
wireframe segment. The default display tessellation creates a 48x48 mesh
(4,608 triangles) and 18 isocurves with 128 segments each (2,304 line segments)
per surface, regardless of the small control net on an extrusion. The CPU draw
path reacquired tessellation through a 128-entry LRU on every redraw; after 128
surfaces, the first visible surfaces were rebuilt on every frame. Selection
hit-testing also tested every triangle of every surface, projecting the same
mesh vertices repeatedly, even when the cursor was nowhere near a surface.

The viewport now carries the sampled surface wireframe in its revision-cached
world geometry, submits it through the existing batched OpenGL stroke renderer,
and reuses prepared double-precision line samples in the CPU fallback. Surface
selection first projects the positive-weight control hull and skips exact mesh
testing when the cursor is outside its screen bounds. Projection failures fail
open to the existing exact path. The tessellation quality and visible isocurve
count are unchanged.

The opt-in viewport runtime benchmark adds 9x2-control-vertex exact circular
extrusions at 16/64/128/160 objects. It measures Top-view CPU drawing and
selection hit-testing at 1280x720/zoom 12; native GL uses the same prepared
geometry but excludes the grid and Qt overlays. The pre-fix and post-fix Top
measurements use the same fixture and camera. The post-fix harness also checks
an isometric cursor hit; it selects surface index 0 at each scene size.

| Surfaces | CPU draw before | CPU draw after | Native GL after | Hit-test before | Hit-test after |
|---:|---:|---:|---:|---:|---:|
| 16 | 4.76 ms | 4.69 ms | 1.81 ms | 15.40 ms | 0.96 ms |
| 64 | 17.97 ms | 16.53 ms | 6.83 ms | 60.66 ms | 0.93 ms |
| 128 | 36.89 ms | 32.60 ms | 14.91 ms | 118.94 ms | 0.99 ms |
| 160 | 392.36 ms | 40.65 ms | 18.11 ms | 500.33 ms | 1.12 ms |

The 160-surface redraw dropped about 9.6x and hit-testing about 446x in these
synthetic fixtures. The sharp pre-fix jump above 128 surfaces matches the LRU
capacity exactly. The native GL timings exclude other viewport work, so they
are not full application frame rates; dense scenes still scale with the number
of displayed isocurve segments and the 48x48 depth mesh.

The runtime harness and commands are opt-in through the `benchmarks` CMake
preset. Re-run on a specific target machine before using these figures for a
performance budget. The CPU fallback data identifies curve drawing and the
Blender-style grid as separate costs; the native GL results do not include the
grid/Qt overlays.

The initial R1 work targets build graph duplication. Runtime measurements are
required when R4/R5/R10 introduce revision-based invalidation, prepared
geometry, or renderer changes; unmeasured runtime improvements must not be
claimed in R12.

## R5b focused microbenchmark

The optional `CLASSICAD_BUILD_BENCHMARKS` target builds
`classicad_nurbs_surface_benchmark`. It compares checked evaluation (prepare
and validate state for each query) with one prepared evaluator reused for the
same 1,200 rational point queries. It also measures cold prepared-mesh
construction and batches of warm revision-keyed cache reads. The table values
are medians from the 2026-10-04 build on this development machine. They
establish per-operation cost only; fixed-scene CPU and native GL measurements
remain for R10/R12.

## R9 erase-scene candidate reuse benchmark

Added opt-in target `classicad_erase_scene_query_benchmark`. It compares one
candidate-vector build reused across the selected curves with rebuilding the
same visible-scene candidate vectors for each target. Both paths run the same
number of curve-intersection queries. Fixtures contain parallel degree-1
NURBS curves, and timings are medians of seven rounds in the Release build on
this development machine; they are a synthetic service microbenchmark, not a
viewport-scene result.

| Visible curves | Selected targets | One candidate build | Reuse candidates + queries | Rebuild candidates per target + queries | Ratio |
|---:|---:|---:|---:|---:|---:|
| 16 | 2 | 0.0031 ms | 0.0427 ms | 0.0774 ms | 1.81x |
| 64 | 8 | 0.0093 ms | 0.5026 ms | 0.5522 ms | 1.10x |
| 256 | 16 | 0.0411 ms | 3.8547 ms | 4.5183 ms | 1.17x |

The measured reuse saves about 0.66 ms at 256 curves/16 selected targets;
intersection work dominates that fixture. This supports reusing candidates
and does not justify a spatial broad phase. Curved, mixed-plane, camera-
projected interactive scenes and tail latency remain unmeasured.

## R10/R12 projected control-hull query benchmark

Added opt-in target `classicad_viewport_query_benchmark`. It creates visible
degree-1 planar NURBS line fixtures and records the median of nine rounds for
whole-scene hit testing and crossing-selection box queries. Before/after runs
used the same Release build configuration, viewport dimensions, camera, query
point, box, and fixture generator. The conservative projected control-hull
filter skips the sampled narrow phase only when every curve control point
projects successfully and the cursor/selection box is disjoint from the hull;
clipped or invalid projections keep the existing query path.

| Visible curves | Hit test before | Hit test after | Box query before | Box query after |
|---:|---:|---:|---:|---:|
| 64 | 0.952 ms | 0.051 ms | 0.959 ms | 0.102 ms |
| 256 | 3.546 ms | 0.174 ms | 3.667 ms | 0.185 ms |
| 1,024 | 15.117 ms | 0.687 ms | 14.671 ms | 0.524 ms |
| 4,096 | 57.720 ms | 2.797 ms | 58.932 ms | 2.116 ms |

These are synthetic service timings, not application frame or user-input
latencies. They support adding this conservative per-curve filter; they do not
establish CPU/GPU redraw performance or justify a maintained spatial index.
