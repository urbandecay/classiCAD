# classiCAD build baseline

Captured on 2026-10-04 before the R1 shared-compilation changes.

## Source state and toolchain

- Branch: feature/3d-workplanes
- HEAD: 4dd4f21 (Add NURBS surfaces and refactoring plan)
- Working tree: clean before baseline measurements
- CMake: 3.28.3; generator: Unix Makefiles
- Configuration: Release; CLASSICAD_BUILD_TESTS=ON
- Compiler: GCC 13.3.0 (/usr/bin/c++)
- Qt: 6.4.2
- Parallel jobs: 4; logical CPUs: 8
- Available memory when recorded: 7.2 GiB
- openNURBS: eb92af3ba1806b0a34a99aba0d3bda83e3d46083

## Build measurements

Times are wall-clock seconds from GNU time; RSS is maximum resident set size.
The cold all-target build includes compiling the pinned openNURBS dependency.

| Scenario | Command/action | Elapsed | Peak RSS | Notes |
|---|---|---:|---:|---|
| Cold clean, all enabled targets | cmake --build build --clean-first --parallel 4 | 431.01 s | 728,036 KB | Built app and all enabled test executables, including openNURBS |
| Warm-dependency clean app | Removed only classiCAD generated objects/autogen output and executable, regenerated CMake, then built --target classiCAD --parallel 4 | 104.05 s | 726,188 KB | openNURBS and its dependencies were already built |
| No-op all-target build | cmake --build build --parallel 4 | 2.70 s | 75,520 KB | No sources needed recompilation |
| One tool implementation | Touched src/tools/point_tool.cpp; built --target classiCAD --parallel 4 | 5.69 s | 289,200 KB | One project object plus app link |
| Shared model header | Touched src/core/model.h; built --target classiCAD --parallel 4 | 79.99 s | 726,228 KB | Recompiled most app sources that include the broad model contract |
| Link only | Touched the existing main.cpp object; built --target classiCAD --parallel 4 | 2.55 s | 122,768 KB | No C++ compilation |

Touch measurements changed timestamps only; source contents stayed unchanged.
These app-only incremental numbers exclude CMake generation time before the
app-only clean measurement. Compare future results using the same compiler,
Qt, configuration, dependency state, and job count. Report cold dependency
builds separately from warm project builds.

Before R1, the committed CMake source lists compiled 266 production .cpp
objects for 67 unique src/ paths across the app and four test executables.
After de-duplicating a source within each target, 65 paths were repeated
between targets, producing 199 redundant object compilations; shared sources
appeared in up to five targets. Four unique .qrc resources produced 17 target
resource objects. The Trim test additionally compiles the body of
src/ui/viewport_widget.cpp through its legacy direct include. These counts
come from the committed CMake target source lists; stale object files left by
older configurations were excluded.

## Build and test status

- The cold clean all-target build completed successfully.
- CTest registered five tests: trim_seam, box_selection, core_contracts,
  viewport_interaction, and vignola_file.
- Baseline CTest completed in 235.66 seconds: four tests passed and trim_seam
  failed. The five pre-existing failing assertions were:
  - Erase did not recognize both tangent-only contacts with a circle.
  - The wrong intersection-bounded Erase interval left one endpoint contact.
  - A middle line cut did not produce two independently selectable tails.
  - Trim did not insert the second line tail as a separate same-layer object.
  - Rotate did not apply one angle to every selected shape.
- box_selection, core_contracts, viewport_interaction, and vignola_file
  passed at baseline. The full log is build/Testing/Temporary/LastTest.log;
  the specific Trim diagnostics are recorded there.
- The bounded offscreen startup reached application start and viewport
  constructed without initialization errors. timeout 5s stopped the still-
  running event loop as expected (exit code 124); this is a startup smoke
  result, not a claim that the app exited normally.

## R1 shared-compilation results

