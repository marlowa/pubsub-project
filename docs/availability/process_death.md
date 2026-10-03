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

**What happens today.** A leader whose process dies and is restarted by its supervisor within the
lease period keeps the lead: it reads back the promises its peer and the arbiter made to it, and asks
again at once ([Majority leases](majority_leases.md), rule 6). Without a supervisor, the follower
takes over once its promise and the arbiter's have run out, about one lease period plus one renewal
interval. Either way the interruption is seconds, against a 50 ms target: a matching engine takes
about 3.5 seconds from dying to leading again (measured below). That is BUG-0029, which is parked
pending this design.

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
failover rather than an outage. A leader that dies and is restarted within the lease period keeps
the lead, so the pair does not fail over, and the venue has a hole for as long as the restart takes.

**What a member experiences in one.** An order sent into the hole is accepted, sequenced and
deferred: no execution report, and the member holds something it believes is live that no book
holds. A cancel takes the same path, so an order already placed cannot be withdrawn. And nothing
is said -- the venue only stops accepting after 45 seconds of *continuous* outage, which a flap
never reaches because the clock restarts on every reconnect. So the member gets repeated silent
windows in which it can neither place nor withdraw, and no signal that anything is wrong.

**Two uncovered interruptions and the instance is fatal.** One is a fault; two is a pattern. The
supervisor stops restarting it, which it can already do -- `launch.py` has `give_up()`, writes
`gave up` to `<name>.launcher.state` and exits. The instance is then absent for longer than a lease
period by construction, so the follower takes over with the arbiter's vote and service resumes. The
supervisor decides only whether to start a process; which instance leads remains a matter for the
lease rules, exactly as [design_notes.md#ha_supervisor_role](design_notes.md#ha_supervisor_role)
requires.

**Why this is needed whatever else is fixed, which is the part worth keeping.** There are two
obvious repairs and neither is sufficient alone:

- Restart each death in place and the flap gives **silent holes**, as above.
- Let the peer take over instead, and each death promotes the peer --
  but `open_orders_on_promotion` is `cancel`, so **every one of those promotions cancels the whole
  book and tells every member their resting orders are gone.** Eight cycles become eight mass
  cancellations, which for a member is worse than not knowing: they are repeatedly told their
  orders are destroyed and asked to replace them into a venue about to do it again.

So making recovery faster changes the shape of the damage rather than removing it. What bounds it
is judging the instance, because that turns an open-ended series of interruptions into exactly
one. Fixing BUG-0029 then makes that one interruption short instead of seconds long, which is the
right order to value the two.

**The last instance halts loudly rather than leaving nothing.** Where the cause is something both
instances share -- a poison order, a bad build, a configuration both read -- the peer meets it too
and is judged in its turn. The venue then has no instance of that component and a supervisor that
has stopped trying. That must be announced rather than discovered: a venue quietly running with
nothing behind a component is the failure that looks like health. This is the same shape as
BUG-0010, where high availability fails over into a condition both nodes share.

**There is no window, and that is deliberate.** The obvious shape is a lookback -- two deaths
within some period -- and it was rejected. Asked how often one interruption of this kind is
acceptable, the answer was once: *"Members are bound to complain but the response has to be there
was a failure and that many seconds is how long recovery took. Would they rather the venue had
been halted?"*

That answer does not admit a period. An instance gets **one interruption per trading session**;
the second is fatal. The count resets at a session boundary or when an operator restarts the
instance, and at no other time.

The reason to prefer this over a window is that it survives the conversation with the member. A
period cannot be explained to someone whose orders were interrupted twice: it would have the venue
saying that two failures counted as one pattern because they happened to fall close together, or
that they did not count because they fell far apart. One failure is a failure; a second from the
same instance is that instance being unfit to serve.

**What it costs.** An instance is taken out of service on evidence that would not convince anyone
it was permanently broken. That is accepted: the peer serves, the venue keeps trading, an operator
can put it back, and the alternative is a member exposed to an open-ended series of interruptions
nobody is counting.

**Measured, so the figure being traded is known.** An engine goes from process start to leading in
about 2.4 seconds, and about 3.5 seconds from the moment it dies -- 23,105 open orders recovered
from its region, 2,000 catch-up records applied, on a venue with a small retained log. That is the
length of the interruption a member is being asked to accept once. The design target for a process
dying on a healthy host is under 50 ms, so this figure should fall; the rule does not depend on
it, but how tolerable one interruption is does.

## What is still open

- **Does the order book live in shared memory?** It is the only way to the stated target, and it
  is a substantial change to the matching engine's storage. If the answer is no, the target has to
  move instead -- and saying so is better than carrying a number nothing aims at.
- **How long is the lease period?** It sets two things at once: how quickly a follower takes over
  from a leader that has gone, and how long a supervised restart has to bring a leader back before
  its follower takes over instead. It is three seconds; whether one value suits both is open.
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
