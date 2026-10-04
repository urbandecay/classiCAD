# classiCAD modularity, scalability, and build refactor plan

Status: complete as of 2026-10-04. R10 now shares prepared world geometry with the CPU fallback and has measured snap, pick, CPU/native-GL redraw, and single-object cache invalidation. R12 has warm R0/current build measurements, undo latency/RSS measurements, configured isolated CMake presets, and passing full-build/test/startup checks. Results include regressions and limitations; they do not imply that every build or renderer path is faster.

Prepared from the working tree on 2026-10-04. Paths and sizes below describe
that tree, including its uncommitted NURBS surface and Fill work. Recheck paths
before continuing. This document is the living execution plan.

## Current status

Read `AGENTS.md`, then all of `refactoring/MASTER_REFACTOR_PROMPT.md`, then
this file. The master prompt remains the architecture contract; this plan
continues its unfinished ownership migrations and adds measurable build and
runtime improvements. Historical phases marked complete in the master ledger
do not mean every compatibility bridge has already been removed.

The earlier suggested next-chat instruction below is historical and superseded
by the final checkpoint at the end of this file. R0–R12 are complete against
the stated exit criteria; retain the documented app-only build and CPU fallback
costs as follow-up performance opportunities, not as unmeasured gates.

Historical suggested instruction to paste into the next chat:

> Follow `refactoring/MODULARITY_AND_BUILD_REFACTOR_PLAN.md`. Read `AGENTS.md`
> and the full `refactoring/MASTER_REFACTOR_PROMPT.md` before starting the next
> iteration. Preserve the current working tree. R0–R6 implementation is
> complete pending behavioral verification; R7 event-result and legacy fixture
> migration and R8 tool ownership are in progress. R8 transform, Join, and
> Subdivision ownership has advanced. Continue in dependency order, build each
> checkpoint, update both ledgers, and preserve modeling and interaction behavior. Do not
> commit or push unless I request it.

R0–R12 implementation and completion evidence are recorded below. The latest
checkpoint supersedes historical continuation notes and is the source of truth
for the final build/runtime measurements.

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

These are source observations from the planning audit. The pre-refactor build
and test results are recorded in `BUILD_BASELINE.md`; runtime limitations and
the measurements still required are recorded in `PERFORMANCE_BASELINE.md`.

| Observed code | Evidence in this working tree | Required direction |
|---|---|---|
| Large viewport implementation | `src/ui/viewport_widget.cpp`: 16,630 lines; concrete widget class and its members are defined in the `.cpp` | Move state and behavior into actual owners; leave Qt event routing and presentation orchestration |
| Large window implementation | `src/ui/main_window.cpp`: 4,506 lines; custom widgets, panels, settings, actions, and application wiring coexist | Extract panels, preferences, and project/update workflow |
| Repeated production compilation | `CMakeLists.txt` gives executable targets overlapping core/service/tool/viewport source lists | Introduce reusable compiled targets and link them from executables |
| Current build configuration | `build/CMakeCache.txt`: Release, Unix Makefiles, `CLASSICAD_BUILD_TESTS=ON`, `CLASSICAD_BUILD_BENCHMARKS=ON`; isolated `app-dev` and `full-test` presets now exist | Measure app-only and all-target builds separately; retain current build directory compatibility |
| Implementation included by a test | `tests/trim_seam.cpp` no longer includes `viewport_widget.cpp` or widens access with private/protected macros | Trim geometry, query, tool, command, selection-box, and session checks now compile against their owning contracts; restore the removed viewport-event assertions in the public interaction suite |
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
  core/geometry/nurbs_surface_tessellator.* trimmed wireframe and visible-face meshes shared by view, pick, depth, and Blender proxy
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
  panels/tool_shelf.*                tool-button layout, selection group, scroll, and help presentation
  input/tool_input_translator.*      Qt mouse/key/wheel fields plus resolved positions translated into ToolInput
  panels/layers_panel.*
  panels/tool_shelf.*
  dialogs/preferences_dialog.*
  viewport/viewport_widget.*         Qt event/lifetime adapter
  viewport/navigation_controller.*   Qt gestures/animation, using existing camera math
  viewport/viewport_render_frame.*  visible scene entries, render styling, camera, revisions, and active preview snapshot
  viewport/viewport_geometry_cache.* revision-keyed prepared world geometry shared by GPU strokes and the depth pass
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

#### R0 execution record (2026-10-04)

The working tree was clean at `4dd4f21` on `feature/3d-workplanes`. The build used
GCC 13.3.0, Qt 6.4.2, CMake 3.28.3, Unix Makefiles, Release, tests enabled,
and four jobs. A cold clean all-target build passed in 431.01 seconds at
728,036 KB peak RSS, including openNURBS. The warm-dependency clean app build
took 104.05 seconds; the no-op all-target build took 2.70 seconds. A single
tool edit took 5.69 seconds, a common `core/model.h` rebuild took 79.99 seconds,
and a link-only app rebuild took 2.55 seconds. The committed CMake source lists
compiled 266 production source objects across 67 unique source paths; 65 paths
were duplicated between targets, producing 199 repeated compilations. Full
details and counting boundaries are in `BUILD_BASELINE.md`.

All five CTest registrations ran in 235.66 seconds: four passed, while
`trim_seam` had five pre-existing Erase, Trim, and Rotate assertion failures.
The bounded offscreen startup logged successful application and viewport
construction before the five-second timeout. Runtime geometry/query/render
benchmarks have not yet been measured because no representative fixture
harness exists; the required measurements and limits are recorded in
`PERFORMANCE_BASELINE.md` for R5b/R10/R12. This records the initial R0
baseline; the later R1–R12 implementation and final results are recorded below.

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

#### R1 execution record (2026-10-04)

Added `cmake/ClassicadTargets.cmake` with one target-configuration helper and
one explicit object-module assembly helper. `CMakeLists.txt` now compiles core
contracts, logging, services, tools, shared viewport rendering/resources,
viewport-only support, the ordinary viewport widget, and MainWindow through
reusable object targets. The app compiles only `main.cpp` directly. Test
executables link those production objects; the Trim test still includes
`viewport_widget.cpp` under its private/protected access macros and does not
also link the ordinary widget object. The core-contract test excludes the
explicit logging object, preserving its prior link behavior.

The duplicate `document_settings.cpp` entry was removed. The repeated
openNURBS/zlib link sequence was retained with a comment: bundled zlib's
allocator hooks are defined by openNURBS, so static linkers need the openNURBS
archive after zlib. The Blender adapter, dimension font, grid shader, and
navigation icon resources each have one object-target owner; explicit object
inclusion keeps their resource initializers in each required executable.

With warm dependency outputs, a project-only all-target rebuild took 111.33
seconds at 730,776 KB peak RSS. The all-target no-op build took 2.63 seconds.
A controlled point-tool edit compiled one shared object and relinked its four
consumers in 6.57 seconds. The module targets contain 66 production `.cpp`
objects for 66 unique paths, with no repeated source path; the app's `main.cpp`
is its only additional production translation unit. The Trim test's direct
widget include remains the one intentional extra widget compilation. See
`BUILD_BASELINE.md` for counting boundaries and the before/after measurements;
the cold R0 build includes openNURBS and is not a direct comparison with this
warm project-only result.

The app and all four test executables built and linked, and the unchanged five
CTest registrations ran in 223.33 seconds. Box selection, core contracts,
viewport interaction, and Vignola persistence passed. `trim_seam` retained the
same five baseline Erase/Trim/Rotate failures. The offscreen app again logged
successful startup and viewport construction before the five-second timeout.
`git diff --check` passed. R1 is complete. Next: R2, split broad model/service
contracts and refine the object targets into enforceable dependency layers.

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

R2 completion record (2026-10-04): Added explicit Shape/dimension, snap,
sampling, curve-construction, workplane-mapping, planar-geometry, and JSON-codec
contracts. Moved factories and frame mapping into `core/geometry`, moved Shape
JSON into `core/serialization/shape_json_codec.*`, and kept `core/model.h` as
a forwarding umbrella. `ToolContext` now forward-declares reference-only
collaborators; production services and tools import their owning contracts.
CMake now builds geometry, document/history, serialization, Rhino interchange,
services, tools, viewport rendering, viewport adapter, and app composition
modules. Core/service/tool modules use Qt Core/Gui; viewport/app targets add
Widgets/OpenGL. openNURBS is linked only for Rhino interchange consumers. The
dependency audit runs in the default build; it passed for 162 source files and
reports only the documented model facade and Trim test exceptions. All targets
built, `git diff --check` passed, and the offscreen app logged successful
startup and viewport construction before its expected five-second event-loop
timeout. A point-tool edit rebuilt one object and relinked four consumers in
4.30 s at 194,120 KB peak RSS; the no-op build took 2.83 s at 77,340 KB. CTest
was not rerun in this checkpoint. R3 is next: move application/session,
project/update workflows, and widget camera/update-session codecs behind
explicit app/service contracts.

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

#### R3 execution record (2026-10-04)

Added `src/app/ApplicationSession` to own the active Document, SelectionModel,
History, and ToolRegistry. MainWindow creates the session before the viewport;
the viewport borrows these owners and retains compatibility references during
incremental migration. Session notifications drive MainWindow history, layer,
and dirty-state refreshes. Document replacement sends a pre-replacement
notification so the active tool/grab/duplicate interaction can be cancelled
against the old model.

Moved update-session v1–v4 JSON parsing/writing and camera/workplane codecs to
`SessionSerializer`. Restore validates a candidate Document before changing the
session. Moved native project open/save, Rhino import, and New document workflow
into `ProjectController`; imports are staged on a candidate Document and added
to snapshot history once. Moved update build/relaunch and window-geometry
handoff into `UpdateController`. MainWindow now presents dialogs/status and
applies viewport/window presentation after controller operations. Narrow
ViewportWidgetApi compatibility methods remain for interaction callers and
delegate to ProjectController.

The CMake build completed for the application and all test executables, and the
default dependency audit passed for 170 source files. Offscreen startup logged
successful application and viewport initialization before its expected
five-second event-loop timeout. CTest was not run per the current execution
instruction; `git diff --check` passed in the following R4 checkpoint. No
measured performance claim is made for R3. Next: R4 transactions,
revision/invalidation contracts, and the ObjectId storage lookup.

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

#### R4 checkpoint (2026-10-04)

Added `DocumentChangeSet` and `DocumentTransaction` with snapshot rollback,
changed object/layer IDs, geometry/structure/layer/visibility/settings/selection
categories, ordered insertion, object replacement, and explicit broad geometry
invalidation. `Document` now has an ObjectId-to-storage-index hash maintained by
insert/remove/replace/restore paths, per-object geometry revisions, and
monotonic document geometry/structure/layer/visibility/settings epochs. The
mutable `Document::objects()` overload was removed; ordered object access is
read-only. Session replacement preserves the document instance and advances
runtime revisions.

