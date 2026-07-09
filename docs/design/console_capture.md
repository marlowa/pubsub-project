# Console Capture → Quill

**Status:** design (not yet implemented) · **Target:** pubsub now, work project later

## Purpose

Redirect everything a process might write to the console (`stdout`/`stderr`) into
the Quill log, so that output which escapes the normal logging path is not lost.
The **only** thing that should ever reach the real console is the bootstrap
failure "cannot open the Quill logfile".

Cases that must be captured:

- rogue `std::cout` / `std::cerr`;
- rogue `printf` / `fprintf(stderr, …)` (libc `FILE*` → fd, not a C++ stream);
- an escaped exception → the runtime's `std::terminate` message, written by libc
  straight to fd 2;
- a library — notably the C-coded HK framework at work — writing at the fd level.

pubsub itself is disciplined and rarely writes to the console, so here this is
mainly **crash insurance**. The real driver is the work project, whose
environment is adversarial: no `main`-level exception wrapper, rogue non-stream
output, and C code. That rules out any C++ `streambuf`/`rdbuf` scheme (e.g. the
KEW approach), because those layers are bypassed by libc- and fd-level writers.
The one place all console output converges — regardless of language or API — is
**file descriptors 1 and 2**. We capture there.

## Design decisions

| Decision | Choice | Why |
|---|---|---|
| Capture layer | **fd-level** (`dup2` fds 1 & 2) | Only layer that catches libc/C/terminate output |
| Pipe topology | **one pipe** (stdout+stderr merged) | Preserves true chronological order in a single stream (see Ordering) |
| stdout buffering | **forced unbuffered** (`_IONBF`) | Prevents buffered stdout being overtaken by unbuffered stderr in the merged pipe |
| Consumer | **one reader thread** → Quill wrapper | Splits the byte stream into line records, logs each |
| Activation | **opt-in**, installed in application `main()` | Zero cost when unused; keeps unit-test console backend unaffected |
| Quill sinks | **file-only, no console sink** while active | Avoids an infinite feedback loop |
| Saturation | **block + large pipe** (configurable) | Never lose diagnostics; stall only under pathological spew |
| Crash handling | terminate handler + fatal-signal handlers with a final synchronous drain | Capture the last, most valuable message before the process dies |
| Bootstrap errors | saved `dup` of original fd 2 | The one message allowed on the real console |

Trade-off accepted with **one pipe**: we lose the stdout-vs-stderr distinction, so
all captured lines log at a single level. Ordering (seeing what happened *just
before* an error) was judged more valuable for post-mortem than per-stream
severity.

## Architecture

```
 fd 1 (stdout, unbuffered) ─┐
                            ├─ dup2 ─►  pipe[1] ══► [reader thread] ══► line splitter ══► Quill "console" logger ──► logfile
 fd 2 (stderr, unbuffered) ─┘                pipe[0]

 dup(original fd 2) ─► saved_stderr_fd  ──►  used ONLY for the "cannot open logfile" bootstrap error
                                          ──►  and as the raw destination for the crash-flush signal handler
```

All console output funnels into a single pipe in real time order; a dedicated
reader thread drains it into the Quill wrapper as discrete records.

## Lifecycle

Install order is critical — the logger must be up before the reader can feed it,
and the redirect must go in before any code writes:

1. **Initialise the Quill wrapper** (file sink, no console sink). If the logfile
   cannot be opened → write the one bootstrap error to the *real* fd 2 and do
   **not** install capture. Abort or continue per the app's policy.
2. `dup(2)` → `saved_stderr_fd` (kept for bootstrap/crash use).
3. `pipe2(fds, O_CLOEXEC)`; enlarge with `fcntl(F_SETPIPE_SZ)`.
4. `setvbuf(stdout, nullptr, _IONBF, 0)`; `sync_with_stdio(true)` so `std::cout`
   rides the same unbuffered path. (stderr is unbuffered by standard.)
5. `dup2(pipe_write, 1)`, `dup2(pipe_write, 2)`; close the extra write fd.
6. Install the terminate handler and fatal-signal handlers.
7. Start the reader thread.

Teardown (RAII, reverse order): stop the reader, final drain, restore original
handlers, `dup2` the saved fds back onto 1 & 2, close the pipe.

## Ordering guarantee

The inversion risk is the classic one: after `dup2` onto a pipe, glibc makes
stdout *fully buffered* while stderr stays unbuffered, so `printf("context")`
can sit in stdout's `FILE` buffer while `fprintf(stderr,"boom")` races ahead down
the pipe. That buffer lives in the **writer's** libc, unreachable from the reader,
so we cannot "flush stdout when we see stderr". Instead we remove the buffer at
the source (`setvbuf … _IONBF`), so both streams hit the pipe in true program
order and nothing can overtake anything. Works uniformly for `printf`, HK-FW C
code, and `std::cout`.

## Reader thread

Blocking `read()` on the pipe into a scratch buffer; accumulate into a line
assembler that emits a Quill record on each `'\n'`, carrying a partial line
across reads, and force-flushing an over-long line at a cap (e.g. 8 KiB) so a
newline-less spewer cannot grow the buffer unbounded. This is KEW's
`PrefixStreambuf` record logic, reused. Each record is logged at severity
**ERROR** through a dedicated Quill logger named `console`. ERROR because, by
policy, *any* console write is a defect — including a leftover debug trace — and
support staff filter on ERROR; a lower severity would hide these records from
exactly the people who need them. The dedicated `console` logger name keeps
captured output filterable and stops it masquerading as a first-class application
error.

