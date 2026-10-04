# classiCAD modularity, scalability, and build refactor plan

Status: planned; implementation has not started under this plan.

Prepared from the working tree on 2026-10-04. Paths and sizes below describe
that tree, including its uncommitted NURBS surface and Fill work. Recheck them
before implementation. This document is the execution plan for a new chat.

## Start here in the next chat

Read `AGENTS.md`, then all of `refactoring/MASTER_REFACTOR_PROMPT.md`, then
this file. The master prompt remains the architecture contract; this plan
continues its unfinished ownership migrations and adds measurable build and
runtime improvements. Historical phases marked complete in the master ledger
do not mean every compatibility bridge has already been removed.

Suggested instruction to paste into the next chat:

> Follow `refactoring/MODULARITY_AND_BUILD_REFACTOR_PLAN.md`. Inspect the current
> working tree, preserve all existing work, and complete R0 and R1 first.
> Establish the baseline, make application and test targets reuse compiled
> production modules, validate the result, measure the build change, and update
> both refactoring ledgers. Continue subsequent phases in dependency order with
> a working application at each checkpoint. Preserve modeling and interaction
> behavior. Do not commit or push unless I request it.

The first implementation checkpoint is **R0 + R1**, not a rewrite of the
viewport. After that, complete one coherent phase or subphase at a time. A
phase is complete only when its exit conditions have evidence.

## 1. Goals and boundaries

1. Each responsibility has one owner and a predictable folder/file name.
2. Core geometry, document edits, tools, persistence, and Qt presentation can
   change independently through small interfaces.
3. Common production implementations compile once per configuration and are
   reused by the application and compatible test executables.
4. An edit to one tool normally rebuilds that tool's implementation and the
   necessary links, rather than every executable's copy of it.
5. Geometry evaluation, snapping, picking, and rendering scale with scene size
   through measured algorithms, reusable samples, and reliable invalidation.
6. Existing interactions and files retain their behavior and compatibility.

Retain C++/Qt for the application and C++ as the authoritative geometry model.
Keep planar curves as the shared local `NurbsCurve2D` plus `WorkPlaneFrame`.
Keep surface XYZ control nets, rational weights, U/V knots, and UV trim curves.
Keep Rhino reduced knot conventions and exact circles/arcs. A tessellation is
derived data; a display grid is not a control net or persistent topology.

This plan does not authorize new modeling features, an automatic conversion
of geometry formats, or an unrequested change to keyboard/mouse behavior.
New nonplanar curve modeling, mesh editing, general Brep modeling, and a new
Rhino export feature require their own contracts. Existing trimmed planar
Fill behavior must remain supported during the refactor.

## 2. Audit findings and priorities

These are source observations, not performance measurements. No fresh build,
runtime benchmark, or test run was performed for this planning document.

| Observed code | Evidence in this working tree | Required direction |
|---|---|---|
| Large viewport implementation | `src/ui/viewport_widget.cpp`: 16,630 lines; concrete widget class and its members are defined in the `.cpp` | Move state and behavior into actual owners; leave Qt event routing and presentation orchestration |
| Large window implementation | `src/ui/main_window.cpp`: 4,506 lines; custom widgets, panels, settings, actions, and application wiring coexist | Extract panels, preferences, and project/update workflow |
| Repeated production compilation | `CMakeLists.txt` gives executable targets overlapping core/service/tool/viewport source lists | Introduce reusable compiled targets and link them from executables |
| Current build configuration | `build/CMakeCache.txt`: Release, Unix Makefiles, `CLASSICAD_BUILD_TESTS=ON` | Measure app-only and all-target builds separately; retain current build directory compatibility |
| Implementation included by a test | `tests/trim_seam.cpp` defines private/protected access macros and includes `viewport_widget.cpp` | Keep legacy test operational initially, then migrate to extracted contracts and public event tests |
| Broad test program | `tests/core_contracts.cpp`: 4,029 lines, including geometry, services, tools, and UI rendering | Partition tests by responsibility without losing assertions |
| Tool names without full ownership | Select/Rotate/Mirror/Trim/Erase `.cpp` files are about 25 lines each; Arc is 21 lines | Finish moving mature interaction state from the viewport into these tools |
| Wide shared header | `core/model.h` includes geometry, snap/sample values, JSON APIs, and `QImage` | Separate persistent Shape contracts, service values, factories, and codecs |
| Broad tool includes | `tools/tool_context.h` includes document/history and all major service definitions | Forward declare reference-only dependencies and expose narrow ports |
| Large algorithm modules | Snap engine: 2,960 lines; point construction tool: 2,255 lines | Separate geometry solvers from interaction and candidate ranking |
| Document bypasses mutation tracking | Mutable `objects()`, `shape()`, `operator[]`, and `Document &shapes_` bridge | Introduce controlled edits and revision notifications before relying on caches |
| Linear identity lookup | `Document::indexOf` scans all objects | Add a derived ID lookup index with restore/reorder invariants |
| Full snapshot history | `History` stores `Document::Snapshot` on both stacks | Keep initially; measure detach/copy cost before introducing reversible deltas |
| Repeated evaluator work | Curve/surface evaluators validate, expand knots, and recursively evaluate basis functions for sampled points | Share prepared evaluators and local-span basis evaluation after equivalence tests |
| Trim containment duplicated | Surface core, CPU renderer, depth geometry, and hit tester contain polygon containment logic | Share parameter-region preparation, clipping, and tessellation |
| Trim cache coverage gap | `viewport_depth_geometry.cpp` hashes surface control nets and knots but omits `trimLoops` | Include trim geometry in invalidation before relying on that cache |
| Blender trim display gap | Python `add_nurbs_surface_mesh` makes every rectangular grid face; it does not use trim loops | Preserve exact Text data and make display proxies respect the visible face |
| Approximate trim consumers differ | CPU grid drops sampled segments; depth drops boundary cells; hit testing uses cell-center inclusion | Establish one derived trimmed-face representation and consistent boundary behavior |
| Small CMake maintenance issues | `document_settings.cpp` listed twice; `opennurbsStatic` repeated in link lists; logger excluded in one test source list | Audit intent and linkage, then remove redundant entries with validation |

