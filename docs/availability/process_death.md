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
| Recovery target | **within the lease period, so the lead never moves** | 100 ms to seconds |
| What is known | the process is gone; everything else is healthy | nothing, which is the problem |
| Correct response | restart it in place | decide whether to promote the peer |

The targets differ because the questions differ. A closed socket is
**evidence**: the kernel closed it because the process no longer exists. Silence is not evidence,
and resolving it needs a third party and a timer.

**What happens today.** A leader whose process dies and is restarted by its supervisor within the
lease period keeps the lead: it reads back the promises its peer and the arbiter made to it, and asks
again at once ([Majority leases](majority_leases.md), rule 6). Without a supervisor, the follower
takes over once its promise and the arbiter's have run out, about one lease period plus one renewal
interval. A supervised matching engine takes about 2 to 2.5 seconds from dying to leading again
(measured below), so the lead does not move, and the target is met. Most of that time is the
sequencer's wait before it connects to the engine again ([BUG-0237](../bug_list.md#bug_0237)).

## What exists

`scripts/launch.py` starts one component and restarts it if it dies. It knows nothing about
topology, roles or peers, and writes the *component's* pid to the plain pid file so the existing
tools need no changes. `devenv.py --supervised` wires it in, and it is off by default.

Three rules it follows, and each is deliberate:

- **It does not decide leadership.** Starting a process and assigning it a role are two jobs, and
  a supervisor that did both would become mandatory --
  [design_notes.md#ha_supervisor_role](design_notes.md#ha_supervisor_role).
- **It never gives up on a component that cannot start, by default.** `--max-consecutive-failures 0`
  means retry forever, because abandoning a component guarantees there is none, whereas a slow retry
  keeps trying.
- **It gives up on a component that keeps dying, when told to.** `--max-interruptions N` makes it
  give up at the Nth time the component dies and returns, however long it had been running, and
  nothing but starting the launcher again resets the count. `devenv.py --supervised` passes
  `--max-interruptions 2` (`MAX_INTERRUPTIONS_PER_SESSION`), which is the rule in the section below.
  When it gives up, `launch.py` writes `gave up` to `<name>.launcher.state` and exits with status 1.

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

A restart measured in tens of milliseconds is not reachable by replaying anything; only the book
itself living in shared memory and being attached again, rather than rebuilt, could reach it, and
every other step of a restart would have to shrink as well (the table under "The target, measured"
below). The target is therefore set as the lead never moving, not as a number of milliseconds.

## A slow flap is a loss of service, and the instance is judged for it

**Settled, and built in the supervisor; the halt for the last instance is not built.** An instance that dies and returns repeatedly is not a component that keeps
recovering; it is a venue that keeps losing service. The question this answers is what the venue
does about it.

**What makes an interruption uncovered.** A pair exists so that one instance failing costs a
failover rather than an outage. A leader that dies and is restarted within the lease period keeps
the lead, so the pair does not fail over, and the venue has a hole for as long as the restart takes.

**What a member experiences in one.** An order sent into the hole is accepted, sequenced and
held: no execution report, and the member holds something it believes is live that no book
holds. A cancel takes the same path, so an order already placed cannot be withdrawn. And nothing
is said -- the venue only stops accepting after 5 seconds *continuously* without a leading engine,
which a flap never reaches because the clock restarts each time an engine leads. So the member gets repeated silent
windows in which it can neither place nor withdraw, and no signal that anything is wrong.

**Two uncovered interruptions and the instance is fatal.** One is a fault; two is a pattern. The
supervisor stops restarting it: `launch.py --max-interruptions 2` gives up at the second
interruption, writes `gave up` to `<name>.launcher.state` and exits. It counts every time the
component dies and returns, because the supervisor cannot tell whether the peer covered a death;
the rule as designed counts only the deaths the pair did not cover, which is the same thing for a
leader restarted within its lease period. The instance is then absent for longer than a lease
period by construction, so the follower takes over with the arbiter's vote and service resumes. The
supervisor decides only whether to start a process; which instance leads remains a matter for the
lease rules, exactly as [design_notes.md#ha_supervisor_role](design_notes.md#ha_supervisor_role)
requires.

**Why this is needed whatever else is fixed, which is the part worth keeping.** There are two
obvious repairs and neither is sufficient alone:

- Restart each death in place and the flap gives **silent holes**, as above.
- Let the peer take over instead, and each death promotes the peer. The promoted engine keeps the
  book, but **every one of those promotions is a pause of about a lease period in which no engine
  leads**, and every order sent in it waits unanswered. Eight cycles become eight pauses, which for
  a member is a venue that keeps stopping without saying why.

So making recovery faster changes the shape of the damage rather than removing it. What bounds it
is judging the instance, because that turns an open-ended series of interruptions into exactly
one. Fixing BUG-0029 then makes that one interruption short instead of seconds long, which is the
right order to value the two.

**The last instance halts loudly rather than leaving nothing.** Where the cause is something both
instances share -- a poison order, a bad build, a configuration both read -- the peer meets it too
and is judged in its turn. The venue then has no instance of that component and a supervisor that
has stopped trying. That must be announced rather than discovered: a venue quietly running with
nothing behind a component is the failure that looks like health. This is the same shape as
a failure both instances of a pair share, such as the book growing until the machine runs out of memory
([BUG-0028](../bug_list.md#bug_0028)).

For the matching engine, the venue does announce it. When the supervisor of the last matching engine
gives up, the leading sequencer has no engine to send orders to. After five seconds without one, it
tells the gateways to refuse new orders, saying that no matching engine is available. After ten
seconds, the longest the venue recovers by itself, it writes into the WAL the commands that reject
every order it was holding, cancel every order on the book, and halt trading, and it tells the
gateways to refuse orders because the recovery took too long. Every member is sent a report of each
of its orders, and a person decides when trading resumes
([voided_commands.md](voided_commands.md), section 5).

**For every other component, nothing implements this announcement yet.** When the supervisor of the
last instance of a sequencer, a gateway, a publisher or an arbiter gives up, the venue is left without
that component, and only `<name>.launcher.state` says so. The announcement needs the means for the
venue to say anything about itself, described in [Trading phases](../venue/trading_phases.md).

**There is no window, and that is deliberate.** The obvious shape is a lookback -- two deaths
within some period -- and it is rejected. One interruption of this kind is acceptable: members will
complain, and the venue's answer is that there was a failure and recovery took that many seconds,
which is better for them than a halt. That rule does not admit a period. An instance gets **one interruption per trading session**;
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

**Measured, so the figure being traded is known.** An engine goes from dying to leading in about 2
to 2.5 seconds, divided into its steps under "The target, measured" below. That is the length of the
interruption a member is being asked to accept once. The rule does not depend on it, but how
tolerable one interruption is does.

## The target, measured

**The target for a process that dies on a healthy machine is that the lead never moves.** The
process is restarted by its supervisor and is leading again within its lease period of 3 seconds, so
its peer never takes over and no change of leader happens. The interruption is a measured figure,
not a goal.

Measured on 2026-11-08, twice: 20,000 orders over four binary sessions with three in ten cancelled,
leaving 14,004 open orders, then the leading matching engine killed with SIGKILL under `launch.py`.
The times are from the restarted engine's and the sequencers' Quill logs.

| Step | First run | Second run |
|---|---|---|
| The supervisor notices the death and starts the new process | 0.15 s | 0.03 s |
| Process set-up, as far as opening the open-order region | 0.08 s | 0.08 s |
| Reading the region back: a 496 MB file with room for 2,000,000 orders, holding 14,004 | 0.85 s | 0.23 s |
| Starting threads, pinning cores, and a trial of 20 writes to the epoch file | 0.10 s | 0.12 s |
| Recording the new epoch, and the peer granting the lease | 0.11 s | 0.11 s |
| Waiting for the sequencer to connect to the engine again | 1.21 s | 1.54 s |
| Catch-up, 4 records, and taking the lead | 0.004 s | 0.004 s |
| **From death to leading** | **2.50 s** | **2.11 s** |

The lease was granted 1.3 and 0.6 seconds after the death, well inside the lease period. The
largest step is the sequencer's: after losing the engine it tries to connect again only every 2
seconds, so the engine, ready within about a second, waits for it. That is
[BUG-0237](../bug_list.md#bug_0237); fixing it would bring the interruption under a second if the
other steps stay as measured. Reading the region back took 230 ms in one run and 847 ms in the
other, from a file of the same size; whether the pages were already in memory has not been checked.

## What is still open

- **Why does reading the region back vary?** The book lives in a memory-mapped file of fixed-size
  records that survives the process, and a restarted engine reads it back rather than replaying a
  journal ([Open order checkpoint](../durability/open_order_checkpoint.md)). The read is one pass
  over the whole file, so its cost is fixed by the file's size rather than by the number of orders.
  It took 230 ms and 847 ms in two runs with the same file size, and the cause is not known.
- **How long is the lease period?** It sets two things at once: how quickly a follower takes over
  from a leader that has gone, and how long a supervised restart has to bring a leader back before
  its follower takes over instead. It is three seconds; whether one value suits both is open.
- **What happens on a crash loop?** Partly settled above: the instance is judged after two
  uncovered interruptions, and the last one is meant to halt the venue loudly. What that does not answer is
  *why* it was dying. For the matching engine, one cause is handled: a command that crashes the
  engine applying it. An engine that restarts and finds that its previous process died applying a
  command asks the other engine whether it applied that command. If the other engine did not, the
  restarted engine asks the sequencers to void the command. The leading sequencer agrees only when the
  command is a member's new order or cancel, no engine leads, and no engine has reported on that command
  or any later one. It then writes a `VoidCommands` record into the WAL, so that every engine rejects the
  command instead of applying it, and its member is sent the rejection
  ([voided_commands.md](voided_commands.md), section 4). A command the sequencer wrote itself, such as a
  `CancelBook`, is never voided: if it crashes both engines, the supervisors stop restarting them and
  the venue halts at the limit on how long it recovers by itself. Section 7 of the
  decision record proposes the same idea under the name of a poison-pill filter. Nothing does this
  for any other component, and judging the instance bounds the damage without diagnosing it.
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
