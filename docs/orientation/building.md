# Building

> **One thing the build cannot do for you.** The filesystem holding the sequencer's log must be
> mounted `lazytime`, or the venue stalls for hundreds of milliseconds at a time under load. It
> is a mount option, not a build or configuration setting, and the sequencer warns at startup
> when it is missing. See [filesystem requirements](../operations/filesystem_requirements.md).


`build.sh` is a thin wrapper that sets the third-party environment variables for the detected
platform and then calls `build.py`, which does the work: configure, compile, run the test suites,
generate coverage or Doxygen if asked, and install into a staging directory.

```bash
./scripts/build.sh                       # normal build, all tests, install into installed/
./scripts/build.sh --no-java             # skip the Maven build
./scripts/build.sh --no-tests            # compile and install only
./scripts/build.sh --coverage --coverage-report
./scripts/build.sh --asan
./scripts/build.sh --tsan
./scripts/build.sh --valgrind
```

---

## Build flavours

| Flag | Effect |
|---|---|
| *(none)* | `-O2`, the configuration that ships |
| `--coverage` | `-O0 -g --coverage -fprofile-arcs -ftest-coverage -fno-inline` |
| `--asan` | `-fsanitize=address -fno-omit-frame-pointer` |
| `--tsan` | `-fsanitize=thread`, lock-free optimisations disabled |
| `--valgrind` | `USING_VALGRIND` defined, lock-free optimisations disabled |
| `--debug` | `CMAKE_BUILD_TYPE=Debug` |

`--asan` and `--tsan` are mutually exclusive; `build.py` rejects the combination.

---

## You do not need to clean between flavours

This is worth stating plainly, because the opposite is a natural assumption and it costs real
time: **switching instrumentation in an existing build directory recompiles correctly. There is no
need to delete the build directory first.**

The reasoning that suggests otherwise is that `a.o` is newer than `a.cpp`, so `make` will skip it
and link a stale, uninstrumented object. That reasoning is sound for a build system that does not
track compile flags -- hand-written makefiles among them -- but CMake's Makefile generator does
track them. Every object rule carries `flags.make` as a prerequisite:

```
.../pubsub_itc_fw.dir/src/CpuRegistry.cpp.o: .../pubsub_itc_fw.dir/flags.make
```

`flags.make` holds the `CXX_FLAGS` line and is rewritten at configure time. Change flavour and its
content and mtime change, so every object that depends on it is out of date and is recompiled.
Because the instrumentation options are applied with `add_compile_options()` at global scope in the
top-level `CMakeLists.txt`, this holds for every target rather than only some.

Measured 2026-07-26, one build directory, reconfigured in place and rebuilt with no cleaning:

| | before | after `cmake . -DENABLE_COVERAGE=ON` |
|---|---|---|
| object path | `.../src/CpuRegistry.cpp.o` | identical |
| gcov symbols in the object | 0 | **903** |
| md5 | `42b23a1b3e06` | `05a60649261a` |

Same path, new content. The object is rebuilt where it already was.

**This remains true, and it is the reason the per-flavour build directory below is a convenience
rather than a correctness measure.** If you point two flavours at one directory -- by passing
`--build-dir` explicitly -- what you get back is correct. It simply costs a full rebuild each way.

---

## Each flavour builds in its own directory

A plain build uses `build/`. An instrumented one uses `build-coverage/`, `build-asan/`,
`build-tsan/` or `build-valgrind/`, combined flavours concatenating, plus a platform tag off the
ordinary dev host. This is derived from the flags, so there is nothing to remember; `build-*/` is
gitignored. An explicit `--build-dir` is obeyed verbatim and overrides all of it, which is how
`release_check.py` keeps its container build in `build-rocky/`.

The build directory and the staging directory are named from **one** shared `flavour_suffix()`, so
`build-coverage/` and `installed-coverage/` cannot drift apart. They were derived separately once,
and only the staging one was automatic -- so a coverage build whose caller forgot `--build-dir` put
instrumented objects into `build/` while dutifully staging them to `installed-coverage/`.