Captured after creating the reusable object targets, with the same compiler,
configuration, four jobs, and already-built external dependencies.

| Scenario | Command/action | Elapsed | Peak RSS | Notes |
|---|---|---:|---:|---|
| Clean project sources, all active targets | Removed generated project object files under build/CMakeFiles while retaining make rules and build/_deps; then cmake --build build --parallel 4 | 111.33 s | 730,776 KB | Built common modules once, then app and all enabled test executables |
| No-op all-target build | cmake --build build --parallel 4 | 2.63 s | 76,128 KB | No project sources needed recompilation |
| One shared tool implementation | Touched src/tools/point_tool.cpp; built all targets with cmake --build build --parallel 4 | 6.57 s | 289,088 KB | One tool object rebuilt; app, Trim, core-contract, and viewport-interaction executables relinked |

The module object directories contain 66 production .cpp objects for 66
unique source paths. No common production path is compiled by more than one
module. The app compiles src/main.cpp directly; each test compiles its own test
entry source. The Trim test's test entry still directly includes the ordinary
viewport widget implementation under access macros, without also linking the
widget object. All four .qrc resources now compile once through their owning
object module. The pre-R1 source-list count above is derived from the committed
CMake source lists; generated files left by older build configurations are
excluded from both sides.

The 111.33 second project-only build has warm openNURBS dependencies and builds
the app plus tests. The R0 431.01 second cold build also compiles openNURBS, so
those clean-build wall times are not directly comparable. The build graph
shows the compile duplication was removed; R12 will repeat cold and warm
measurements under matched conditions before making a final speed comparison.

After R1, CTest took 223.33 seconds: box_selection, core_contracts,
viewport_interaction, and vignola_file passed; trim_seam reported the same five
pre-existing failures listed above. The offscreen startup smoke logged
application start and viewport construction before its expected timeout.

## R2 contract and target-layer results

The target graph now separates geometry, document/history, serialization,
Rhino interchange, logging, services, tools, shared viewport rendering,
viewport adapter support, the widget adapter, and main-window composition.
Core, document, serialization, service, and tool object modules compile with
Qt Core/Gui. Viewport and executable targets add Widgets/OpenGL. The Rhino
interchange object is the only project module with an openNURBS compile
dependency; only final targets that include it receive the openNURBS/zlib link
sequence. The dependency audit is an `ALL` target and passed in the build.

| Scenario | Command/action | Elapsed | Peak RSS | Notes |
|---|---|---:|---:|---|
| No-op all-target build | cmake --build build --parallel 4 | 2.83 s | 77,340 KB | Includes the dependency-audit script |
| One tool implementation | Touched src/tools/point_tool.cpp; built all targets with cmake --build build --parallel 4 | 4.30 s | 194,120 KB | Recompiled one shared tool object and relinked app, Trim, core-contract, and viewport-interaction executables |

The CMake source groups contain 73 shared production `.cpp` paths plus the
application `src/main.cpp`; no common production implementation is repeated
across target modules. The Trim test's documented direct include still
compiles `viewport_widget.cpp` in its own translation unit. The four `.qrc`
resources each have one owning object module.

`cmake --build build --parallel 4` completed after the R2 header and target
migrations. The default dependency audit passed for 162 source files. The
bounded offscreen startup logged application start and viewport construction
before the expected event-loop timeout (exit 124). `git diff --check` passed.
CTest was not rerun in R2; the most recent run remains the R1 result with four
of five registrations passing and the same five pre-existing Trim/Erase/Rotate
assertions failing as recorded above.

## R10/R11 continuation measurements (2026-10-04)

Measured after the viewport preview and window preference extractions, with
openNURBS dependencies warm, Release configuration, Unix Makefiles, GCC 13.3,
and four build jobs. The single-tool measurement touched only the source
timestamp; its contents did not change for measurement.