Migrated tool single/batch commits, settings changes, duplication, Join,
Explode, Fill, Delete, Mirror, Scale, Rotate, and multi-object Trim/Erase to
transactions. Snapshot History remains the backing store. Document object,
shape, layer, vector, and indexed shape access are read-only; transaction-only
mutable access is private. `mutateGeometry()` is the explicit non-undo edit
boundary for live drag and associative-dimension updates, and advances both
document and per-object geometry revisions. Bulk replacement and all insert,
remove, settings, layer, restore, and import paths report invalidations. The
ObjectId index is maintained across structural edits and restore, and runtime
epochs remain monotonic across snapshot restores. No writable document API can
silently bypass geometry tracking.

`cmake --build build --parallel 4` passed, including the default dependency
audit (179 source files). The offscreen app logged application start and
viewport construction before the expected five-second event-loop timeout;
`git diff --check` passed. CTest was not run per the current execution
instruction. R4 is complete; no performance result is claimed.

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

#### R5a checkpoint (2026-10-04)

Moved the exact rational knot insertion, Bezier span extraction/splitting, and
parameter-domain-preserving NURBS trim implementation from
`viewport_widget.cpp` into `core/geometry/curve_editing.*`. The viewport now
calls the shared geometry function for trim interval construction.

Added `PreparedNurbsSurfaceTrimRegion` in `core/geometry/surface_trim_region.*`
and routed NURBS surface trim containment through it in surface evaluation,
CPU wire drawing, depth mesh generation, and hit testing. Existing caller
sample counts (128 for the core containment function, 256 for display/depth/
hit testing), outer-loop handling, hole exclusion, and ray-crossing boundary
behavior remain the same. Moved scale and world-axis rotation math into
`core/geometry/geometry_transform.*`; the viewport supplies only interaction
state and the current surface workplane.

Added `PreparedNurbsSurfaceTessellation` in
`core/geometry/nurbs_surface_tessellator.*`. It produces clipped isocurve
polylines, sampled trim boundaries, and an indexed visible-triangle mesh with
adaptive subdivision near trim boundaries. Viewport wire drawing, depth
occlusion, surface picking, and Blender proxy generation now use this shared
derived data. The Blender adapter receives transient C++ mesh data and removes
it before writing the authoritative document Text; exact NURBS control nets
and UV trim curves remain in the document. This keeps surface tessellation in
the geometry layer so serialization does not depend upward on viewport
services.

`cmake --build build --parallel 4` passed and the dependency audit checked 179
source files. `git diff --check` passed. The offscreen application logged
startup and viewport construction before its expected five-second timeout.
CTest was not run per the current execution instruction. Boundary subdivision
is a viewport approximation (48 base cells and up to four subdivision levels);
R5a implementation is complete.

#### R5b checkpoint (2026-10-04)

Added `PreparedNurbsSurfaceEvaluator`, which validates the surface, expands U/V
knot vectors, and records parameter domains once. Surface tessellation retains
one prepared evaluator for all sampled vertices, isocurves, and trim boundaries.
The checked public evaluation functions still prepare from arbitrary input;
repeat-sampling callers can retain prepared state. Evaluation uses the existing
recursive basis calculation to avoid changing numerical behavior as part of
this optimization.

Added a bounded 128-entry `SurfaceTessellationCache`, keyed by document
`ObjectId` and geometry revision. Committed display, curve hit testing, and
depth/picking share the cached prepared mesh. Transformed previews bypass the
persistent cache. The GPU depth cache uses object/revision keys for unchanged
visible scenes; its compatibility fingerprint includes UV trim loops for
transient geometry. This cache stores derived display data only; persistent
surface CVs, weights, knots, and trims remain authoritative.

Added opt-in CMake target `classicad_nurbs_surface_benchmark` under
`CLASSICAD_BUILD_BENCHMARKS`. In this build, median evaluation times for 1,200
points were 0.90 ms checked versus 0.44 ms prepared (4x4 CVs), 2.92 ms versus
2.02 ms (12x12), and 6.77 ms versus 4.24 ms (24x24), a measured 1.45–2.06x
speedup. Cold tessellation took 2.53, 8.93, and 17.79 ms respectively; 1,200
warm cache lookups took 0.040, 0.039, and 0.039 ms. These are local
microbenchmark results, not full-frame timings. The checked path recreates
prepared state per sample to model repeated checked evaluation. Full viewport
scenes remain for R10/R12.

`cmake --build build --parallel 4` passed and the dependency audit checked 183
files. The optional benchmark target built and ran successfully. The offscreen
application logged startup and viewport construction before its expected
five-second timeout. `git diff --check` passed. CTest was not run. R5b is
complete; R6 starts with standalone Delete, Explode, and Fill commands.

#### R6/R7 starting checkpoint (2026-10-04)

R6 added `src/app/command_router.*`, which maps stable application command IDs
to registered handlers without depending on Qt or viewport types. The viewport
API keeps source-compatible `ViewportCommand` aliases, while `ViewportWidget`
registers the existing operation handlers. Existing Delete, Explode, Fill,
Join, subdivision, layer, and transform modules remain responsible for their
edits; widget methods continue as temporary delegates where interaction state
is still needed.

R7 began selection ownership migration. `SelectionModel` no longer exposes
mutable collection, primary-ID, or control-point-index references. It now
prunes missing, hidden, locked, and frozen objects centrally. `SelectTool` owns
click/Shift-click transitions, Select All, and application of object IDs
collected by the viewport's existing box query. The viewport supplies hit
tests, drag initialization, and selection-box presentation through
`ToolContext`. The legacy Trim fixture now sets selection through
`SelectionModel` rather than writing viewport aliases.

Validation at this starting checkpoint: `cmake --build build --parallel 4`
passed and the dependency audit checked 199 source files. R7 work described
below records the subsequent migration state. No new performance claim is made.

#### R7 ownership checkpoint (2026-10-04)

SelectionModel remains the single selection state owner. `SelectTool` now owns
selection-box lifecycle and selection drag targets, including control-point
drag state; ViewportWidget retains screen-space hit tests, geometry mutation,
and painting. The shared drawing-plane and world-constraint services remain
under `src/services/input/`, with Line preserving its world-space exception and
snap behavior. `GrabTool` owns move selection, base point, cursor offset, moved
state, and its cancellation snapshot. `DuplicateTool` owns source IDs,
base-point stages, destination, and translated preview values.
`DuplicateCommand` plans valid live-source copies and commits them atomically,
including dimension-anchor remapping. `NavigationController` owns pan/orbit,
gizmo gestures, preset animation, wheel zoom, cursor, and navigation tooltips;
camera math remains in `ViewportTransform`. The Trim fixture's direct access to
old viewport drag fields was migrated to the owning APIs.

Validation: `cmake --build build --parallel 4` passed after these changes. The
dependency audit checked 211 source files. CTest and an offscreen startup smoke
were not run. No new performance claim is made. Remaining R7 work is to make
event translation/consumption explicit, introduce the viewport header boundary
if needed, and record a coverage-preserving migration plan for private-macro
test fixtures. R6 behavior still needs its planned regression run.

#### R7 typed event dispatch checkpoint (2026-10-04)

`ui/input/tool_input_translator.*` now owns conversion of mouse, key, and wheel
event fields into `ToolInput`, with camera-dependent cursor projection supplied
by the viewport. `InteractionTool` exposes `EventResult::Handled` and
`EventResult::Unhandled` dispatch adapters over its existing handlers. The
viewport uses those results for generic mouse, key, and wheel routing. A
mouse-press route constructs one `ToolInput` after resolving the view-space
cursor and reuses it across the active tool, Rotate, Mirror, Select, and Arc
paths. Key translation now captures key, text, modifiers, repeat state, screen
position, and workplane once and reuses that value for Arc, Rotate, axis, and
generic tool paths. Mouse-move translation occurs once on the valid or
camera-clipped route. Qt-specific priority branches remain in the widget; this
checkpoint does not reorder gizmo, pan, grab, duplicate, Join, Subdivision,
Trim, or Erase handling.

The macro-based Trim fixture still includes `viewport_widget.cpp`. Its
remaining direct-access cases are inventoried for migration: camera/session
round trips and selection-box clipping; Join topology and selection outcomes;
Trim/Erase cache preparation, hover intervals, exact edits, and undo; and
Rotate/Scale/Mirror selection previews and committed geometry. Core geometry,
curve sampling, curve hit-testing, selection, navigation, and transaction
operations should move to their current owning modules; interaction coverage
for screen input remains in focused widget-event checks during R9. The CMake
dependency audit exception now points to R9, where the legacy include is to be
removed after its assertions have an owner.

Event priority remains the existing widget order: gizmo handling precedes
world-point validation; duplicate, grab, and Subdivision modes precede ordinary
tool input; command-specific right-click completion/cancellation precedes pan;
pan precedes generic active-tool input; transform, Join, Trim/Erase, Select,
and creation fallbacks follow in that order. Keyboard priority is active modal
commands (Join/Subdivision and transform input), then selection/grab shortcuts,
then Arc and generic tool handlers, then selection/delete and widget fallback.
Wheel priority is Subdivision, Polygon side control, then camera zoom. This
records the current behavior for later extraction; it does not change it.

Validation: `git diff --check` and `cmake --build build --parallel 4` passed;
the dependency audit checked 225 source files and all application/test
executable targets compiled. Tests and offscreen startup were not run. The
viewport class remains defined in its `.cpp`; no broad test-only accessor or
speculative header was added. Remaining R7 work is subsequent fixture
migration and behavior verification.

#### R8 Arc ownership checkpoint (2026-10-04)

`ArcTool` now owns the Arc mode plus the persistent interaction-state record
for captured workplanes, chord points, plane/axis constraints, perpendicular
mode, vertical hysteresis, numeric input, and winding preview. Its tool module
also owns angle snap, one-point sweep unwrapping, and the planar endpoint
constraints for one- and two-point arcs. `ViewportWidget` currently adapts its
legacy event flow to that state and calls the extracted algorithms; HUD/GPU
presentation, scene snap acquisition, and stage/commit routing remain in the
widget for the next Arc subphase. Exact rational construction still uses the
existing arc curve factory.

Validation: `cmake --build build --parallel 4` passed and the dependency audit
checked 211 source files. `git diff --check` passed in the build checkpoint.
Tests were not executed and an offscreen startup smoke was not run. The first
build attempt caught and corrected an extra ArcMode argument at the GPU overlay
call site; the subsequent full build passed. Remaining R8 work begins by moving
the Arc stage/event transitions and typed input behind ArcTool/ToolContext,
then moving Rotate/Mirror, Scale, and Subdivision/interactive Join ownership.

#### R8 Join/Subdivision state checkpoint (2026-10-04)

`JoinTool` now privately owns activation and its selected ObjectIds, with begin,
cancel, completion, add, and query operations. `SubdivisionTool` privately owns
its target ID, section count, and angle/pixel wheel accumulators; it implements
begin/finish, detent normalization, and bounded section adjustment. The
viewport delegates these state transitions and queries while retaining scene
hit testing, Join topology planning/command invocation, subdivision geometry
sampling, and Qt routing. `tests/trim_seam.cpp` now checks the JoinTool state
instead of the removed viewport field. CMake already grouped the new tool
implementations in `CLASSICAD_TOOL_SOURCES`.