Given the section above, the cost of that mistake was never a wrong build; it was two full
rebuilds, for a mistake with no visible symptom until you noticed the clock.

---

## Instrumented builds stage into their own directory

A normal build installs into `installed/`. An instrumented build installs into
`installed-coverage/`, `installed-asan/`, `installed-tsan/` or `installed-valgrind/` (combined
flavours concatenate). `installed-*/` is gitignored.

This separation matters more than it first appears, and **a separate `--build-dir` does not provide
it** -- the install prefix is independent of the build directory, so before this was added a
coverage build with its own build directory still replaced the libraries in `installed/`.

The reason it matters is the interaction between `LD_LIBRARY_PATH` and `RUNPATH`. The developer
profile sets `LD_LIBRARY_PATH` to `installed/lib`, and `LD_LIBRARY_PATH` takes precedence over a
binary's `RUNPATH`. So whatever sits in `installed/lib` is what `devenv.py`, `perf_run.py`,
`ha_test.py`, `auth_service_test.py` and `fix_capture_test.py` all load. An instrumented library
installed there becomes what every one of those tools runs against.

The dangerous case is not coverage but performance. A coverage library is `-O0` with inlining
disabled and roughly 10% larger; a `perf_run.py` executed after a coverage build would produce
figures that mean nothing, with nothing in its output to indicate why.

To run the system deliberately instrumented, name the prefix. `perf_run.py` and `ha_test.py` take
it as a positional argument; `devenv.py` reads it from the environment file instead, so point one
at the instrumented tree:

```bash
./scripts/build.sh --asan
./scripts/perf_run.py installed-asan
./scripts/ha_test.py --scenario 1 installed-asan

# for devenv.py: copy environments/dev.toml, set install_dir = "installed-asan"
./scripts/devenv.py --env environments/dev-asan.toml start
```

Under valgrind, use `callgrind_run.py`, which is built for it -- callgrind runs the guest in an
instrumentation virtual machine some twenty to fifty times slower, so it establishes readiness by
polling component logs rather than by sleeping.

---

## Tests run against the library just built

For the same `LD_LIBRARY_PATH` reason, `build.py` prepends the build tree to `LD_LIBRARY_PATH` for
every test binary it runs (`test_environment()`). Without that, the test suites would exercise the
library in `installed/lib` -- that is, the *previous* build's library, since the install step runs
after the tests -- and a change to a library `.cpp` could be validated against the old `.so` and
pass.

The same applies to coverage: the `.gcda` files are written by the instrumented objects that are
actually loaded, so if the uninstrumented installed library were loaded instead, the report would
show nothing for `libraries/pubsub_itc_fw`.

If you run a test binary by hand rather than through `build.py`, set `LD_LIBRARY_PATH` yourself:

```bash
LD_LIBRARY_PATH=$PWD/build/libraries/pubsub_itc_fw \
    ./build/libraries/pubsub_itc_fw/tests/pubsub_itc_fw_tests
```

---

## The documentation build, and why the Doxyfile is not used directly on RHEL8

This is worth reading before touching anything Doxygen-related, because there is more
machinery here than the single committed `Doxyfile` suggests.

**On RHEL8 the committed Doxyfile is not the file Doxygen runs.** `CMakeLists.txt` sets
`DOXYGEN_DISABLE_DOT` ON by default for the el8 family (rhel, rocky, centos, almalinux with
`VERSION_ID=8`), and in that case generates `${CMAKE_BINARY_DIR}/Doxyfile.nodot`:

```
@INCLUDE = <source>/Doxyfile
HAVE_DOT            = NO
COLLABORATION_GRAPH = NO
INCLUDE_GRAPH       = NO
INCLUDED_BY_GRAPH   = NO
GRAPHICAL_HIERARCHY = NO
DIRECTORY_GRAPH     = NO
```

