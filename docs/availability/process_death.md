# Process death: the inner loop {#ha_process_death}

**Status: partly built, and the mechanism the design first proposed has been measured and ruled
out.** This document collects what is settled, what exists, and what is still to decide. It does
not settle the open questions; they are marked as open.

A component dies on a machine that is otherwise healthy: the kernel is fine, the disk is fine, the
peer is fine, and the socket closed because the process no longer exists. That is the **inner
loop**, and it is a different problem from a machine going silent. The decision record argues the
separation at [design_notes.md#ha_process_vs_machine](design_notes.md#ha_process_vs_machine); this
document is about the half of it that is not finished.

## The target, and why it is not the outer loop's

| | Inner loop -- the process died | Outer loop -- the machine went quiet |
|---|---|---|
| Recovery target | **under 50 ms** | 100 ms to seconds |
| What is known | the process is gone; everything else is healthy | nothing, which is the problem |
| Correct response | restart it in place | decide whether to promote the peer |

The targets differ by two orders of magnitude because the questions differ. A closed socket is
**evidence**: the kernel closed it because the process no longer exists. Silence is not evidence,
and resolving it needs a third party and a timer.

**Today the venue treats the first as the second**, and that costs a 16-second outage against a
50 ms target. That is BUG-0029, which is parked pending this design.

## What exists

`scripts/launch.py` starts one component and restarts it if it dies. It knows nothing about
topology, roles or peers, and writes the *component's* pid to the plain pid file so the existing
tools need no changes. `devenv.py --supervised` wires it in, and it is off by default.

Two rules it follows, and both are deliberate:

- **It does not decide leadership.** Starting a process and assigning it a role are two jobs, and
  a supervisor that did both would become mandatory --
  [design_notes.md#ha_supervisor_role](design_notes.md#ha_supervisor_role).
- **It never gives up by default.** `--max-consecutive-failures 0` means retry forever, because
  abandoning a component guarantees there is none, whereas a slow retry keeps trying.

**There is no systemd and there will not be.** The supervision design has to work without it.

## What measurement has ruled out

Section 7 of the decision record proposes a **shared-memory journal** for the inner loop: state
changes written to `/dev/shm`, replayed by the restarted process, on the reasoning that the kernel
keeps the segment when the process dies.

**That cannot meet the target at any realistic book size.** Rebuilding the matching engine's book
by replaying entries was measured on 2026-08-21, pre-reserved, with no migration, no decode and no
I/O -- so a lower bound on any journal replay:

| Book size | Rebuild time |
|---|---|
| 2^21 | 438 ms |
| 2^22 | 921 ms |
| 2^23 | **2034 ms** |

Fifty milliseconds is not reachable by replaying anything. **Only the book itself living in shared
memory and being re-attached rather than rebuilt can hit it.** That is a much larger change than a
journal, and it is the main thing this design has to decide.

## A slow flap is a loss of service, and the instance is judged for it

**Settled 2026-09-06.** An instance that dies and returns repeatedly is not a component that keeps
recovering; it is a venue that keeps losing service. The question this answers is what the venue
does about it.

**What makes an interruption uncovered.** A pair exists so that one instance failing costs a
failover rather than an outage. That only works when the survivor is given time to notice: the
matching engine's peer arms `ha_timing.heartbeat_timeout_seconds` -- fifteen seconds -- on losing
the replication connection. An instance that dies and is restarted inside that window is back
before any failover begins, so the pair does nothing and the venue simply has a hole. Measured:
eight kill-and-restart cycles produced outages of 8 to 12 seconds each, and no promotion at any
point.

**What a member experiences in one.** An order sent into the hole is accepted, sequenced and
deferred: no execution report, and the member holds something it believes is live that no book
holds. A cancel takes the same path, so an order already placed cannot be withdrawn. And nothing
is said -- the venue only stops accepting after 45 seconds of *continuous* outage, which a flap
never reaches because the clock restarts on every reconnect. So the member gets repeated silent
windows in which it can neither place nor withdraw, and no signal that anything is wrong.

**Two uncovered interruptions and the instance is fatal.** One is a fault; two is a pattern. The
supervisor stops restarting it, which it can already do -- `launch.py` has `give_up()`, writes
`gave up` to `<name>.launcher.state` and exits. The instance is then absent for longer than the
promotion timeout by construction, so the arbiter grants leadership to the peer and service
resumes. The supervisor decides only whether to start a process; which instance leads remains the
arbiter's, exactly as [design_notes.md#ha_supervisor_role](design_notes.md#ha_supervisor_role)
requires.

**Why this is needed whatever else is fixed, which is the part worth keeping.** There are two
obvious repairs and neither is sufficient alone:

- Leave failover at fifteen seconds and the flap gives **silent holes**, as above.
- Make failover fast, which is what BUG-0029 asks for, and each death promotes the peer instead --
  but `open_orders_on_promotion` is `cancel`, so **every one of those promotions cancels the whole
  book and tells every member their resting orders are gone.** Eight cycles become eight mass
  cancellations, which for a member is worse than not knowing: they are repeatedly told their
  orders are destroyed and asked to replace them into a venue about to do it again.

So making recovery faster changes the shape of the damage rather than removing it. What bounds it
is judging the instance, because that turns an open-ended series of interruptions into exactly
one. Fixing BUG-0029 then makes that one interruption short instead of fifteen seconds, which is
the right order to value the two.

**The last instance halts loudly rather than leaving nothing.** Where the cause is something both
instances share -- a poison order, a bad build, a configuration both read -- the peer meets it too
and is judged in its turn. The venue then has no instance of that component and a supervisor that
has stopped trying. That must be announced rather than discovered: a venue quietly running with
nothing behind a component is the failure that looks like health. This is the same shape as
BUG-0010, where high availability fails over into a condition both nodes share.

**Still to settle: the window.** Two deaths a year is not a flap. The count needs a period over
which it is taken, and that period is not the promotion timeout -- a slow flap can run on any
cycle. It should be long enough that two unrelated faults are unlikely to be judged as one
pattern, and short enough that a genuine flap is caught within a trading session.

## What is still open

- **Does the order book live in shared memory?** It is the only way to the stated target, and it
  is a substantial change to the matching engine's storage. If the answer is no, the target has to
  move instead -- and saying so is better than carrying a number nothing aims at.
- **How long is the local recovery grace period?** The follower currently borrows
  `ha_timing.heartbeat_timeout_seconds`, which is a different quantity measured for a different
  purpose. Its correct value is a consequence of how fast a supervised restart is, which is why
  BUG-0029 is blocked on this document rather than the other way round. One number serving two
  meanings cannot be tuned for either.
- **What happens on a crash loop?** Partly settled above: the instance is judged after two
  uncovered interruptions and the last one halts the venue loudly. What that does not answer is
  *why* it was dying. Section 7 proposes a poison-pill filter: recovery identifies the input that
  caused the crash and skips it. Nothing implements this, and skipping an order because it crashed
  the engine is a decision with its own consequences. Judging the instance bounds the damage; it
  does not diagnose it.
- **Which components get an inner loop at all?** The matching engine is the expensive case because
  of the book. A gateway or the arbiter may be cheap enough to restart cold, in which case the
  supervisor is the whole answer for them.

## Related

- [design_notes.md#ha_process_vs_machine](design_notes.md#ha_process_vs_machine) -- the two loops
  and why they are separate
- [design_notes.md#ha_supervisor_role](design_notes.md#ha_supervisor_role) -- a supervisor starts
  processes and does not decide roles
- BUG-0029 -- a process death on the same host takes the machine-death path; parked on this design
- [Bug List](../bug_list.md) -- BUG-0028 is the book's memory behaviour, which bears on whether it
  can live in shared memory

---

Back to the [documentation contents](../README.md).