Validation: `git diff --check` passed. The first all-target build exposed the
Trim fixture's two references to the removed `joinActive_` field; those checks
were migrated to `JoinTool::isActive()`. The subsequent
`cmake --build build --parallel 4` passed, including compilation of the legacy
Trim fixture, and the dependency audit checked 217 source files. Tests were not
executed; no offscreen startup smoke was run. Remaining R8 work is to move the
Arc stages and event decisions, then give Rotate/Mirror/Scale real interaction
ownership and complete the Join/Subdivision command boundary.

#### R8 Transform/prompt and Arc frame checkpoint (2026-10-04)

`MirrorTool` now privately owns its selected ObjectIds and two-point axis stage;
`MirrorCommand` creates reflected copies in a document transaction. `ScaleTool`
owns its point stages, preview/factor calculations, typed-factor editing, and
prompt. `RotateTool` owns angular preview and snap calculations, typed-angle
calculation, and full interaction reset; the viewport no longer repeats manual
field clearing. `JoinTool` and `SubdivisionTool` produce their selection prompts
from their own state. `ArcTool` now owns its input reset and first-point
workplane capture alongside its existing preview solvers. Qt event routing,
most stage transitions, viewport preview drawing, and final transform commits
remain in the adapter.

Validation: `git diff --check` passed. `cmake --build build --parallel 4`
passed for the application and all test executable targets; the dependency
audit checked 219 source files. Tests and offscreen startup were not run. The
build reports existing `Shape` aggregate missing-initializer warnings from
viewport/test call sites. Next, move Arc's numeric-value application and stage
transitions behind its tool API, then finish Rotate/Scale/Join/Subdivision
command and input ownership before beginning R9.

#### R8 Arc numeric-input checkpoint (2026-10-04)

`ArcTool` owns numeric-input mode and buffer operations. `ViewportWidget`
delegates begin, append, backspace, clear, and buffer reads; the Arc HUD reads
the tool API as well. Key handlers retain allowed-character checks, while
document-unit parsing and numeric preview application initially remained in
the widget.

Validation: `git diff --check` passed and `cmake --build build --parallel 4`
passed; the dependency audit checked 219 source files. Tests and offscreen
startup were not run.

#### R8 Arc numeric-preview calculation checkpoint (2026-10-04)

`ArcTool` now computes radius-based start points, one-point sweep-angle input,
and two-point sagitta positions. It updates the preview angle tracking and
returns cursor/point values; the viewport applies those results to the legacy
pending-point, cursor, and snap presentation. Plane-lock and angle-snap toggles
also now mutate state through `ArcTool`. Document-unit parsing and the two-point
chord-length workflow (which also changes the captured input frame) remain view
responsibilities.

Validation: `git diff --check` passed and `cmake --build build --parallel 4`
passed for the application and test executable targets; the dependency audit
checked 219 source files. Tests and offscreen startup were not run. Next: move
Arc chord-length direction solving and its state updates behind ArcTool, then
move key stage decisions and commit intent behind the tool interface.

#### R8 Arc chord endpoint checkpoint (2026-10-04)

`ArcTool::resolveChordLengthEndpoint` now computes the fixed-distance endpoint
in world space, applies an optional world-axis constraint, and updates its
second-point/chord state. The viewport supplies the current cursor's world
position and maps the current X/Y/Z key to an axis vector, then retains the
workplane update and local pending-point synchronization. This keeps the
viewport-dependent view-forward/perpendicular-plane rule in the view while
moving the chord direction and distance solver into the tool.

Validation: `git diff --check` passed and `cmake --build build --parallel 4`
passed; the dependency audit checked 219 source files. Tests and offscreen
startup were not run. Next: move remaining Arc stage decisions, plane/frame
state mutations, and commit intent behind tool methods; then continue Rotate,
Scale, interactive Join/Subdivision, R7 event-result boundaries, and R9–R12.

#### R8 Shared Join curve primitives checkpoint (2026-10-04)

Added `nurbsCurveEndpoints` to `core/geometry/curve_evaluator.*` and
`reverseNurbsCurve` to `core/geometry/curve_editing.*`. The Join command's
duplicate endpoint helper was removed, and the viewport now delegates its
legacy endpoint method to the evaluator. Join component reversal now delegates
to the exact geometry editing function, which preserves weights, knots, and
the source parameter domain. This is the first step in extracting Join's
remaining order/fuse/gap algorithms; viewport-derived screen tolerance stays
in the view adapter and is passed as a value.

Validation: the first build found one stale `curveEndpoints` call, which was
updated to the shared API. The subsequent `git diff --check` and full
`cmake --build build --parallel 4` passed; the dependency audit checked 219
source files. Tests and offscreen startup were not run. Next: move the pure
curve ordering, overlap fusion, and gap validation into a named geometry
module while keeping workplane and screen tolerance inputs explicit.

#### R8 Planar Join ordering checkpoint (2026-10-04)

Added `core/geometry/curve_join.*` and registered it with the geometry object
module. `orderConnectedNurbsCurves` now owns the planar connected-component
search and delegates exact component reversal to `curve_editing.*`. The
viewport passes its existing world tolerance and keeps the helper as a thin
adapter, so Join selection grouping and Join commit planning use the same
ordering implementation. Mixed-workplane world ordering, line-overlap fusion,
and tolerance-based gap adjustment remain in the viewport.

Validation: `git diff --check` passed. CMake regenerated the build, and
`cmake --build build --parallel 4` passed for the application and test targets;
the dependency audit checked 221 source files. Tests and offscreen startup
were not run. Next: extract mixed-plane ordering and continuity/gap rules, then
move overlap fusion and the remaining R8 Arc/transform commit boundaries.

#### R8 Join topology geometry checkpoint (2026-10-04)

Moved mixed-workplane ordering, planar/world continuity checks, tolerance-based
gap adjustment, and degree-1 line overlap fusion from `ViewportWidget` into
`core/geometry/curve_join.*`. The geometry API receives the existing
view-derived tolerance explicitly. Bent degree-1 curves are still split at
their original knot spans with domain-preserving trims; line fusion retains the
same collinearity, plane, overlap, and gap tolerances. The viewport now keeps
only thin delegates and reports aggregate merge diagnostics.

Validation: `git diff --check` passed and the full
`cmake --build build --parallel 4` passed for the application and all test
executable targets; the dependency audit checked 221 source files. The first
build caught one stale knot-vector API name, corrected before the passing build.
Tests and offscreen startup were not run. Next: move Arc's stage/input decisions
and its document-unit conversion boundary into the tool, then finish the
remaining transform/Join commit orchestration.

#### R8 Arc key and numeric input checkpoint (2026-10-04)

`ArcTool` now chooses the one/two/three-point keyboard action, selects numeric
input mode from the Arc stage, filters/accumulates typed characters, parses
document-unit lengths, and applies radius, angle, chord-length, and sagitta
input calculations. The viewport delegates Qt key actions and applies returned
point/cursor updates; it retains screen/camera-dependent plane changes and the
final commit presentation. The duplicated mode-specific key branches and the
viewport-side Arc length parser calls were removed.

Validation: `git diff --check` and `cmake --build build --parallel 4` passed;
the dependency audit checked 221 source files and all application/test targets
compiled. Tests and offscreen startup were not run. Next: move Arc endpoint
input stage and commit construction into the tool, including removal of the
viewport's Arc click-state exception, then continue transform/Join command
ownership and the R7 event boundary.

#### R8 Arc commit checkpoint (2026-10-04)

`ArcTool::commitAt` now owns completion geometry for all three Arc modes. It
updates one-point sweep tracking, applies the existing quarter-turn or sagitta
constraints, calls the shared shape factory, and commits through ToolContext's
transaction port before finishing the tool. The viewport's three completion
methods are now adapters that pass the cursor/snap state and report the result.

Validation: `git diff --check` passed and the full
`cmake --build build --parallel 4` passed after adding the explicit
`ViewportTransform` implementation include required by the tool. The dependency
audit checked 221 source files and application/test targets compiled. Tests and
offscreen startup were not run. Next: move Arc click-stage/input ownership into
the tool so the viewport no longer keeps a separate Arc point list, then
continue transform/Join commit coordination and the R7 event boundary.

#### R8 Arc staged-point ownership checkpoint (2026-10-04)

`ArcTool` now owns the canonical staged local points. First-point capture,
mouse point placement, numeric radius input, camera-driven reprojecting, and
perpendicular-plane changes all update that state through the tool API. The
viewport's `pendingPoints_` is now a derived mirror used by shared preview and
constraint presentation. Arc commit paths consume the tool-owned points, and
the generic viewport creation commit branch no longer handles Arc. Arc Escape
also clears the canonical point list. Corrected the radius text-input path so
the point appended by `applyRadiusInput` is not appended again by its caller.
Mouse click sequencing and camera-specific frame construction remain in the
viewport and are the next Arc extraction boundary.

Validation: `git diff --check` and the full
`cmake --build build --parallel 4` passed; the dependency audit checked 221
source files and all application/test executable targets compiled. Existing
`Shape` aggregate missing-initializer warnings remain. Tests and offscreen
startup were not run. Next: extract the remaining Arc click-stage decisions
from `ViewportWidget`, then continue transform and Join commit ownership.

#### R8 Arc click-stage checkpoint (2026-10-04)

Added `ArcTool::inputStage` and `ArcTool::handleClick`. The tool now decides
whether a click captures the first point, captures the radius/chord point, or
completes the arc. It owns right-click completion/cancellation and all point
state changes; the three duplicate mouse completion branches and viewport
click-stage mutation code are removed. The viewport still prepares the point
using its camera ray and runs the camera-specific two/three-point chord-frame
construction/reprojection after the tool accepts the world chord point.

Validation: `git diff --check` and
`cmake --build build --parallel 4` passed; the dependency audit checked 221
source files and all application/test executable targets compiled. Existing
`Shape` aggregate missing-initializer warnings remain. Tests and offscreen
startup were not run. Next: migrate Arc axis/perpendicular/plane state
transitions and projection-dependent frame math to ArcTool/ViewportTransform,
then continue Rotate/Scale/Join ownership.

#### R8 Arc axis and chord-frame checkpoint (2026-10-04)

Moved Arc's X/Y/Z state transitions into `ArcTool::handleAxisKey` and moved
two/three-point chord workplane construction, endpoint remapping, and the
0.995/0.98 vertical hysteresis into `ArcTool::updateChordWorkPlaneForView`.
The tool reads camera direction and projection through the existing
`ViewportTransform`; the viewport now delegates and mirrors accepted points.
The view still performs screen-ray cursor acquisition and cursor refresh.
One-point/two-point perpendicular toggle calculations remain in the viewport.

Validation: the first build caught a missing local vector-cross helper in the
new tool implementation; after adding it, `git diff --check` and the full
`cmake --build build --parallel 4` passed. The dependency audit checked 221
source files and all application/test executable targets compiled. Existing
`Shape` aggregate missing-initializer warnings remain. Tests and offscreen
startup were not run. Next: move the remaining Arc perpendicular-plane state
and frame transitions into ArcTool, then continue Rotate/Scale/Join ownership.