The biggest immediate build improvement is eliminating production source
duplication between targets. The biggest architectural improvement is moving
real state ownership out of the viewport. File moves alone do not achieve
either goal. More translation units can even increase clean-build overhead;
use timings to judge the result.

## 3. Target ownership and dependency graph

```text
classiCAD executable / app composition
  -> Qt window and viewport adapters
  -> application session and command routing
  -> tools and document commands
  -> geometry/query/sampling services
  -> geometry values and document/history contracts

Persistence adapters -> document/geometry codecs
Qt viewport renderer -> prepared scene geometry + camera/overlay state
```

Dependency rules:

- Geometry must not include QtWidgets, QPainter, viewport classes, tools, or
  serialization backends. QtCore value types are acceptable.
- Persistent document/Shape types may temporarily use QtGui values such as
  `QColor` and `QImage`. Record this existing dependency explicitly; do not
  claim the entire core is QtCore-only while it remains present.
- Services use document/geometry and small view/query values. They do not own
  windows or invoke tool lifecycle methods.
- Tools own transient interaction state and produce edits/previews. They do
  not receive a concrete widget or a pointer to MainWindow.
- Document commands implement mutations; the application router selects a
  command/tool; Qt actions only invoke that router.
- Serialization converts persistent values. Blender/openNURBS dependencies
  are confined to their adapters and target link requirements.
- The application owns Document, SelectionModel, History, project state, and
  tool/session lifetime. The viewport receives access to the session and owns
  camera presentation, GL resources, and Qt event routing.
- Headers include what they need for value members. Use forward declarations
  for pointer/reference-only types; never introduce incomplete-type errors
  merely to reduce an include count.

### Proposed module map

Create these modules when their responsibility is actually extracted. Exact
names may be adjusted with a documented reason and updated master map.

