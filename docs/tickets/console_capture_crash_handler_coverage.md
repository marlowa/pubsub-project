# Ticket: cover ConsoleCapture's crash-handler paths

**Status:** Done (commit 5d86196) · **Component:** `libraries/pubsub_itc_fw` (ConsoleCapture) · **Type:** test coverage

**Outcome:** implemented as planned — crash-dump helpers extracted into
`console_capture_detail` (`ConsoleCaptureCrashDump.hpp`) and covered directly by
`ConsoleCaptureCrashDumpTest`; `ConsoleCaptureDeathTest` verifies the terminate (thread-
escaping exception) and signal (`::raise(SIGSEGV)`) paths end to end. `ConsoleCapture.cpp`
rose to 80.4% line coverage; only the handler shells' final `abort()`/`raise()` remain
uncovered, as anticipated. The detail below is retained as the design record.

## Problem

After the install-coverage work, `ConsoleCapture.cpp` sits at ~59% line / ~67% function
coverage. The uncovered remainder is entirely the **crash path**:

- `ConsoleCapture::Impl::terminate_handler()` — the `std::terminate` handler.
- `ConsoleCapture::Impl::signal_handler(int)` — the SIGSEGV/SIGABRT/SIGBUS/SIGFPE handler.
- `write_all_raw(int, const char*, size_t)` — anonymous-namespace helper.
- `drain_pipe_to_fd(int, int)` — anonymous-namespace helper.

`write_all_raw` and `drain_pipe_to_fd` are **never called** except from the two handlers, so
today nothing exercises them at all.

## Two obstacles

1. **Triggering the handlers ends the process.** `terminate_handler` calls `std::abort()`;
   `signal_handler` re-raises with `SIG_DFL`. So they can only be driven inside a forked
   child — gtest **death tests** (`EXPECT_DEATH` / `EXPECT_EXIT`).

2. **A dying child does not record coverage.** gcov only writes `.gcda` counters via
   `__gcov_dump` on normal exit / `exit()` / an `atexit` handler. A child that `abort()`s
   or dies from a re-raised signal skips that entirely, so a death test **verifies behaviour
   but adds nothing to the coverage number**. (This is the same reason `ConsoleCapture` must
   never route crash output through Quill — see `feedback_no_quill_flush` / `QuillLogger.hpp`.)

## How to trigger each handler (well-defined, not UB)

- **terminate handler:** let an exception escape — e.g. `throw std::runtime_error("boom");`
  in the death-test body with no matching `catch`. This is deterministic and standard,
  unlike a real null-dereference (UB, and the optimiser may elide it). It also exercises the
  `std::current_exception()` / `rethrow_exception` / `what()` branch inside the handler.
- **signal handler:** `::raise(SIGSEGV);` (or SIGABRT/SIGBUS/SIGFPE). `raise()` is a
  deterministic, defined way to deliver the signal to the current process; do **not** rely on
  dereferencing a bad pointer.

## Recommended approach (behaviour + coverage)

Do both — they are complementary:

### 1. Extract a testable crash-dump helper (recovers the coverage)

Move the *non-terminating* core of the handlers — write the marker, format the exception
`what()`, and `drain_pipe_to_fd` — into a small internal helper (e.g. a
`console_capture_detail` namespace in an internal header, or a private static on `Impl`
reachable by a friend test). `write_all_raw` and `drain_pipe_to_fd` move out of the
anonymous namespace so a **normal, non-crashing** unit test can call them directly:

- `write_all_raw`: write to a temp fd / pipe; assert all bytes land, including the
  short-write and `EINTR` retry paths.
- `drain_pipe_to_fd`: pre-fill a pipe, drain to a temp fd, assert the bytes copied and that
  it does not block on an empty non-blocking pipe.
- the crash-dump-writing core: call it directly with a crash-dump fd pointing at a temp
  file and a pipe holding some bytes; assert the file contains the marker + drained content.

`terminate_handler`/`signal_handler` then shrink to "call helper, then abort/re-raise", so
only those final one or two lines stay uncovered. This gets the bulk of the crash code into
the coverage number **without** any death test.

### 2. Death tests for end-to-end behaviour

Add a `ConsoleCaptureDeathTest` fixture (gtest death-test style `threadsafe`):

- `TerminateHandlerWritesCrashDump`: in the child, `install_engine`/`install` with a
  `crash_dump_file_path`, write a line to stdout, then `throw` an uncaught exception. Assert
  (in the parent, after the child dies) that the crash-dump file contains the
  `"[console-crash] std::terminate"` marker, the exception text, and the drained stdout line.
- `SignalHandlerWritesCrashDump`: same, but `::raise(SIGSEGV)` with
  `install_fatal_signal_handlers = true`; assert the fatal-signal marker + drained content.

These assert the crash contract holds; they are expected **not** to raise the coverage
figure (see obstacle 2).

### 3. (Optional) force a gcov dump in the death-test child

If we later want the death tests to *also* count toward coverage, a test-only build could
call `__gcov_dump()` at the top of the handlers behind a compile guard. This is gcc/gcov
specific and pollutes the crash path, so it is a last resort — prefer approach 1.

## Acceptance criteria

- `write_all_raw` and `drain_pipe_to_fd` are covered by ordinary unit tests (no crash).
- The crash-dump-writing core is covered by an ordinary unit test.
- Death tests verify the terminate and fatal-signal crash-dump contract end to end.
- `ConsoleCapture.cpp` coverage rises to the point where only the final `abort()` / re-raise
  lines remain uncovered, documented as such.
- `check_standards` and clang-format clean; full suite green.