#### R8 Arc perpendicular-plane checkpoint (2026-10-04)

Moved one-point perpendicular-plane orientation/reprojection, two-point
perpendicular-plane entry/exit, three-point chord-plane toggling, and completed
vertical-chord X/Y override transitions into `ArcTool::togglePerpendicularPlane`.
The tool also rebases one-point sweep preview state after a frame change. The
viewport now refreshes the camera cursor and mirrors updated local points.

Validation: `git diff --check` and the full
`cmake --build build --parallel 4` passed; the dependency audit checked 221
source files and all application/test executable targets compiled. Existing
`Shape` aggregate missing-initializer warnings remain. Tests and offscreen
startup were not run. Next: extract Arc endpoint axis inference, world-axis
constraint solving, and chord-length stage completion; then continue
Rotate/Scale/Join ownership.

#### R8 Arc endpoint and chord-length checkpoint (2026-10-04)

Moved projected world-axis inference, snap projection onto constrained axes,
screen-ray intersection, resolved endpoint tracking, workplane remapping, and
chord-length stage completion into `ArcTool`. The tool now uses the shared
`InputConstraintService`; the viewport passes the raw cursor, screen position,
viewport size, modifiers, and existing `SnapResult`, then applies the returned
cursor/snap presentation state. Arc input-stage checks in the cursor constraint
router and plane-lock adapter now use `ArcTool::inputStage`. Removed the now
unused viewport-local axis and vector-add helpers.

Validation: `git diff --check` passed and the full
`cmake --build build --parallel 4` passed. The CMake dependency audit checked
221 source files; the application and all test executable targets compiled.
Existing `Shape` aggregate missing-initializer warnings remain. Tests and
offscreen startup were not run. No timing comparison was made. Next: finish
Rotate/Mirror/Scale stage and commit coordination and interactive Join
ownership, then close the remaining R7 event boundary before R9.

#### R8 transform stages and Join input checkpoint (2026-10-04)

`RotateTool::acceptPoint` now owns pivot capture, reference capture, final
angle calculation, and the decision to request commit. It receives translated
`ToolInput`, applies the existing angular snap and typed-angle state, and
returns the resulting cursor/frame/angle values for the viewport adapter.
`ScaleTool::acceptPoint` now returns explicit base/reference/commit actions
with the factor and axis direction. `JoinTool` now validates clicked targets,
reports duplicate selections, decides when the second curve completes the
selection, and maps Enter/Escape to complete/cancel actions. The viewport keeps
scene hit testing and applies the existing `TransformCommand`, `MirrorCommand`,
and `JoinCommand` document transactions. A `QPointF` Rotate adapter remains
for the legacy Trim fixture's direct helper calls.

Validation: the first build found `RotateTool` needed the complete
`ViewportTransform` definition; after including it, the Trim fixture exposed
its old `handleRotatePoint(QPointF)` call, so a thin compatibility adapter was
retained. `git diff --check` and the full `cmake --build build --parallel 4`
then passed. The CMake dependency audit checked 221 source files. Existing
`Shape` aggregate missing-initializer warnings remain. Tests and offscreen
startup were not run. Next: move Rotate key and plane transitions into the
tool, then finish transform commit coordination and the R7 event boundary.

#### R8 Rotate keyboard and plane ownership checkpoint (2026-10-04)

`RotateTool` now owns XYZ axis-plane toggles, the pre-pivot perpendicular
plane, later perpendicular-plane switching, and Rotate's key decisions for
commit, typed angle entry/editing, snap toggling, and plane changes. It returns
cursor/frame changes and commit intent through `RotateKeyResult` and
`RotateFrameResult`; `ViewportWidget` translates `QKeyEvent`, applies cursor
and snap presentation updates, and invokes the existing `TransformCommand`
path when commit is requested. Backspace with an emptied angle field resumes
the numeric preview at the last pointer position.

Validation: `git diff --check` passed and the full
`cmake --build build --parallel 4` passed. The CMake dependency audit checked
221 source files; all application and test executable targets compiled.
Existing `Shape` aggregate missing-initializer warnings remain. Tests and
offscreen startup were not run. No timing comparison was made. Remaining R8
work includes closing the tool/view state boundary. Then close R7 event-result
and legacy fixture migrations before R9.

#### R8 transform, Join, and Subdivision ownership checkpoint (2026-10-04)

`ToolContext` now provides transaction commit and layer/selection notification
ports. Rotate and Scale execute their transform commands through that boundary
and retain associative-dimension updates. Mirror owns its command transaction,
selection result, and completion. `JoinTool::executeJoin` now owns selected
curve collection, workplane mapping, ordering/fusion/gap checks, command plan,
transaction, result selection, and completion; the viewport supplies the
view-derived endpoint tolerance and presents status/diagnostics. Join geometry
remains in `curve_join.*`. `SubdivisionTool` owns preview parameter caching,
input decisions, command transaction, and completion; equal arc-length
parameter calculation is in `core/geometry/curve_subdivision.*`, while the
viewport supplies the selection and section count and draws the preview.
`classicad_core_contracts_test` now links the command object module required by
the tool implementations. Thin Join and Rotate adapters remain for direct
legacy fixture calls.

Validation: `git diff --check` and the full `cmake --build build --parallel 4`
passed; the CMake dependency audit checked 223 source files and application
and test executable targets compiled. Existing `Shape` aggregate
missing-initializer warnings remain. Tests and offscreen startup were not run.
No performance comparison was made. Remaining R8 work is adapter/state
boundary cleanup and behavior verification. R7 event-result and legacy fixture
migrations remain before R9.

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

ArcTool owns one/two/three-point stages, typed radius/angle input, winding,
perpendicular/axis/plane locks, vertical hysteresis, completion, and canonical
preview tracking. Preserve the existing exact arc factory and add-on interaction
thresholds. Output tool preview values for HUD/compass presentation rather than
copying viewport rendering into the tool. `pendingPoints_` is now only a derived
presentation mirror for shared legacy render/constraint code; remove that
special case when those consumers use the typed preview directly.

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

#### R9 geometry extraction checkpoint (2026-10-04)

Added `core/geometry/curve_erase_intervals.*` for the pure operation that
splits erase hits at curve intersections, merges selected parameter pieces,
and keeps closed-curve seams connected unless the seam is an intersection.
Added `core/geometry/curve_intersections.*` for planar span-hull filtering,
iterative curve/curve intersection refinement, and closest curve-to-point
search. The viewport still gathers visible scene candidates, applies frame and
point visibility rules, and maps geometry hits to object IDs. The seam fixture
now calls the geometry interval API directly for its intersection-bounded
interval assertion.

TrimTool now owns its box selection and hover/component state, candidate IDs,
single-point interval-preview path, nearest-target tie rule, and box-candidate
gathering. EraseTool owns stroke start/move/finish/cancel state, cursor state,
the screen path, candidate IDs, and the sampled-radius candidate accumulation.
Viewport callbacks still perform camera-dependent curve distance and box-hit
queries. The viewport no longer stores duplicate Trim/Erase lifecycle fields;
the fixture now inspects the owning tool state for its existing assertions.

Validation: `git diff --check` passed, the dependency audit checked 229 source
files, and `cmake --build build --parallel 4` passed. The direct-include
Trim fixture compiled against the tool APIs. Tests and offscreen startup were
not run. Next: extract exact shape rebuilding and route candidate commits
through a document command.

#### R9 exact curve editing checkpoint (2026-10-04)

Moved parameter-complement rebuilding into
`core/geometry/curve_erase_intervals.*`; it uses the existing exact rational
`trimNurbsCurve` operation and preserves the source parameter domain. Moved
endpoint-connected fragment grouping into `core/geometry/curve_join.*`, with
the existing camera-derived ordering tolerance passed as a value. Trim's
viewport adapter now asks those geometry owners to split and group the kept
NURBS fragments.

Validation: `git diff --check` passed; the dependency audit checked 229 source
files; and `cmake --build build --parallel 4` passed. Tests and offscreen
startup were not run. Next: move the candidate replacement transaction to a
document command and continue moving screen-dependent curve/eraser queries to
a service.

#### R9 document command checkpoint (2026-10-04)

Added `core/commands/trim_erase_command.*`. The viewport now passes stable
source ObjectIds, source indices, and exact replacement pieces to the command;
the command applies removals, first-piece geometry replacements, and extra
fragment insertions in descending source-index order inside one existing
`DocumentTransaction`. Selection cleanup and timing/presentation logs remain in
the viewport adapter.

Validation: `git diff --check` passed; the dependency audit checked 231 source
files; and the full CMake build passed. Tests and offscreen startup were not
run. Next: extract the screen-dependent curve proximity and eraser interval
queries to a service.

#### R9 screen-query service checkpoint (2026-10-04)

Added `services/erase/curve_erase_query.*` for screen-space nearest-distance
queries, eraser-stroke parameter intervals, and curve intervals inside a screen
box. The viewport now supplies `ViewportTransform` and viewport size explicitly
and retains scene candidate gathering, visibility checks, and sampled-curve
caching. Moved shape-to-NURBS component extraction to
`core/geometry/shape_mapping.*`. `curve_erase_intervals.*` now owns exact shape
rebuilding from kept fragments. Trim rebuild carries each component's workplane
frame through splitting, uses world-space endpoint grouping, and persists
per-component frames on PolyCurve results. This preserves mixed-plane geometry.
The erase query service now owns curve-intersection parameter collection and
point-contact search after the viewport supplies visible curve and point
candidates. Erase cache intersection references use stable `ObjectId` values.
Removed the duplicate screen-space geometry implementations and the unused point-to-stroke
helper from `ViewportWidget`. Migrated the fixture's direct eraser-interval
calculation to the service. The fixture still includes
`viewport_widget.cpp` and uses private-access macros because it also contains
R7/R8 event and tool ownership assertions that have not yet moved to their
owning contracts.

Validation: `git diff --check` passed; the dependency audit checked 233 source
files; and `cmake --build build --parallel 4` passed. Tests and offscreen
startup were not run. Next: migrate the legacy fixture assertions into owning
geometry/tool/viewport contracts, remove its implementation include and access
macros, then finish R7/R8 ownership and verification before starting R10.

#### R7/R9 selection-box query checkpoint (2026-10-04)

Added `services/hit_testing/selection_box_query.*` for sampled NURBS and point
window/crossing checks, including camera-clipped samples, component workplane
frames, the crossing tolerance, and projected-segment clipping. The viewport
delegates those geometry hits and keeps its projected-bounds fallback for
pictures, dimensions, and surfaces. The existing clipped-curve box assertions
now call the service directly; the visible-object selection assertion uses
`SelectTool`, `SelectionModel`, and a `Document`/`ToolContext` fixture.

Validation: `git diff --check` passed, the dependency audit checked 235 source
files, and `cmake --build build --parallel 4` passed. No tests or startup smoke
were run. The fixture still includes `viewport_widget.cpp` for its other
selection, Join, Trim/Erase, transform, and drag cases. Next: migrate those
assertions to geometry/services, tool/command contracts, or public viewport
events without dropping coverage; then remove the direct include and macros.

