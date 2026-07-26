# Building

`build.sh` is a thin wrapper that sets the third-party environment variables for the detected
platform and then calls `build.py`, which does the work: configure, compile, run the test suites,
generate coverage or Doxygen if asked, and install into a staging directory.

```bash
./build.sh                       # normal build, all tests, install into installed/
./build.sh --no-java             # skip the Maven build
./build.sh --no-tests            # compile and install only
./build.sh --coverage --coverage-report
./build.sh --asan
./build.sh --tsan
./build.sh --valgrind
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

Same path, new content. The artefact locations do not vary by flavour; the object is rebuilt where
it already was.

Separate build directories per flavour are still worth using if you switch back and forth often --
`--build-dir` exists for that -- but as a way to keep each flavour's objects warm and avoid a full
rebuild, not because correctness requires it.

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

To run the system deliberately instrumented, name the prefix -- both `start_fix_seq_system.py` and
`perf_run.py` take it as their first positional argument:

```bash
./build.sh --asan
./start_fix_seq_system.py installed-asan
```

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

## Coverage reports

```bash
./build.sh --coverage --coverage-report
```

Output lands in `<build-dir>/coverage_html/index.html`. `gcovr` captures, the tracefile is
rewritten, and `genhtml` renders. Application code, tests, third-party code and benchmark
`performance/` mains are excluded; the report covers the framework libraries.

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

## See Also

- [CPU Pinning](design/cpu_pinning.md) -- `SCHED_FIFO`, `isolcpus`, and what a machine needs for
  low-latency measurement
- [Roadmap](roadmap.md)
