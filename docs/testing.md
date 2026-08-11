# Testing and Code Coverage

How this project decides what to test, how it reads a coverage figure, and — mostly — what it
deliberately does not do. [Building](building.md) covers the mechanics: which flags produce a
coverage build, where the report lands, how to update the baseline. This page is the policy, and
exists because most of the reasoning behind it previously lived only in commit messages.

---

## Coverage is reported, never gated

There is no threshold. `coverage_baseline.py` always exits 0, whether coverage rose, fell or
stayed put, and nothing in the build fails because of a coverage number.

This is a decision, not an omission, and the reason is specific: **a line-coverage gate is
satisfiable without doing the valuable thing.** The cheapest way past one is a test that executes
the hard path and asserts nothing. It passes the check in ten minutes and leaves behind a test
that can never fail and must be maintained forever. Under deadline pressure that is the rational
move, so it is the move that gets made — and the gate has then made the codebase worse while
reporting an improvement.

Three further properties turn such a rule from irritating into harmful, and they compound:

- **A gate, not a signal.** Blocking is only appropriate for a metric with no legitimate
  exceptions. Coverage is riddled with them — see *Excluding code*, below.
- **The exemption is out of reach.** If the only way to exempt a line is a configuration file the
  contributor cannot change in the same commit, that is not an exemption, it is an escalation.
  Here the exemption is a `LCOV_EXCL_LINE` comment sitting in the diff, reviewable alongside the
  code it applies to.
- **A single percentage is a scalar summary of a distribution.** It cannot distinguish 80%
  everywhere from 100% of the trivial code and 20% of the matching engine. Those are different
  risks. The baseline is therefore per file, and records counts rather than percentages: delete
  fifty well-tested lines and a percentage falls although nothing got worse.

**The current coverage is adequate and closed.** It is not a target and not an open task. Raising
it is not work anyone needs to schedule.

---

## Function coverage is the signal; line coverage is weather

Measured across four clean runs of the whole suite at one commit, changing nothing between them.
The absolute totals have moved since (tests have been added); what matters is the *pattern*, and
that is a property of the suite rather than of those numbers:

| run | lines | functions |
|---|---|---|
| 1 | 5634/6866 | **1293/1445** |
| 2 | 5644/6866 | **1293/1445** |
| 3 | 5643/6865 | **1293/1445** |
| 4 | 5649/6865 | **1293/1445** |

**Function coverage was identical every time — the same count and the same set. The line count was
different every time.** Given identical `.gcda` files the `gcovr` capture is byte-for-byte
reproducible, so the variance is test *execution*, not tooling.

The residual variance is shutdown races: an event arriving while a thread is winding down
(`ApplicationThread.cpp`, `TimerHandler.cpp`) and the `as_string()` call in the log statement that
reports it (`EventType.hpp`). Real code, reached only when the timing falls a certain way.

Two consequences:

- `coverage_baseline.py` headlines function changes and files line changes as informational.
  A function that stops being covered is a finding; a line count that shifts by a few is noise.
- **Do not build a line-level ratchet for this suite.** It would report a change most runs and
  train the reader to skim.

---

## Excluding code, and when not to

`LCOV_EXCL_LINE` and `LCOV_EXCL_START`/`STOP` are honoured (`gcovr` has markers on by default).
Every exclusion carries a comment saying why, because a future reader will otherwise assume the
usual meaning — *unreachable* — and several of ours are excluded for a different reason.

The rule that decides:

> **If the handler only logs and returns, exclude it. If it changes state, test it.**

Testing that a throw throws is theatre. But a `write()` failure that tears down a connection and
schedules a reconnect is behaviour, and "untested but critical" is how a failover comes to crash
on the worst day available.

**Prefer writing the test to adding the marker.** This is not a slogan; it paid immediately. Ten
lines of socket error handling were being covered *by accident* — some other test happened to
provoke a send failure, and whether it did was a matter of timing, which is what made two
identical runs disagree. The proposal was to exclude all three sites. Applying the rule above,
only one qualified. One deliberate test
(`OutboundConnectionManagerTest.OnWriteReadySendErrorTeardownsConnection`) covered two of the
three sites properly, and the tree now holds exactly **one** exclusion marker.

That test also shows the technique worth reusing: it shuts down **its own** end of the socket
rather than closing the peer. Closing the peer reproduces the very race the accidental coverage
depended on; `shutdown()` makes `EPIPE` arrive on the next `send()` with no dependence on anyone
else's timing.