#### R7/R8 tool and transform fixture checkpoint (2026-10-04)

Added a world-plane-aware translation operation to
`core/geometry/geometry_transform.*`; ViewportWidget now delegates its shared
shape-translation step to that implementation. The fixture's Join overlap,
reversal, mixed-workplane continuity, result selection, hit-test, JSON round
trip, and translation assertions now use `JoinTool`, `ToolContext`, the
document command path, geometry/services, and the geometry codec directly.
The update-session projection round trip now exercises `SessionSerializer`
and `ViewportTransform` directly. These cases no longer need viewport private
fields. The body-drag interaction still needs migration to public viewport
events.

Validation: `git diff --check` passed, the dependency audit checked 235 source
files, and `cmake --build build --parallel 4` passed. No tests or startup smoke
were run. The fixture still contains direct widget access for the remaining
Trim/Erase lifecycle, Join body drag, Explode/transform interactions, snapping,
and grab/selection routing. Next: move exact Trim/Erase assertions to their
geometry/query/command contracts and move interaction cases to the public
viewport event API, then remove the implementation include and macros.

#### R7/R8/R9 direct-contract fixture checkpoint (2026-10-04)

Moved the remaining geometry-focused `trim_seam.cpp` cases off the
`ViewportWidget` implementation and onto the owning contracts. The fixture
now covers seam-independent circle trimming, tangent-only intersections,
intersection-bounded line cuts, untouched PolyCurve components, camera-aware
screen queries, atomic Trim/Erase replacement and Undo, Explode command plans,
Rotate/Scale/Mirror tool commits, selection-box queries, Join topology, and
session projection restoration. Removed the `viewport_widget.cpp` inclusion
and the private/protected access macros from the fixture. Removed the
corresponding test-only allowance from `cmake/check_dependencies.py`.

The refactor build passed after the fixture rewrite. The former Trim preview
refresh and joined/drawn curve body-drag checks now use `ViewportWidgetApi`
mouse events in `tests/viewport_interaction.cpp`; that target compiled after
the migration. Transform key/axis routing and event precedence remain to be
covered through public events. `git diff --check` must be rerun. Tests and
offscreen startup were not run.

#### R10 immutable scene-frame and active-preview checkpoint (2026-10-04)

Added `ui/viewport/viewport_render_frame.*`. Each frame snapshots the camera,
viewport size, visible object geometry, stable IDs and geometry revisions,
layer styling, selection/highlight state, and any active Scale/Rotate geometry
preview. GPU scene rendering, the depth pass/picker, CPU committed-scene
drawing, and control-point presentation now use those shared entries. The
Scale/Rotate geometry transform is applied once per frame and reused across
those consumers. Visibility continues to exclude hidden and frozen layers;
selection still includes the primary selection and Join targets.

`ViewportWidget` now keeps one canonical active `ToolPreview` for the migrated
Line, point-construction, Extrude, and creation-tool previews, including shape,
guide, workplane, point, and HUD data. Removed the former per-tool copies for
those preview values. Qt cursor/snap updates and specialized preview-overlay
assembly remain in the widget for later R10 work.

Validation: `git diff --check` passed. `cmake --build build --parallel 4`
passed for the app, library object modules, and all test executable targets;
the dependency audit checked 239 source files. Existing `Shape` aggregate
missing-initializer warnings remain. Test executables and offscreen startup
were not run, and no runtime/build timing claim was measured. Next: route the
remaining CPU overlay/fallback work through the frame and move presentation
assembly behind the renderer/overlay contracts, then profile scene scans before
considering a broad-phase index.

#### R10 shared prepared-geometry checkpoint (2026-10-04)

Added `ui/viewport/viewport_geometry_cache.*`. The frame now retains immutable,
camera-independent world geometry per visible ObjectId and geometry revision.
GPU committed curve/point strokes reuse the frame's prepared vertices, and the
GPU depth pass assembles its combined arrays from those same values. This
removes duplicate NURBS curve sampling between those consumers. Surface
tessellation still uses the shared `SurfaceTessellationCache`; picture alpha
masks are included in the prepared per-object geometry. Scale/Rotate preview
objects remain transient because they do not match their source revision.
The scene renderer no longer owns a second per-stroke world-sample cache; its
combined GPU buffer still rebuilds when the visible stroke key sequence
changes. CPU projection remains screen-dependent and separate.

Validation: `git diff --check` passed. `cmake --build build --parallel 4`
passed for the application and all test executable targets; the dependency
audit checked 241 sources. Existing `Shape` aggregate missing-initializer
warnings remain. Test executables and offscreen startup were not run, and no
runtime/build timing claim was measured. Next: move specialized preview and
overlay assembly behind rendering contracts, then assess shared bounds and
scene-query work.

#### R10 committed-stroke presentation checkpoint (2026-10-04)

Moved native-stroke eligibility, curve validation, layer linetype conversion,
highlight styling, control-guide setup, and prepared-geometry assignment into
`makeViewportSceneStrokes` in `ui/viewport/viewport_scene_renderer.*`. The
widget now handles picture-specific drawing and fallback routing around that
contract instead of constructing GPU line styles itself. Existing selected,
transformed, and unsupported-linetype fallbacks retain their prior branches.

Validation: `git diff --check` passed. `cmake --build build --parallel 4`
passed for the application and all test executable targets; the dependency
audit checked 241 sources. Test executables and offscreen startup were not run.
Next: move the remaining specialized preview/overlay presentation decisions
behind renderer contracts, then assess shared bounds and scene-query work.

#### R11 Tool Shelf extraction checkpoint (2026-10-04)

Added `ui/panels/tool_shelf.*` and moved the tool-button layout, exclusive
selection group, custom wheel scrolling, control-point toggle widget, action
buttons, and help label into `ToolShelf`. The shelf exposes ToolId keyed button
access and invokes typed callbacks for tool requests, popup-menu setup, and
secondary actions. MainWindow supplies those callbacks and keeps specialized
tool menus and application commands at the composition boundary. Button text,
tooltips, order, grouping, click behavior, wheel scrolling, and preferences
updates were carried across unchanged. Existing button pointers in MainWindow
remain short-lived access aliases for menu and shortcut state updates.

Validation: `git diff --check` passed. `cmake --build build --parallel 4`
passed for the application; the dependency audit checked 243 sources. Test
executables and offscreen startup were not run. Next: extract the layer panel
behind `ViewportWidgetApi` callbacks, then move preferences UI toward a
dedicated dialog module.

#### R11 Layers panel extraction checkpoint (2026-10-04)

Added `ui/panels/layers_panel.*` and moved the right-side layer table, filter,
row selection, local enablement, and table controls into `LayersPanel`. Stable
`LayerId` values cross its callback boundary; MainWindow retains layer command
execution, status messages, and dialogs. Added `layer_style_widgets.*` for the
shared linetype combo, icon, description, and dropdown population used by both
the panel and the top layer-properties toolbar. The MainWindow-owned table
fields and row construction were removed.

Validation: `git diff --check` passed. `cmake --build build --parallel 4`
passed for the application and all test executable targets; the dependency
audit checked 247 sources. Test executables and offscreen startup were not run.
Next: extract preferences UI and continue the R10 overlay/presentation split.

#### R11 Preferences dialog extraction checkpoint (2026-10-04)

Moved preference page construction and widget state from `main_window.cpp` to
`ui/panels/preferences_dialog.*`. The dialog now accepts one typed
`PreferencesDialogValues` snapshot and returns the selected values through a
small function boundary. MainWindow still owns when the dialog opens and how
accepted settings are applied/persisted. Preference category order, defaults,
control object names, and settings application calls were preserved.

Validation: `git diff --check` passed. `cmake --build build --parallel 4`
passed for the application and all test executable targets; the dependency
audit checked 249 sources. Test executables and offscreen startup were not run.
Next: continue R10 presentation extraction, then address remaining R6–R9
behavior evidence, R11 test organization, and R12 measurement/enforcement.

#### R11 document-grid dialog extraction checkpoint (2026-10-04)

Moved the document-grid settings dialog to
`ui/panels/document_grid_dialog.*`. MainWindow now requests the dialog result
through a `DocumentSettings` value and retains viewport validation, command
application, and status reporting. The unit choices, spacing control,
defaults, and invalid-setting message are unchanged.

Validation: `git diff --check` passed. `cmake --build build --parallel 4`
passed for the application and all test executable targets; the dependency
audit checked 251 sources. Test executables and offscreen startup were not run.
Next: resume R10 preview/overlay ownership and keep the R6–R9 verification
gaps visible in the phase ledger.

#### R11 application stylesheet extraction checkpoint (2026-10-04)

Added `ui/theme/classicad_theme.*` and moved the application-wide Qt
stylesheet out of `MainWindow::applyTheme()`. MainWindow still applies it at
startup through `classicadThemeStyleSheet()`. The CSS content and selector
rules are preserved; this gives the global presentation resource a clear owner.

Validation: `git diff --check` passed. `cmake --build build --parallel 4`
passed for the application and all test executable targets; the dependency
audit checked 263 sources. Test executables and offscreen startup were not run.
Remaining: continue R10 erase/trim overlay and query ownership, organize the
existing test suites without changing assertions, then complete R12
measurements/enforcement.

#### R10 navigation gizmo extraction checkpoint (2026-10-04)

Added `ui/viewport/navigation_gizmo.*` and moved Blender-style axis/button
drawing plus gizmo hit-testing out of `ViewportOverlay`. The viewport owns the
gizmo instance for painting; `NavigationController` receives the gizmo
interface for action hit-testing and retains gesture, cursor, and camera action
routing. The previous `ViewportOverlay` methods remain as temporary forwarding
delegates for current contract callers. Their rendering and hit results come
from the same extracted implementation, while production paint and navigation
paths call `ViewportNavigationGizmo` directly.

Validation: `git diff --check` passed. `cmake --build build --parallel 4`
passed for the application and all test executable targets; the dependency
audit checked 253 sources. Tests and offscreen startup were not run. Remaining:
complete the rest of R10's overlay/query ownership, then remove the forwarding
methods after their callers migrate and behavior evidence permits it.

#### R10 tool-status HUD extraction checkpoint (2026-10-04)

Added `ui/viewport/viewport_hud_renderer.*` and moved active-tool status,
instruction text, and point-command HUD panel presentation out of
`ViewportOverlay`. The renderer receives a typed `ViewportHudState`; the
widget assembles that snapshot from existing tool/controller state. Kept the
same displayed text, branch order, fonts, colors, and panel placement. Moved
the shared rotate snap-label formatter with the HUD and reused it in rotate
preview presentation.

Validation: `git diff --check` passed. `cmake --build build --parallel 4`
passed for the application and all test executable targets; the dependency
audit checked 255 sources. Tests and offscreen startup were not run. Remaining:
split remaining preview/selection/snap overlay presentation and scene-query
work; the widget still assembles specialized previews.

#### R10 Arc/Rotate compass extraction checkpoint (2026-10-04)