| Scenario | Command/action | Elapsed | Peak RSS | Notes |
|---|---|---:|---:|---|
| No-op all-target build | `/usr/bin/time cmake --build build --parallel 4` | 3.71 s | 85,132 KB | No project source recompiled; dependency audit checked 267 source files |
| One tool, application target | Touched `src/tools/point_tool.cpp`; built `--target classiCAD --parallel 4` | 3.22 s | 201,748 KB | One shared tool object rebuilt, application relinked |
| One tool, all active targets | Touched `src/tools/point_tool.cpp`; built all targets with `--parallel 4` | 4.94 s | 201,720 KB | One shared tool object rebuilt; app and dependent test executables relinked |

The matched all-target one-tool result is 1.63 seconds faster than the R1
measurement of 6.57 seconds, with peak RSS lower by about 87 MB. This is a
single incremental sample, not a clean-build comparison. The current no-op
sample is 1.08 seconds slower than the R1 2.63-second sample; the project and
audit now cover 267 sources, so R12 needs repeated matched runs before
attributing that difference. No tests or startup check were run in this
checkpoint; the build compiled the existing test executables.

### Repeat after the render-contract target (268-source graph)

Repeated on the settled graph with the same Release/Makefiles/GCC 13.3/four-job
setup and warm external dependencies. Three no-op builds ran consecutively,
then three one-tool cycles touched only `src/tools/point_tool.cpp`'s timestamp
and rebuilt all targets. Source contents did not change; no tests were run.

| Scenario | Command/action | Elapsed | Peak RSS | Notes |
|---|---|---:|---:|---|
| No-op all-target build, median of 3 | `/usr/bin/time cmake --build build --parallel 4` | 3.53 s (3.44–3.55 s) | 85,556 KB median (85,496–85,596 KB) | No project source recompiled; dependency audit checked 268 source files |
| One tool, all active targets, median of 3 | Touched `src/tools/point_tool.cpp`; built all targets with `--parallel 4` | 4.97 s (4.95–4.98 s) | 201,920 KB median (201,920–202,032 KB) | One shared tool object rebuilt; app and dependent test executables relinked |

The repeated one-tool median is 1.60 seconds faster and about 87 MB lower peak
RSS than the R1 sample (6.57 s / 289,088 KB). The no-op median is 0.90 seconds
slower than R1 (2.63 s). Source/audit size has grown. The warm clean build and
runtime scene measurements remain to be repeated under matched conditions
before drawing an overall performance conclusion.
The R9 reusable-candidate assembly change was made after these samples. Its
functional build passed, but its runtime effect has not been measured yet.

### Repeated all-target timings after adding the erase-query benchmark target

The current build enables both standalone benchmark executables. Three no-op
builds ran first; then three cycles touched only the timestamp of
`src/tools/point_tool.cpp` and rebuilt all targets. The source contents did not
change. The dependency audit checked 268 `src/` files; benchmark sources live
outside that audit's source set.

| Scenario | Command/action | Elapsed | Peak RSS | Notes |
|---|---|---:|---:|---|
| No-op all-target build, median of 3 | `/usr/bin/time cmake --build build --parallel 4` | 3.61 s (3.50–3.63 s) | 86,640 KB median (86,600–86,684 KB) | Includes app, tests, both benchmark targets, and dependency audit; no project source recompiled |
| One tool, all active targets, median of 3 | Touched `src/tools/point_tool.cpp`; built all targets with `--parallel 4` | 5.75 s (5.64–5.84 s) | 201,748 KB median (201,680–201,852 KB) | One shared tool object rebuilt; dependent app/test executables relinked; benchmark targets did not depend on the tool |

The one-tool median is 0.82 seconds faster and about 87 MB lower peak RSS than
R1 (6.57 s / 289,088 KB). The no-op median is 0.98 seconds slower and about
10.5 MB higher peak RSS than R1 (2.63 s / 76,128 KB). These remain comparisons
against single R1 runs; the larger target/source graph and scheduling variance
limit attribution. The R9 query reuse and later Mirror/Subdivision routing
changes are included in these samples. No tests or startup were run.