```text
cmake/
  ClassicadTargets.cmake              reusable targets and target options
  ClassicadTests.cmake                focused test registration and environments

src/app/
  application_session.*              document/selection/history/tool ownership
  command_router.*                   typed command and active-tool dispatch
  project_controller.*               New/Open/Save/import and replacement lifecycle
  update_controller.*                process update/relaunch and session restore
  preferences.*                      validated application preferences

src/core/geometry/
  shape.h                            persistent geometry payload, initially compatible
  shape_workplane.*                  Shape/component frame resolution
  curve_factories.*                  existing curve creation factories
  nurbs_basis.*                      shared knot-span and basis calculations
  curve_evaluator.*                  curve evaluation and prepared evaluator
  surface_evaluator.*                surface evaluation and prepared evaluator
  surface_trim_region.*              UV loop containment/clipping contracts
  curve_editing.*                    knot insertion, splitting, domain-preserving trims
  curve_intersections.*              geometry intersection solvers
  geometry_transform.*               all shared geometry/frame transforms

src/core/document/
  document.*                         object/layer storage, lookup, controlled mutation
  document_change.h                  IDs and change categories/revisions
  selection_model.*                  sole authoritative selection state
  commands/                          named document edits: fill, join, explode, delete,
                                     transform and layer operations

src/core/history/
  history.*                          transaction history; snapshots initially retained
  edit_command.*                     reversible edit records when justified

src/core/serialization/
  geometry_json.*                    Shape/curve/surface/frame codecs
  document_serializer.*              document/layer/object format
  session_serializer.*               versioned update-session state
  blender_project_file.*             native project adapter boundary
  rhino3dm_interchange.*              existing import adapter boundary

src/services/
  snapping/snap_candidate.h          snap result/candidate values and IDs
  snapping/snap_engine.*             selection and ranking of enabled candidates
  sampling/curve_samples.h           sample/cache values
  sampling/scene_geometry_cache.*    shared derived geometry by object/revision
  sampling/surface_tessellator.*      trimmed/untrimmed display/depth meshes
  hit_testing/scene_query.*           candidate objects and world/view queries
  hit_testing/control_point_hit_tester.*
  input/drawing_plane_resolver.*      existing hover/fallback/frame rules
  input/input_constraints.*          shared spatial constraint calculations
  viewport/viewport_transform.*      existing camera/projection calculations

src/tools/
  existing tool modules              actual tool interaction/state owners
  grab_tool.*, scale_tool.*, duplicate_tool.*, subdivide_tool.*, join_tool.*

src/ui/
  main_window.*                      composition of panels/actions/status
  panels/layers_panel.*
  panels/tool_shelf.*
  dialogs/preferences_dialog.*
  viewport/viewport_widget.*         Qt event/lifetime adapter
  viewport/navigation_controller.*   Qt gestures/animation, using existing camera math
  viewport/viewport_render_state.*   immutable frame inputs and preview presentation
  viewport/viewport_renderer.*       CPU/fallback geometry presentation
  viewport/viewport_scene_renderer.* GPU presentation
  viewport/viewport_overlay.*        overlays grouped by actual responsibility

tests/
  core/ services/ tools/ ui/         focused behavioral suites
  support/                          shared fixtures and input-event support
```

`core/model.h` remains a forwarding compatibility facade while callers move.
Do not create another authoritative Shape, curve, or surface representation.
Renaming `point_extrude_tool` is optional and late; its unified behavior is
already more important than the filename.

## 4. Execution phases

### R0 — Establish a recoverable and measurable baseline

Dependencies: none. Changes: documentation/benchmark harness and only repairs
needed to establish the current baseline.

1. Read the required contracts, inspect `git status`, and record current branch,
   HEAD, untracked geometry files, and existing user edits. Preserve them; do
   not reset, clean, stash, or commit the tree automatically.
2. Record toolchain/compiler, Qt version, CMake generator, configuration,
   openNURBS pin, hardware concurrency, and memory available for builds.
3. Build the current tree, list actual CTest registrations, run the existing
   suite, and do bounded offscreen startup. Establish native GL coverage where
   the environment supports the existing viewport test. External Blender
   runtime failures must be reported distinctly from C++ failures.
4. Record geometry/interaction cases needing characterization: Fill boundaries,
   extrusion mixed selections, oriented planes, trim/erase intervals, Join and
   Explode, layers, keyboard completion/cancellation, previews, undo, and reload.
5. Save benchmark procedures/results in `refactoring/BUILD_BASELINE.md` and
   runtime procedures/results in `refactoring/PERFORMANCE_BASELINE.md`.

Build measurements: clean app build, clean all-target build, no-op build,
one implementation edit, common Shape-header edit, and link-only time.
Use separate temporary build directories for clean baselines. Keep the same
compiler, optimization, parallel job count, test option, and machine conditions
for before/after comparison. Measure implementation/header rebuild scenarios
using controlled timestamps or temporary edits that are fully restored; avoid
mixing them with ongoing source modifications. Record compiled object counts,
wall time, and peak memory when available. Do not promise a speedup percentage
before measuring. Distinguish cold and warm compiler caches.

Runtime measurements: curve/surface evaluation throughput, snap/pick latency,
world sample generation, camera-only redraw, single-object edit invalidation,
and undo memory. Use fixed representative scenes at increasing object counts.
Separate GL timings from offscreen CPU fallback timings.

Exit: baseline recorded, current failures identified, exact next action known.
A pre-existing failure can be documented without being silently attributed to
the refactor; fix blockers before migrating the affected responsibility.

### R1 — Compile shared production code once

Dependencies: R0. Main files: `CMakeLists.txt`, new `cmake/` target definitions;
small resource initialization changes only if required.

1. Replace repeated executable source lists with reusable compiled targets.
   Start with boundaries the current code already supports: common runtime,
   viewport support, and viewport adapter; refine into smaller targets in R2.
2. Object libraries are a practical first step for the existing `.qrc` resources
   and overlapping executables. A static library is also valid if resource
   object retention is proven. Choose one justified scheme per module.
3. Keep `main.cpp` and MainWindow composition in the application executable.
   Tests link compatible production objects, never the application entry point.
4. Preserve the Trim test's direct widget inclusion temporarily. Its target
   must not also link the ordinary widget object, which would create duplicate
   definitions. Other tests should reuse the ordinary compiled widget adapter
   where required. This is an explicit temporary exception, removed in R7/R9.