Added `ui/viewport/viewport_arc_compass_renderer.*` and moved the workplane
projection, camera-scale calculation, color choice, compass ring, ticks,
protractor arcs, and center cross out of `ViewportOverlay`. Arc and Rotate
preview paths now call the dedicated renderer with the same center, frame,
rotation, and snap interval values. The 72-sample ring, 125-pixel projected
diameter, oblique-plane contrast, and perspective behavior are retained.

Validation: `cmake --build build --parallel 4` passed for the application and
all test executable targets; the dependency audit checked 257 sources.
`git diff --check` passed. Tests and offscreen startup were not run. Remaining:
split remaining preview/selection/snap presentation and scene-query work.

#### R10 snap-marker renderer extraction checkpoint (2026-10-04)

Added `ui/viewport/viewport_snap_marker_renderer.*` and moved snap glyph
shapes, color, and label visibility into that renderer. `ViewportOverlay`
retains a forwarding API and owns one renderer instance, so the UI label setting
and every existing tool/drag snap marker still share the same presentation
state and screen projection. Snap candidate calculation and snap result values
remain in the existing `SnapEngine`/snap contracts.

Validation: `cmake --build build --parallel 4` passed for the application and
all test executable targets; the dependency audit checked 259 sources.
`git diff --check` passed. Tests and offscreen startup were not run. Remaining:
complete the R10 overlay and query review, then profile scene scans before
considering a broad-phase structure.

#### R10 geometry tool-preview renderer extraction checkpoint (2026-10-04)

Added `ui/viewport/viewport_tool_preview_renderer.*` and moved Line/world
Line, Arc, Circle, Ellipse, Rectangle, Polygon, Point, and Rotate preview
drawing into that module. It receives the same cursor, points, frame, snap
result, and display preferences from the widget; its shared snap-marker
dependency delegates to the dedicated marker renderer. The widget now calls
the named preview renderer through the overlay composition boundary. Tool
stages and committed geometry remain outside the renderer.

Validation: `git diff --check` passed. `cmake --build build --parallel 4`
passed for the application and all test executable targets; the dependency
audit checked 261 sources. Tests and offscreen startup were not run. Remaining:
review erase/trim overlays and scene queries, then profile object scans.

#### R10 navigation-gizmo compatibility cleanup checkpoint (2026-10-04)

Updated `tests/core_contracts.cpp` to exercise `ViewportNavigationGizmo`
directly, then removed the obsolete navigation draw/hit forwarding methods
from `ViewportOverlay`. Production navigation already used the extracted
module; this removes the last test-only dependency on the old overlay API.
`ViewportOverlay` still coordinates one `ViewportSnapMarkerRenderer` instance
because both its preview composition and the widget use the shared label
setting.

Validation: `git diff --check` passed. `cmake --build build --parallel 4`
passed for the application and all test executable targets; the dependency
audit checked 261 sources. The test executables were not run. Remaining: finish
R10 erase/trim overlay and query work.

#### R10 projected shape-bounds query extraction checkpoint (2026-10-04)

Moved the projected control-hull and surface-control-point bounds calculation
from `ViewportWidget` into `queryProjectedShapeBounds` in
`services/hit_testing/selection_box_query.*`. The selection fallback and
Scale's selected-object bounds center now use the same service result. It keeps
source-point inclusion for legacy/malformed curves, exact unpadded projected
bounds, camera clipping, and the existing surface control-net bounds.

Validation: `git diff --check` passed. `cmake --build build --parallel 4`
passed for the application and all test executable targets; the dependency
audit checked 261 sources. Tests and offscreen startup were not run. Remaining:
review whether camera-independent bounds are needed across consumers; don't add
a broad-phase index until scene scans are profiled.

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
| R0 Baseline | Complete | Recorded compiler/configuration, clean/warm/no-op/edit/link timings, CTest baseline failures, and startup behavior in `BUILD_BASELINE.md` and `PERFORMANCE_BASELINE.md` |
| R1 Shared compilation | Complete | Production objects compile once across compatible app/test targets; Trim's direct widget include and resource/link ownership are documented |
| R2 Contracts/dependencies | Complete | Owning contracts and implementations are split; target groups and default dependency audit match the source layers; build, audit, diff check, smoke, and tool-edit timing recorded |
| R3 Application/project state | Complete | Session owns document/selection/history/tool registry; session codec and project/update controllers are extracted; current and legacy session parsing is retained and candidate-based; build and startup smoke passed |
| R4 Edits/revisions/identity | Complete | Writable document access is restricted to transactions or tracked mutation callbacks; Trim/Erase and live geometry updates report invalidation; ObjectId indexing and restore epochs are maintained |
| R5a Shared geometry correctness | Complete | Exact curve edits and shared transforms are extracted; renderer, picking, depth, and Blender proxy now consume one trim-aware tessellation |
| R5b Evaluation/cache performance | Complete | Prepared evaluator and revision-keyed surface mesh cache are shared by drawing, hit testing, and depth; benchmark results are recorded above |
| R6 Commands | Complete | `DeleteCommand`, `DuplicateCommand`, `FillCommand`, `ExplodeCommand`, `JoinCommand`, layer, transform, subdivision, and Trim/Erase commands own document edits behind transactions. New core-only `tests/command_contracts.cpp` directly covers Delete, Duplicate, Fill, Explode, and Join plans/commits, Undo, layer preservation, locked or invalid targets, and no-op history behavior. The focused CTest registration passes. |
| R7 Selection/input/navigation | Complete | Selection/input/navigation state owners, one-pass event translation, typed handled/unhandled results, `selection_box_query.*`, and public viewport interaction coverage are in place. `trim_seam.cpp` has no widget access. The event-precedence suite confirms modal Join/Duplicate retain input from Select All, Fill, and Delete; idle Select All still works, and Arc Escape returns keyboard routing to Select. Full ten-suite CTest passes. |
| R8 Tool ownership | Complete | ArcTool owns staged input, constraints, numeric input, preview, completion, and Escape cancellation. Rotate owns typed key decisions, angle-snap settings, accepted stages, and transform commit; Scale owns typed click/key stage transitions, selection-center acceptance, Escape/right-click reset, and direct commits. Mirror owns axis completion and cancellation. JoinTool owns curve planning/transaction; SubdivisionTool owns preview parameters and its transaction. `geometry_transform.*` owns world-plane-aware transforms. Direct Arc/Rotate/Scale/Mirror contracts and the public interaction suite pass in the complete ten-suite run. Viewport code retains view-dependent input preparation and presentation adaptation. |
| R9 Trim/Erase | Complete | `curve_erase_intervals.*` owns intersection-bounded removal, exact complement splitting, and frame-preserving fragment rebuilding. `shape_mapping.*` extracts NURBS components with their workplane frames. `curve_intersections.*` owns refined planar intersections and tangent-contact deduplication; `services/erase/curve_erase_query.*` owns scene-candidate intersections, endpoint proximity, box, and stroke queries; `services/erase/trim_erase_query.*` owns complete screen-input replacement calculation. `curve_join.*` groups fragments in world space. TrimTool owns box/hover candidate decisions; EraseTool owns stroke state and candidate accumulation. `TrimEraseCommand` owns atomic object replacement/removal/insertion. The fixture has no widget implementation include or access macros. Tangent-only contacts and small view-scaled endpoint gaps pass; the full ten-suite run passes. |
| R10 Rendering/queries | Complete | CPU `ViewportRenderer::drawShape` consumes the frame's cached double-precision world samples, with the existing adaptive renderer retained for clipped/unsupported cases. Projected positive-weight control-hull bounds conservatively filter curve hit, selection-box, and near-snap work. Three-scene CPU/native-GL redraw, snap/pick, and per-object invalidation results are in `PERFORMANCE_BASELINE.md`; the ten-suite run and focused render/query checks pass. At 1,024 curves the synthetic CPU fallback remains about 96 ms versus 7 ms native GL, so the measurement records a remaining CPU-path cost rather than claiming parity. |
| R11 UI/tests | Complete | ToolShelf, LayersPanel, shared layer styles, PreferencesDialog, document-grid dialog, preference storage, and theme are separate modules. Focused command, Arc tool, scene-query, render, event, Trim/Erase, serialization, and viewport interaction suites preserve public behavior. `rg` confirms tests include no production `.cpp` implementation and use no private-access macros. The complete ten-suite CTest run passes in 266.52 s. |
| R12 Measurement/enforcement | Complete | Matched Release/GCC 13.3/Qt 6.4.2/four-job warm build results are in `BUILD_BASELINE.md`; the all-target warm source build fell from about 286.5 s at R0 to 127.96 s, while app-only clean and several incremental cases stayed level or regressed. The current 10-suite CTest run passes, the 274-source dependency audit passes, and the offscreen startup reaches viewport construction. Snap/pick, CPU/native-GL redraw, cache invalidation, and snapshot history measurements are in `PERFORMANCE_BASELINE.md`. The `app-dev`, `full-test`, and `benchmarks` presets configure in isolated directories. No ccache or Ninja is installed. |

For continuation, append a checkpoint record with: phase/subphase, source files
changed, behavior evidence, build/test/smoke outcomes, before/after measurements
where relevant, known issues, and the next actionable task. Preserve prior
records so the following chat can resume without reconstructing the work.
#### R11 typed preference store checkpoint (2026-10-04)

Added `ui/panels/preferences_store.*` and moved typed preference loading,
QSettings key reads/writes, snap-mode persistence, and shipped grid-palette
migration out of MainWindow. The window still applies loaded values to the
viewport and updates actions, controls, and status messages. Existing keys,
defaults, invalid-value fallbacks, palette migration rules, and sync timing are
preserved. MainWindow dropped about 220 lines across the extracted persistence
logic.

Validation: `git diff --check` passed. `cmake --build build --parallel 4`
passed for the application and all test executable targets; the dependency
audit checked 265 sources. Tests and offscreen startup were not run. Remaining:
continue R10 overlay/query review and R11 test organization.

#### R11 viewport render-contract suite split checkpoint (2026-10-04)

Moved the existing layer-line pattern and GPU stroke-style assertions from
`tests/core_contracts.cpp` into `tests/viewport_render_contracts.cpp`, with a
dedicated `classicad_viewport_render_contracts_test` CMake target. The 115
assertion lines and expected outcomes were carried over unchanged. This
separates presentation contracts from the remaining mixed core/tool suite.

Validation: `git diff --check` passed. `cmake --build build --parallel 4`
passed, including the new executable and the existing test targets; the
dependency audit checked 268 sources. Test executables and offscreen startup
were not run. Remaining: continue R11 suite organization, then update R12
measurements for the settled target graph.

#### R10 erase overlay renderer checkpoint (2026-10-04)

Added `ui/viewport/viewport_erase_overlay_renderer.*` for sampled erase
candidate intervals, the eraser cursor, and candidate-count label. The widget
supplies active tool/cursor/cache values directly, and `ViewportOverlay` no
longer owns erase rendering. The same colors, sample-to-parameter interpolation,
10-pixel cursor radius, dash style, and label placement are retained.

Validation: `git diff --check` passed. `cmake --build build --parallel 4`
passed for the application and all test executable targets; the dependency
audit checked 267 sources. Tests and offscreen startup were not run. Remaining:
finish trim-overlay ownership and scene-query review.