## Backpressure

With unbuffered stdout every write reaches the pipe immediately; the reader
drains into Quill's async queue, so the pipe normally stays near-empty. Under a
pathological sustained spew the pipe fills and, with a **blocking** write end,
the writer stalls until the reader catches up. That is the safe default — we
never drop diagnostics — and the stall only occurs under abuse that is itself a
bug. A **non-blocking / drop-with-counter** policy is available for callers who
must never stall (e.g. if console output could ever land near a hot path); it
trades lost lines for guaranteed non-blocking. Configurable; default **block**.

## Crash & terminate handling (the hard part)

The reason the facility exists is to capture the crash message — but at crash
time the reader thread and Quill's backend thread are dying too, so bytes can sit
undrained in the pipe. Two distinct paths:

- **`std::terminate` handler** (ordinary function context, not a signal): perform
  a full synchronous drain of the pipe and log it via Quill, then flush Quill,
  then chain to the previous terminate handler. Quill calls are legal here.
- **Fatal-signal handlers** (`SIGSEGV`, `SIGABRT`, `SIGBUS`, `SIGFPE`): a signal
  handler may call only async-signal-safe functions, so **Quill must not be
  touched here**. Instead the handler drains the pipe with `read()` and writes the
  raw bytes with `write()` directly to a pre-opened fd (the Quill logfile fd, or
  `saved_stderr_fd`) — both async-signal-safe — then re-raises the default
  handler. The captured console bytes land in the log even though Quill's
  formatting/async path is unavailable. Because these bytes bypass Quill they
  carry no severity, so the handler prefixes the dump with a plain
  `ERROR [console-crash]` marker line, so an ERROR-based log filter still
  surfaces it.

Best-effort caveat: the faulting thread and the reader thread may both touch the
pipe; some bytes may already have been consumed by the reader and may or may not
have flushed through Quill before `abort()`. The signal handler captures whatever
remains. This is acceptable for crash diagnostics.

## Bootstrap error path

`saved_stderr_fd` is a `dup` of the original fd 2 taken before the redirect. If
the logfile cannot be opened at step 1, the single line "cannot open Quill
logfile: …" is written there and capture is not installed — so it appears on the
genuine console. This is the *only* expected console output in normal operation.

## Not captured / limitations

- Output written **before** install (pre-log bootstrap) goes to the real console.
- A `fork`+`exec`'d child resets its own stdio buffering; its output is still
  captured (it inherits the redirected fd) but may reorder against the parent.
- Sub-line interleaving between threads writing concurrently to 1 and 2 is
  inherent and not corrected.
- Signal-context capture is best-effort (see above).

## Interaction with backtrace logging

The roadmap's planned Quill backtrace-on-`Error`/`Critical` feature would, once
enabled, flush a diagnostic backtrace ring for *every* captured console line,
since they are logged at ERROR — almost certainly not wanted for a chatty rogue
writer. When that feature lands, the `console` logger should be exempt from the
backtrace trigger (or the capture level reconsidered).

## Public API (sketch)

```cpp
class ConsoleCapture {                       // name provisional
public:
    enum class Saturation { block, drop };
    struct Options {
        Saturation   saturation      = Saturation::block;
        std::size_t  pipe_capacity   = 1u << 20;   // 1 MiB
        std::size_t  max_line_length = 8u << 10;   // force-flush cap
        LogLevel     level           = LogLevel::error;  // any console write is a defect
        std::string  logger_name     = "console";
    };

    // Install once, early in main(), AFTER the Quill wrapper is up.
    // Returns nullopt on failure (caller then writes the bootstrap error itself).
    static std::optional<ConsoleCapture> install(Logger& logger, const Options& = {});

    ~ConsoleCapture();                 // restore fds, stop reader, final drain
    ConsoleCapture(ConsoleCapture&&) noexcept;
    ConsoleCapture(const ConsoleCapture&) = delete;
};
```

Single-instance: it owns process-global state (fds 1/2, terminate/signal
handlers), so at most one may exist at a time.

## Portability

Pure Linux/POSIX: `pipe2`, `dup`/`dup2`, `fcntl(F_SETPIPE_SZ)`, `setvbuf`,
`std::set_terminate`, `sigaction`. All present on RHEL8 / gcc 8.5 and on Mint.
No third-party dependency.

## Verification plan

Prove it in the Rocky 8 (gcc 8.5) container, matching the RHEL8 target, with a
small harness that exercises each capture case and asserts it lands in the log
and **not** on the console:

1. `std::cout` / `std::cerr` line;
2. `printf` / `fprintf(stderr, …)`;
3. interleaved stdout-then-stderr → assert chronological order in the log;
4. a thrown exception left to escape `main` → assert the `std::terminate`
   message is in the log;
5. a deliberate `SIGSEGV` / `abort()` → assert the pending pipe bytes reach the
   logfile via the signal handler;
6. logfile-open failure → assert the single bootstrap line appears on the real
   console and capture is not installed.
```