Doxygen applies configuration lines top to bottom with the last assignment winning, so the
generated file overrides the included one. The committed `Doxyfile` stays untouched, and hosts
with dot run it verbatim. Consequences of the no-dot path:

- Documentation builds much faster on RHEL8, which is the point.
- Built-in class inheritance diagrams still appear; every dot-generated graph does not.
- **The clickable architecture map is not rendered at all** -- it is a `\dotfile` graph. Its
  post-build link checker, `scripts/check_architecture_map.py`, therefore only runs on the
  dot-enabled path.

To force either behaviour regardless of platform, configure with `-DDOXYGEN_DISABLE_DOT=ON`
or `=OFF`.

**Doxygen runs at install time, not only via `--doxygen`.** `ENABLE_DOXYGEN` defaults ON and an
`install(CODE ...)` hook builds the `doxygen_docs` target during `cmake --install`. The
`--doxygen` and `--doxygen-only` flags to `build.py` are an *additional* explicit run.

**WARN_AS_ERROR does not work on doxygen 1.8.14.** The Doxyfile sets
`WARN_AS_ERROR = FAIL_ON_WARNINGS`, which is a doxygen 1.9 value. On the 1.8.14 that RHEL8
ships, it is rejected -- `argument 'FAIL_ON_WARNINGS' for option WARN_AS_ERROR is not a valid
boolean value` -- and falls back to `NO`. So **on RHEL8 the documentation build never fails,
whatever it emits.** Measured in the Rocky 8 container on 2026-07-26: the same tree that
produces zero diagnostics on doxygen 1.9.8 produces **474 warnings and exit 0** on 1.8.14,
mostly `found subsection command outside of section context` from the older markdown handling
of `##`/`###` headings. That backlog is not addressed; it is recorded here so nobody mistakes
a green RHEL8 docs build for a clean one.

The architecture-map checker exists precisely because of this gap -- its comment says doxygen
1.8.14 does not fail on an unresolved `\ref` inside a dot URL -- but it only runs where dot is
enabled, so it does not cover RHEL8 either.

**Cost.** One second for a clean run on the development workstation with dot and 151 generated
images, and one second in the Rocky 8 container on 1.8.14 without dot. Documentation is not what
makes a build slow.

**Validating the RHEL8 documentation path** needs doxygen and graphviz in the container. They are
installed by the Dockerfile from the `powertools` repo, which is disabled by default and is where
Rocky 8 keeps doxygen (RHEL8 calls the equivalent repo
`codeready-builder-for-rhel-8-*-rpms`). To run just the docs there:

```bash
docker run --rm --entrypoint bash -v "$PWD":/workspace:ro pubsub-rhel8:latest \
    -lc '( cat Doxyfile; echo "OUTPUT_DIRECTORY=/tmp/dox"; echo "WARN_AS_ERROR=NO" ) | doxygen -'
```

**`WARN_AS_ERROR=NO` is required there and is not a workaround for bad documentation.** 1.8.14
maps a markdown `##` to `\subsection` and `###` to `\subsubsection`, and requires each to sit
inside the level above; it will not infer that from a page title. Every document written with
markdown headings therefore produces warnings under it -- 893 across the tree, 771 of them that
one complaint. The documents are correct and 1.9.8 builds them with none. See BUG-0050.

**No anchor links to a heading slug.** A markdown `[text](#anchor)` link becomes a
`\ref anchor` command, and a GitHub-style heading slug is not a label Doxygen knows, so it is an
error under `WARN_AS_ERROR`. `MARKDOWN_ID_STYLE = GITHUB` does not exist in 1.8.14. This holds
inside a document as much as across documents: `[Implementation order](#implementation-order)`
fails exactly as a cross-document citation of a slug would.