#### R12 warm-dependency clean-build measurement checkpoint (2026-10-04)

Removed only 180 generated project `.o` files under `build/CMakeFiles`, retained
the warm dependency tree and generated rules, then completed a clean all-target
build in 117.38 seconds at 597,380 KB peak RSS. The active CMake cache enables
both tests and benchmarks; the dependency audit checked 268 source files.
`BUILD_BASELINE.md` records the result and notes that the R1 clean-build
comparison predates the benchmark target, so it is not a matched measurement.
The build passed. Test executables and startup were not run. Remaining: collect
matched repeated build samples and runtime scene measurements, while continuing
R6–R11 review.

#### R12 repeated incremental-build checkpoint (2026-10-04)

On the settled 268-source Release/Unix Makefiles/GCC 13.3 graph with four jobs
and warm dependencies, ran three all-target no-op builds followed by three
all-target cycles that touched only the timestamp of
`src/tools/point_tool.cpp`. No source contents changed. No-op median was 3.53 s
and 85,556 KB peak RSS (ranges: 3.44–3.55 s, 85,496–85,596 KB). One-tool
rebuild median was 4.97 s and 201,920 KB (ranges: 4.95–4.98 s,
201,920–202,032 KB). These medians are stable across the three samples; versus
the single R1 samples, one-tool rebuild is 1.60 s faster and about 87 MB lower,
while no-op is 0.90 s slower. These measurements do not resolve clean-build
comparability or runtime performance.

Validation: all six CMake build invocations passed. They compile active test
targets but do not run them. `BUILD_BASELINE.md` now records the samples.
Remaining: matched clean-build comparisons, runtime scene measurements, and
R6–R11 ownership/behavior review.

#### R12 warm-dependency app-only build checkpoint (2026-10-04)

Removed only the 144 generated project object files referenced by the current
`classiCAD` link command; external dependencies, autogen sources, and source
files were retained. The app-only Release target rebuilt successfully in
103.01 seconds at 580,984 KB peak RSS. Its audit checked 268 sources. R1's
app-only sample was 104.05 seconds / 726,188 KB, but removed generated autogen
files as well, so the comparison is directional. The subsequent full all-target
build passed and relinked all active test/benchmark executables. No tests or
startup ran. `BUILD_BASELINE.md` records both timings and the caveat.

#### R10 Trim overlay ownership review checkpoint (2026-10-04)

Reviewed the remaining Trim box presentation. Select and Trim both call the
same generic dashed selection-rectangle drawing from `ViewportOverlay`; Trim
only changes how candidates are queried and previewed. No Trim-specific paint
responsibility remains to extract, so no extra renderer module was added. The
master architecture map now describes this shared owner and the already
extracted erase renderer accurately. Remaining R10 work is scene-query/cache
review and profiling before any spatial-index change.

Validation: read-only source review and `git diff --check` passed. No build,
test, or startup was run for this documentation-only checkpoint.

#### R9 reusable scene-intersection candidates checkpoint (2026-10-04)

Added `makeEraseIntersectionCandidates` to `services/erase/curve_erase_query.*`
to build visible curve and point intersection candidates from the document or
an existing sampled-scene cache. `prepareEraseGeometryCache` now creates that
candidate set once, then reuses it for each selected curve component instead
of rebuilding and copying the same vectors for every target. The uncached
Trim/Erase query route uses the same builder. Candidate order, visibility
filtering, stable object IDs, component frames, and the existing intersection
algorithm are retained.

Validation: `git diff --check` passed. `cmake --build build --parallel 4`
passed and the dependency audit checked 268 source files. Test executables and
startup were not run. Remaining: review runtime query cost before adding any
broad phase; R6–R11 behavior and ownership work remains.

#### R8 Mirror event-routing correction checkpoint (2026-10-04)

The generic active-tool mouse-press dispatch previously consumed Mirror's
second axis click after `MirrorTool::handleMousePress` staged it, so the
viewport adapter never called `MirrorTool::commitAxis`. The dispatcher now
leaves Mirror mouse presses to `handleMirrorPoint`, which stages each point,
commits the existing `MirrorCommand` transaction on the second point, and
retains viewport logging/presentation. No geometry or axis rules changed.

Validation: `git diff --check` and `cmake --build build --parallel 4` passed;
the dependency audit checked 268 sources. Test executables and startup were
not run, so public Mirror interaction behavior remains pending verification.

#### R12 isolated build-configuration checkpoint (2026-10-04)

Added root `CMakePresets.json` with separate `app-dev` (Release application,
tests and benchmarks disabled) and `full-test` (Release application and
regression-test targets enabled) configure/build presets plus a matching CTest
preset. README build guidance documents the commands and separate binary
directories. The existing `build/` configuration was not reconfigured. This
environment has neither `ccache` nor Ninja available, so no cache or generator
comparison was claimed.

Validation: `cmake --list-presets` parsed and listed both configure presets;
`git diff --check` passed. No build, test, or startup was run for this docs and
configuration-only checkpoint. Remaining: repeat matched build measurements,
measure runtime scenes, and continue R6–R11 review.

#### R8 translated Subdivision wheel input checkpoint (2026-10-04)

`SubdivisionTool::handleWheel` now accepts the shared `ToolInput`, owns wheel
delta accumulation, section-count adjustment, and preview refresh through its
`ToolContext`. `ViewportWidget::wheelEvent` translates the Qt event once,
retains existing event consumption, debug logging, and status notification,
and no longer mutates subdivision state through `applySubdivisionWheelSteps`.
Wheel thresholds, reversal handling, bounds, and preview calculation remain
unchanged.

Validation: `git diff --check` and `cmake --build build --parallel 4` passed;
the dependency audit checked 268 sources. Tests and startup were not run.
Remaining: event precedence and the other R7/R8 interaction boundaries still
need behavior review.

#### R9 erase-query candidate-reuse microbenchmark checkpoint (2026-10-04)

Added `benchmarks/erase_scene_query_benchmark.cpp` and the opt-in
`classicad_erase_scene_query_benchmark` target. It compares a single
visible-scene candidate build reused for selected curve queries with rebuilding
the same candidate arrays per target, while keeping the number of intersection
queries fixed. Median ratios on the synthetic degree-1 curve scenes were 1.81x
(16 curves/2 targets), 1.10x (64/8), and 1.17x (256/16); the largest fixture
saved about 0.66 ms. The result supports candidate reuse and does not justify a
spatial broad phase because exact intersection work dominates the larger case.
`PERFORMANCE_BASELINE.md` records fixture limits and measurements.

Added a separate `benchmarks` CMake preset that builds the two opt-in geometry
benchmarks without tests and does not affect `app-dev` or `full-test` presets.
README build guidance includes the preset and erase-query benchmark command.

Validation: benchmark target compiled, the benchmark ran successfully, and a
full `cmake --build build --parallel 4` passed after CMake regeneration. The
dependency audit checked 268 source files. No tests or startup were run.
Remaining: profile real viewport scenes and complete R6–R11 ownership and
behavior review.

#### R12 repeated timings with both benchmark targets (2026-10-04)

After adding the erase-query benchmark target and its isolated CMake preset,
reran three no-op all-target builds and three timestamp-only rebuilds of
`src/tools/point_tool.cpp`. The current graph includes the app, test
executables, and both benchmark executables; the 268-file source dependency
audit passed. No-op median: 3.61 s / 86,640 KB (ranges 3.50–3.63 s and
86,600–86,684 KB). One-tool all-target median: 5.75 s / 201,748 KB (ranges
5.64–5.84 s and 201,680–201,852 KB). The earlier 3.53/4.97-second repeated
samples used a graph without the erase-query benchmark target, so both sets
remain recorded in `BUILD_BASELINE.md`. These compare against single R1 runs
and do not establish a clean-build or runtime improvement.

Validation: all six builds passed; no test executable or startup was run.
Remaining: collect matched clean builds, profile representative viewport
scenes, and continue R6–R11 ownership and behavior review.

#### R8 Mirror commit-dispatch ownership checkpoint (2026-10-04)

Moved Mirror's second-click commit decision from `ViewportWidget` into
`MirrorTool::dispatchMousePress`. The base typed mouse-press dispatch is now
virtual so a tool can own its completion boundary. Mirror's direct
`handleMousePress` remains a staging operation, preserving the existing
tool-contract API; the viewport consumes an optional commit result for its
existing debug message and presentation refresh. Removed the widget's
`handleMirrorPoint` adapter and Mirror-specific left-click route. Empty-source
Mirror clicks remain consumed as before.

Validation: `git diff --check` and `cmake --build build --parallel 4` passed;
the dependency audit checked 268 sources and all active app/test/benchmark
targets compiled. Tests and startup were not run, so click-to-commit behavior
still needs the public interaction suite. Remaining: continue R7 event
precedence and R8 tool-adapter cleanup.

#### R8 Scale click-dispatch ownership checkpoint (2026-10-04)

`ScaleTool::dispatchMousePress` now owns click-stage acceptance and commits the
transform when the final reference click requests completion. It publishes a
typed `ScaleDispatchResult` for the viewport adapter to update the pending base
point, prompt, commit log, snap state, and redraw. The viewport's dedicated
Scale left-click branch and its direct click-to-commit decision were removed;
keyboard Enter/numeric paths continue through their existing adapter.

Validation: `git diff --check` and `cmake --build build --parallel 4` passed;
the dependency audit checked 268 sources and all active app/test/benchmark
targets compiled. Test executables and startup were not run, so Scale click
behavior remains pending public interaction verification. Remaining: move
keyboard/numeric completion ownership and continue R7 event-precedence review.

#### R8 Scale factor-key dispatch checkpoint (2026-10-04)

Made the base typed key dispatch virtual and moved Scale Backspace, factor
character entry (including comma-to-decimal normalization), factor acceptance,
1D preview activation, and stage-two Enter commit into
`ScaleTool::dispatchKey`. The viewport keeps Escape and selection-center Enter
handling, supplies the already-resolved cursor point for preview parity, and
adapts prompt, debug-log, snap-reset, and redraw results.

Validation: `git diff --check` and `cmake --build build --parallel 4` passed;
the dependency audit checked 268 sources and all app/test/benchmark targets
compiled. Test executables and startup were not run, so Scale key behavior is
not yet verified. Remaining: remove the selection-center/Escape adapter where
the required view inputs can be represented cleanly, then continue Rotate and
R7 event ownership work.

#### R6–R11 contract validation and mixed-workplane transform checkpoint (2026-10-04)

The full CTest run registered six suites and passed four; `viewport_interaction`
passed after Mirror and Scale moved to typed dispatch. The initial
`core_contracts` failure was fixture contamination: the selection-pruning check
restored a locked layer, then expected an object on that layer to remain
selected. The fixture now unlocks the layer for deleted-ID pruning and separately
asserts pruning when the layer becomes locked. `EraseTool::finishStroke()` now
retains its completed screen path and stable candidate IDs until a new stroke or
reset, which is required by the viewport's completed-stroke query/presentation
lifecycle.