5. Audit the core-contract target's logging exclusion. Make production logging
   ownership explicit and preserve the current test's required behavior rather
   than copying an unexplained source-list exception.
6. Apply consistent compile options/definitions to shared objects and their
   consumers. Configuration/ABI/sanitizer-incompatible variants require separate
   objects; do not share them blindly.
7. Give AUTOMOC/AUTORCC/AUTOUIC one correct target owner. Verify fonts, shader
   files, navigation icons, Blender adapter resources, and QObject metadata in
   both app and tests. For static archives, use a real explicit resource
   initialization entry point or proven retention mechanism when needed.
8. Remove the duplicate document-settings source entry. Audit repeated
   openNURBS/zlib links; remove duplication only after correct dependency/order
   handling is established. Keep third-party warnings separate from project
   warnings without suppressing classiCAD diagnostics.
9. Maintain the existing `classiCAD` executable path, test names, test option,
   Qt5/Qt6 selection, source groups, and pinned dependency revision.

Exit: app and all enabled tests build/link, tests/resources work, production
object duplication falls, and measured build comparisons are recorded. The
temporary macro-based Trim widget build is listed as an exception.

### R2 — Separate headers and enforce module dependencies

Dependencies: R1. Main files: `core/model.*`, `tool_context.*`, includes across
core/services/tools/UI, and target definitions.

1. Move persistent Shape and dimension-anchor values to explicit core headers.
   Preserve layout/defaults and serialization fields during this extraction.
2. Move snap results/candidates/drag results to snapping headers. Move screen
   samples and erase-cache values into sampling contracts. Move JSON codecs
   into serialization; move factories and frame mapping into geometry modules.
3. Replace broad includes in consumers with their actual contract headers.
   Forward declare ToolContext's reference-only collaborators; keep member
   value dependencies complete. Use a small view-input contract where a service
   does not require the entire camera implementation.
4. Retain a forwarding `model.h` during migration, then remove individual
   compatibility imports as call sites are converted and validated.
5. Refine compiled targets into geometry, document/history, serialization,
   services, tools, viewport rendering, viewport adapter, and app composition.
   Isolate openNURBS at the interchange adapter. Account for QtGui in Shape
   until picture/image ownership is deliberately redesigned.
6. Add an automated source dependency audit for forbidden upward includes and
   implementation-file includes, allowing only documented temporary test cases.
   Treat target dependencies as enforceable contracts, not just folder names.

Exit: no new cycles, headers can be included independently, target dependencies
match ownership, and a tool `.cpp` edit has the expected small rebuild scope.

### R3 — Move project and application state out of the viewport

Dependencies: R2. Main files: viewport/window implementations, document codecs,
new application session, project/update controllers, session serializer.

1. Introduce `ApplicationSession` owning Document, SelectionModel, History,
   tool registry/session, and change notifications. Inject it into the viewport
   through a narrow interface with a safe lifetime.
2. Move update-session JSON parsing/writing and camera-state codecs from the
   widget into `session_serializer`. Keep existing format versions and aliases.
   Separate persistent document data from transient interaction state.
3. Move New/Open/Save/Save As/import operations and document replacement into
   `ProjectController`. Apply replacement atomically and reset/prune selection,
   caches, history, pending tools, camera state, and active layer as currently
   specified. Parse/validate into a candidate document before replacing state.
4. Move application update/relaunch orchestration into `UpdateController`;
   preserve the working scene handoff and saved settings. MainWindow presents
   errors and dialogs; controllers perform the workflow.
5. Replace `shapes_`, `pan_`, and selected-ID reference aliases gradually with
   explicit session/view access. Rename ID containers currently called indices
   only after verifying every usage and serialization/test dependency.
6. Keep synchronous project behavior during extraction. Investigate background
   Blender execution later using a captured snapshot and GUI-thread delivery;
   do not combine threading changes with ownership migration.

Exit: the widget no longer owns project serialization or authoritative session
storage, legacy/current reload works, and document replacement cancels stale
tools and invalidates every derived consumer.

### R4 — Centralize document edits, identity lookup, and invalidation

Dependencies: R3; begin command contracts before moving individual tool edits.

1. Add a controlled edit/transaction API producing changed object/layer IDs and
   geometry/style/visibility/selection categories. Use existing snapshot History
   inside it initially; one committed user operation is one undo entry.
2. Migrate direct mutable references and vector writes into those operations.
   A cache revision is trustworthy only once writes cannot bypass tracking.
   During migration, conservatively invalidate at the old mutation boundary.
3. Add per-object geometry revisions and document structural/style revisions.
   Restore/undo/redo also advance runtime invalidation epochs; restoring an old
   snapshot must never reuse a stale cache revision accidentally. These runtime
   epochs need not become persistent file fields.