**Give the target heading an explicit `{#label}` and link to that**, which resolves under both
Doxygen and GitHub, whether the citation comes from this document or another one. Naming the
section in bold and dropping the link is the alternative, and it is the right answer only when the
link was not earning its place. The cost
is that GitHub has no such extension and renders the braces literally in the heading, so pay it
only where it buys something. `docs/availability/design_notes.md` is the worked example: seven of
its nineteen sections carry a label because `ArbiterThread`, `MatchingEngineThread`,
`SequencerThread`, `launch.py` and the bug list cite them, and those citations used to be section
numbers -- which is why that document has sections 11a to 11e, since inserting a real section 12
would have renumbered every one of them. The other twelve sections have no label and are referred
to in prose.

**`scripts/check_docs.py` catches a violation of either rule, since 2026-08-28.** It used to
accept a GitHub heading slug as a valid anchor and to enumerate documents with `git ls-files`, so
it passed all sixty-eight documents while ten references across six of them were failing the build,
and it could not see an untracked file at all. Both are fixed: it now rejects what Doxygen rejects
and reads the working tree. See [BUG-0063](../bug_list.md#bug_0063).

Building the documentation is still the authority, because it is Doxygen's opinion that decides.
What changed is that a clean `check_docs.py` is now evidence rather than reassurance -- and it
answers in a second, where the build answers one warning at a time.

---

## Coverage reports

```bash
./scripts/build.sh --coverage --coverage-report
```

Output lands in `<build-dir>/coverage_html/index.html`. `gcovr` captures, the tracefile is
rewritten, and `genhtml` renders. Application code, tests, third-party code and benchmark
`performance/` mains are excluded; the report covers the framework libraries.

### The coverage baseline

```bash
./scripts/build.sh --coverage --coverage-report
python3 scripts/coverage_baseline.py            # what has moved since the baseline
python3 scripts/coverage_baseline.py --update   # record where we are now
```

`coverage_baseline.txt` is committed: per file, hit/total for lines and functions, and the
signatures of the functions with no observations. Update it in the same commit as the change that
moved it, while the reason is still known.

It **reports and never gates**, it records counts rather than percentages, and it treats function
movement as the signal and line movement as weather. Each of those is a deliberate choice with a
measurement or an argument behind it — see **[Testing and Code Coverage](testing.md)**, which is
where the policy lives. This page covers only how to run it.

### Orphaned build directories are removed first

`remove_orphaned_target_directories()` runs before the capture, deleting any build-tree directory
whose source directory no longer exists. CMake mirrors the source layout into the build tree but
never tidies up after a rename, so `applications/binary_gateway/` left its target tree, its
`CMakeFiles/` and its `.gcno` files behind when it became `applications/binary_order_gateway/`.

That is harmless to an ordinary build and fatal to coverage, because **gcovr searches the whole
build tree before the report-level `--exclude` filters apply**. It finds the orphan's `.gcno`, no
`.gcda` beside it, cannot resolve the compilation directory the notes record, searches upward, and
aborts at `/` where it cannot write `.gcov` files. The symptom is hundreds of `Could not open
output file` lines naming standard-library headers, which point nowhere near the cause.

Note the asymmetry that makes the obvious alternative wrong: excluding `applications/` from the
*search* to match the report's excludes would discard real data, because the integration tests
execute the applications, so those objects carry genuine coverage of the library headers compiled
into them.

### What the rewrite corrects, and why

`rewrite_tracefile_for_genhtml()` in `build.py` makes three corrections. The first is a
compatibility fix; the other two exist because the raw report was actively misleading.

**Logging-macro function records are dropped.** Every `PUBSUB_LOG` call site emits an
`FMT_COMPILE_STRING` lambda that gcov records as a function and never marks executed -- 144 of
them when measured on 2026-07-26, every one uncovered, because they are uncoverable by
construction. They ruined the per-file function figures: `OutboundConnectionManager.cpp` read
40.5% while all 17 of its real member functions were being called, and `TimerHandler.cpp` read
25.0% with 3 of 3 called. The log *lines* were already excluded by
`--exclude-lines-by-pattern`; dropping the matching function records makes the two treatments
consistent. Nothing else is removed -- no real function, no line, no branch.

**Summary counters are recomputed from the records.** gcovr's `LF`/`LH`/`FNF`/`FNH` disagree with
its own per-line records for template-heavy headers: `ThreadWithJoinTimeout.hpp` claimed `LF:1145`
for a 196-line file whose records hold 64 lines. `genhtml` always recomputed from the records, so
the HTML was right, but anyone reading the tracefile directly got a figure roughly seven points
too low.

**gcovr's `--print-summary` is not used.** It counts each template instantiation as separate lines,
so it reports a larger denominator than the rendered report -- two different numbers for the same
thing. `build.py` prints the corrected totals instead, so what the build says matches what the
HTML shows.

### Cost, and run-to-run variation

A coverage build from scratch -- empty build directory, all C++ test suites, report generated --
took **226 seconds** on the development workstation (2026-07-26, `-j` default, `--no-java
--no-doxygen`). Budget four minutes rather than the one an incremental build takes, and note that a
300-second timeout is uncomfortably close to the measured figure.

The line total moves by a handful of lines between runs -- 5398, 5421, 5427 across three runs of
the same tree. That is expected: several integration tests are timing-dependent, so which branches
of a concurrent path get taken varies. Treat a change of a few lines as noise and look for tens
before concluding anything moved.

### Reading the report

Line and branch coverage are the trustworthy columns. Function coverage is now meaningful too, but
remember what it measures: a function counts as covered once it has been entered at all, so a file
can show 100% functions and 65% lines -- `OutboundConnectionManager.cpp` does exactly that. Its
untested parts are branches inside functions the tests already call (partial writes, connect
failure, retry), not whole behaviours nobody invokes. Those need integration tests against a real
peer socket, which is what `integration_tests/OutboundConnectionRetryIntegrationTest.cpp` and
`TlsOutboundIntegrationTest.cpp` already do.

---

## Holding the documentation to what the code does {#doc_claims}

The build runs two checks over `docs/`, and skipping both is `--no-docs-check`.

`check_docs.py` checks that links resolve and that no document is reachable from nothing.
`check_doc_claims.py` checks something links cannot: whether what a document *says about the
code* is still true.

It exists because on 2026-08-31 five statements were found to be wrong, none of them by anyone
reading the documentation. One section was headed "Planned Migration to `fix_codec` (not yet
done)" five weeks after the migration landed. Another described a form as "not yet built" that
was built. Four documents required a startup order that each of them contradicted in the same
sentence. Every one read perfectly well, and link checking reported the tree consistent
throughout, because the links were fine --- it was the sentences that were wrong.

**Mark a claim you expect to rot** with a comment naming the fact that would prove it. It is an
HTML comment, so it renders as nothing on GitHub and through Doxygen alike:

```markdown
<!-- verify: present applications/fix_order_gateway/FixParser.cpp "fix_codec::FixMessageReader" -->
The gateway frames inbound messages with a zero-copy reader from `fix_codec`.
```

The forms are `present`, `absent` and `count` (a path, some literal text, and for `count` how
many times), and `exists` / `missing` (a path alone). Text is matched literally, not as a
regular expression: a claim in prose is about a thing with a name, and the name is what should
be written down.

**What it cannot do**, and is not meant to: find an unmarked claim that has gone stale. A
checker that tried to read prose would be wrong in both directions. What this gives you is a
way to bind a sentence to the fact behind it, so the build fails rather than a reader finding
out months later.

**Both scripts existed before either was run by anything.** That is how the faults above
survived: a checker nobody runs reports nothing, which reads exactly like a checker with
nothing to report.

---

## See Also

- [CPU Pinning](../framework/cpu_pinning.md) -- `SCHED_FIFO`, `isolcpus`, and what a machine needs for
  low-latency measurement
- [Roadmap](../roadmap.md)