### Warm-dependency clean build after the 268-source graph (2026-10-04)

After recording the incremental samples above, removed 180 generated `.o`
files only from `build/CMakeFiles` and rebuilt all enabled targets. The warm
`build/_deps` tree and generated build rules were retained; no project source
files were touched. The current cache has both
`CLASSICAD_BUILD_TESTS=ON` and `CLASSICAD_BUILD_BENCHMARKS=ON`.

| Scenario | Command/action | Elapsed | Peak RSS | Notes |
|---|---|---:|---:|---|
| Warm-dependency clean, all enabled targets | Removed generated project `.o` files under `build/CMakeFiles`; then `/usr/bin/time cmake --build build --parallel 4` | 117.38 s | 597,380 KB | Built app, test executables, and enabled benchmark targets; dependency audit checked 268 source files; build passed |

The R1 warm clean result was 111.33 s / 730,776 KB, but it predates the
benchmark target and does not document the benchmark option. Treat these
figures as directional rather than a matched clean-build comparison: the
current target graph builds additional code while peak RSS is about 133 MB
lower. Repeat with explicitly matched target options before attributing the
time or memory difference to the refactor. No test executable or startup
smoke was run.

### Warm-dependency clean app-only build (2026-10-04)

Parsed the current `classiCAD` link command and removed only its 144 referenced
generated project object files. The existing dependency artifacts, generated
autogen sources, and source tree were retained. Then built only the app target:

| Scenario | Command/action | Elapsed | Peak RSS | Notes |
|---|---|---:|---:|---|
| Warm-dependency clean app target | Removed the 144 project `.o` files referenced by `build/CMakeFiles/classiCAD.dir/link.txt`; `/usr/bin/time cmake --build build --target classiCAD --parallel 4` | 103.01 s | 580,984 KB | Dependency audit passed for 268 sources; tests and benchmarks were not build targets |

R1's warm-dependency app-only build measured 104.05 s / 726,188 KB. The current
app-only sample is 1.04 seconds faster and about 145 MB lower peak RSS while
building the larger 268-source graph. The older measurement also removed
generated autogen output, so the comparison is directional rather than exact.
After this sample, `cmake --build build --parallel 4` passed and relinked all
active test and benchmark executables against the rebuilt shared objects. No
test executable or startup smoke was run.
The clean app timing predates the subsequent Mirror dispatch and translated
Subdivision wheel changes. Full builds after those changes passed; the clean
timing was not repeated.

### Final repeated incremental and warm clean build on the 274-source graph

Measured with Release, Unix Makefiles, GCC 13.3, four jobs, tests enabled, and
all three opt-in benchmarks enabled. No-op and timestamp-only samples ran
sequentially. The tool edit changed only `src/tools/point_tool.cpp`'s timestamp;
its contents were unchanged.

| Scenario | Samples | Median elapsed | Median peak RSS | Range |
|---|---:|---:|---:|---|
| No-op all-target build | 3 | 3.79 s | 89,588 KB | 3.78–3.86 s; 89,300–89,796 KB |
| One shared tool, all active targets | 3 | 5.47 s | 201,752 KB | 5.46–5.57 s; 201,736–201,980 KB |
| Warm-dependency clean, all active targets | 3 | 126.55 s | 580,000 KB | 125.16–130.64 s; 579,648–581,088 KB |

For each clean sample, removed only generated project object files beneath
`build/CMakeFiles`; source files, generated build rules, and the external
dependency tree remained. Each rebuilt the app, nine test executables (ten
registered CTest cases), and all three benchmarks; the dependency audit checked
274 source files. All three builds passed. The previous 117.38 s warm clean run
used a 268-source graph, so the clean times are not a matched graph comparison.
R1's single one-tool sample was 6.57 s / 289,088 KB: the current one-tool
median is lower, while current no-op time (3.79 s) exceeds R1's 2.63 s as
targets and dependency-audit scope grew. These directional samples do not
establish a whole-project clean-build speedup.