4. Add an ObjectId-to-storage-index lookup. Maintain/rebuild it after insert,
   remove, reorder, import, snapshot restore, and any legacy replacement.
   Keep ordered layer/object storage as the persistence source of truth.
5. Stop exposing unrestricted mutable object storage after all callers migrate.
   Stable IDs remain authoritative; indexes stay local to traversal/query results.
6. Keep snapshot history until measured memory/latency warrants deltas. Then
   migrate one command family at a time, preserving object/layer IDs, canceled
   gesture semantics, redo invalidation, and selection policy.

Exit: all geometry mutation paths trigger correct notifications, undo is atomic,
ID lookups remain correct after structural edits, and cache invalidation can be
tested without scanning/serializing the whole document.

### R5 — Share geometry preparation, trimming, and tessellation

Dependencies: R2 for contracts; R4 before persistent revision-keyed caches.
Split into R5a extraction/correctness and R5b measured optimization.

1. Extract repeated vector/frame transforms into named existing geometry owners.
   Extract rational knot insertion, Bezier span splitting, curve trimming, and
   intersection math from the viewport into geometry algorithms. Keep screen
   tolerance, hover selection, and candidate ranking in services/tools.
2. Prepare evaluators once per geometry revision: validate input, expand knots,
   determine domains, and retain immutable prepared data. Share span search and
   basis calculations between curve and surface evaluation. Replace recursive
   all-CV work with local-span evaluation only after numerical equivalence tests.
3. Preserve public checked evaluation for callers with unvalidated input.
   Prepared evaluators must not retain dangling references to mutable Shape
   storage. Test rational weights, repeated knots, non-unit domains, seams,
   endpoint behavior, and non-clamped curves; flag pre-existing discrepancies
   explicitly rather than silently altering them during optimization.
4. Introduce one prepared UV trim region, with explicit outer/hole semantics,
   boundary tolerance, closure/degeneracy rules, and containment/clipping.
   Replace copies in core, rendering, hit testing, and depth geometry.
5. Build one derived surface tessellation preserving trimmed boundaries and
   holes. Reuse it for depth/picking and display proxies. Use clipped isocurves
   for wire display, separately from the base four-CV plane/control net.
6. Resolve the observed trim gaps as explicit correctness work: include trim
   loops in the current depth fingerprint until revisions replace it; remove
   cell-center-only picking and boundary-cell shrinkage through shared geometry;
   make Blender proxy meshes respect trims. Prefer passing derived C++ mesh
   data to the Python adapter rather than adding another modeling algorithm.
7. Keep exact NURBS/trim data in the document Text snapshot and internal JSON.
   Proxy changes must never replace it. Test old untrimmed surface records.
8. Consolidate curve/scene sampling behind `SceneGeometryCache`: world samples
   depend on geometry revision/tessellation settings; screen projection depends
   additionally on camera/viewport revision. Camera motion should not rebuild
   the unchanged world geometry of every object.

Exit: display, picking, depth, and proxies agree on the visible face; trim edits
invalidate caches; evaluator accuracy holds; performance changes have timings.
Include tilted-plane circles, rectangles, concave closed curves, rejected
invalid loops, rational trims, and hole data where supported by the contract.

### R6 — Extract standalone document commands

Dependencies: R4; geometry helpers from R5a as needed.

Move Delete, Fill, Explode, Join result construction, subdivision-marker edits,
layer edits, and transform commits into named command modules. Start with
Delete/Explode/Fill; they provide a small, reviewable command boundary.

Keep selected ObjectIds and layer editability checks explicit. Construct and
validate all proposed replacements/additions before applying the transaction.
Maintain component workplanes and parameter domains. Rectangle Explode must
produce the existing independent edge objects. Join must preserve its existing
ordering/component semantics and deliberate line-fusion behavior; do not expand
its supported inputs as part of extraction. Preserve Fill's in-place curve-to-
face policy and exact UV trim curve. Mixed extrusion stays one tool/one edit.

`app/command_router` maps existing typed commands to these operations/tools.
It must not become a new geometry algorithm dumping ground.

Exit: commands are testable without a viewport, invalid/no-op commands add no
undo entry, layer/ID/selection behavior matches baseline, and widget methods
are temporary delegates that can be removed after callers migrate.

### R7 — Extract selection, spatial input, and navigation

Dependencies: R3/R4 and shared query contracts; preserve camera calculations.

1. Move click/Shift-click/box selection and control-point drag state into Select
   and, if responsibilities justify it, a ControlPoint tool. SelectionModel is
   the single state owner; prune missing/hidden/locked objects consistently.
2. Move hover-based drawing-plane resolution and reusable world-axis/normal
   constraints to shared input services. Preserve Line's spatial exception,
   principal-plane fallbacks, modifier remapping, stable anchors, and existing
   SnapEngine markers. Do not flatten world input to an initial plane.