Mixed-workplane Join passed through `JoinTool`, serialization, hit-testing, and
world-space continuity. Its translation contract exposed that
`translateShapeGeometry` used the PolyCurve's first component plane instead of
the supplied input frame. The transform now derives the world displacement from
the input frame and shifts the shape and all component frame origins together,
preserving each component's local NURBS data. The direct Rotate, Scale, and
Mirror contract checks pass. The targeted `core_contracts` rerun passed;
`trim_seam` now fails only the tangent-contact and small-endpoint-gap Erase
assertions already recorded in the R0 baseline. A full build passed and the
dependency audit checked 268 sources. `git diff --check` passed, and the
offscreen startup log reached viewport construction.

Remaining: finish the Scale selection-center/Escape adapter and Rotate input
ownership, close R7 event-precedence coverage, continue R9–R11 boundaries, and
run the complete suite after the next behavior checkpoint. R12 still needs
matched clean-build samples and representative viewport profiling.

#### R8 typed Scale and Rotate key-dispatch checkpoint (2026-10-04)

Moved Scale's selection-center Enter and Escape decisions into
`ScaleTool::dispatchKey`. The viewport now supplies the view-derived selection
bounds center in the typed input and adapts cancellation, prompt, snap, log, and
redraw effects. Removed the widget's `handleScalePoint` and `commitScale`
decision helpers; right-click cancellation remains on the existing adapter.

Added typed key dispatch and a stored result to `RotateTool`. The tool now owns
Escape reset, angle input, snap increment/strength, perpendicular-plane and
axis-key decisions, and commit requests through the shared dispatch boundary.
The viewport provides its resolved cursor/frame and current rotation preferences,
then applies returned cursor/frame, commit, log, and redraw effects. Removed the
widget's special `handleRotateKey` route; right-click cancellation remains.

Added direct contracts for Scale center-Enter/Escape and Rotate typed 45-degree
commit/Escape. Both pass. `cmake --build build --parallel 4` passed with the
268-source dependency audit, and `git diff --check` passed. The latest full
CTest run passed 5/6 suites; `viewport_interaction` passed in 248.01 seconds.
Only `trim_seam` fails, with the same two Erase tolerance cases recorded in the
R0 baseline. Remaining: validate startup after this checkpoint, review remaining
R7 event precedence and R8 cancellation adapters, then continue R9–R12 work.

#### R8 transform-tool right-click cancellation checkpoint (2026-10-04)

`ScaleTool`, `RotateTool`, and `MirrorTool` now consume right-click through
their typed mouse dispatch and reset their own interaction state before asking
ToolContext to finish the command. The viewport's three tool-specific
`cancelScale`/`cancelRotate`/`cancelMirror` right-click decisions were removed;
it keeps the shared snap clear, debug message, and redraw. Cancellation still
works if a tool's source list has become empty.

Added direct right-click cancellation contracts for all three tools. Targeted
`core_contracts` passes; `trim_seam` continues to report only its two R0
baseline Erase tolerance failures, while the new Scale/Rotate/Mirror dispatch
assertions pass. Full build and the 268-source dependency audit passed;
`git diff --check` passed. Full CTest passed 5/6 after this routing change, with
`viewport_interaction` passing in 249.02 seconds. The subsequent guard cleanup
also built successfully and its targeted contracts passed. Offscreen startup
reached viewport construction after the cleanup. Remaining: close R7 event
precedence, continue R9–R11 ownership/test review, and collect matched R12
measurements.

#### R8 Mirror Escape dispatch checkpoint (2026-10-04)

Moved Mirror Escape cancellation into `MirrorTool::dispatchKey` and removed the
viewport's dedicated Escape decision/helper. The widget adapts the completed
tool result by clearing snap presentation, logging, redrawing, and consuming the
key. Direct Mirror Escape contract passes.

Full build and the 268-source dependency audit passed; `git diff --check`
passed; offscreen startup reached viewport construction. The latest full CTest
run passed 5/6 suites, with `viewport_interaction` passing in 248.93 seconds.
`trim_seam` still fails only the two Erase tolerance assertions recorded in the
R0 baseline. Remaining R7 event precedence and public cancellation coverage,
R9–R11 ownership/test review, and matched R12 measurements.

#### R9 Erase intersection tolerance checkpoint (2026-10-04)

`curve_intersections.cpp` now collapses seed results that resolve to the same
geometric contact, including tangent-only contacts whose parameter solutions
differ slightly near a singular tangent. `curve_erase_query.*` now accepts an
explicit endpoint-proximity tolerance and projects candidate-curve endpoints
onto the source curve when exact intersections are absent. The viewport passes
a three-pixel world-space tolerance for this Erase-specific fallback; the
geometry intersection API remains exact. The contract fixture covers the
two-point tangent case and a small endpoint gap, and both now pass. Removed
incomplete aggregate initialization in `viewport_widget.cpp`, eliminating the
missing-field warnings for Shape's newer geometry fields.

Validation: `cmake --build build --parallel 4` passed with the dependency audit
checking 268 sources; `git diff --check` passed; offscreen startup reached
viewport construction. Full CTest passed all six suites in 254.67 seconds,
including `trim_seam`, `viewport_interaction` (248.40 seconds), and
`vignola_file` (5.67 seconds). R9 is complete. Remaining: finish R6–R8 event
and tool ownership review, R10 scene-query/render profiling, R11 focused suite
organization, and matched R12 clean-build/runtime measurements.

#### R7/R8/R10/R11 behavior and query checkpoint (2026-10-04)

R7 public event coverage now verifies that modal Join/Duplicate input retains
priority over Select All, Fill, and Delete; idle Select All remains available.
Arc Escape is now an `ArcTool` key decision that clears its staged interaction
state before the viewport adapts the command finish. Added direct ArcTool Escape
coverage, including active numeric input, and a public viewport assertion that
keyboard routing returns to Select.

Added `services/hit_testing/projected_curve_bounds.*` and used its conservative
positive-weight projected control hull to reject disjoint curve hit and box
queries. Invalid/clipped CV projections fail open. The service test covers
orthographic and perspective cameras, oriented workplanes, point records with
unrelated legacy curve data, disjoint boxes, and clipping fallback. The viewport
query benchmark now has before/after results recorded in
`PERFORMANCE_BASELINE.md`; at 1,024 lines the hit-test median is 0.687 ms
(previously 15.117 ms) and the selection-box median is 0.524 ms (previously
14.671 ms). These are synthetic service timings, not frame timings.

Validation: the full CTest run passed all ten suites in 266.52 seconds. After
the final public Arc Escape assertion was added, the Arc, scene-query, and
viewport-event suites passed 3/3. `git diff --check` passed. The full build
passed and the dependency audit checked 274 source files. The offscreen
application logged startup and viewport construction before the expected
five-second event-loop timeout. Test sources contain no production `.cpp`
includes or private-access macros. Remaining: repeat the warm clean all-target
build twice to close R12, then run the final consistency review. No full-frame
CPU/GL timing is claimed from the service benchmark.

#### R12 274-source build measurement checkpoint (2026-10-04)

On the Release/Unix Makefiles/GCC 13.3 four-job build with tests and all three
benchmarks enabled, three sequential all-target no-op builds measured 3.79 s /
89,588 KB median (3.78–3.86 s / 89,300–89,796 KB). Three timestamp-only
`point_tool.cpp` rebuilds measured 5.47 s / 201,752 KB median (5.46–5.57 s /
201,736–201,980 KB). Source contents did not change during the edit sample.
Removed only generated project object files under `build/CMakeFiles` and
completed one warm-dependency clean all-target build in 130.64 s at
580,000 KB; the dependency audit checked 274 source files. The full CTest run
passed 10/10 in 266.52 s, the focused final Arc/query/event checks passed 3/3,
and the offscreen smoke reached viewport construction before its expected
timeout. The 268-source 117.38 s clean build and R1 clean result used smaller
graphs/target sets, so these are not matched clean-time comparisons. The
one-tool incremental median is lower than R1's single sample, while no-op time
is higher as the graph, targets, and audit have grown. The three warm clean
runs measured 130.64, 126.55, and 125.16 seconds (126.55-second median); no
whole-frame CPU/GL speed claim is made.

#### R10/R12 completion audit checkpoint (2026-10-04)

Re-read the current phase exits, completion criteria, and both progress ledgers
against the worktree. The recorded tests, builds, service-query results, and
warm incremental/clean timings remain valid, but the previous complete status
was too strong. `PERFORMANCE_BASELINE.md` still lists snap/pick, CPU and native
GL redraw, single-object invalidation, and undo memory/latency as unmeasured;
R12 also requires repeating the R0 build scenarios under comparable settings.
Source review confirms the CPU fallback still tessellates NURBS during
`ViewportRenderer::drawShape`, while `ViewportGeometryCache` is consumed by the
GPU/depth paths. The next action is to add an opt-in benchmark for the missing
service, redraw, invalidation, and history cases, then use its evidence to
finish the CPU prepared-geometry path and matched build comparisons. No data or
user changes were discarded; nothing was committed or pushed.

#### Final phase 24 completion checkpoint (2026-10-04)

R10 now routes committed NURBS CPU drawing through the frame-owned
`ViewportGeometryCache` samples, preserving double precision and the existing
adaptive fallback. `drawPreparedCurveSegments` keeps the 0.3-pixel screen
deviation limit while bounding simplification work in overlapping chunks.
`SnapEngine::nearCandidatesForScene` now uses the shared conservative projected
positive-weight control hull to skip disjoint NURBS near-snap searches; clipped
projections fail open. No persistent geometry or input behavior changed.

`benchmarks/viewport_runtime_benchmark.cpp` now records candidate generation,
snap resolution, hit testing, screen sampling, CPU curve/grid/full redraw,
native-GL scene redraw, cache refresh/change count, and snapshot-history latency
and RSS. At 1,024 synthetic curves, snap resolution measured 4.27 ms versus
18.99 ms before the hull filter. Cached CPU redraw measured 96.42 ms versus
196.84 ms before CPU cache use; native GL measured 7.03 ms. Exactly one
prepared world-geometry object changed after a one-object edit. Five-snapshot
commit/undo/redo and process-RSS measurements at 100/1,000/5,000 objects are
recorded in `PERFORMANCE_BASELINE.md`.

`BUILD_BASELINE.md` records the matched warm build comparison and each
incremental scenario. The warm all-target project build is approximately
286.5 s at R0 and 127.96 s currently, with the current graph building ten test
registrations and four opt-in benchmarks. App-only clean, one-tool, common
header, and link-only times do not consistently improve; those costs and the
temporary R0 dependency-link harness are described in the evidence. The
project-target CMake helper disables unused AUTOMOC/AUTOUIC after confirming
there are no Qt meta-object declarations or `.ui` files; resource compilation
remains on.

Validation: clean warm all-target build passed and the dependency audit checked
274 source files; full CTest passed 10/10 in 257.01 s; scene-query and
viewport-render focused checks passed 2/2; the offscreen app reached viewport
construction; `git diff --check` passed. The `app-dev`, `full-test`, and
`benchmarks` presets each configured in their isolated directories. ccache and
Ninja are unavailable. No commit or push was made. R10 and R12 exit criteria
are met; phase 24 is complete.