The full current CTest run passed 10/10 in 266.52 s. The final Arc/query/event
focused rerun passed 3/3. An offscreen startup smoke logged application start
and viewport construction before the expected five-second event-loop timeout
(exit code 124). The clean compile still reports partial-aggregate warnings in
legacy `Shape` construction sites and warnings from vendored openNURBS headers;
there were no build errors.

## R12 matched R0/current measurements (2026-10-04)

Both source states used GCC 13.3, Qt 6.4.2, Release, Unix Makefiles, and four
jobs. The R0 source was checked out at `4dd4f21` in a detached temporary
worktree. Its pinned openNURBS and zlib artifacts were reused from the primary
build as imported libraries, so the clean measurements below cover the
classiCAD project graph with warm external dependencies. The R0 all-target
clean measurement is approximate: the first compile pass took 271.42 s but the
temporary imported-target harness omitted `uuid` at link time; after adding
that link dependency, the remaining Vignola target compile and all relinks
completed in 15.12 s. Together this is about 286.5 s, with 727,608 KB peak RSS.
No source files in the primary worktree were used or changed by the baseline
build.

| Scenario | R0 | Current R12 | Notes |
|---|---:|---:|---|
| Warm clean all project targets | ~286.5 s / 727,608 KB | 127.96 s / 581,120 KB | Current includes ten CTest registrations and four opt-in benchmarks; R0 had five CTest registrations and no benchmarks. Current shares production object modules across executables. |
| Warm clean application target | 104.05 s / 726,188 KB | 106.67 s / 581,344 KB | R0 is the original baseline app-only measurement; current app-only compilation is roughly level, with a lower peak RSS. |
| No-op all-target build | 2.70 s / 75,520 KB | 2.42 s / 91,492 KB | Current includes the broader dependency audit and more targets. |
| One tool, application target | 5.69 s / 289,200 KB | 6.89 s / 318,356 KB | Timestamp-only edit of `point_tool.cpp` in R0 and `point_construction_tool.cpp` in R12; current graph has more target checks. |
| Model/Shape header, application target | 79.99 s / 726,228 KB | 81.42 s / 580,584 KB | R0 touched `core/model.h`; R12 touched `core/document/shape.h`. The widely used geometry contract remains a large rebuild boundary. |
| Link-only application target | 2.55 s / 122,768 KB | 3.15 s / 127,896 KB | Touched only the existing `src/main.cpp` object in each build directory. |

The all-target warm source build is about 55% faster in this run, with about
20% lower peak RSS, despite the current tree building more focused test and
benchmark executables. App-only clean and incremental one-tool/header/link
times did not improve consistently; no blanket claim that every build is
faster is warranted. The fully cold R0 result includes building openNURBS and
is not compared to a warm current build.

After disabling unused `AUTOMOC` and `AUTOUIC` on project targets (there are no
`Q_OBJECT`, `Q_GADGET`, namespace meta-object declarations, or `.ui` files;
`AUTORCC` remains enabled), a no-op all-target build measured 2.42 s / 91,492
KB and a one-tool app build measured 6.89 s / 318,356 KB. Before that target
cleanup, the one-tool app sample was 7.34 s. These are single samples, so the
effect on incremental timing is modest and should be remeasured on another
machine.

All three CMake presets (`app-dev`, `full-test`, and `benchmarks`) configured
successfully in their separate directories with the already-fetched pinned
openNURBS source. The presets were listed and parsed; their isolated targets
were not all rebuilt. Neither ccache nor Ninja is installed. The current full
build and dependency audit passed for 274 source files; CTest passed 10/10 in
257.01 s, and the five-second offscreen startup reached viewport construction.