3. Move grab/base-point move, duplicate, and their cancellation snapshots into
   dedicated tools using document transactions and shared spatial constraints.
4. Move Qt navigation gestures, timers, preset animation, and gizmo routing to
   NavigationController. Keep quaternion/picking/zoom math in ViewportTransform.
   Define event precedence between UI focus, navigation, active tools, and
   application shortcuts before changing routing.
5. Translate Qt events into ToolInput once. Keep handling/consumption results
   explicit; avoid replaying one event through both a migrated tool and the
   old widget branch. Preserve Enter/Space/Esc/right-click and wheel semantics.
6. Introduce a real viewport class header when required by tests/composition.
   Begin replacing direct `.cpp` inclusion and private macros with extracted
   service/command tests and public viewport-event tests. Avoid a broad test-only
   accessor API that exposes all the widget's internals.

Exit: selection/navigation/drag state lives with its owner, boundary/box/depth
picking and cancellation regressions pass, and macro-based test cases have a
documented migration list with no lost coverage.

### R8 — Finish Arc and transform tool ownership

Dependencies: R5a/R6/R7. Complete in subphases: Arc, Rotate/Mirror, Scale,
Subdivision/interactive Join. Read the complete add-on issue record if actual
port behavior needs modification or renewed source comparison.

ArcTool must own one/two/three-point stages, typed radius/angle input, winding,
perpendicular/axis/plane locks, vertical hysteresis, completion, and preview
tracking. Preserve the existing exact arc factory and add-on interaction
thresholds. Output tool preview values for HUD/compass presentation rather than
copying viewport rendering into the tool. Remove the special case protecting
Arc's viewport-owned points only after it has one authoritative state owner.

Rotate/Mirror/Scale own their pivot/reference stages, axis constraints, numeric
input, previews, commit/cancel behavior, and selected IDs. Use shared transforms
for curve frames, PolyCurve component frames, surface nets, and image geometry;
surface UV trim coordinates remain fixed under world transforms.

Subdivision owns wheel accumulation and staged marker preview. JoinTool owns
interactive input; a command owns resulting geometry edits. Keep point/curve
extrusion unified; reuse its proven world input behavior rather than splitting
it into separate point/surface tools.

Exit: migrated tool state members and event branches disappear from the widget;
direct tool tests and focused Qt event regressions cover actual behavior.

### R9 — Finish Trim and Erase ownership

Dependencies: R4/R5/R7; keep late because of geometric and interaction risks.

1. Move pure intersections, parameter intervals, exact split/rebuild operations,
   and reusable sample preparation into geometry/services.
2. TrimTool owns hover targets, box selection, click choice, interval selection,
   transient status, and commit intent. EraseTool owns stroke input, stroke
   accumulation, affected interval previews, and completion/cancellation.
3. Feed stable object/component references and revision-keyed samples through
   narrow context/query ports. Preserve workplane filtering and snap/lock rules.
4. Replace viewport mutation bodies with document commands/transactions. Keep
   exact rational curves and source parameter domains after trimming.
5. Complete migration of every assertion from the legacy Trim test before
   removing its implementation include/access macros. Do not delete difficult
   cases or replace meaningful assertions with checks that only mirror code.

Exit: Trim/Erase tools contain the real lifecycle, geometry algorithms have one
owner, existing seam/crossing/overlap/box regressions remain covered, and the
widget `.cpp` is compiled once through its ordinary production target.

### R10 — Simplify viewport rendering and scene queries

Dependencies: shared prepared geometry and migrated preview contracts.

1. Create explicit immutable frame/render inputs: camera, visible object IDs,
   styling, prepared geometry, selection, and one active ToolPreview.
2. Replace tool-specific mirrored widget preview members with a single preview
   presentation path. Tool previews describe geometry/guides/HUD, while CPU/GL
   renderers handle presentation and fallback.
3. Split overlay responsibilities when they truly differ: HUD/compass,
   selection/control guides, and snap markers. Preserve one snap marker model.
4. Share culling/bounds/geometry between GL, CPU fallback, depth, and queries.
   Keep screen-dependent styles/projection distinct from world tessellation.
5. Profile object scans before adding a spatial index. If justified, add a
   scene broad-phase query structure with conservative bounds, followed by
   existing exact/accurate narrow-phase tests. Rational positive-weight bounds,
   oriented frames, visibility, and high-depth geometry must remain correct.
6. Preserve GL context/resource lifetime, high-DPI behavior, styles, pictures,
   fonts, transparent pixels, and native versus fallback behavior.

Exit: the viewport primarily translates input and schedules rendering; renderer
changes no longer require touching tool state or persistent geometry. A camera
change reuses unchanged world samples and query results remain consistent.