### What is deliberately left uncovered

- **Code that interrogates the running machine.** `CpuLayout::verify_cores_present()` and
  `apply_background_affinity()` call `sched_setaffinity` and read the host's real topology. A test
  for them would pass on whatever machine the suite lands on, right up until it mattered.
- **POSIX error branches that only classify and return.** These are not naturally reachable
  without fault injection, and injecting a failure to reach a branch that does nothing but return
  an errno costs more than it proves.

Both are honest positions, and both are stated in the source where a reader will meet them.

---

## At release time

A release is when coverage gets *considered* — deliberately, once, by a person. That is a
different activity from the day-to-day check, and it is still not a gate.

`release_check.py` has a `coverage` stage. It runs a clean coverage build and **fails on exactly
one thing**: the committed `coverage_baseline.txt` not matching a freshly generated one. That
means the code changed and nobody regenerated the baseline, so nobody looked. It is the
mechanical form of "perform a coverage analysis and consider the results", and it cannot be
satisfied by writing a test that asserts nothing — only by looking.

It never fails on a coverage number. If coverage fell, you regenerated the baseline and tagged
anyway, that is a decision taken with the figures in front of you, which is the point. `PASS
coverage` means *the analysis was done and the baseline is current*, never *coverage is high
enough*.

**Only function coverage is compared for the staleness check** — see the section above. A
line-level check would fail spuriously and be skipped within a fortnight.

Once the baseline is current, the stage prints the review a human reads: what moved since the
previous release tag.

```bash
python3 scripts/coverage_baseline.py --since v0.2.0
```

This needs no stored history. The baseline is a committed file, so `git show
v0.2.0:coverage_baseline.txt` **is** the coverage at that release. Read it for, in descending
order of value: functions that are uncovered and were not before; whether the affected file's
function total also grew, which distinguishes *added without tests* from *regression*; totals and
per-file movement; and whether the deliberately-uncovered list has quietly expanded.

### Coverage is measured on this host, not in the Rocky container

The container answers a different question — *does it compile and do the tests pass on the target
toolchain* — and `stage_rocky` already answers it by running the full `devsetup.py`.

A baseline is **toolchain-specific**: gcc 8.5 and gcc 13 emit different function lists for
identical sources (template instantiations, lambda naming, `[abi:cxx11]` decoration), and the
container builds against different third-party versions. So the baseline records a `PLATFORM`
line, and `--update` **refuses** to overwrite a baseline produced by a different toolchain unless
given `--force-platform`. Without that guard, a coverage build run in the container — which now
lands in `build-coverage-rocky8/` of its own accord — could silently replace the host baseline
with one that legitimately differs in hundreds of places, surfacing later as a wall of phantom
regressions.

A second, container-generated baseline would measure much the same thing and be read by nobody,
which is the documentation equivalent of the test that cannot fail.

---

## Verify a new test batch by breaking the code

A green test suite is evidence only if the tests can go red. Before trusting a new batch, mutate
the code it covers — invert a condition, disable a check, return a constant — rebuild, and confirm
that **exactly** the expected tests fail.

`CpuLayoutTest` was checked this way: disabling the empty-background-pool check and the
absent-component check failed precisely those two tests and no others. That is two findings, not
one. The tests detect real breakage, *and* they are not over-coupled — a single change did not
cascade through unrelated assertions.

This is the cheap version of mutation testing, and it is the only real defence against the
assertion-free test the coverage figure cannot see.

---

## Where the tests live

- `libraries/pubsub_itc_fw/tests/` — unit tests, one file per unit, listed in that directory's
  `CMakeLists.txt`. Several are integration-flavoured by necessity (real loopback sockets, a real
  `Reactor`), which is fine; the reactor is not meaningfully testable through a mock.
- `libraries/pubsub_itc_fw/integration_tests/` — multi-component tests.
- Both are excluded from the coverage report: they are the instrument, not the subject.

Each test file opens with a docblock naming every test in it and what it pins down. Keep that
list current — it is the fastest way to find out whether a behaviour is already covered, and
noticeably faster than reading the assertions.

---

## See Also

- [Building](building.md) — coverage build flags, the baseline workflow, the report
- [CPU Core Layout](design/cpu_pinning_anti_affinity.md) — what `CpuLayout` decides