### R11 — Organize window UI and test suites

Dependencies: command/session/project interfaces established.

Extract LayersPanel, ToolShelf, preferences dialogs/store, and relevant custom
widgets from MainWindow. Give panels typed inputs/callbacks; model changes go
through session commands. MainWindow retains composition, action/status wiring,
and top-level presentation. Avoid a catch-all controller replacing the widget.

Split the existing monolithic tests into geometry/document/history/serialization,
service, tool, and UI suites. Share fixtures in test support; preserve assertions,
test environments, and meaningful regressions. Pure geometry tests should not
need QApplication, GL, or Blender. Native GL and external Blender checks should
be clearly identifiable so a missing runtime is reported accurately. Test
executables link production libraries; they do not recompile entire source lists.

Exit: UI panels compile independently through narrow interfaces; tests are
focused, coverage is preserved, and no production `.cpp` is included in tests.

### R12 — Measure, tune, and enforce the completed architecture

Dependencies: prior ownership/build phases complete; optional tuning can be
evaluated earlier after R1 when it is isolated and measurable.

1. Repeat R0 build/runtime measurements under identical conditions. Record
   actual improvements/regressions, compiled-object counts, and peak memory.
2. Evaluate compiler caching if available, and optional Ninja/presets in
   separate build directories. Never switch an existing configured directory's
   generator in place. Provide app-development and full-test configurations.
3. Consider narrowly scoped precompiled headers only after include reduction.
   Prefer stable Qt/system headers; adding frequently edited model/tool headers
   to a PCH can enlarge incremental rebuilds. Prove app/test consistency.
4. Evaluate unity builds only as an optional measured experiment. They can
   expose anonymous-namespace collisions and enlarge rebuild units. Do not make
   them the default architecture or substitute for module extraction.
5. Profile remaining runtime bottlenecks before changing evaluator precision,
   curve sampling, history storage, or adding threads. Worker jobs, if warranted,
   consume immutable snapshots and publish revision-checked results on the GUI
   thread; stale jobs must not overwrite current geometry.
6. Remove unused compatibility facades/delegates after callers/tests migrate.
   Enforce target/include boundaries and update the final source map/README.
7. Add appropriate continuous checks using actual project infrastructure:
   build, dependency audit, core/tool tests, offscreen startup, and native GL/
   Blender coverage where the runner supports them.

Exit: report measured outcomes without invented speed claims; all phase exit
conditions hold; a new feature has an obvious geometry/service/tool/UI owner;
the final build graph and folder map agree.

## 5. Build target rollout

Use this as the eventual graph, not a demand to create every target on day one.

| Target responsibility | Compiled content | Expected dependencies |
|---|---|---|
| `classicad_geometry` | NURBS values, frames, evaluation, factories, pure editing math | QtCore |
| `classicad_document` | Shape/document/layers/selection/history/commands | geometry; QtCore/QtGui while existing payloads require them |
| `classicad_serialization` | geometry/document/session JSON | document |
| `classicad_blender_adapter` | process/native project bridge and adapter resource | serialization, Qt process APIs |
| `classicad_rhino_adapter` | existing Rhino import | document, openNURBS and required compression deps |
| `classicad_services` | sampling/snapping/queries/dimensions/view math | core contracts; QtGui where presentation calculations require it |
| `classicad_tools` | tool lifecycle and interactions | services, document command ports |
| `classicad_viewport_rendering` | CPU/GL renderers, overlays, shader/font resources | services/core, QtGui/OpenGL/Widgets as actually used |
| `classicad_viewport` | widget adapter/navigation | rendering, session interface, QtWidgets/OpenGLWidgets |
| `classicad_app` | session/router/project/preferences integration | tools and adapter modules; no concrete widget dependency in session contracts |
| `classiCAD` | main/window composition | app, viewport and panel modules |

Avoid cycles between app and viewport: session/command port contracts must be
defined in headers usable by both, and concrete widget construction belongs
at the application composition boundary. Tests choose the smallest needed
targets. Resource initialization remains explicit and covered by integration
checks. Third-party sources never become duplicated classiCAD sources.

## 6. Validation and migration rules

For each implementation checkpoint:

1. Record files/state responsibility moved and identify all live callers.
2. Add meaningful characterization only where coverage is missing and the
   migration could change behavior. Use existing tests first.
3. Keep adapters/delegates temporarily when required, with named removal phase.
   Move state and behavior together; do not keep two independent live copies.
4. Build and run the relevant suites. Follow the master prompt's full checkpoint
   validation, including the existing CTest suite and bounded startup, before
   declaring the phase complete.
5. Inspect changes and update this ledger plus the master map/ledger. Record
   failed/skipped checks and their reasons; never mark unrun tests as passed.

Typical commands, adjusted to the actual environment:

```sh
git status --short
git diff --check
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
QT_QPA_PLATFORM=offscreen timeout 5s ./build/classiCAD
```

For startup, a timeout after successful initialization means the expected event
loop was stopped; it is not automatically a failure or proof of a clean startup.
Inspect logs and exit behavior. Choose job count from measured available memory;
the audit observed eight logical CPUs, which does not justify eight heavy C++
compilations automatically. Run native GL checks when rendering/input changes
and supported infrastructure make them relevant.

Review this behavioral matrix throughout migration:

| Responsibility | Required cases |
|---|---|
| Geometry | rational circles/arcs, positive weights, reduced knots, repeated knots, domain preservation, valid/invalid inputs |
| Workplanes/input | XY/XZ/YZ and oblique frames, hover inheritance, perspective fallback, Line world axes/normal, stable anchors |
| Selection/queries | additive selection, crossing/window boxes, hidden/locked layers, depth overlap, corner/boundary snaps |
| Tool lifecycle | numeric input, snapping priority, wheel accumulation, completion, Esc/right-click, tool switches |
| Commands | ID/layer preservation, exact component geometry, no-op edits, atomic undo/redo, cancellation |
| Surface faces | extrusion exactness, planar Fill trim, concavity, boundary clipping, trim invalidation, transforms, proxy agreement |
| Persistence | supported session versions, exact native snapshot, oriented components, surface trims, candidate-load failure leaves current document intact |
| Presentation | CPU/GL previews and committed geometry, high-DPI resources, pictures/alpha, dimensions/fonts, styles |

Separate intentional bug corrections from mechanical extraction in reviewable
changes. Capture the failing case first. The known trim/cache/proxy gaps in R5
are explicitly listed corrections; they are not permission to invent features.

## 7. Completion criteria

- Production sources compile once for compatible app/test consumers, with any
  justified variants listed and measured.
- Small implementation edits have demonstrably smaller rebuild scope; clean
  build results are recorded independently of incremental build results.
- Widget code owns Qt lifecycle/routing, not modeling commands or tool states.
- Every migrated tool owns its transient lifecycle and has behavioral coverage.
- Document edits are atomic, ID-based, observable, and cache-safe.
- Prepared geometry is shared without changing exact persistent definitions.
- Project and update serialization are separate from Qt widget implementations.
- No production `.cpp` inclusion/private-access macro remains in tests.
- Core/services/tools dependencies point downward; CMake enforces real modules.
- Runtime optimizations have evidence and preserve precision and visible faces.
- Build/tests/startup and applicable external/native checks pass or have clearly
  stated environment limitations; no source migration is hidden as complete.

File length is a review signal, not a substitute for ownership. Aim for a thin
viewport adapter whose contents fit the four duties in the master contract.
Split any growing module when it gains a second responsibility; avoid creating
dozens of tiny forwarding files to meet an arbitrary line-count target.

## 8. Progress and next-action ledger

Update after every execution checkpoint with actual files, validation results,
measurements, compatibility delegates remaining, and the next concrete action.

| Phase | Status | Next action / completion evidence |
|---|---|---|
| Audit and plan | Complete | Source review and plan written; no implementation or fresh benchmark claimed |
| R0 Baseline | Not started | Preserve working tree; build/test current state; record comparable build/runtime baselines |
| R1 Shared compilation | Not started | Extract reusable targets; preserve legacy Trim inclusion exception and resource ownership |
| R2 Contracts/dependencies | Not started | Separate model/service/codec headers; refine target graph |
| R3 Application/project state | Not started | Introduce session ownership; extract project/update/session serialization |
| R4 Edits/revisions/identity | Not started | Centralize transactions and writes before revision-keyed caches |
| R5a Shared geometry correctness | Not started | Extract math, prepare trim regions, characterize known trim gaps |
| R5b Evaluation/cache performance | Not started | Benchmark prepared/local-span evaluation and shared tessellation caches |
| R6 Commands | Not started | Extract Delete, Explode, Fill first, then remaining standalone edits |
| R7 Selection/input/navigation | Not started | Migrate selection and spatial input; begin legacy test replacement |
| R8 Tool ownership | Not started | Migrate Arc and transform tools in independent subphases |
| R9 Trim/Erase | Not started | Move exact editing/query algorithms and lifecycle; finish legacy test replacement |
| R10 Rendering/queries | Not started | Consume shared prepared geometry and one active preview contract |
| R11 UI/tests | Not started | Extract panels/preferences; partition behavioral suites |
| R12 Measurement/enforcement | Not started | Compare baseline, apply justified tuning, remove remaining bridges |

For continuation, append a checkpoint record with: phase/subphase, source files
changed, behavior evidence, build/test/smoke outcomes, before/after measurements
where relevant, known issues, and the next actionable task. Preserve prior
records so the following chat can resume without reconstructing the work.
