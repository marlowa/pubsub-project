# Bug List

| | |
|---|---|
| Bugs recorded | 66 |
| Open | 30 (21 defects, 9 tasks) |
| Closed | 36 |
| Next id | BUG-0067 |

## Open bugs by severity

11 high, 13 medium, 6 low.

| Id | Severity | Kind | Title |
|---|---|---|---|
| [BUG-0009](#bug_0009) | high | defect | The venue accepts orders indefinitely with no matching engine, and tells nobody |
| [BUG-0010](#bug_0010) | high | defect | HA fails over into a condition both nodes share |
| [BUG-0028](#bug_0028) | high | defect | Growing the order book by doubling needs more memory than the machine has |
| [BUG-0029](#bug_0029) | high | defect | A process death on the same host takes the machine-death path |
| [BUG-0056](#bug_0056) | high | defect | The FIX gateway stopped completing logons while still running |
| [BUG-0057](#bug_0057) | high | defect | The sequencer segfaulted on 2026-08-21 and nothing recorded it |
| [BUG-0061](#bug_0061) | high | defect | HA cannot actually be turned off, and the venue silently stops trading |
| [BUG-0062](#bug_0062) | high | defect | Two instances led with HA off, and nothing notices when they are reunited |
| [BUG-0064](#bug_0064) | high | defect | Deferred orders are never recovered, and the venue logs that they were |
| [BUG-0065](#bug_0065) | high | task | The venue has no way to declare a trading halt |
| [BUG-0066](#bug_0066) | high | defect | A flapping matching engine resets the deferral clock, so the venue never stops accepting |
| [BUG-0001](#bug_0001) | medium | defect | Shutdown timeout errors in timer tests |
| [BUG-0002](#bug_0002) | medium | defect | The FIX order gateway's `process_message` exit paths are not audited |
| [BUG-0003](#bug_0003) | medium | defect | Environment placeholders are missing outside dev |
| [BUG-0006](#bug_0006) | medium | defect | ResendRequest under load |
| [BUG-0018](#bug_0018) | medium | defect | The idle-connection reaper tears down the pre-warmed failover link |
| [BUG-0030](#bug_0030) | medium | task | Restart coverage: what ha_test.py exercises, and what it does not |
| [BUG-0040](#bug_0040) | medium | defect | The order-accounting check reports lost orders when it means it could not count them |
| [BUG-0041](#bug_0041) | medium | defect | Five ways the venue will not start on a RHEL8 target host |
| [BUG-0045](#bug_0045) | medium | task | A member has no defined way to discover its primary gateway is down |
| [BUG-0046](#bug_0046) | medium | task | The binary order gateway has no in-flight report recovery |
| [BUG-0047](#bug_0047) | medium | task | Disaster recovery is not modelled |
| [BUG-0048](#bug_0048) | medium | defect | Nothing truncates the WAL, so it grows for the life of the venue |
| [BUG-0059](#bug_0059) | medium | task | No defence against a member reconnecting in a loop with the wrong protocol |
| [BUG-0060](#bug_0060) | medium | task | Microbursts are not measured, and the venue has no story for them |
| [BUG-0004](#bug_0004) | low | defect | Doxygen 1.8.14 turns `\ref` labels into bare directory links |
| [BUG-0005](#bug_0005) | low | defect | fix-test-client reports a dead gateway poorly |
| [BUG-0014](#bug_0014) | low | defect | Python style warnings across the top-level scripts, and a lint gate that ignores them |
| [BUG-0050](#bug_0050) | low | task | Doxygen 1.8.14 cannot build the documentation with warnings as errors |
| [BUG-0058](#bug_0058) | low | task | A member halted by a sequence gap is invisible to monitoring |

---

## Conventions

Defects found and not yet fixed, and defects fixed recently enough to be worth remembering.

**Why this file exists.** BUG-0007, the metrics-inside-CPU-pinning defect, was found on 2026-08-04,
judged not worth fixing at that moment, and then forgotten — it survived only in a working note
nobody else could see, and had to be rediscovered on 2026-08-08 before anything was done about it.
A defect that is known and invisible is worse than one nobody has found: the project carries the
risk without carrying the knowledge.

**Every entry records the date it was found and how it was found.** The second is the more useful
half. "Found by the trading-day load run" tells you which activity is worth repeating; "found by
reading the code" tells you the tests would not have caught it.

Closed entries are kept for one release cycle and then deleted -- the commit is the permanent
record. **An id is permanent and is never reused**, so a citation from the code always resolves.

**Most entries are defects. A few are tracked tasks**, marked with a `Kind` row; an entry with no
`Kind` row is a defect. A task is kept here rather than in a list of its own, deliberately: a
separate list is opened less often than this one, and an item nobody reads is the failure this
file was created to prevent. BUG-0030 is the current example -- a coverage matrix with four of
eighteen cells done, which belongs in neither Open-as-a-defect nor Closed-as-finished without
saying which it is.

**Severity** is about what the defect can do to the venue, not how hard it is to fix.

- **high** -- can lose or corrupt orders, stop the venue trading, or leave an operator believing
  one of those did not happen when it did.
- **medium** -- degrades operation, costs a run or recovery time, or leaves a risk unbounded,
  without losing orders by itself.
- **low** -- documentation, tooling ergonomics, or contained to a developer workflow.

## Two shapes that keep coming back {#bug_list_shapes}

Noticed on 2026-08-28, after a week in which the open count rose faster than it fell. Almost
everything found was one of two shapes, which is worth saying because a habit can be fixed and
thirty separate defects cannot.

### A claim asserted, and never checked

A component states a consequence it does not verify, and the statement is then believed -- by an
operator reading a log, by a later reader of a document, by whoever wrote the next thing on top
of it.

| | |
|---|---|
| [BUG-0009](#bug_0009) | `dropped=0` stayed true while 924,000 orders went nowhere. Nothing was dropped; it just was not the question anybody needed answered |
| [BUG-0064](#bug_0064) | The sequencer logs that deferred orders "are recovered by its WAL replay". Nothing checks, and on a cold start nothing recovers them |
| [BUG-0066](#bug_0066) | A connection is treated as recovery. "Reachable again" says nothing about whether anything was applied |
| [BUG-0015](#bug_0015) | `deploy.py` reported doing nothing in the same words as doing something |
| [BUG-0063](#bug_0063) | `check_docs.py` reported the tree consistent while the build was failing on the very links it had approved |

**What to do about it.** Do not state a consequence you have not checked. Either verify it -- the
sequencer knows which sequence numbers it deferred and can ask whether they were applied -- or say
only what was observed: "a matching engine has connected" is true and useful, where "the orders are
recovered" is neither. The wording is not a detail; it is the whole of what the reader takes away.

### Two things that must agree, and only one was changed

The same value, or the same rule, kept in more than one place. Every instance was found by the two
disagreeing in production rather than by anyone noticing the duplication.

| | |
|---|---|
| [BUG-0061](#bug_0061) | Four places held `ha_enabled`. Turning high availability off changed one of them |
| [BUG-0062](#bug_0062) | The reasoning was checked against the branch and not against the value reaching it, so "not split brain" was true of the code and false of the configuration |
| [BUG-0063](#bug_0063) | Two gates disagreed about what a valid link is, and the cheap one granted what the expensive one withheld |
| [BUG-0055](#bug_0055) | A member's sequence reset reached the gateway and not the sequencer |

**What to do about it.** One value, one place, and everything else derives from it. BUG-0061's fix
is the worked example: the venue-wide `[ha] enabled` already existed, and the repair was to make
nine component configs expand it rather than to add a tenth place able to disagree. When a second
place is genuinely unavoidable, the check that they still agree belongs in the build, not in
somebody's memory.

### Why both shapes are found late

Neither produces an error. A venue reporting `dropped=0` through an outage, a gate reporting a
consistent tree, a config saying high availability is on while the flag says off -- all of these
look exactly like everything working. They are found by measuring something, and never by reading
the code that contains them, which is why the entries above are dated to the days when somebody
went looking.

---

## See Also

- [Roadmap](roadmap.md) — planned work, as distinct from defects
- [Testing and Code Coverage](orientation/testing.md)

---

## Open

### BUG-0064: Deferred orders are never recovered, and the venue logs that they were {#bug_0064}

| | |
|---|---|
| Severity | high |
| Found | 2026-08-28 |
| Recorded | 2026-08-28 |
| How | Andrew asking what becomes of a deferred order if the matching engine never starts, while reviewing [BUG-0009](#bug_0009)'s fix; scope then settled by measuring a routine failover |
| Impact | Orders deferred while every matching engine is down are silently lost when one starts cold: no execution report, no rejection, nothing to cancel -- and the sequencer logs that they were recovered. A routine failover is unaffected (measured) |

**Measured, not reasoned.** Three orders were placed into a venue with both matching engines
killed. They were WAL-committed and deferred, as designed. A matching engine was then restarted; it
adopted the leader role at epoch 301; and:

- the engine's log contains **no WAL line and no order at all** -- it replayed nothing;
- the gateway's `awaiting` stayed at **3**, where it remains;
- the member received no execution report and no rejection, then or since.

The sequencer nevertheless logged:

```
SequencerThread: a matching engine is reachable again after 22s -- 3 order(s) were deferred
and are recovered by its WAL replay
```

**The recovery it asserts did not happen.** That line is this project's own, written as part of
BUG-0009 step 1, and it states a consequence it does not verify.

**The claim is older than that line.** The deferral policy has always rested on it -- the
per-order message it replaced said `recovered via WAL replay on ME promotion`, and BUG-0009 quoted
that as the reason deferring was safe. It is the assumption the whole policy stands on, and it was
never true for this path.

**Why nothing catches it.** A deferred order is never sent to any matching engine, so no engine
holds it and replication between the pair cannot supply it. The route back is the sequencer sending
what the engine has not applied -- which happens on promotion and not on a start, as below.

**And the member cannot act.** An order joins the gateway's open-order set only when an execution
report arrives for it (`FixOrderGatewayThread.cpp:751`). A deferred order never produces one, so it
is never tracked -- which means cancel-on-disconnect cannot reach it either. The member has no
report to reconcile against and no order to cancel.

**What [BUG-0009](#bug_0009) did and did not do.** It bounds how many orders can join this state,
tells the member when the venue stops accepting, and refuses rather than deferring once an outage
outlives a plausible failover. All of that stands. It explicitly did not rescue the orders already
deferred, and recorded that as out of scope -- **on the understanding that they were recovered by
replay, which is what this entry disproves.**

**Not yet designed, and the shape is not obvious.** Rejecting deferred orders to the member is not
simply the answer: the records are in the WAL, and a replay run afterwards would process orders the
member has been told are dead. Any fix has to make the two agree -- either replay becomes something
that reliably happens and is verified, or the deferred records are positively resolved before
anything can replay them. The first question to settle is which of those the venue is promising.

**Measured 2026-08-28: a routine failover does NOT lose them.** This was the open question and it
is now answered, which bounds the entry considerably. With the secondary already running, the
primary was killed under a live FIX session and orders kept flowing across the gap. The sequencer
deferred **27** of them over a 14-second outage, and every one of the 88 orders sent was answered:

```
RESULT: sent=88  answered=88  never answered=0
```

The promoted secondary recovered them by **reconciliation**, not by anything resembling a replay
run:

```
MatchingEngineThread: entering RECONCILING (last_replicated_seq_no=47549974, book_size=...)
MatchingEngineThread: MePositionAck received -- book reconciled to seq_no=47550003
```

It reports where its replica reached and the sequencer sends everything after it -- 29 records,
covering the 27 deferred orders.

**Root cause: a cold start never enters reconciliation at all.** `MatchingEngineThread.cpp:1108`,
in `handle_arbitration_decision`:

```cpp
if (ha_role_state_ == MeRole::Follower) {
    begin_reconciliation();   // catch up on the WAL before accepting anything
} else {
    adopt_leader_role();      // "Nothing to take over. This is a start rather than a promotion"
}
```

An instance that was never a Follower takes the second branch and applies nothing. The comment is
right that no replica book was maintained there and wrong that this means there is nothing to take
over: the WAL may hold orders no engine has ever seen, which is exactly the state a deferral
creates.

**The rule it follows was written on purpose, and undoing it needs care.** That branch is
[BUG-0043](#bug_0043)'s fix -- a cold-start instance routed through reconciliation waited on a
connection that never arrived and the venue came up with no matching engine leading. So the
question is not whether to reconcile on a cold start but **what the test should be**. "Was I a
Follower?" is a proxy for "is there anything I have not applied?", and it is the wrong proxy: those
differ precisely when the venue deferred orders and then lost every engine. Note that
`begin_reconciliation` already handles the arriving-connection case -- it waits for a sequencer
order connection and re-enters when one appears -- so the stranding BUG-0043 describes may no
longer be a consequence of reconciling on a start. That needs checking before anything is changed.

**Designed around, 2026-08-28, in
[Knowing there is no matching engine](availability/matching_engine_presence.md).** That note does
not fix this entry -- a cold start still applies nothing -- but it reduces how many orders reach
this state, by letting the sequencer learn from the arbiter that no engine exists instead of
waiting 45 seconds to guess.

**What this narrows.** Ordinary HA operation does not lose orders. What loses them is losing every
matching engine and starting one cold -- which is exactly the incident behind
[BUG-0009](#bug_0009), where an engine was promoted, died two minutes later, and nothing existed
after that.

Related: [BUG-0048](#bug_0048), since nothing truncates the WAL and these records live in it
indefinitely. [BUG-0010](#bug_0010), since both concern a promotion assumed to put things right.

---

### BUG-0065: The venue has no way to declare a trading halt {#bug_0065}

| | |
|---|---|
| Severity | high |
| Kind | task |
| Found | 2026-08-28 |
| Recorded | 2026-08-28 |
| How | Andrew observing that members will not put up with "service not available" indefinitely, while reviewing the escalation design for [BUG-0009](#bug_0009) |
| Impact | The venue cannot tell members trading has stopped, for any reason. A prolonged outage reaches them only as an unbounded stream of per-order rejections, which invites retries and never says "stop asking" |

There is no notion of a trading halt anywhere in this project -- not in the code, not in the
documentation. `TradingSessionStatus` (FIX 35=h) is neither sent nor understood, and the venue has
no state meaning "trading has stopped" as distinct from "this order cannot be processed".

**How it works in practice, from a venue Andrew has operated:** the matching engine holds a halted
state, and a member placing an order is told so in the FIX reply. That is the mechanism that must
exist here too -- it answers the question the member actually asked, and it reaches the member who
is trading. A broadcast should exist as well rather than instead: the reply leaves a connected but
idle member unaware, and leaves a member reconnecting into a halt to find out by trial.

**A halt must be rare.** Also from practice: halts get declared at the drop of a hat, and a venue
that halts readily trains its members to ignore the signal. That is the reason the escalation ladder
puts two automatic, self-clearing states beneath it -- everything recoverable is recovered before a
halt is reached. The error to avoid is not automating too little but automating so completely that
no halt is possible.

**It is needed for ordinary reasons before it is needed for failures.** A venue halts for a
scheduled pause, a market-wide event, an instrument suspension, an operator decision. None of those
can be expressed today.

**And it is what the escalation ladder needs at the top.** See
[design notes 15](availability/design_notes.md#ha_recovery_ends_at_loss). Deferring and refusing are
both automatic and both self-clearing, which is right while nothing has been lost. Once orders have
been stranded -- [BUG-0064](#bug_0064) -- resuming automatically conceals the damage, and the venue
needs a state that does not lift by itself.

**Halting is not refusing, and the two must not be conflated.** Refusing is a statement about
capacity: this order cannot be processed now, and the venue will accept again on its own. Halting is
a statement about the venue's condition: trading has stopped, it will not restart by itself, and a
person is dealing with it. A member reads them differently and should.

What a design has to settle:

- **What declares it.** An operator certainly. Automatically on a condition -- no matching engine
  service, orders known stranded -- is the harder question, and it is the one that decides whether
  a halt can ever be entered without a person.
- **What lifts it.** A person, on the argument in design notes 15. Whether anything may lift it
  automatically is a separate decision and the answer is probably no.
- **Scope.** Venue-wide, or per instrument. Real exchanges do both, and per-instrument is what an
  instrument suspension needs.
- **Where the state lives, which is not obvious here.** Holding it in the matching engine is natural
  when the engine is up and the halt is a trading decision. It cannot be the whole answer for this
  venue: the condition that prompted the ladder is *there is no matching engine*, so a halt entered
  for that reason must be held by something outliving the engine -- the sequencer, the gateways, or
  both -- or it vanishes exactly when it is needed.
- **What a halted venue does with an order.** Reject with a halt reason is the obvious answer, but
  a member's cancels are a different case: a member may reasonably want to withdraw orders during a
  halt, and whether the venue can honour that depends on why it halted.
- **What members are told on logon during a halt**, so a member connecting into one is not left to
  discover it by sending an order.

Related: [BUG-0009](#bug_0009) and [BUG-0064](#bug_0064), which is what made the top of the ladder
necessary rather than tidy. [BUG-0061](#bug_0061), since a non-HA venue has no arbiter and reaches
these states differently.

---

### BUG-0066: A flapping matching engine resets the deferral clock, so the venue never stops accepting {#bug_0066}

| | |
|---|---|
| Severity | high |
| Found | 2026-08-28 |
| Recorded | 2026-08-28 |
| How | Andrew asking what happens when the launcher restarts a component that keeps dying, then measuring it |
| Impact | Each restart cycle strands the orders deferred during it, while orders on either side go through normally. The venue looks half-healthy because it is, and reports recovery on every cycle. [BUG-0009](#bug_0009)'s refusal never fires |

**Measured.** A matching engine was killed and restarted on a 24-second cycle, eight times, with
orders flowing. The sequencer's deferral clock restarted on every reconnect:

```
a matching engine is reachable again after 7s ... after 10s ... after 2s ... after 5s
                                  after 8s ... after 11s ... after 3s ... after 6s
```

It never approached the 45-second refusal threshold. **Across roughly 190 seconds in which the venue
could not process a single order, it refused nothing** -- `refused=0` -- and finished with
`awaiting=16`: sixteen orders taken from a member and never answered.

`note_matching_engine_reachable` clears `deferring_orders_` and zeroes `deferred_order_count_` when
a connection is established. **Connectivity is treated as recovery.** For a real promotion that is
right, and it was measured working. For a flap it is false, and it resets the only mechanism that
would eventually have told the member anything.

**Every one of those eight cycles also lied.** The recovery line says the deferred orders "are
recovered by its WAL replay". Each restart is a cold start, and per [BUG-0064](#bug_0064) a cold
start reconciles nothing -- so the orders were not recovered, eight times, in writing.

**The launcher does not catch this either, and its definition of flapping is why.**
`launch.py --minimum-runtime` defaults to 2 seconds: a child dying inside that counts as a failed
start and earns a `--failure-sleep`. A child that survives ten seconds and then dies is not a failed
start at all -- `consecutive_failures` resets to zero and it is restarted at once, indefinitely.
**Flapping on any period longer than two seconds is invisible to the supervisor.**

#### The fix, corrected 2026-08-28 before it was built

**The first fix recorded here was wrong, and working it out is worth keeping.** It proposed ending
the deferral on a successful forward rather than on a connection -- "recovery evidenced by progress,
not connectivity" -- so that a flapping engine forwarding nothing would show an ever-growing age.

Two things defeat it. **It deadlocks:** if a connection does not restore acceptance, the gateway
refuses every order, so no order is ever forwarded, so the deferral never clears and the venue never
reopens. And more fundamentally, **forwarding did resume on every cycle.** The measured run answered
18 of 34 orders: during each up phase orders flowed normally. A progress-driven clock would have
been cleared by those forwards exactly as the connection-driven one was.

**The 16 stranded orders were not stranded because forwarding stopped. They were stranded because
each restart applied nothing.** Every cycle deferred a few orders, the engine came back as a cold
start, reconciled nothing, and those particular orders were lost while the next ones went through
normally. The venue looked half-healthy throughout because it *was* half-healthy.

**So this is [BUG-0064](#bug_0064) happening repeatedly, and BUG-0064's fix is this one's fix.** A
cold-starting leader that reconciled -- reporting its position and being sent what it has not
applied -- would pick up each cycle's deferred orders like a promoting follower already does. The
deferral clock is a red herring.

**What remains specific to flapping**, once BUG-0064 is fixed: an engine that dies faster than
reconciliation completes accumulates orders no cycle ever applies. The sequencer knows the sequence
numbers it deferred and the engine's applied position is already exchanged during reconciliation, so
it can tell -- **the venue should clear a deferral only when the deferred range is known to have
been applied**, rather than when an engine appears or when forwarding resumes. That is the honest
version of "evidenced by progress", and it is a check on the orders themselves rather than on a
proxy for them.

**And the launcher's flap detection should widen** to restarts within a rolling window rather than
consecutive starts shorter than `--minimum-runtime`. It already writes a restart count to
`<name>.launcher.state`, so the raw material is there.

**With high availability off this should halt the venue** rather than merely refuse. See
[design notes 15](availability/design_notes.md#ha_recovery_ends_at_loss) and
[BUG-0065](#bug_0065): with no peer, every restart is a cold start, so every cycle strands what was
deferred, and a venue that resumes automatically conceals it.

Related: [BUG-0009](#bug_0009), whose refusal this defeats. [BUG-0064](#bug_0064), which is why the
repetition costs orders rather than merely time. [BUG-0029](#bug_0029), on the supervision grace
period.

---

### BUG-0057: The sequencer segfaulted on 2026-08-21 and nothing recorded it {#bug_0057}

| | |
|---|---|
| Severity | high |
| Found | 2026-08-28 |
| Recorded | 2026-08-28 |
| How | Looking for a core dump while investigating [BUG-0056](#bug_0056), and finding several that had nothing to do with it |
| Impact | Unknown, and that is the problem. The venue crashed, carried on being developed for a week, and no entry, session note or commit mentions it |

**Not investigated. Deliberately parked** so it does not derail the FIX session-layer work, and
recorded now because the artefact is perishable and the knowledge nearly was.

`coredumpctl` lists **three crashes of this project's own binaries**, none of which appears in this
file, in the session log, or in any commit message:

| When | Signal | Binary | Core |
|---|---|---|---|
| 2026-07-24 11:17 | SIGABRT | `pubsub_itc_fw_integration_tests` | rotated away |
| 2026-08-08 13:16 | SIGSEGV | `matching_engine` | rotated away |
| **2026-08-21 19:04:44** | **SIGSEGV** | **`installed/bin/sequencer`** | **kept -- 227.7K** |

**The timing of the third is the part to look at first.** It is four minutes before the OOM kill
recorded in [BUG-0028](#bug_0028): *"matching_engine_primary OOM-killed at 19:08:41 with 10.3 GB
resident"*. So during the trading-day run that ended in the OOM killer, **the sequencer segfaulted
first**, and BUG-0028 records only the matching engine. Whether the two are the same story --
memory exhaustion reaching the sequencer first -- or two separate faults is exactly what has not
been established. A SIGSEGV is not how the OOM killer announces itself; it sends SIGKILL.

**The core has been preserved** at `~/mystuff/cores/`, outside the repository, because
systemd-coredump rotates these away and two of the three are already gone.

**The matching binary is almost certainly lost**, which is the practical obstacle. `installed/bin/
sequencer` has been rebuilt many times since; the tree as of that crash was around commit
`b113fca`. A rebuild from there would be close but not necessarily byte-identical, so line numbers
from a backtrace should be treated as indicative rather than exact.

**Worth doing regardless of what the core says:** nothing noticed. The venue crashed during a
measured run and the fact reached no log this file reads, no session note, and no commit. Whatever
the cause, a crash that leaves no trace anyone would encounter is its own defect.

### BUG-0058: A member halted by a sequence gap is invisible to monitoring {#bug_0058}

| | |
|---|---|
| Severity | low |
| Kind | task -- observability the design called for and did not get |
| Found | 2026-08-28 |
| Recorded | 2026-08-28 |
| How | Building step 3 of BUG-0038; the design asks for a gap-age metric and only the log line was built |
| Impact | A member whose order flow has stopped is visible only to someone reading the gateway log at the time |

When a member's numbering gaps, the venue stops processing its orders until the gap is filled --
for up to fifteen seconds, and then the session ends. That is deliberate and correct, and it is
also **exactly the condition an operator would want to see without going looking.**

Today it produces a WARNING per request and an ERROR on the Logout, and nothing else. Nobody
watching a dashboard would know. [BUG-0009](#bug_0009) is the same shape and the reason this is
worth recording rather than shrugging at: the sequencer knew for seven minutes that it had no
matching engine, said so a million times at INFO, and told nobody who could act.

**What is wanted** is the age of the oldest open gap, per gateway, as a gauge -- so a member stuck
in this state shows up as a number that climbs rather than as a line in a file. The count of
sessions currently waiting on a resend would be the natural companion.

Not built because it needs a gauge registered through the reactor's metrics registry rather than a
constant in `GatewayMetrics.hpp`, which is where the gateway's existing metrics constants live, and
that is a larger change than the checking it would observe. See
[Inbound sequence checking](fix/inbound_sequence_checking.md).

### BUG-0062: Two instances led with HA off, and nothing notices when they are reunited {#bug_0062}

| | |
|---|---|
| Severity | high |
| Found | 2026-08-28 |
| Recorded | 2026-08-28 |
| How | Reasoning through what HA-off should mean while recording [BUG-0061](#bug_0061) |
| Impact | Two WALs and two epoch sequences that both look authoritative. Nothing detects it, and the harm lands at a later start rather than at the time |

**Recorded on its own rather than left inside BUG-0061, because fixing BUG-0061 is what makes this
reachable.** Today HA cannot properly be turned off, so this cannot happen by that route. Make it
possible -- which is what BUG-0061 asks for, and rightly -- and it can.

With HA off, a primary and a secondary can both be started and both lead. That is the agreed
behaviour and it is right: role means nothing without a peer or an arbiter, and refusing would
block running a venue on the surviving machine after the primary's hardware has died.

**While it lasts, nothing is wrong.** The gateway reaches only one sequencer with HA off --
`forward_pdu_to_sequencers` sends to the secondary only inside `if (config_.ha_enabled)` -- so the
same order cannot enter two books. It is not split brain.

**Correction, 2026-08-28: that was true of the code and false of the configuration.** The gateway's
`ha_enabled` lived under `[sequencer]` in its own template and was **hardcoded `true`**, so it never
followed the venue switch at all. With high availability off the gateway went on sending to both
sequencers, and two instances leading would have put the same order into two books -- real split
brain, not the deferred trap described above. The reasoning was checked against the branch and not
against the value reaching it.

Fixed while completing [BUG-0061](#bug_0061): every component config now expands `${ha_enabled}`
from the one `[ha] enabled`, so the gateway follows the venue and the paragraph above is true as
written. There were **four** places able to disagree, and this was the one that mattered most.

**The damage is deferred, which is what makes it nasty.** Both instances advance state that is
meant to be single-valued:

- **Each grows its own WAL**, from its own sequence numbers, with no relationship between them.
- **Each burns leadership epochs** from the `epoch_state_file`, which exists precisely so that a
  restart cannot reuse a generation the venue has already spent.

Nothing records that this happened. When someone later starts the pair **with HA on** -- the
obvious thing to do once the failed machine is back -- the venue has two divergent histories and
two epoch sequences, and no mechanism compares them. The arbiter arbitrates leadership; it does not
ask whether the two candidates have been leading separately since Tuesday.

**What is wanted is detection, not prevention.** Preventing it would block the legitimate case. The
venue should be able to tell, when instances come together, that they have diverged -- and halt
rather than pick one. Halting is the venue's established answer to exactly this class of thing:
mid-segment WAL corruption, both arbiter halves unreachable, and snapshot validation failure on the
only snapshot all halt rather than guess. See the decision log in [Roadmap](roadmap.md).

Where to look first, none of it investigated: whether a WAL carries anything identifying the
instance and generation that wrote it; whether the epoch state file records enough to spot a
sequence that has advanced without this instance's involvement; and what a component should do on
finding it -- refuse to join, most likely, and say so in terms an operator can act on.

Related: [BUG-0042](#bug_0042), closed, where a restarted primary matching engine promoted itself
and produced two leaders. Different mechanism, same underlying fact -- that two instances each
believing themselves leader is not something the venue currently detects after the event.

### BUG-0061: HA cannot actually be turned off, and the venue silently stops trading {#bug_0061}

| | |
|---|---|
| Severity | high |
| Found | 2026-08-28 |
| Recorded | 2026-08-28 |
| How | Using `devenv.py --no-ha` to set up a matching-engine outage while building BUG-0009's step 1, and finding no orders moved |
| Impact | A venue started with HA disabled accepts orders, acknowledges them, and forwards none. It looks healthy: every process is up and nothing is logged as wrong |

**`devenv.py --no-ha` produces a venue that cannot trade**, and the way it fails is quiet.

The flag decides *which components to launch* and nothing else. It skips everything marked
`ha_only`, so the arbiters do not start -- and it never touches the deployed configuration, which
still says `ha_enabled = true`. The sequencer therefore takes its HA path, arms a startup election
timeout, and waits for an arbiter that will never exist. It never becomes leader, so every order
returns on the `role_ != leader` branch and is never forwarded. The member is acknowledged
regardless.

**There is no way to turn HA off properly either**, which is the part that makes this more than a
flag bug. The mechanism exists and the sequencer was simply never wired to it:

| | |
|---|---|
| Matching engine | `applications/matching_engine/matching_engine_primary.toml` has `enabled = ${matching_engine_ha_enabled}`, filled from `[matching_engine] ha_enabled` in the environment file |
| Sequencer | `applications/sequencer/sequencer_primary.toml` has `ha_enabled = true`, hardcoded. There is no `[sequencer]` section in `environments/dev.toml` at all |

So an operator can turn HA off for the matching engine by editing the environment and redeploying,
and cannot do the same for the sequencer by any means short of editing the installed file by hand.

**The sequencer already has the behaviour that is wanted.** `SequencerThread` reads
`config_.ha_enabled` and, when it is false, logs *"ha_enabled=false -- starting as leader
immediately"*. Nothing needs designing; the value simply never arrives. With no peer and no
arbiter, a lone primary leading immediately is the obviously correct thing, and the code already
says so.

**Fix, in the order that makes each step useful:** give the sequencer template a
`${sequencer_ha_enabled}` placeholder and the environment a `[sequencer]` section to fill it, so HA
can be turned off deliberately. Then make `devenv.py --no-ha` consistent -- either by having it
refuse to start a venue whose deployed configuration disagrees with it, or by removing the flag in
favour of the environment file, which is the single source of truth everything else already uses.

**Built 2026-08-28, and NOT the way this paragraph proposed.** A per-component `[sequencer]` section
would have been a *third* place able to disagree, which is the shape of this bug rather than a fix
for it. `[ha] enabled` already existed as the venue-wide switch and `devenv.py` already read it, so
the sequencer templates and the matching engine's now expand `${ha_enabled}` from that one value,
and `[matching_engine] ha_enabled` is gone. One switch, one meaning, everywhere.

Verified end to end with `[ha] enabled = false`: no arbiter and no witness start, the sequencer logs
*"ha_enabled=false -- starting as leader immediately"*, and a member's orders come back
`OrdStatus=0`. That is the first time a venue with high availability off has traded.

**And it was more than the sequencer.** The gateways, the binary gateways and the matching-engine
publishers all carried a hardcoded `ha_enabled = true` of their own, so nine component configs in
total now expand `${ha_enabled}` from the one venue switch. The gateway's was the consequential one:
see the correction in [BUG-0062](#bug_0062), where it meant that two sequencers leading with high
availability off would have been genuine split brain rather than the deferred trap that entry
describes.

**Also built:** `devenv.py --no-ha` now refuses when the environment still says `[ha] enabled = true`,
naming the fix, rather than starting a venue whose configs expect components it will not launch. The
arbiter and the witness refuse to start when the switch is false and say why -- not launching them
is what `devenv.py` does, and refusing is what covers a hand-started process or a stale supervisor
manifest.

**Still open here:** a secondary started alone with high availability off leads but cannot be
reached, because the gateway then talks only to its primary. Making it send to both would recreate
the split brain corrected above, so what a non-HA venue needs is to be told *which* single sequencer
to use -- a configuration decision this entry does not settle. Scenario 46 asserts the gap rather
than hiding it.

**Related in shape to several found this week:** two places that have to agree, only one of which
is updated. See [BUG-0055](#bug_0055), where a member's sequence reset reached the gateway and not
the sequencer.

#### What "HA off" should mean, agreed 2026-08-28

Settled while recording this, because the fix above is not worth building without knowing what it
is aiming at. **None of it is built.**

- **Every primary starts, sees that it is primary and that HA is off, and leads immediately.** The
  sequencer already does exactly this; it is only the configuration that never reaches it.
- **A secondary started with HA off also leads, and says loudly that it is doing so.** Role stops
  meaning anything without a peer or an arbiter -- "secondary" then names only which file was used
  to start it. Refusing would block the case someone actually reaches for HA off to do: run a venue
  on the surviving machine after the primary's hardware has died.
- **The arbiter and the witness refuse to start with HA off, and say why.** A running arbiter in a
  venue that has disowned arbitration is something an operator will later trust.

**Two instances leading at once is not split brain, and is still a trap.** With HA off the gateway
reaches only one sequencer -- `forward_pdu_to_sequencers` sends to the secondary only inside
`if (config_.ha_enabled)` -- so the same order cannot enter two books. But both instances advance
state independently: each grows its own WAL, and each burns leadership epochs from the
`epoch_state_file` that exists precisely so a restart cannot reuse a spent generation. Nothing is
wrong while it lasts. The damage is deferred to the next time someone starts them together under
HA, and **is recorded separately as [BUG-0062](#bug_0062) -- because fixing this entry is what
makes that one reachable.**

**And a secondary started while the gateway still points at the primary trades nothing at all.**
The gateway logs *"primary sequencer not connected"* and the orders go nowhere -- accepted,
acknowledged, forwarded to no one, which is [BUG-0009](#bug_0009) arriving by another road.

#### Scenarios that should exist and do not

`ha_test.py` has no HA-off coverage at all, which is why this survived. Wanted:

- HA off: every primary starts, leads, and the venue trades end to end.
- HA off: the arbiter refuses to start, with a message naming the reason.
- HA off: the witness refuses likewise.
- HA off: a secondary started alone leads and trades.
- HA off: a secondary started while a primary is already leading -- both lead, and the venue says
  so loudly enough that an operator would notice before the next HA start.

**It gates more than itself, noted 2026-08-28.** With high availability off there is no arbiter, so
[Knowing there is no matching engine](availability/matching_engine_presence.md) falls back on the
45-second age threshold -- which is how a member would find out that the venue cannot process its
orders. But deferral is counted inside the leader branch of the sequencer's forward path, so a
sequencer that never adopts leadership never defers, never refuses, and tells nobody anything. Until
this entry is fixed, a non-HA venue that loses its matching engine informs its members of nothing at
all.

### BUG-0059: No defence against a member reconnecting in a loop with the wrong protocol {#bug_0059}

| | |
|---|---|
| Severity | medium |
| Kind | task -- a class of abuse the venue has no answer for |
| Found | 2026-08-28 |
| Recorded | 2026-08-28 |
| How | Raised from operational experience: members really do this, repeatedly |
| Impact | A misconfigured member can open connections as fast as it likes, and each one costs the venue accept, session setup and teardown |

A member configured for **FIX 4.x against a FIX 5.0 / FIXT.1.1 venue** fails the preamble check and
is disconnected, which is correct. What it then does in practice is **reconnect immediately, and
keep doing so**, because the misconfiguration is on its side and nothing tells it to stop.

The venue has no answer for that. Each attempt costs an accept, a `FixSession` construction, a
logon timer, and a teardown; nothing counts attempts per peer, nothing backs off, and nothing
refuses a source that has just failed the same way fifty times. It is not malicious and does not
need to be -- an ordinary misconfiguration produces it, and the effect on a venue is the same.

**This is a denial-of-service question, not a FIX one**, and it belongs with two others of the same
kind: a connection that opens and sends nothing (the logon timeout covers that one), and a member
that connects and then does not read (measured 2026-08-28 as harmless up to 6,000 unread messages,
so the venue does not block on it -- but the buffering is not free either).

#### The answer is graduated, not a blacklist

Worth stating before anyone builds a switch, because "block the member" is the obvious response and
is the wrong first one.

Real venues escalate, roughly in this order:

| | |
|---|---|
| **Throttle** | per-session message rate limits -- orders per second, often with a separate cap on session-level messages. Exceeding it earns rejects, then disconnection |
| **Order-to-trade ratios** | charging for or penalising messaging out of proportion to executions; the main economic lever against a member that floods |
| **Logon attempt limits** | refusing further logons for a period after repeated failures; a configuration option in most FIX engines |
| **Comp id suspension** | disabling a member's credentials -- normally an operations action, not an automatic one |
| **Kill functionality** | cutting the member off *and* pulling its resting orders |

**It is also a regulatory obligation for a real venue, not merely good engineering.** MiFID II
requires trading venues to throttle, to limit order-to-trade ratios, and to have kill
functionality; the SEC's Market Access Rule puts pre-trade risk controls on brokers providing
market access. The shape of that is solid; the specific article numbers are not stated here on
purpose, because they were not checked.

**Why graduation matters for the case this entry is about.** A member configured for FIX 4.x is not
attacking anything. It is misconfigured, and it is someone's real trading connection. So: count per
peer, back off, log loudly, expose it as a metric -- and make a hard block a deliberate act by an
operator rather than something the venue decides on its own. Wrongly locking out a legitimate
member is its own serious event.

**The deliberate act already has a route through this venue, which makes the operations half much
cheaper than it sounds.** Per-comp-id settings already travel from the database through
`export_credentials` into `credentials.toml`, reach the authentication service, and arrive at the
gateway on `AuthenticationResult` -- that is how cancel-on-disconnect settings and gateway pinning
work today. *"This comp id is suspended"* rides the same path, and the gateway already refuses a
session whose provisioning does not name it -- `ha_test.py` scenario 20 tests exactly that refusal.

What is genuinely missing is the automatic half: something that counts per peer, decides when a
count is excessive, and acts on it without an operator watching. Nothing here is built.

### BUG-0060: Microbursts are not measured, and the venue has no story for them {#bug_0060}

| | |
|---|---|
| Severity | medium |
| Kind | task -- a performance characteristic nothing currently reports |
| Found | 2026-08-28 |
| Recorded | 2026-08-28 |
| How | Raised from operational experience: short intense bursts degrade the requests that follow them |
| Impact | Latency for orders arriving just after a burst is worse than any figure the venue reports, and nothing shows it |

A **microburst** is a short, intense arrival spike -- far above the average rate, over in
milliseconds. Its cost is not paid by the burst itself but by **what arrives just afterwards**:
queues are still draining, caches and allocators are still recovering, and an order that arrives in
that shadow is served worse than the same order a second later.

**Nothing in the venue reports this.** Averages hide it by construction, and even a latency
histogram over a whole run mixes the shadow in with everything else, so the tail it produces is
attributed to nothing in particular. The trading-day runs measure throughput and totals; a burst
that doubled the latency of the next two hundred orders would leave no mark on either.

Related to, but not the same as, the Prometheus work in the roadmap's item 16. That gives per-phase
histograms, which is the instrument -- this is the question to ask with it: **latency conditioned
on recent arrival rate**, rather than latency overall.

Two things worth settling when it is picked up: how a burst is defined for this venue (a rate over
a window, and which window), and whether the venue should do anything about one or merely report
it. Reporting first is the safer order.

### BUG-0056: The FIX gateway stopped completing logons while still running {#bug_0056}

| | |
|---|---|
| Severity | high |
| Found | 2026-08-28 |
| Recorded | 2026-08-28 |
| How | Ad-hoc sessions driven by `scripts/fix_raw_client.py` while building BUG-0038's step 3 |
| Impact | The gateway accepted TCP connections and answered none of them. It looked healthy: the process was up, the reactor was running, and nothing was logged as wrong |

**Not reproduced, and the evidence is gone** -- the logs were overwritten by a later run before
they were preserved. What follows is what was observed at the time, recorded because a defect that
is known and invisible is worse than one nobody has found, and because the venue looked *fine*
throughout.

**What was seen.** After a series of ad-hoc client sessions -- several of which the venue itself
ended (a too-low sequence number, an unanswered `ResendRequest`), and several of which exited on a
Python exception without closing the socket -- a new client could open a TCP connection and then
got nothing. No Logon reply, and **no `FIX client connection N established` line in the gateway
log at all**, so the connection never reached the application thread.

**The process was alive and did not crash.** `devenv.py status` showed `fix_order_gateway_a`
running. There is no core dump: the newest on the machine is from 2026-08-21. The reactor thread
was still logging `Reactor created timer` every two seconds throughout.

**The application thread had gone quiet.** The last line from the `FixOrderGatewayThread` thread
was a connection-lost and session-unbind at 08:05:23; after that only reactor-thread lines
appeared. So the reactor kept running while the thread that accepts sessions produced nothing.

**The reactor watchdog did not report a stuck thread**, which is worth its own attention: that
watchdog is what made the order-book rehash stall diagnosable, and a thread that stops without it
noticing is the case [BUG-0002](#bug_0002) is about -- `process_message` exit paths that may skip
updating `time_event_finished_` have never been audited.

**A restart cleared it, and it recurred once more** after further ad-hoc sessions, so it is
unlikely to be a one-off.

**Hypotheses, in the order worth testing:**

- **The application thread exited or blocked.** `ThreadWithJoinTimeout` exists precisely because a
  raw `std::thread` terminates on an early return; a handler that threw would take the thread down.
  Nothing was logged, which fits an exit more than a block.
- **A session or connection leak.** Several sessions ended in the cancel-on-disconnect grace path
  -- the log recorded *"disconnected with 1 open order(s) -- holding 90s for reconnect"* -- and
  sockets were left unclosed by scripts that died. An exhausted connection or session table would
  refuse new work without any single thing looking wrong.
- **Something specific to the new inbound paths**, since these were the first sessions ever ended
  by the venue's own sequence checking. `end_session_on_sequence_error` sets
  `session_established = false` and then disconnects, which is a path nothing else takes.

#### Two hypotheses tried and disproven, 2026-08-28

Attempted with logs preserved. **Not reproduced**, and two of the three hypotheses above are now
ruled out.

- **Repeated sessions the venue ends.** 75 sessions across 25 rounds -- a too-low `MsgSeqNum`, an
  abrupt socket close with an order still open, and a gap opened then abandoned -- with a fresh
  logon attempted after every round. The application thread stayed up and every logon succeeded.
  So neither the sequence-error path nor the cancel-on-disconnect grace queue leaks on its own.
- **A member that stops reading.** 6,000 orders sent by a member that never read a byte, its socket
  held open throughout. All six threads stayed idle in `ep_poll`, and the gateway kept serving. So
  the venue does not block writing to a member that has stopped reading, which was the most
  plausible mechanism and is now excluded.

**A false positive on the way, worth recording because it nearly became a finding.** The first
slow-reader run reported the gateway wedged after 250 messages. It was not: the probe used comp id
`CLIENT-RECOVERY`, which is not in `credentials.toml` -- only `APM001`, `CLIENT` and `test2a001`
are -- so the venue accepted the connection, saw an unknown member and disconnected it, exactly as
it should. The probe was rewritten to use `CLIENT` and the wedge disappeared. **A test that cannot
log on looks identical to a venue that will not answer.**

**What remains untried** is the third hypothesis: that the application thread exited or blocked for
a reason specific to the state that day. The original observation stands -- the thread had gone
quiet while the reactor continued -- but nothing yet reproduces it, and the logs from that instance
were lost before they were kept.

**To reproduce:** the recipes above did not do it. What differed on the day was a long-running
venue with many ad-hoc sessions over an extended period, rather than a burst of them. Next attempt
should run for longer and vary the pacing, and **preserve `installed/log/` before anything else
runs** -- the step that was missed the first time and honoured the second.


### BUG-0001: Shutdown timeout errors in timer tests {#bug_0001}

| | |
|---|---|
| Severity | medium |
| Found | before 2026-07 (carried over from the roadmap's Known Issues) |
| Recorded | 2026-08-08 (8cc0ced) |
| How | Timer test logs |
| Impact | Unknown — the errors appear but nothing is known to misbehave |

After the timer SEGV fix, "did not stop within shutdown_timeout" and "failed to join within
shutdown_timeout" still appear in timer test logs. **Root cause not identified.** Worth noting that
`ThreadWithJoinTimeout` exists precisely because a raw `std::thread` terminates on an early return
before join, so a join that times out is not obviously benign.

### BUG-0002: The FIX order gateway's `process_message` exit paths are not audited {#bug_0002}

| | |
|---|---|
| Severity | medium |
| Found | before 2026-07 (carried over from the roadmap's Known Issues) |
| Recorded | 2026-08-08 (8cc0ced) |
| How | Code reading |
| Impact | Possible false stuck-thread detection |

`ApplicationThread::process_message` (`ApplicationThread.hpp:726`) is expected to update
`time_event_finished_` before it returns. If any exit path from it skips that, the reactor
watchdog reports the thread as stuck when it is not. **Not yet audited**, on the FIX order
gateway thread or on any other.

Given that the reactor watchdog is what made the order-book rehash stall diagnosable, false
positives from it would be costly: a watchdog that cries wolf gets ignored, and it is the
instrument that turned a year-old latency mystery into a measurement.

### BUG-0003: Environment placeholders are missing outside dev {#bug_0003}

| | |
|---|---|
| Severity | medium |
| Found | 2026-07 (exact date not recorded) |
| Recorded | 2026-08-08 (8cc0ced) |
| How | Reading `deploy.py` against the environment files while working on config templating |
| Impact | `deploy.py` exits on preprod, prod and test-1 |

`environments/preprod.toml`, `prod.toml` and `test-1.toml` each lack 10–12 of the placeholder
values their component templates require. Only `dev.toml` is complete, so only dev can be deployed.

**Deliberately not fixed**: the missing values are real hostnames, ports and certificate paths for
environments that do not exist yet. Inventing them would produce a file that deploys and then
fails at run time, which is worse than one that refuses to deploy.

**The gap widened by 60 on 2026-08-09**, when the reactor queue pools became templated (BUG-0025,
now closed). Unlike the hostnames, these are safe to fill in from `dev.toml` whenever those
environments are built, because a queue depth is a capacity decision rather than a fact about a
host that has to be looked up — so the count is larger but the difficulty is unchanged.

### BUG-0004: Doxygen 1.8.14 turns `\ref` labels into bare directory links {#bug_0004}

| | |
|---|---|
| Severity | low |
| Found | 2026-07 (exact date not recorded) |
| Recorded | 2026-08-08 (8cc0ced) |
| How | Building the docs on RHEL8, where 1.8.14 is the newest packaged release |
| Impact | Documentation only; the architecture map's cross-links break |

An unresolved `\ref` collapses to `href="../../"`, which a browser opens as a directory listing —
or, on Windows, a file chooser. 1.8.14 does **not** fail the build on an unresolved reference, so
this is silent. `docs/architecture_map_howto.dox` proposes a post-build check; not written.

### BUG-0005: fix-test-client reports a dead gateway poorly {#bug_0005}

| | |
|---|---|
| Severity | low |
| Found | 2026-07 (exact date not recorded) |
| Recorded | 2026-08-08 (8cc0ced) |
| How | Manual testing of the logon page |
| Impact | The UI misleads about connection state |

Two undecided items: `lastError` is left empty, and `connected` stays true after the gateway dies.
FIX produces no disconnect message, so there is nothing to display without inferring it.

### BUG-0006: ResendRequest under load {#bug_0006}

| | |
|---|---|
| Severity | medium |
| Found | before 2026-08 (carried over from the roadmap's Known Issues) |
| Recorded | 2026-08-08 (8cc0ced) |
| How | Noted when the feature was written |
| Impact | Unknown |

Partly overtaken by the 0.3.0 work, which replaced the blanket `SequenceReset-GapFill` with real
resends carrying `PossDupFlag`. The original concern — never exercised under load — still stands,
and the trading-day profile is a natural place to exercise it once it drives the FIX gateway.

### BUG-0009: The venue accepts orders indefinitely with no matching engine, and tells nobody {#bug_0009}

| | |
|---|---|
| Severity | high |
| Found | 2026-08-08 |
| Recorded | 2026-08-08 (49d9dd1) |
| How | The first clean trading-day load run, after the matching engine was OOM-killed |
| Impact | 924,000 orders taken with nothing able to process them and no answer sent to the member; 1,087,912 orders deferred over 7 minutes |
| Partly built | 2026-08-28 -- the venue refuses orders and cancels it cannot process, tells the member why, and resumes on its own. Held by ha_test.py scenario 42 |
| Still open | the orders deferred BEFORE refusal begins are still never answered. See the reopening note below and [BUG-0064](#bug_0064) |

**Reopened 2026-08-28, the same day it was closed.** Closing it was premature, and the reason is
worth keeping because it is a reasoning failure rather than a coding one.

The five steps bound how many orders can be deferred, tell the member when the venue stops
accepting, and refuse rather than defer once an outage outlives a plausible failover. That part is
built and held by scenario 42. But the title of this entry has two halves, and only the first is
done. **The venue no longer accepts indefinitely. It still tells nobody about the orders it
deferred before the threshold** -- up to 45 seconds, or 250,000 orders, of them.

Those were scoped out of the design deliberately, on the stated grounds that they "stay deferred
and are recovered by WAL replay, as now". [BUG-0064](#bug_0064) records the measurement that
disproves it: a restarted matching engine replays nothing, the orders are never executed and never
rejected, and the sequencer logs that they were recovered. **A scope decision resting on a false
premise is not a valid scope decision**, so the ground for closing this went away with it.

What remains here is the member-facing half: an order this venue took and cannot process must end
in an answer, whatever happens to the engine. The mechanism is BUG-0064's to settle, because
nothing can be promised to the member until it is known whether replay happens at all.

**Next step designed 2026-08-28 in
[Knowing there is no matching engine](availability/matching_engine_presence.md), not built.** The
45-second threshold exists only because the sequencer cannot tell a failover in progress from a
matching engine service that no longer exists. The arbiter already holds that fact and discards it,
so the sequencer should ask it -- a query, not a subscription, which is the shape
[section 11b](availability/design_notes.md#ha_arbiter_only_arbitrates) requires. The threshold then
becomes the fallback where an arbiter exists, and stays the only mechanism where one does not.

The startup race that design turns on is settled: **a negative answer requires positive evidence.**
The arbiter reports absence only when it has seen an instance and seen it go, never from elapsed
time, and every reachable arbiter must agree. See
[the decision](availability/matching_engine_presence.md#ha_me_presence_race).

When the matching engine connection drops, the sequencer commits each order to the WAL and
defers forwarding it:

```
SequencerThread: no matching engine connected -- order seq=59678842 WAL-committed,
forward deferred until an ME reconnects (recovered via WAL replay on ME promotion)
```

That policy is sound for a brief failover — the orders are durable and a promoted matching engine
replays them. Three things about it are not.

**The assumption can stop holding, and nothing notices.** A matching engine was promoted and did
reconcile, then died two minutes later. The sequencer went on deferring for another five
minutes, waiting for a recovery that could no longer happen because no matching engine
existed at all.

**It is logged at INFO, once per order — 1,087,912 times.** A million lines saying the
venue is degraded, at the level used for routine progress. Volume that large hides the
condition rather than reporting it.

**Nothing propagates to the gateway.** The sequencer knew there was no matching engine for
seven minutes. The gateway kept taking orders, logging `dropped=0` throughout, and the member saw
no difference. The sequencer has the knowledge, the gateway has the member relationship, and there
is no path between them.

**Correction, 2026-08-28: the member was not acknowledged, it was told nothing.** This entry and
the design note both said orders were "accepted and acknowledged". Measured while writing the
scenario for step 5: a `NewOrderSingle` placed with no matching engine reachable receives no
ExecutionReport at all, because the report is the engine's to send. The figures below already said
so -- 230,572 orders arrived in the window and 14,000 were accounted for -- and were read as
"acknowledged" anyway. The correction makes the defect worse, not better: an acknowledgement is a
state a member's risk system can reason about, and silence is not.

Suggested shape, not yet designed: deferring a handful of orders across a brief failover
should stay silent; deferring thousands over minutes should escalate — a rate-limited
WARNING, a metric for deferred-order count and age, and ultimately a signal that makes the
gateways stop accepting. **A venue that takes orders it cannot process is worse than one
that refuses them.**

**Designed 2026-08-28 in [Refusing orders the venue cannot process](availability/order_acceptance.md),
not built.** Three decisions were taken: refusal is triggered by **age first with a count as
backstop**, because the harm is the member's exposure and exposure is measured in time; the refusal
is a **rejected ExecutionReport**, which is an order outcome the member already handles rather than
a protocol fault; and the venue **refuses and resumes automatically**, because this is its own
capacity rather than a judgement about a member — and a design whose safety depends on someone
watching is no safer, this being a case where nobody watched for seven minutes.

One thing the design note records that changes the framing: **deferring costs the venue nothing.**
The handler calls `release_pdu_payload` and returns, so no order is held in memory; the WAL is the
whole mechanism. The cost falls entirely on the member, which has been told nothing, cannot tell a
deferred order from a slow one, must assume it may be live, and cannot cancel it because a cancel
needs the same matching engine. So the thresholds do not protect the venue's memory — they bound how far a
member's picture of its own position may drift from the truth.

Cancels are refused alongside orders, which sounds wrong and is not: a member believing it had
cancelled would be more dangerously wrong than one believing it had traded.

Related: BUG-0010, since the deferral policy assumes a promotion that will
succeed.

#### The gateway's own health line reads green throughout, 2026-08-09

Seen from the gateway's side in run 10, and worse than the sequencer half because this is
the line an operator would actually watch. `GW-PROGRESS` reports accounted, sent, dropped
and orders received. Across the failover it stopped being emitted at all for **2 minutes
19 seconds**, then caught up in a burst of three reports inside 0.2 seconds:

```
18:39:36  accounted=10,247,000  sent=10,247,000  dropped=0  nos_received=9,315,468
          <- no progress report for 2m19s
18:41:55  accounted=10,261,000  sent=10,261,000  dropped=0  nos_received=9,546,040
18:41:55  accounted=10,275,000  ...
18:41:55  accounted=10,289,000  ...
```

**230,572 orders arrived during that window and 14,000 were accounted for.** `dropped`
stayed at zero the whole way through, and it was not lying: nothing was dropped. The orders
were accepted, answered to nobody, and queued behind a matching engine that no
longer existed.

Two distinct faults, and the second is the awkward one:

- **The health line goes silent exactly when it is most wanted.** It is emitted per N
  orders accounted, so when accounting stalls the reporting stalls with it. A line driven
  by progress cannot report an absence of progress. It needs a time-based emission as well,
  or a reader watching a terminal sees the last healthy line and nothing after it.
- **`dropped=0` is true and misleading together.** An operator watching for trouble watches
  that counter, and a venue can accept a quarter of a million orders it cannot process
  without moving it. Whatever replaces this needs to distinguish *accepted and processed*
  from *accepted and queued behind nothing* — the gap between `nos_received` and
  `accounted` already carries that information and nothing reports it.

### BUG-0010: HA fails over into a condition both nodes share {#bug_0010}

| | |
|---|---|
| Severity | high |
| Found | 2026-08-08 |
| Recorded | 2026-08-08 (49d9dd1) |
| How | The first clean trading-day load run — the primary was OOM-killed and the promoted secondary died 2 minutes later. Recurred 2026-08-21 |
| Impact | Failover postponed an outage by about 4 minutes instead of preventing it |

The HA mechanism itself performed correctly and quickly:

```
16:49:30  primary OOM-killed (9.9 GB, book 8,388,608)
16:49:31  secondary: replication connection lost -- replica book now stale
16:49:46  promotion timeout fired, arbitration requested, elected leader
16:49:49  reconciled 58,444 records in 2.8s -- book 8,445,780
16:51:41  reactor watchdog: callback stuck [109105 ms] -- orderly shutdown
```

Detection, arbitration, promotion and reconciliation all worked. **The secondary then died
of the same thing**, because it came up holding a book of 8,445,780 orders on a machine
that had just killed a process for holding 9.9 GB.

**HA protects against independent failures. It cannot protect against a systemic condition
both nodes share** — memory exhaustion, a poison message, a bug reached by the same input,
a shared dependency. Failing over into it converts an outage into a slightly later outage
and burns the standby doing it.

**2026-08-21: the same failure, and this time the standby survived it.** The primary was
OOM-killed again, at a larger book still, and the promoted secondary kept trading:

```
19:08:41  primary OOM-killed (anon-rss 10.3 GB, 2^25-bucket table)
19:08:42  secondary arms the 15s promotion timeout
19:08:57  timeout fires, arbitration requested -- decision received 3 ms later
19:09:04  duplicate ArbitrationDecision correctly ignored
19:11     back to 1926 orders/s, and stayed there for the remaining three phases
```

The difference is not that HA improved. It is that killing the primary **freed 10 GB**, so the
condition the two nodes shared stopped being true the moment one of them died. Memory available
went from zero to 12.9 GB. That is luck rather than design -- had the survivor needed to grow its
own table again it would have gone the same way -- but it is worth recording that the mechanism
recovers cleanly when the shared condition lifts, and that arbitration itself took 3 ms under
genuine exhaustion with a ~10 GB book to take over.

It also sharpens the point above. The systemic condition here is the growth policy, and the entry
on it is *Growing the order book by doubling needs more memory than the machine has*. Reserving
the table instead of growing into it removes the condition rather than surviving it.

Worth designing for explicitly. Open questions rather than a plan:

- Should a promoted node **check its own headroom** before accepting promotion, and decline
  if it is in the same state the primary died in?
- Should the arbiter know *why* the primary went, so it can distinguish "the process died"
  from "the process died of something I am also suffering"?
- Is there a signal a node can publish that means "I am not a safe failover target"?
- What is the right behaviour when there is no safe target — refuse orders rather than
  accept ones that cannot be processed? See BUG-0009, on accepting orders with no
  matching engine.

Note this run's book growth was itself an artefact: the matching engine does no matching and the load
client cannot yet cancel, so nothing removed orders. The **failover behaviour** is the
finding here, not the growth.

#### Revised 2026-08-09: the outcome is not always this bad, and the reason is worth keeping

Run 8 reached the same state — machine exhausted, MemAvailable 0.42 GB, primary matching
engine OOM-killed at 12.9 GB while rehashing — and **failover succeeded**. The venue
completed the trading day:

```
11:17:52  primary OOM-killed (anon-rss 12.9 GB, mid-rehash at 2^23)
11:18:07  ArbitrationDecision (group=matching_engine leader=2 follower=1 epoch=1)
11:19:23  secondary adopts LEADER (91s after the kill)
          phases 5, 6 and 7 all PASS -- every order acknowledged
11:48:51  clean shutdown, every component
```

The secondary went on to reach 14.88 GB — **larger than the primary ever was** — and
survived, because killing the primary freed 7.6 GB.

**When the shared condition is memory, the OOM killer's victim selection is load shedding.**
Removing the largest consumer relieves the survivor, so the systemic condition is not
uniformly fatal the way the original entry implies. That does not make HA a defence against
systemic failure — it made no difference to the *cause*, and phase 4 still failed with a p99
of 107 seconds — but the failure mode is "may or may not survive, depending on which process
the kernel picks and how much its death releases", not "will die too".

This sharpens the open questions above rather than answering them. A promoted node checking
its own headroom would, in run 8, have **wrongly declined** a promotion it went on to
complete successfully, because the headroom it needed did not exist until the moment the
primary died.

### BUG-0014: Python style warnings across the top-level scripts, and a lint gate that ignores them {#bug_0014}

| | |
|---|---|
| Severity | low |
| Found | 2026-08-09 |
| Recorded | 2026-08-09 (2174fb6) |
| How | Adding the top-level scripts to the pylint gate after a release check found an undefined variable in `perf_run.py` |
| Impact | Style only today. The errors that matter are now gated; the warnings are not |

The pylint gate ran `pylint dsl fix_dictionary` — two package directories — so every script in
the repository root was checked by nothing. That is how `NameError: name 'prefix' is not
defined` survived in `perf_run.py`'s FIX path until `release_check.py` ran it.

The gate now also runs the top-level scripts, but **errors only**, because that is the bar they
can pass today. `perf_run.py` scores 9.15/10 on style: 26 lines over the limit, imports out of
position, several functions with too many locals or arguments, `subprocess.run` without an
explicit `check=`, and files opened without an encoding.

**The resolution is to fix those warnings and then make the gate fail on them**, so the
top-level scripts are held to the same standard as the DSL. Two of the warning classes are
worth more than tidiness:

- `W1514` unspecified-encoding — a file opened without one takes the locale's default, and
  these scripts read and write config and log files.
- `W1510` `subprocess.run` without `check` — silently ignoring a non-zero exit is how a
  deployment step appears to succeed.

Not urgent, but it should not sit indefinitely: the gate as it stands protects against the
class of defect that stops a script dead, and nothing else.

### BUG-0018: The idle-connection reaper tears down the pre-warmed failover link {#bug_0018}

| | |
|---|---|
| Severity | medium |
| Found | 2026-08-09 |
| Recorded | 2026-08-09 (d9b38e5) |
| How | The trading-day load run — the sequencer logged `connection lost` at WARNING every ten minutes while both matching engines were healthy |
| Impact | The secondary's order connection is absent for part of every ten-minute cycle, so failover latency depends on when the failure lands |

`MatchingEngine.cpp` registers the order listener on both roles, and says why: *"Pre-warming
this listener means the sequencer's connection is already established when the secondary is
promoted, so WAL reconciliation can begin without a connect delay."* On the secondary that
connection carries no data — the engine discards sequenced orders while in FOLLOWER mode — so
`InboundConnectionManager::check_for_inactive_connections()` closes it after
`socket_maximum_inactivity_interval_`, 600s. **Being idle is precisely what a standby link
does**, so the reaper defeats the pre-warming it exists to provide.

Twelve teardowns in the first 26 minutes of run 7 — two cycles of six connections, at 08:26:20
and 08:36:23, exactly 600s after start and 600s after each other. All on
`matching_engine_secondary`; none on the primary, whose connections carry orders and so never
idle out. It recurs for the whole life of the process.

The framework already has the mechanism: `register_inbound_listener()` takes an
`IdleTimeoutFlag`, and `matching_engine_publisher` and `fix_order_gateway` already pass
`BypassIdleTimeout` for their quiet infrastructure links. The matching engine's two listeners
take the `UseIdleTimeout` default instead.

**Second-order cost, and the more insidious one.** A WARNING that fires every ten minutes in a
healthy system teaches a reader to skim past `connection lost` — which is the line that matters
when a connection is genuinely lost.

### BUG-0028: Growing the order book by doubling needs more memory than the machine has {#bug_0028}

| | |
|---|---|
| Severity | high |
| Found | 2026-08-21 |
| Recorded | 2026-08-21 (875259f) |
| How | The trading-day run that proved the stall cured -- it reached a table size no earlier run survived to |
| Impact | `matching_engine_primary` OOM-killed at 19:08:41 with 10.3 GB resident; 280,901 of 13,016,000 orders lost across the outage |

With latency no longer the limit, memory is. At 18:54:17 both engines allocated 2^25-bucket
tables of 9760 MB. At 19:08:41 the kernel took the primary: `anon-rss 10340720 kB`, zero memory
available on a 31 GB machine.

**The arithmetic was always going to end here, and two things had been left out of it.**

First, **the venue runs two order books, not one.** The secondary maintains a full replica via
BookUpdate PDUs, so every figure in this entry -- and every figure in the memory reasoning that
preceded it -- doubles. Growing into a 2^25 table by doubling costs 14.6 GB transient per engine
while both tables are held: 29.2 GB for the pair.

Second, **the binary gateway reserves 4.3 GB before the first order arrives.** `[open_order_pool]`
is `initial_pools = 21` of `objects_per_pool = 1048576` at `sizeof(OpenOrderEntry)` = 168 bytes =
3.45 GB, and it is resident immediately because building the pool's free list writes a next
pointer into every slot. It reads like a leak in `htop` and is not one, but it is 4.3 GB the
engines cannot have.

**The fix is to reserve rather than to grow.** `[order_book] initial_capacity` already exists and
is set to 262144 -- about four minutes of this profile. Sized to the expected book it removes the
growth steps altogether, and it *lowers* peak memory rather than raising it:

| | peak per engine |
|---|---|
| grow into a 2^25 table by doubling | 14.6 GB transient |
| reserve 2^25 at startup | 9760 MB steady |

That is the difference between 29.2 GB for the pair and 19.5 GB, which is the difference between
being killed and not. `allocate_table()` memsets only the state array -- one byte per slot -- and
leaves entry storage to be faulted in as the book fills, so the reservation costs address space
and one allocation at startup rather than resident memory up front.

This does not make `IncrementalRehashMap` redundant. Sized right, the map never migrates; sized
wrong, it is what makes the mistake survivable instead of a second-long stall. Reservation handles
the expected case, the container handles being wrong about it.

**Not yet changed.** The number to use should come from the book size the venue is meant to
support, not from this profile.

### BUG-0029: A process death on the same host takes the machine-death path {#bug_0029}

| | |
|---|---|
| Severity | high |
| Found | 2026-08-21 |
| Recorded | 2026-08-21 (875259f) |
| How | The trading-day run that proved the order-book stall cured -- the primary was OOM-killed and the venue stopped trading for 16 seconds |
| Impact | 16 s outage against a design target of under 50 ms for this class of failure. ~280,000 orders lost across it |

`docs/availability/design_notes.md` separates two recovery paths and gives them very different targets:

| | Local (process) recovery | Network (machine) failover |
|---|---|---|
| Recovery target | **under 50 ms** | 100 ms to seconds |
| Trigger | the process died; hardware and kernel are healthy | total silence from a node |

**What happened on 2026-08-21 was the first kind and was handled as the second.** The kernel
killed one process on a healthy machine and closed its sockets; the follower saw the
replication connection close within a second. It then took the outer loop:

```
19:08:41  primary OOM-killed
19:08:42  follower sees the socket close, arms a 15 s timer
19:08:57  timer fires, asks the arbiter
19:08:57  ArbitrationDecision received -- 3 ms later
19:11     back to 1926 orders/s
```

Fifteen of the sixteen seconds are `ha_timing.heartbeat_timeout_seconds`, armed at
`MatchingEngineThread.cpp:289` on connection loss. The arbitration it was waiting to perform
took 3 ms.

**The wait bought nothing.** The arbiter decides from live connection state -- `peer_connected`
at `ArbiterThread.cpp:553`, backed by a map erased on disconnect at line 102 -- not from
heartbeat recency. Its own connection to the dead primary closed at the same instant the
follower's did, so it would have returned the same decision at 19:08:42 as it did at 19:08:57.
The follower detected the death immediately, discarded that signal, and waited for a timer
sized for a node that has gone silent.

**Why the timer is not simply wrong.** It is the correct trigger for the outer loop, where the
question is whether a node that has stopped answering is dead or merely unreachable. What is
missing is the distinction: a socket closed by the kernel because the peer process no longer
exists is not silence, it is evidence. The design already draws that line and the code does
not act on it.

**What this does not depend on.** Promotion safety here rests on the arbiter, not on a race
being avoided by waiting -- see `docs/availability/design_notes.md#ha_no_stonith`, which records that STONITH
is not implemented and that arbiter-mediated leadership plus epoch fencing stands in its place.
A follower asking sooner is refused just as surely if the peer is alive and still connected to
the arbiter, because leadership goes to the lower instance id when both are connected, and the
primary's id is always the lower one. Asking earlier changes when the answer arrives, not what
it is.

**PARKED 2026-08-21, pending a process-supervision design.** That design is
`docs/availability/process_death.md`, started 2026-08-24: it records what `launch.py` already
does, the measurement that rules out a shared-memory journal, and the four questions still open --
of which the grace period is this entry's. Nothing here should be changed
until that is settled -- see the correction below, which reverses this entry's first recommendation.

**The 15 seconds is the local recovery grace period, and it is doing nothing only because
nothing fills it.** `docs/availability/design_notes.md#ha_process_vs_machine` gives the outer-loop trigger as "the
heartbeat timer expires and the primary fails to reconnect **after the local recovery grace
period**". That period is this timer. Its purpose is to give a locally-restarted primary time
to come back so the follower never has to promote at all. Today the venue has no supervisor --
components are started ad hoc by `scripts/devenv.py` for testing, which is not how a real
deployment would start them -- so nothing restarts a dead engine and the window is simply dead
time before a promotion.

That means **the outage is not fixed by shortening the wait; it is fixed by filling it.** How
long the grace period should be is a consequence of how fast a supervised restart is, which
cannot be answered before process supervision is designed. That design is the next piece of
work, and this entry is blocked on it.

**Possible fixes, and a correction.** These were recorded before the above was understood.

1. **WITHDRAWN: treat a peer-initiated close as evidence and arbitrate at once.** This was
   this entry's original first recommendation and it is wrong. Promoting the moment the socket
   closes pre-empts the local restart, forcing a cross-machine failover for a failure that did
   not need one -- the opposite of what the layered design intends. It would only be right for
   a venue that has decided never to recover a process in place.
2. **Still valid: give the promotion delay its own setting.** It currently borrows
   `ha_timing.heartbeat_timeout_seconds`, which is a different quantity measured for a
   different purpose. Even keeping the outer-loop behaviour, one number serving two meanings
   cannot be tuned for either.
3. **The real fix: build the inner loop the design describes.** Section 7 of `docs/availability/design_notes.md` calls
   for local process recovery -- a shared-memory journal, restart in place -- with a sub-50 ms target, and
   it does not exist. Fix 1 shortens the outage to about a second by promoting the peer
   faster; this is what would meet the stated target, by not needing a promotion at all for a
   process that can simply be restarted. Much the largest piece of work of the three, and the
   only one that addresses the design gap rather than the symptom.

   Measured 2026-08-21, and it bears on how this is built: rebuilding the book by replaying
   entries costs 438 ms at 2^21, 921 ms at 2^22 and **2034 ms at 2^23** -- pre-reserved, no
   migration, no decode, no I/O, so a lower bound. A shared-memory *journal* replayed on restart
   therefore cannot reach section 8's sub-50 ms target at any realistic book size. Only the
   book itself living in shared memory, re-attached rather than rebuilt, can. That is a much
   larger change and it belongs after the supervision design, not before it.

**Not investigated:** what a restarted primary does when it rejoins after the secondary has
been promoted. `decide_and_broadcast` recomputes leadership rather than consulting
`leadership_state_`, and the reconnect path at `ArbiterThread.cpp:509` looks the stored state
up under the connecting instance's own key. Whether that yields a clean failback or a
disagreement was not traced, and is a separate question from this entry.

### BUG-0030: Restart coverage: what ha_test.py exercises, and what it does not {#bug_0030}

| | |
|---|---|
| Severity | medium |
| Kind | task -- a coverage matrix, not a defect |
| Found | 2026-08-21, extended into a full matrix 2026-08-22 |
| Recorded | 2026-08-22 (63889b8) |
| How | Reading every scenario against the restart cases an HA pair actually has |
| Impact | Three defects were found in the two cases that were covered. The uncovered ones have not been looked at |

Every scenario kills a component and leaves it dead, which models **machine** death correctly --
a dead machine does not come back on its own. It leaves **process** death, where the instance is
restarted on a machine that never failed, almost entirely unexercised. That is the half a
supervisor makes normal, and it is where every defect found on 2026-08-21 and 2026-08-22 lives.

**The cases an HA pair has, and where each stands:**

| | sequencer | arbiter | matching engine |
|---|---|---|---|
| **R1** restart the leader inside the peer's grace period; the peer must not promote | **27** | **28** | **26** |
| **R2** restart the leader *after* the peer has promoted -- must rejoin as follower | 14 | **25** | **24** |
| **R3** restart the *follower* -- must stay follower, leader untouched | **30** | **31** | **29** |
| **R4** after R2, kill the new leader -- the rejoined instance must take over | 14 | **33** | **32** |
| **R5** cold start both, in either order -- deterministic leader | **36** | **38** | **34** |
| **R6** restart with no arbiter reachable -- degraded, and said so | **37** | **39** | **35** |

Scenarios 10 to 13 restart a matching engine but run a single one, so no role is ever in
question; they test that it comes back, not what it comes back as.

**What the two covered cells cost to find.** R2 for the matching engine is scenario 24, written
2026-08-22, and it found three defects in a row: the arbiter re-running the cold-start tie-break
on a rejoin, the engine promoting itself on arbiter connect, and the sequencer routing orders by
socket rather than by role. All three are in this file. That is one cell of eighteen.

**Completed 2026-08-23.** Eighteen of eighteen cells. The last four were taken by
ha_test.py scenarios 36-39; the suite went from 23 scenarios to 39.

**Where it stood, 2026-08-22.** Fourteen of eighteen cells. Every cell taken so far found at
least one defect except R1, R3 and R4-arbiter, which passed first time -- and that pattern is
itself informative: the defects all lived in what an instance comes back *as*, so the cells where
nothing has to decide that were the ones that already worked.

**Complete, 2026-08-22.** All eighteen cells. Ten defects were found on the way, every one of
them a consequence of the same omission: process death was not in the model, so nothing decided
what a restarted instance comes back as, and channels, records and fallbacks were all built on
identities that stop being true once roles can move.

**The cells that passed first time are as informative as the ones that did not.** R1, R3 and
R4-arbiter, and both R5/R6 cells for the sequencer and arbiter, needed no fixes. The defects all
lived in what an instance comes back *as*; where nothing has to decide that, the code was already
right. And where a component already had a symmetric design -- the sequencer's `peer` channel,
its unconditional startup election timer -- it had none of the faults its matching-engine
counterpart did.

**One thing worth carrying forward.** The sequencer settles leadership with its peer directly,
through StatusQuery/StatusResponse and the instance-id rule, and needs no arbiter to do it. The
matching engine cannot: it must ask an arbiter or fall back to a unilateral rule. Whether the
engine should gain the same peer-to-peer resolution is a design question this coverage raised and
did not answer.

### BUG-0040: The order-accounting check reports lost orders when it means it could not count them {#bug_0040}

| | |
|---|---|
| Severity | medium |
| Found | 2026-08-23 |
| Recorded | 2026-08-23 (03ba5d8) |
| How | A 113-minute trading-day run ended `FAIL -- the matching engine did not accept every order`, having just said it could not read the counter it needed |
| Impact | A clean run is reported as order loss. That is the conclusion this check exists to prevent being drawn wrongly |

`perf_run.py` finishes a run by comparing the matching engine's `orders_processed_total`
against the orders the profile offered. When the counter cannot be read it sets the count to
-1, prints `COULD NOT READ the matching engine's metrics endpoint`, and then falls into the
same comparison as any other run. -1 is not the expected total, so the run is declared a
failure with the words *the matching engine did not accept every order*.

Nothing had gone wrong with the venue. **"I could not measure this" and "this measurement came
out wrong" are different results, and only one of them is about the venue.** Reporting the
first as the second is the more damaging error, because it sends someone to look for a fault
in the system rather than in the instrument.

That is worth more here than it would be elsewhere. This check exists *because* of a false
loss report: a June run reported 166 orders missing, which was investigated at length and
turned out to be a Quill flushing artefact with every one of the million orders present. The
post-shutdown ground-truth count was the answer to that. It has now produced a false loss
report of its own, by a different route.

**The reporting is fixed. The read failure itself is not**, and the obvious causes are all
eliminated: the counter exists (`orders_processed_total`, registered in
`MatchingEngineThread.cpp`), the scrape happens before `full_shutdown()` rather than after it,
and the metrics port resolves correctly from the deployed configuration.

**A leading hypothesis, and how to test it.** `read_counter` allows the scrape **two seconds**
and turns any `URLError` or `OSError` into `None`, which the caller cannot tell apart from the
metric being absent. At the end of the trading-day run the matching engine held 8.6 million
resting orders in a table whose largest allocation was 9.5 GiB, with 8.0 GB resident. Rendering
the Prometheus exposition under that is plausibly slower than two seconds, or the endpoint's
thread simply did not get scheduled promptly.

The evidence fits: the read failed after 113 minutes and 8.6 million orders, and succeeded twice
immediately afterwards on runs of 1,000 orders. If the hypothesis holds, the failure follows the
size of the book rather than anything about the code path -- so it will reproduce at the end of
a long run and never at the end of a short one, which is the worst possible shape for noticing
it.

Two things follow if it is confirmed. The timeout is too short for a process holding several
gigabytes, and more importantly a timeout and an absent metric should not arrive at the caller
as the same value: one is "ask again", the other is "this venue does not publish that".

Two things to fix, and the first does not depend on diagnosing the second:

- Report an unreadable counter as *not verified*, distinct from both PASS and FAIL. A run whose
  accounting could not be taken is not a run that failed.
- Find out why the counter could not be read.

Related: `component_metrics_port()` calls `die()` when a deployed config still holds
unexpanded `${...}` placeholders, which is the state `build.sh` leaves `installed/etc` in until
`deploy.py` runs again. A stale deployment therefore kills a run at the accounting step, long
after the point where the cause would have been obvious.

### BUG-0041: Five ways the venue will not start on a RHEL8 target host {#bug_0041}

| | |
|---|---|
| Severity | medium |
| Found | 2026-08-24 |
| Recorded | 2026-08-24 (95b8713) |
| How | Bringing the venue up on a RHEL8 target host -- the first run of `create_db.py` and `devenv.py` outside this machine and the Rocky container |
| Impact | Database creation fails outright; the venue starts only after hand-editing `environments/dev.toml` |

BUG-0026 found this class of thing at build and install time. These are the next
stage along: creating the database, and launching. Nothing here is a fault in the C++ -- the
binaries were fine and the venue came up once the scripts were worked around.

Two of the five are the same mistake made twice: **a tool located by where one distribution's
package manager puts it, rather than by asking the system where it is.**

**1. The Liquibase launcher is found by a hardcoded absolute path.** `_check_jdbc_driver()`
opens with `Path(os.path.realpath("/usr/bin/liquibase"))` (`db/create_db.py:149`) and derives
everything else from it. On a host where Liquibase is installed anywhere else -- under `/opt`,
from the vendor tarball, from an RPM that does not drop a `/usr/bin` symlink -- that resolves
to a path that does not exist, and the rest of the function reasons from it anyway.
`shutil.which("liquibase")` is what this wants, and it is the same question the script asks
again in point 3 by a different means.

**2. The JDBC driver is sought in the wrong directory of the Liquibase tree.** Line 150 takes
`liquibase_real.parent / "lib"`. Liquibase 4.x and later ship their bundled drivers under
`<root>/internal/lib`, so the glob finds nothing even when the driver is present and correct.
The search should start from the Liquibase root and accept either layout rather than assuming
one.

The fallback makes this harder to diagnose rather than easier. When the glob comes up empty the
function copies the JAR bundled in `db/drivers/` to the same place, and calls
`liquibase_lib.mkdir(parents=True, exist_ok=True)` first (line 163). On a system install that
needs root; on a wrong path it quietly creates a plausible-looking empty `lib` directory beside
the real `internal/lib`. **A repair path that acts on an unchecked guess leaves the host in a
state that looks deliberate.** The error text it prints when even that fails names `apt` and
`libpostgresql-jdbc-java`, which is the same single-distribution assumption stated out loud.

**3. `liquibase` as a bare command is not found.** `liquibase_cmd` is `["liquibase", ...]`
(line 294), and running it reports command not found. **Not diagnosed**, and it disagrees with
point 1 in a way that is itself the clue: the script hardcodes `/usr/bin/liquibase` *and*
invokes the bare name, and both fail on the same host. So Liquibase is neither at `/usr/bin`
nor on the `PATH` the script sees -- consistent with an `/opt` install reachable through an
interactive login shell, by alias or a `PATH` set in a profile script, but not through
`subprocess.run` without a shell.

To settle it on the host, run these side by side:

```bash
command -v liquibase
python3 -c 'import shutil; print(shutil.which("liquibase"))'
```

Fixing point 1 with `shutil.which` addresses this one too, and turns it into a clear message:
`which()` returning `None` is the same absence, and the script can say so at the point it looks
instead of failing several steps later on a path it invented.

**4. The install tree is named in the environment file, and the build already disagrees with
it.** `environments/dev.toml:22` sets `install_dir = "installed"`, and `devenv.py` resolves
every binary, config and library path off it. But `build.py:58` and `devsetup.py:40` stage a
gcc-8.5 Rocky/RHEL8 build to `installed-rocky8/` **precisely so it cannot overwrite the host's
`installed/`**. The run side therefore looks in the one tree the build deliberately avoided,
and `authentication_service` is reported missing because it is genuinely not there.

**The fix is a choice, not a lookup**, and the axes are the reason. `environments/*.toml` varies
by deployment environment -- dev, preprod, prod, test-1. The install tree varies by target
platform. An `rhel8.toml` is a dev environment that happens to be RHEL8, and the cross product
arrives the day there is a preprod on one. Three ways to settle it:

- An `--install-dir` override on `devenv.py`, matching the `--db-port` precedent -- cheapest,
  and it keeps the two axes apart.
- Default `install_dir` from the detected platform, so the run side derives what the build side
  already decides.
- A per-platform environment file, accepting the cross product when it comes.

Related: *Environment placeholders are missing outside dev*, which is the same file family seen
from the other side.

**5. `devenv.py --db-port` exists, and is invisible from where it is needed.** It is a global
option, so it must precede the subcommand:

```bash
python3 scripts/devenv.py --db-port 5433 start     # works
python3 scripts/devenv.py start --db-port 5433     # unrecognized arguments
```

`devenv.py start --help` lists only `[-h] [name]`. Its own help text says the flag matches
`deploy.py`'s of the same name so that a host with a non-default cluster "names it the same way
at deploy time and at start time" -- and then does not appear at the point of use. The cost is
not the typing: the environment file was edited instead, which is a change that then has to be
un-picked, and which is how a host ends up running a profile against configuration nobody meant
to keep.

**Not a defect: Prometheus did not start.** `devenv.py:239` skips any component whose command is
absent from `PATH` and prints that it is doing so. The comment above it gives the reason --
refusing to start a trading system because a monitoring tool is absent gets the priority
backwards. Recorded here because it appears alongside the five as a sixth failure and is not
one.

### BUG-0045: A member has no defined way to discover its primary gateway is down {#bug_0045}

| | |
|---|---|
| Severity | medium |
| Kind | task -- an open design question, not a defect in what exists |
| Found | 2026-08-06, recorded as an open question while designing session pinning |
| Recorded | 2026-08-24, lifted out of `docs/availability/gateway_ha.md` so it can be triaged |
| How | Reading the gateway HA design's own open-questions section against the bug list |
| Impact | Failover to the backup gateway depends on member behaviour the venue has not specified |

Sessions are pinned to a primary gateway and a backup. Nothing states how a member learns it
should try the backup.

**Connection refusal is the simple answer and is what venues rely on**, but it interacts with
logon timeouts: a member that dials a dead instance and waits for a timeout rather than a refusal
spends that timeout before trying the backup, and the venue has no say in how long it is. A
refusal is immediate; a black-holed SYN is not, and which one a member sees depends on how the
instance died.

Related: BUG-0019, where a logon racing the gateway's sequencer links took the five-second
degraded path. The same class of problem -- the member's timeout budget deciding the outcome.

### BUG-0046: The binary order gateway has no in-flight report recovery {#bug_0046}

| | |
|---|---|
| Severity | medium |
| Kind | task -- a capability the FIX gateway has and this one does not |
| Found | 2026-08-06, when session pinning was settled for both gateways but resend only for FIX |
| Recorded | 2026-08-24, lifted out of `docs/availability/gateway_ha.md` so it can be triaged |
| How | Reading the gateway HA design's own open-questions section against the bug list |
| Impact | A binary member reconnecting after a gateway failure cannot recover the reports it missed |

Session pinning was settled for both gateways at step 4: **both enforce it and both refuse the
same way.** Steps 5 and 6 -- the resend half -- were built on the FIX session layer, which has
`ResendRequest` and a sequence number to anchor a replay to.

**The binary protocol has neither**, so "in-flight reports survive a failover" means something
different there and needs its own mechanism rather than the same one. It was left open rather
than assumed, which was right, and it has stayed open since.

Until it is answered, a binary member and a FIX member get materially different guarantees from
the same venue across the same failure.

**Design it with [Resend provenance](availability/resend_provenance.md), added 2026-08-27.** That
design changes the sequencer's replay contract, and this task will consume the same contract, so
specifying them separately means specifying it twice. The note also records why a binary mechanism
built on a cursor cannot have BUG-0051 -- and why a proposed one that could is a warning sign
rather than a gain in test coverage.

### BUG-0047: Disaster recovery is not modelled {#bug_0047}

| | |
|---|---|
| Severity | medium |
| Kind | task -- design not started, deliberately deferred |
| Found | 2026-08-06, noted while settling the primary/backup gateway pair |
| Recorded | 2026-08-24, lifted out of `docs/availability/gateway_ha.md` so it can be triaged |
| How | Reading the gateway HA design's own open-questions section against the bug list |
| Impact | No second-site capability, and the shape of one is not decided |

Venues publish a third address at a second site. It runs under a **different regime** from the
primary/backup pair: typically a start-of-day sequence reset, and no continuity of in-flight
state.

**So it is not a third backup and must not be built as one.** Adding it as another column in the
session-provisioning table would produce exactly the wrong thing -- a site that claims continuity
it cannot deliver, discovered at the moment it is needed.

Deliberately out of scope for 0.3.0 and not urgent. Recorded here because "we decided not to do
this yet" and "nobody has thought about it" are different states, and only one of them is true.

### BUG-0048: Nothing truncates the WAL, so it grows for the life of the venue {#bug_0048}

| | |
|---|---|
| Severity | medium |
| Found | 2026-08-24 |
| Recorded | 2026-08-24 |
| How | Checking a claim in `wal_and_ha.md` about snapshot-anchored truncation against the code that implements it |
| Impact | WAL segments accumulate on disk with nothing reclaiming them. Two design documents state a retention bound that does not exist |

`Wal::truncate_below()` is implemented and correct: it finds the segment holding the first record
at or after the safe sequence number and unlinks every segment before it. `delete_segments_before`
is the only path in the venue that deletes a WAL segment.

**Nothing in the venue calls it.** The callers are `WalTest.cpp`, `TopicPubSubIntegrationTest.cpp`
and `TopicPubSubBench.cpp` -- two tests and a benchmark. No sequencer, no matching engine, no
snapshot path. So the WAL grows for as long as the venue runs, bounded by the disk rather than by
any policy.

**Two documents state a bound that does not hold.** `gateway_ha.md` step 6 tells the reader
*"Snapshots truncate the WAL, and anything older than the retained segments cannot be replayed"*,
and `wal_and_ha.md` describes truncation anchored to an older verified snapshot. Both describe the
design. Neither describes what runs, and a member's resend depth is currently bounded by disk
capacity instead.

**Why this has not bitten.** The longest run so far is 113 minutes. Retention only becomes visible
over a trading day, and the failure when it arrives is a full disk rather than a slow degradation
-- which is the worst shape for noticing it.

**What it depends on.** Truncation must not delete history the follower still needs, nor history
behind the newest snapshot -- and the safe anchor is the *older verified* snapshot, which is
roadmap slice 9 and not started. So this is not simply a missing call: calling
`truncate_below(newest_snapshot)` today would delete WAL history with only one unvalidated
snapshot standing behind it, which is the case the invariant exists to prevent.

Related: BUG-0046, since the WAL is what a member's resend is served from.

### BUG-0050: Doxygen 1.8.14 cannot build the documentation with warnings as errors {#bug_0050}

| | |
|---|---|
| Severity | low |
| Kind | task -- a limitation to decide about, not a defect in the documents |
| Found | 2026-08-24 |
| Recorded | 2026-08-24 |
| How | Setting `WARN_AS_ERROR = YES` for BUG-0049 and running the full docs build in the RHEL8 container |
| Impact | The RHEL8 docs build needs `WARN_AS_ERROR=NO` on the command line. The gate is real on the development host and unavailable on the target |

Doxygen 1.9.8 builds this documentation with **zero** warnings. Doxygen 1.8.14, the newest release
packaged for RHEL8, produces **893**:

| Count | Complaint |
|---|---|
| 393 | found subsection command outside of section context |
| 362 | found subsubsection command outside of subsection context |
| 104 | unexpected token TK |
| 16 | found paragraph command outside of subsubsection context |
| 18 | everything else |

**771 of them are one thing.** 1.8.14 maps a markdown `##` to `\subsection` and `###` to
`\subsubsection`, and requires each to sit inside the level above. It will not infer that from a
page title, so every document written with markdown headings fails it. The documents are not
wrong; 1.9.8 reads the same files and says nothing.

**So this is a property of the old Doxygen, not a defect to fix in the documentation.**
Restructuring 64 documents to satisfy a parser that one machine runs would be the wrong trade, and
would make them worse to read.

Three ways to settle it, none obviously right:

- **Leave it.** `docs/orientation/building.md` documents `WARN_AS_ERROR=NO` for the container, with
  the reason. The gate protects the development host, which is where documentation is written.
- **Stop publishing docs from RHEL8 at all**, and say so. Nothing automated builds them there
  today -- `build.py --doxygen` is opt-in and the Rocky stage never passes it -- so this would
  record what is already true.
- **Carry a second Doxyfile for 1.8.14.** Most faithful, and a second configuration to keep in
  step with the first, which is its own failure mode.

Related: BUG-0004, which is the same version's other markdown weakness -- an unresolved `\ref`
renders as a bare directory link rather than failing.

---

## Closed

### BUG-0011: `cmake --install` re-lays config templates unexpanded {#bug_0011}

| | |
|---|---|
| Severity | low |
| Found | 2026-08-08 |
| Recorded | 2026-08-08 (8cc0ced) |
| How | A trading-day run failed to start immediately after a rebuild |
| Impact | `devenv.py` refuses to start until `deploy.py` is re-run |
| Fixed | 2026-08-28 -- the refusal now names the deployment path instead of advising the shortcut |

Every build re-installs the `${placeholder}` templates over the deployed, expanded configs, so
**`deploy.py` must be re-run after every build**. `devenv.py` does detect it and says so clearly,
which is why this is a trap rather than a fault — but it is easy to hit when iterating on a
component and then starting the venue.

#### Reworded 2026-08-28: this is the cost of skipping the release, not a defect

The sequence that produces it is `cmake --install` followed by `deploy.py`, which is **not the
project's deployment path**. That path is build, release, deploy, run -- `devsetup.sh` runs all
four -- and `deploy.py --artefact` unpacks a release rather than expanding whatever happens to be
sitting in the install tree.

Nothing re-lays templates over deployed configs in that sequence, because the deployed configs come
out of the artefact and the artefact is immutable. The trap exists only when the cmake install
prefix is also used as the runtime tree, which is a development shortcut, not the design.

**Closed 2026-08-28, and what it needed was a better sentence.** `devenv.py` already detected the
condition and refused to start. What it then said was:

> Run deploy.py to expand the configs for this environment before starting (cmake --install re-lays
> the templates unexpanded, so re-deploy after every build).

**That is advice to take the shortcut**, and it is almost certainly where the shortcut was learned
-- it was followed throughout the 2026-08-28 session before anyone questioned it. Detection was
never the problem. The message now says that the install tree was laid by a build rather than a
deployment, and points at `scripts/devsetup.sh`, which is build, release, deploy, run.

Advice that teaches a shortcut is how a shortcut spreads, and it spreads with the authority of the
tool that gave it.

What did NOT happen, and should not: `deploy.py` growing machinery to make expanding in place work
properly. That was attempted the same day and reverted, because it moves configuration outside the
artefact and destroys the property that makes a release worth having. See
[BUG-0015](#bug_0015).

See also BUG-0015, which is the converse and the more dangerous half.

### BUG-0015: `deploy.py` silently ignores a change to an environment file {#bug_0015}

| | |
|---|---|
| Severity | medium |
| Found | 2026-08-09 |
| Recorded | 2026-08-09 (d9b38e5) |
| How | Raising the binary gateway's open-order pool in `environments/dev.toml` before a load run; the deployed config still held the old value after `deploy.py` reported success |
| Kind | task -- the defect it described was a misreading of deploy.py's contract |
| Fixed | 2026-08-28 -- the tools warn when the deployment predates the source, and deploy.py no longer reports doing nothing in the same words as doing something |
| Impact | A run can execute a whole profile against the *old* configuration while every command in the runbook appears to have succeeded. Reworded 2026-08-28: only reachable by skipping the release step |

`deploy.py` substitutes `${placeholder}` values into the deployed configs; it does not re-copy the
templates. Once a config has been expanded it contains no placeholder, so a later edit to
`environments/dev.toml` has no effect: `deploy.py` reports `0 template(s) expanded` and exits
zero. Nothing warns, and the venue starts happily on the previous values.

The working sequence is a re-install first, so there is something left to expand:

```bash
cmake --install build                     # re-lay the templates, unexpanded
python3 scripts/deploy.py --skip-db --skip-certs  # expand them from environments/dev.toml
```

**Worth fixing rather than documenting**, because the failure is silent and the cost is a whole
run. Either re-copy a template when it or the environment file is newer than the deployed config,
or refuse to report success when a deployment expanded nothing — `0 template(s) expanded` is
almost never what the caller intended, and it is the one case that currently looks identical to
success.

#### Reworded 2026-08-28: the premise was wrong, and only the last sentence survives

**`deploy.py` deploys a release. It does not deploy an environment file.** The four environment
TOMLs are packaged *into* the artefact by `release.py`, and `--env` selects which of them to expand
from. So changing a value in `environments/dev.toml` and expecting a redeploy to pick it up is
asking deploy to use an input the release never carried. **The answer is to cut a new release**, and
the behaviour recorded above is the contract working rather than failing.

That was not obvious from this entry, because the "working sequence" it recommends -- `cmake
--install` then `deploy.py` -- is the development shortcut described in
[BUG-0011](#bug_0011) and not the deployment path. The entry documented the workaround and was then
read as documenting the contract.

A fix along the lines the paragraph above proposes was built on 2026-08-28 and reverted the same
day. Keeping a pristine copy of each template so an environment edit always takes effect does work,
and what it costs is the reason not to have it: configuration would reach a running venue without
passing through a release, so "what is deployed" would no longer be answerable from the artefact.
**The immutability is the feature.**

**What survives is the last sentence.** `0 template(s) expanded` followed by exit zero is
indistinguishable from a successful deployment, and it is worth distinguishing even when doing
nothing is the correct outcome -- it read as success twice in one session. Saying "nothing to
expand; these configs already match the artefact" costs a line and removes the ambiguity.

#### Answered 2026-08-28: the tools say when the deployment predates the source

Blaming a person for a mistake the layout produces is not an answer, and neither is a rule they
have to remember. `scripts/deployment_freshness.py` compares the deployed tree's `built_at`
against the modification times of tracked files, and `devenv.py start`, `ha_test.py` and
`perf_run.py` report what it finds. It warns and never blocks -- deploying an older release on
purpose is legitimate -- and it disables itself outside a git work tree, so a real target host
never sees it.

**Its first version was wrong, in the way worth recording.** It compared the artefact's git hash
against HEAD, on the assumption that a release is built from a commit. **It is not: a release is
built from the working tree as it stands**, uncommitted changes included, which is the normal case
here because changes are tested before they are committed. So the hash could match perfectly while
the deployed venue lacked every edit made since -- silent in exactly the workflow it was written
for. The comparison is now `built_at` against file modification times, which asks the question that
matters: *was this built after my last edit?*

Timestamps were rejected in the first design for being noisy, and they are: a `git checkout`
rewrites them without changing content. That cost is accepted, because the alternative was a check
that answered a question nobody was asking.

**One case it cannot catch, and will not try to.** Entirely new code that no existing file
references is invisible, because only tracked files are read. Whether a new file belongs to the
project is a decision the developer makes with `git add`, and a check that guessed would warn about
working notes on every run for ever -- which is how a warning stops being read.

**And the ambiguous message is gone.** `deploy.py` now distinguishes the three outcomes it used to
report identically: how many of how many templates were expanded, or that none were because they
had already been expanded by an earlier deployment and so nothing changed -- with the thing to do
about it if a change was expected -- or that no templates were found at all.

```
16 of 17 template(s) expanded in installed/etc/

0 of 17 template(s) expanded in installed/etc/ -- they were already expanded by an
earlier deployment, so nothing in them changed.
If you expected an edit to take effect, deploy a release built since that edit:
scripts/devsetup.sh, or deploy.py --artefact.
```

**Closed 2026-08-28.** The defect this entry names does not exist -- deploying a release does not
consult an environment file the release did not carry, and that is the contract rather than a
fault. What was real was that nothing told anybody, and two things now do.

### BUG-0063: `check_docs.py` passes links the documentation build rejects {#bug_0063}

| | |
|---|---|
| Severity | medium |
| Found | 2026-08-28 |
| Recorded | 2026-08-28 |
| How | The build failing on `doxygen_docs` while starting BUG-0009 step 3, and `git stash` showing it already failed at HEAD |
| Impact | The documentation gate can be landed broken and stay broken, and it does not see untracked files at all. Nobody notices until the next person runs a full build, and it is not their change that caused it |
| Fixed | 2026-08-28 -- check_docs.py rejects what Doxygen rejects and reads the working tree instead of the index |

**The documentation build was already failing on `main`.** Commit `4d91263` turned on
`WARN_AS_ERROR` for Doxygen, and the three commits after it added documents the gate rejects.
Confirmed by stashing the working tree and rebuilding: `doxygen_docs` fails at HEAD with nothing
of mine in it.

Ten unresolvable references across six documents, in two classes:

- **Markdown heading slugs.** `[Implementation order](#implementation-order)` becomes a
  `\ref implementation-order` command, and a GitHub-style slug is not a label Doxygen knows.
  Fixed by giving each target heading an explicit `{#label}`, the mechanism the tree already
  uses a hundred times.
- **Same-directory `README.md` links.** Doxygen resolved a bare `README.md` against the *project
  root* rather than the document's own directory, landing on a file outside its input set.
  `../availability/README.md` resolves correctly.

**`check_docs.py` reported all sixty-eight documents consistent, every link resolving, throughout.**
It validates links by its own rules, and its rules are not Doxygen's. Two gates that disagree about
what a valid link is means the cheap one gives permission the expensive one withholds -- and the
expensive one runs late, so the breakage is attributed to whoever next builds rather than to
whoever introduced it.

`docs/orientation/building.md` already documents both rules. It says to name the target section in
bold rather than link to it; an explicit `{#label}` is the better answer, keeps the navigation, and
is what the rest of the tree does. That document should be corrected too.

**A second way it gives false assurance, found 2026-08-28.** It enumerates documents with
`git ls-files '*.md'`, so an **untracked** file is not checked at all. A new document with broken
links, unresolvable anchors and no inbound reference passes silently until someone stages it --
and the count in its own success message does not move, which is the only visible clue. Confirmed
by staging one file: "68 documents" became "69 documents" with no other change. A new document is
exactly when link checking is most wanted, and is precisely when this gate is blind.

**Fixed 2026-08-28.** `check_docs.py` reads the working tree -- `git ls-files --cached --others
--exclude-standard`, which is everything not ignored, so an unstaged document is checked like any
other. Rule 6 no longer accepts a heading slug as a valid anchor, and a new rule 7 rejects a bare
`README.md` link in a document's own directory. Both messages name the fix rather than only the
fault.

Applying it immediately found **eleven** slug citations across seven documents that Doxygen would
have rejected one build at a time, all now carrying explicit `{#label}` anchors. Each rule was then
shown to fire against a deliberate violation, because a check that has never failed is not known to
work.

The original wording of this section, kept because it was the plan: `check_docs.py` should check
the working tree rather than the index, and reject what Doxygen rejects -- a `](#target)`
reference with no matching `{#target}` anywhere in the tree, and a same-directory `README.md`
link. Both are mechanical. Writing that check needs care: the first attempt at finding these by
pattern produced a false negative on single-word anchors like `#retention` and a false positive on
`building.md`, where the offending form appears inside backticks as an example of the rule.

Related: [BUG-0004](#bug_0004) and [BUG-0050](#bug_0050), which are about what Doxygen 1.8.14
cannot do. This one is about the gate not being checked, and applies whichever version is in use.

---

### BUG-0038: Inbound FIX sequence numbers are never checked, so a member's lost order is not noticed {#bug_0038}

| | |
|---|---|
| Severity | high |
| Found | 2026-08-23 |
| Recorded | 2026-08-23 (03ba5d8) |
| How | Reading the gateway's session handling against FIXT.1.1 while writing docs/fix/sequence_numbers_and_gaps.md |
| Impact | An order a member believes it placed can fail to reach the venue with neither side detecting it |
| Fixed | 2026-08-28 -- the venue checks every inbound MsgSeqNum, asks for what is missing, and bounds the wait |

`MsgSeqNum` (tag 34) on an inbound message is never compared against an expected value. The
gateway keeps no expected-inbound counter, never detects a gap in what a member sends, and never
sends a `ResendRequest` -- the only `ResendRequest` code in the gateway is the handler for
receiving one from a member.

**What this costs.** A member sends an order; the connection drops before it arrives; the member
reconnects and continues numbering from the next value. The venue was expecting the missing
number and receives the one after it, processes it, and carries on. The order that never arrived
is not requested, not logged and not missed. The member has an order it believes is resting and
the venue has never heard of it. Noticing exactly this is the purpose of the numbering.

A message arriving with a number *lower* than expected is accepted for the same reason. FIXT.1.1
calls that a serious error, on the grounds that the far side has gone backwards and its state
can no longer be trusted.

**A related defect with the same root.** `PossDupFlag` (tag 43) is written on the outbound
resend path and never read on inbound. A member recovering a gap of its own retransmits with
`PossDupFlag=Y`, meaning "you may already have this"; the gateway treats it as a new order.
What prevents a duplicate order today is the matching engine rejecting a repeated ClOrdID
within a session -- the application layer catching a session-layer failure. The member gets a
rejection for an order that does exist, and under load the rejections arrive in bulk: a run has
been observed producing 132,000 duplicate-ClOrdID warnings from a client retransmitting orders
it had not been acknowledged.

**What is needed.** An expected-inbound counter per session, checked on every message, with the
three outcomes the specification defines: equal, process; higher, hold the message and send a
`ResendRequest`; lower, a serious error unless `PossDupFlag=Y`, in which case the message has
already been processed and should be discarded rather than forwarded. The counter has to
survive a reconnect, since the series belongs to the session and not to the connection, and it
has to survive a gateway failover for the same reason the outbound counter does.

Not a small change, and it touches the session state that HA already carries across a failover.
Worth sizing before starting.

**Designed 2026-08-27 in [Inbound sequence checking](fix/inbound_sequence_checking.md); steps 1
and 2 of 4 built, 2026-08-27 and 2026-08-28.** The note carries the full plan and the state of each
step. **The venue now checks what a member sends**, which is the substance of this entry; what
remains is the retry timer for an unanswered `ResendRequest` (step 3) and the scenarios (step 4),
so this stays open until those land.

Measured against a running venue with `scripts/fix_raw_client.py`: a gap produces
`ResendRequest(expected, 0)` and nothing past it is processed; a number below expected without
`PossDupFlag` ends the session with a Logout naming both numbers; the same marked `PossDupFlag=Y`
is discarded and the session stays usable; a message with no `MsgSeqNum` is rejected without the
counter moving; a Logon above expected completes **first** and is then asked about; a Logon below
it never opens the session.

**Three things about it were wrong first, and all three were found by testing rather than
reasoning** -- the gap re-asking mid-resend, the venue starving the keepalive layer while it
waited, and then deadlocking because a member's own `ResendRequest` arrives numbered inside the
gap. See the design note.

Decisions were taken, and one of them is the reason the design was written down
before any code:

- **The resume bias after an unclean gateway death is the opposite of the outbound one.** The
  outbound number resumes deliberately high, because too low sends the member a fatal number. The
  inbound number must resume deliberately **low**, because too high makes the venue treat an
  innocent member as committing a serious error and disconnect it. The two fields will sit beside
  each other on the same three PDUs, so the instinct to treat them alike is the trap.
- **A message arriving while a gap is open is discarded, not buffered**, and the `ResendRequest`
  names `EndSeqNo=0` so the member sends it again with the rest. Its later messages wait until the
  gap is filled -- they have to, or a cancel is applied to an order the venue never received.
- **An unanswered `ResendRequest` is repeated twice and then ends the session.** A member that
  never answers would otherwise have its flow stopped for as long as it stayed connected, while
  looking healthy to anyone not reading the log -- the shape of [BUG-0009](#bug_0009). It
  reconnects against session state the venue still holds, so the disconnect is recoverable.
- **Lower than expected without `PossDupFlag` ends the session**, as the specification requires.

The note also records the awkward part: the venue does not know what to expect when a Logon
arrives, because the expected number comes back asynchronously on `SessionBoundAck`. The existing
`awaiting_sequence_state` window is where that check belongs.

Testing is unusually well served: `f8test -S` sets the client's next *send* number, the mirror of
the `-R` that manufactured the outbound gaps for BUG-0037.

**Fixed 2026-08-28**, in four steps, designed first in
[Inbound sequence checking](fix/inbound_sequence_checking.md), which carries the reasoning and the
measurements. In outline: the venue keeps an expected-inbound number per session; checks every
message against it on both inbound paths; asks for what is missing and processes nothing past a gap;
discards a marked retransmission; ends a session whose numbering has gone backwards; repeats an
unanswered request twice and then ends the session. The number survives a gateway failover on the
same PDUs that already carried the outbound one.

**The decision most likely to be got wrong** is written down where the two fields sit side by side:
the inbound number resumes **low**, with no allowance, because the two errors are not symmetrical in
the same direction as the outbound one. Too high there leaves a member a gap it can close; too high
*here* makes the venue treat an innocent member as having gone backwards and disconnect it.

**Verified by `ha_test.py` scenario 41, seen to fail first.** With the check stubbed out it fails on
its first assertion -- *"an order numbered 43 arrived when the venue expected 3 and it asked for
nothing"*, which is this entry restated by the test that catches it.

**Four defects were found while building it**, none of them in the original scope, and all by
running rather than reasoning: [BUG-0055](#bug_0055) (a member restarting its numbering left the
sequencer remembering the old one, which also reached the provenance work committed the same day),
[BUG-0056](#bug_0056), [BUG-0057](#bug_0057) and [BUG-0058](#bug_0058).

**Still open around it:** the gap-age metric, BUG-0058, so a member halted by a gap is visible
without reading a log.

### BUG-0055: A member restarting its numbering leaves the sequencer remembering the old one {#bug_0055}

| | |
|---|---|
| Severity | high |
| Found | 2026-08-27 |
| Recorded | 2026-08-27 |
| How | Raised in design review of BUG-0038: a member can set its sequence number at Logon, and the venue's own Java test client offers it |
| Impact | The venue's memory of a session is stuck on a numbering the member has abandoned, and the record of which numbers held reports becomes a mixture of two numberings |
| Fixed | 2026-08-27 -- `SessionBound` carries the reset and the sequencer discards what it remembers |

`ResetSeqNumFlag=Y` on a Logon means "forget where we were, both of us start at 1". The gateway
honours it. **The sequencer was never told**, and `SessionBound` had no field for it.

Everything the sequencer remembers about a session then describes a series the member has
abandoned. Observed on `ha_test.py` scenario 19, whose client reconnects with the flag set:

```
sequence state is new      -- outbound=1    inbound=1
unbound ... remembered outbound=1002 and 1 report range
sequence state restored    -- outbound=1002 inbound=1002   <- the reconnect asked to restart at 1
```

**Two consequences, and the second is the serious one.**

The **sequence numbers** are caught, just, by the gateway: its `reset_seq_num_requested` branch
discards whatever it is handed. But the sequencer's copy then sticks, because the updates that
follow report the new low numbers and both guards there refuse to lower. So its record stays on
the old numbering for the life of the process. Harmless while nothing reads it; fatal once
[BUG-0038](#bug_0038) does, because a returning member would be expected at a number a thousand
above where it is, judged to have gone backwards, and logged out on every reconnect thereafter.

The **report-number ranges** are not caught at all, and this reaches work that was committed the
same day. [BUG-0051](#bug_0051)'s fix records which outbound numbers held execution reports. After
a reset the gateway clears its own copy and starts again from 1 -- and ships the new ranges to the
sequencer, which **merges** them into the ranges from the old numbering. The next gateway to bind
the session is then told that numbers 2 to 1001 held reports, when in the new numbering they hold
a Logon and heartbeats. A resend would replay reports onto them, which is BUG-0051 exactly,
arriving by a road its fix did not consider.

**This is an ordinary path, not an edge case.** A member may reset at any logon, the venue's Java
test client offers it, and it is the default in the stock fix8 configuration -- so the common case
in this project's own testing is the one that was wrong.

**Fixed 2026-08-27.** `SessionBound` (120) gains `reset_seq_nums`, set from the flag the gateway
already reads at Logon and sent at bind time, before anything is handed back. The sequencer erases
the whole remembered entry -- both numbers, the report count, and the ranges -- because a reset
invalidates all of it equally. The bind then answers `known=false`, which is the truth: the venue
remembers nothing about this numbering, because the member asked it not to.

Verified both ways on scenario 19 and scenario 22. A member that asks now sees
`its remembered sequence state discarded` followed by `is new -- outbound=1 inbound=1`; a member
that does not ask still sees `restored -- outbound=1011 inbound=1011`, with no discard.

### BUG-0044: Scripts cannot answer `--help` without their plotting dependencies {#bug_0044}

| | |
|---|---|
| Severity | low |
| Found | Known when `build.py`'s pylint gate was written; the date it was first noticed is not recorded |
| Recorded | 2026-08-24, after a citation sweep found `build.py` pointing at an entry that did not exist |
| How | `check_bug_list.py` compared every citation of the bug list against the entries actually in it |
| Impact | A script that cannot describe itself on a machine without matplotlib or psutil. The gate tolerates it, so nothing fails |
| Fixed | 2026-08-27 -- the imports are lazy, and the gate runs under `-S` so it means the same on every machine |

`scripts/build.py` runs each top-level script with `--help` as part of the pylint gate, and treats
a `ModuleNotFoundError` as a skip rather than a failure. The comment there explains why, and is
right to: the Rocky container carries no matplotlib or psutil, and failing a C++ toolchain check
because a visualisation library is absent would be the same mistake as making Prometheus a
dependency of starting the venue.

**What it tolerates is still a defect.** A script imports its plotting stack at module scope, so
`--help` cannot run without it. The fix is a lazy import at the point of use, which
`pubsub_metrics.py` already does so that it can run headless.

The gate is therefore weaker than it looks on exactly the machine that most needs it: on a host
without the optional packages, those scripts are checked for nothing at all.

**Fixed 2026-08-27, both halves.**

**The scripts.** Four imported their plotting stack at module scope. `plot_performance.py`,
`plot_latency_histogram.py` and `plot_behavioural_histogram.py` now import inside the one or two
functions that plot. `monitor_memory.py` needed a different shape -- it is a top-to-bottom script
whose argparse parser and figure are built at module scope -- so its import block moved to just
after `parse_args()`, which is where `--help` has already exited. Its error message now names the
module that is missing instead of guessing.

**The gate, which mattered more.** Tolerating a `ModuleNotFoundError` was the reason this could
persist, so the gate no longer tolerates one, and its failure message says what to do about it.

**That alone would have been close to cosmetic**, and finding out why is the useful part of this
entry. The check runs `<script> --help` on the machine doing the build, where matplotlib usually
*is* installed -- so a module-scope import passes there and fails only on the container that
lacks it. Failing rather than skipping would have moved the alarm to the one machine nobody is
watching. The check therefore runs the script under **`python3 -S`**, without site-packages, so
only the standard library and the script's own directory are importable. It now means the same
thing everywhere, which is what its success line claims: *all 33 scripts answer --help using only
the standard library*.

Both directions verified: all 33 pass, and re-adding a module-scope `import matplotlib` to
`plot_performance.py` fails the build on this machine with the message pointing at the fix.

### BUG-0017: Slab allocator design notes do not mention the tripwire {#bug_0017}

| | |
|---|---|
| Severity | low |
| Found | 2026-08-09 |
| Recorded | 2026-08-09 (f413a72) |
| How | Noticed while fixing the tripwire |
| Impact | Documentation only |
| Fixed | 2026-08-27 -- the tripwire's behaviour and its two-condition rule are in the allocator notes |

`drain_empty_slab_queue()` carries a safety tripwire that throws and takes the reactor down
with it. That is a significant behaviour of the allocator and it appears nowhere in the
design notes -- only in a comment inside the function. Anyone reasoning about failure modes
from the documentation would not know the allocator can terminate a component.

Worth adding when the allocator documentation is next touched, together with the rule that
the condition needs both a spent budget and a spun loop.

**Fixed 2026-08-27** in [Allocators](framework/allocators.md), under *Wall-Clock Drain Tripwire*.

A section of that name already existed and said the drain "aborts after 1 second of wall-clock
time". It omitted both things this entry was raised about: that aborting means **throwing**, and
that the throw terminates the reactor. It also omitted the two-condition rule.

That rule is now written down with its reasoning, because it is not obvious and looks like
belt-and-braces until you see why. Wall-clock time alone does not describe progress: a tight retry
loop runs hundreds of thousands of iterations inside a sub-millisecond preemption, while a thread
the scheduler has not run manages **one** in a whole second. Only the iteration count tells a
stuck producer from an ordinary preemption, so the budget alone would fire on a healthy system
under load.

Also recorded: why the clock is read every iteration rather than short-circuited away by the
cheaper test — the read yields the few tens of nanoseconds a mid-enqueue producer needs, so the
spin does not starve what it is waiting on — and why the exception message leads with counters and
puts the conclusion last.

### BUG-0016: `start_fix_seq_system.py` launches from configs that no longer exist {#bug_0016}

| | |
|---|---|
| Severity | low |
| Found | 2026-08-09 |
| Recorded | 2026-08-09 (d9b38e5) |
| How | Tracing what still referenced `matching_engine.toml` before deleting it |
| Impact | The script cannot start a venue; it fails at its first component, and nothing says why until you read it |
| Fixed | 2026-08-27 -- deleted as superseded |

`start_fix_seq_system.py` in the repository root starts the sequencer from
`etc/sequencer/sequencer.toml` and the matching engine from `etc/matching_engine/matching_engine.toml`.
Neither file exists. They are pre-rename names: `arbiter.toml` and `sequencer.toml` were removed
when the HA `_primary`/`_secondary` pair replaced them, and `matching_engine.toml` was deleted on
2026-08-09 as the last of that family. `ha_test.py` already carries a comment noting these three
are "orphaned and rejected by today's binaries".

It is not obviously dead code — 10 KB, executable, in the root — so the next person to reach for
it will spend time on it before finding out.

**Fix is a choice, not a lookup.** Either repoint it at `sequencer_primary.toml` and
`matching_engine_primary.toml`, the way `ha_test.py` does for its single-engine topology, or delete
it as superseded by `devenv.py` and `perf_run.py`. That question was deliberately left open rather
than answered in passing.

**Deleted 2026-08-27**, taking the second of the two options above.

**It was worse than recorded here.** Four configuration files it names are gone, not two:
`authentication_service.toml` and `arbiter.toml` as well as `sequencer.toml` and
`matching_engine.toml`. The authentication service is the *first* component it starts, so it failed
at step one rather than at the sequencer.

**Everything it did is covered, on every axis:**

- **Interactive running** -- `devenv.py`, which is strictly more capable: `start`, `stop`, `status`
  and `restart [name]`, driven by the environment TOML rather than a hard-coded component list, with
  `--supervised` to restart what dies, and the Java components handled alongside the C++ ones.
- **Running from an instrumented install prefix** -- `perf_run.py` and `ha_test.py` both take the
  prefix positionally; `devenv.py` reads it from the environment file, so an instrumented tree needs
  a copy of `dev.toml` with `install_dir` repointed.
- **Valgrind** -- `callgrind_run.py`, which is built for it and explains in its own docstring why a
  copy of `perf_run.py` will not do: callgrind runs the guest twenty to fifty times slower, so
  readiness has to be established by polling logs rather than by sleeping.

Its hard-coded component list is also why it rotted. `devenv.py` reads the same environment TOML
that `deploy.py` expands, so a component renamed in one place cannot be missed in the other.

References repointed rather than left dangling: the hint `build.py` prints after an instrumented
build, the instrumented-run recipe in `docs/orientation/building.md`, its entry and two mentions in
`docs/framework/summary.md`, the startup-order comment in `ha_test.py`, and the roadmap's item 5.
The mentions in `docs/history/sessions.md` are left alone: they record what happened at the time,
which is still true.

### BUG-0054: `build-log.txt` looks like a build log and is three days stale {#bug_0054}

| | |
|---|---|
| Severity | low |
| Kind | task -- a repository artefact to remove, not a code defect |
| Found | 2026-08-27 |
| Recorded | 2026-08-27 |
| How | Checking whether a newly added unit test had actually run, and finding the figure came from a file no build writes |
| Impact | Test results read from it are confident and wrong |
| Fixed | 2026-08-27 -- deleted |

`build-log.txt` sits in the repository root, is untracked, and is **not written by anything**.
Neither `scripts/build.py` nor `scripts/build.sh` produces it. It was last written on 2026-08-24
and its contents name the pre-move path,
`pubsub-poc-attempts/simple-publish-poc/pubsub-project-10-copilot`.

It contains plausible `[  PASSED  ]` lines, so grepping it after a build returns a clean answer
that has nothing to do with the build. On 2026-08-27 "646 unit and 46 integration tests pass" was
reported several times across a working session; every one of those figures came from this file
rather than from any build actually run. The real figure that day was about 1023 tests across the
framework unit and integration suites, `fix_codec`, `scram_crypto` and five application suites.

**The same shape as the `coverage_baseline_fresh.txt` trap**: an artefact that was true once,
still looks authoritative, and is never regenerated to contradict itself. Deleting it costs
nothing -- the build prints its own output and exits non-zero when a suite fails.

Related: BUG-0040, which is the same class of fault in an instrument rather than an artefact.

**Deleted 2026-08-27.** Nothing referenced it. The build prints its own output and exits
non-zero when a suite fails, so reading a file for the answer was never necessary.

### BUG-0053: A bounded resend is answered with the most recent reports, not the ones asked for {#bug_0053}

| | |
|---|---|
| Severity | medium |
| Found | 2026-08-27 |
| Recorded | 2026-08-27 |
| How | Reading the resend path against the sequencer's reply contract, while designing the fix for BUG-0051 |
| Impact | A member asking about an old range of its stream is sent recent reports under those numbers, with the right sequence numbers on them |
| Fixed | 2026-08-27 -- the gateway names which reports it wants rather than asking for a count |

`SequencerThread::handle_session_replay_request` keeps a sliding window of the session's matching
records and returns **the most recent `max_records`** of them. The gateway's request says how many
it wants and nothing about which, and its comment explains why that was thought sufficient:

> The sequencer returns the most recent that many reports, which is what a member has missed --
> it is the tail of its stream that went undelivered, never the beginning.

That holds for the case the venue actually sees: a member reconnects after a disconnect and asks
for everything from where it stopped, so the gap is the tail. **It does not hold for a bounded
request.** A member asking for 500 to 550 out of a stream that has reached 10,000 is asking about
the middle, and the sequencer answers with the fifty most recent reports instead. They carry the
right sequence numbers and the wrong contents, which is the same class of fault as
[BUG-0051](#bug_0051) and equally invisible to the member.

**Reachable only since `EndSeqNo` was honoured**, on 2026-08-27 under
[BUG-0039](#bug_0039). Before that every resend ran to the head of the stream, so every resend was
a tail request and the assumption held. Bounding the replay bounded the count without changing
which reports come back, so the latent half surfaced. That is an argument for having honoured it,
not against: the venue was previously papering over this by ignoring what the member asked for.

**Reproduced 2026-08-27** by `ha_test.py` scenario 40, which asks for numbers 100 to 149 out of a
thousand-order session and compares what comes back, by ClOrdID, against what the member itself
received under those numbers:

```
50 message(s) came back, none outside 100..149 -- OK
50 of 50 resent message(s) carry a different order than the member was originally sent
under that number (100: was 'ord99', resent 'ord951'; 101: was 'ord100', resent 'ord952';
102: was 'ord101', resent 'ord953' ...)
```

The offset is constant: they are the fifty **most recent** reports, `ord951` to `ord1000`, wearing
the numbers 100 to 149. Every other property of the reply is correct -- the count, the bounds, the
numbering, `PossDupFlag`, `OrigSendingTime` -- which is why nothing on the member's side
distinguishes it from a correct resend.

**The range has to come from the middle of the session's history to see this.** A venue that
ignores what was asked for and returns the most recent reports answers a request for the *tail*
correctly by accident, so a tail request discriminates nothing.

**Scenario 40 also settles what BUG-0039 was closed without.** That nothing came back outside
100..149 is the test that `EndSeqNo` is read and honoured, which was owed and missing.

**The fix belongs in the design for BUG-0051**, at
[Resend provenance](availability/resend_provenance.md), because both come from the same missing
fact. Once the venue records which outbound number each report went out on, it can name the
reports it wants by position in the session's stream rather than asking for a count and trusting
the sequencer to guess which end.

**Fixed 2026-08-27.** `SessionReplayRequest` gained `skip_most_recent`: how many of the session's
newest reports to pass over before collecting. The sequencer widens its window by that much and
drops the newest that many at the end, so "the most recent N" becomes "the most recent N below a
point the caller names", and the tail case is the same code with the point at zero.

The gateway can always compute it, and only because of the record built for
[BUG-0051](#bug_0051): the reports above the range being replayed are recent, so the record covers
them and they can be counted exactly. The two defects came from one missing fact and were fixed
together.

Verified by `ha_test.py` scenario 40, which reproduced it: a member asking for numbers 100 to 149
out of a thousand-order session now receives, under each of those numbers, the very order it was
originally sent under that number.

### BUG-0052: The resend truncation warning fires on every healthy resend {#bug_0052}

| | |
|---|---|
| Severity | low |
| Found | 2026-08-27 |
| Recorded | 2026-08-27 |
| How | Reading the gateway log of the scenario 22 run that found BUG-0051 |
| Impact | A WARNING that says the member was short when it was not, on every resend a busy session makes |
| Fixed | 2026-08-27 -- truncated now means the reply was short of what was asked for |

`SequencerThread::handle_session_replay_request` sets `truncated` when the session has more
matching records in the WAL than the reply window holds:

```cpp
truncated = total_matched > static_cast<int64_t>(window.size());
```

That is true of every resend on a session with any history: the window holds the gap the member
asked for, and the WAL holds everything the session has ever done. The gateway then logs

```
resend was truncated at the sequencer's record cap (last_seq_no=38503449) --
the member has been sent what fitted and gap-filled the rest
```

immediately after reporting the resend complete with no gap left. **The member was not short.**
It asked for twenty-one messages and received twenty-one.

Two separable faults. The condition means "there is history older than what was asked for",
which is not truncation and is the normal state of affairs; truncation would be the window
failing to hold the range the member requested. And `last_seq_no` is a WAL record id, not a FIX
sequence number, which is why the figure reads as alarming next to a member whose numbering is
in the low thousands -- the message does not say which numbering it is quoting.

**Fixed 2026-08-27.** `truncated` is now `record_count < max_records`: the caller was given less
than it asked for, so part of its range reaches further back than the WAL still holds. The old test,
`total_matched > window.size()`, was true of every resend a session with any history makes, which is
why the warning fired on healthy ones. Verified by the runs that fixed BUG-0051: zero occurrences
across both gateway logs, where before it fired on every resend.

The misleading `last_seq_no` in the message is unchanged and still a WAL record id rather than a FIX
sequence number. It now only appears when something really was truncated, which is when a reader
wants the WAL number anyway.

### BUG-0051: A resend reuses sequence numbers that carried administrative messages, and the session dies {#bug_0051}

| | |
|---|---|
| Severity | high |
| Found | 2026-08-27 |
| Recorded | 2026-08-27 |
| How | `ha_test.py` scenario 22, once it was made to manufacture a gap -- BUG-0037. Found on the first run that had anything to resend |
| Impact | A member that asks for a gap is answered correctly message by message and then loses the session on the next heartbeat. The resend reports success |
| Fixed | 2026-08-27 -- the venue records which outbound number each report went out on, and gap-fills the rest |

The gateway holds no store of what it sent, so it replays by rewinding its outbound sequence
number to `BeginSeqNo` and filling every number in the range with an execution report from the
sequencer's WAL. **The range does not contain only execution reports.** Whatever administrative
traffic occupied those numbers -- a Logon, a Heartbeat -- is not in the WAL and has no record
anywhere, so the venue puts a report on that number instead, and the number goes out twice
carrying two different messages.

Observed on 2026-08-27, with the member returning into a gap of twenty:

```
16:51:22.158874  FIX OUT  8=FIXT.1.1|35=A|34=1002|...      <- Logon
16:51:32.206291  FIX OUT  8=FIXT.1.1|35=0|34=1003|...      <- Heartbeat
16:51:32.206879  ResendRequest BeginSeqNo=983 -- resending 983..1003, will resume at 1004
16:51:32.223220  resend complete -- 21 report(s) resent, no gap left
```

Twenty-one reports were sent into 983..1003. Numbers 1002 and 1003 had already gone out as the
Logon and the Heartbeat. The member had received the heartbeat, held it as the message that
revealed the gap, and processed it after the replay filled in behind it -- so it counted
twenty-two messages where the venue had counted twenty-one, and came out expecting 1005 while
the venue stood at 1004. Ten seconds later the venue sent its next heartbeat:

```
Fatal  Message Sequence too low, received: 1004 expected: 1005 - will logoff
```

**Every message-level assertion passes.** The twenty-one reports are real, all inside the
requested gap, all marked `PossDupFlag=Y`, all stamped `OrigSendingTime`, and the range closes
where the gateway said it would. The resend is correct in every particular and the session is
dead. That is why scenario 22 asserts that the session survives the heartbeat that follows,
which is the only one of its six assertions that fails.

**The venue cannot currently know which of its outbound numbers were administrative.** The
gateway knows at the moment it sends -- it is the one choosing between an execution report and a
heartbeat -- and it does not record it. Nothing else can reconstruct it: the WAL holds reports
and knows nothing of the FIX numbering laid over them, and the sequencer's session state carries
only where the numbering had reached.

**The symptom is intermittent, and the reason matters for diagnosing it.** The member ends the
resend ahead of the venue by one for each such number, but a terminating `SequenceReset-GapFill`
sets its expected number outright rather than counting, so a resend that ends in one resynchronises
the member and hides the fault. The session dies only when the reports exactly fill the range and
no gap-fill is emitted -- the `no gap left` case above. Measured on 2026-08-27: with the range
deliberately over-filled by 3883 reports, the session **survived**, because the trailing gap-fill
corrected it. So the reliable symptom is not a dead session; it is a member handed reports under
numbers that never carried them. The sequencer returns the *most recent* N reports for the
session, so what it receives are recent reports presented as the old missing ones, and its record
of the session is wrong in a way neither side can detect.

**Designed 2026-08-27 in [Resend provenance](availability/resend_provenance.md), not built.** In
outline: record the outbound number each report went out on -- the primary fact -- so a resend
places each report back on its own number and gap-fills every number no report claims. That needs
no separate notion of which numbers were not reports, because it is the complement. It belongs in
the session state the sequencer already hands to whichever gateway binds the session, because a
resend is served by whichever instance holds the session now, which after a failover is not the
one that sent the messages being asked about.

**A gateway-local mechanism recording the complement was built the same day and reverted.** It
worked, and it was the wrong shape: a derived fact that could not grow into the real answer, a
second concept bolted on to cover the first one's lifetime, an unbounded per-comp-id map, and
silent degradation when that map was trimmed. The design note records it in full so the reasoning
is not repeated.

**What is tested. Two scenarios fail on this, from different angles, and both are expected to
until the design above is built.**

- **Scenario 22**, the ordinary reconnect. Its other six assertions pass, which is the point:
  every message-level property of the resend holds while it is broken. It fails on the heartbeat
  put deliberately in the middle of the gap, which is filled with a report instead of skipped.
- **Scenario 23**, the failover. The member returns on the surviving instance, asks for what it
  missed, and is left expecting a number the venue will not send, because that instance filled the
  whole range with reports. Measured 2026-08-27: gap-fills received, none.

What no test can reach is whether the reports were placed on the numbers they came from. The
sequencer returns the most recent reports for the session, so after a failover the member is
handed recent ones presented as the old missing ones, and nothing on its side distinguishes that
from a correct answer.

Related: BUG-0037, which is how this was found, and BUG-0006.

**Fixed 2026-08-27**, by building [Resend provenance](availability/resend_provenance.md). The
gateway records the outbound number each execution report goes out on; the record travels to the
sequencer on `SessionSequenceUpdate` and `SessionUnbound` and comes back on `SessionBoundAck`, so
it outlives the instance that made it. A resend places each report back on its own number and
gap-fills, in runs, every number the record does not cover -- which is one rule for "held something
unreplayable" and "older than the venue remembers", because both want the same treatment.

**The failover half is fixed too, and that was the point of putting the record in the sequencer.**
`ha_test.py` scenario 23 kills the gateway holding a session and brings the member back on the
surviving instance, which never sent the messages being asked about: it resent **1000 real
reports**, where before it filled the range with whatever the sequencer returned. A gateway-local
record was built first and reverted because it is empty in exactly this case; the design note
records why in full.

Three scenarios pin it, all of which failed on this until it was built: 22 (ordinary reconnect,
with a heartbeat placed mid-gap on purpose), 23 (failover), and 40 (bounded range).

**What remains true.** A session that lives and dies inside one reporting interval leaves the
sequencer no record, and the surviving instance then gap-fills the whole range rather than
guessing. The member keeps its session and its numbering and loses those reports. That is the
designed behaviour, not a residue of this defect -- see the note's Retention and shipping
sections.

### BUG-0037: The resend scenario never creates a gap, so nothing tests the resend {#bug_0037}

| | |
|---|---|
| Severity | medium |
| Found | 2026-08-23 |
| Recorded | 2026-08-23 (5bc615c) |
| How | Reading the reconnecting client's own output while planning assertions for it |
| Impact | The FIX resend path has no member-observable test coverage, while a scenario named for it passes |
| Fixed | 2026-08-27 -- the scenario manufactures a gap with `f8test -R` and asserts six things about what the member is handed |

`ha_test.py` scenario 22, `resend_recovery`, is described as "A reconnecting member is sent the
execution reports it missed" and promises the member "receives real execution reports marked
PossDupFlag=Y instead of a blanket gap-fill".

It asserts neither. What it asserts is that the venue resumed the session's numbering across the
reconnect, which it does correctly and which is worth having. The reconnecting client's output
was checked after a run: **579 bytes, containing one Logon and nothing else.** Zero execution
reports, zero `PossDupFlag`, zero `SequenceReset`, zero `OrigSendingTime`. Scenario 23,
`inflight_gateway_death`, produces an identical 579-byte Logon-only output.

The cause is straightforward once seen. The scenario sends its baseline orders, waits for their
reports, and only then drops the session -- so at the moment of the drop the venue owes the
member nothing. Nothing is missed, so the member has no gap to notice, sends no ResendRequest,
and there is nothing to resend. `orders_during_override=0` and `orders_after_override=0` ensure
no traffic arrives during the window either.

**A gap has to be manufactured, and the choice of how is a design decision.** Three candidates:

- **Reconnect after the cancel-on-disconnect grace expires.** The member leaves orders resting,
  the venue cancels them and emits a report per order into a socket that is gone, and the member
  returns to a real gap of its own orders being cancelled. Uses only behaviour the venue already
  has, and is the most realistic of the three. It needs checking that the session's sequence
  state outlives the grace period -- if it does not, the member's numbering restarts and the
  test fails for an unrelated reason.
- **Stop the client with orders in flight**, so the reports land while it is away. Closest to
  scenario 23's intent, and depends on timing that will vary by machine.
- **Have the venue generate reports for a disconnected session directly**, which is the most
  controllable and the least like anything that happens.

Until one is chosen, three assertions have nothing to run against: that `PossDupFlag` is set
inside the requested gap and absent beyond it, that `OrigSendingTime` accompanies every resent
report, and that the terminating gap-fill leaves the member expecting the number the venue will
send next. All three are cheap once a gap exists, and all three read tags from the client's own
output, which is the member-observable fact rather than the venue's account of itself.

A helper for the first of them, `_client_report_counts`, already exists in `ha_test.py` and is
never called.

**The scenario should not be left describing work it does not do.** Whatever is decided about
manufacturing the gap, the description and expected outcome need to match what is asserted, or
the next person to ask "is the resend path tested?" gets the same wrong answer twice.

**Fixed 2026-08-27.** None of the three candidates above was used, because the first two cannot
produce a gap at all and that is worth recording before anyone tries them again.

**The venue only advances a session's outbound number when it actually sends.** A report for a
session that has gone is dropped before that point -- at the sequencer
(`SequencerThread.cpp`, "session not bound to any instance, dropping") and again at the gateway,
where `find_session_by_conn_id` returns nothing. `on_connection_lost` unbinds the session the
instant the socket closes, so the cancel-on-disconnect reports that the first candidate depends
on are emitted into an unbound session and consume no numbers. The member returns expecting
exactly what the venue is about to send. The second candidate fails the same way outside a short
race: only the reports numbered before the gateway notices the socket died are lost.

**What was used instead is a fourth option none of the three anticipated.** `f8test` takes `-R`,
"set next expected receive sequence number", so the reconnecting client is started believing it
has received twenty fewer messages than it has. The gap is exact, chosen, free of timing, and
needs no change to the venue -- and it is what a member looks like after messages died in a
socket, which is the case the resend path exists for.

One thing had to be added for the member to be able to ask. fix8 decides what to do about a
too-high inbound number in `Session::enforce`, and the branch depends on the session state: once
continuous it sends a `ResendRequest`, but at logon -- where a member reconnecting into a gap
first sees the number -- it throws `InvalidMsgSequence` and logs off, unless the session config
sets `ignore_logon_sequence_check`. Without it the client answered a venue that had resumed its
numbering correctly by dropping the session. The generated no-reset config now sets it.

The scenario asserts six things, five of them read from the client's own received messages: the
venue continues the numbering; the member asks for the right range; the reports that come back
are real, marked `PossDupFlag=Y`, and inside the requested gap and nowhere beyond it; every one
carries `OrigSendingTime`; the range closes where the gateway said it would; and the session is
still there afterwards.

**The sixth is the one that matters, and it fails.** The first five pass and the session dies on
the next heartbeat. That is BUG-0051, which this test was written to be capable of finding and
found on its first run with anything to resend.

### BUG-0039: ResendRequest ignores EndSeqNo, so every resend runs to the head of the stream {#bug_0039}

| | |
|---|---|
| Severity | medium |
| Found | 2026-08-23 |
| Recorded | 2026-08-23 (03ba5d8) |
| How | Reading the resend path against the FIX session-layer rules rather than against its own comments |
| Impact | None in normal operation. A member asking for a bounded range gets an unbounded replay, and the venue pays for it |
| Fixed | 2026-08-27 -- a non-zero `EndSeqNo` inside the stream is honoured, and the remainder is gap-filled |
| Tested | `ha_test.py` scenario 40, added 2026-08-27: a bounded request returns nothing outside its bounds |

`FixOrderGatewayThread::handle_resend_request` reads `BeginSeqNo` (tag 7) and nothing else.
`EndSeqNo` (tag 16) is never read anywhere in the codebase, though the data dictionary carries
it and marks it required on ResendRequest. The gateway replays from `BeginSeqNo` to wherever
the session had reached, which is to say it treats every request as though `EndSeqNo` were 0.

**Recorded as a deviation rather than a bug**, because in the case that actually happens it is
the correct behaviour. `EndSeqNo=0` means "everything from here on" and is what an engine sends
after a disconnect, which is when resends occur. A member that asked for 100-150 and receives
100 onwards sees messages in sequence, and its inbound counter advances exactly as it would
have; nothing in the session breaks.

**Where it stops being benign is a bounded request.** A member asking for fifty messages gets
every report since, and the reports come from the sequencer's WAL rather than from a buffer in
the gateway -- so a small request from one member turns into a large slice read on the shared
path that live traffic depends on. On a session with millions of reports behind it that is a
substantial amount of work, asked for by a message the member was entitled to send.

The gap between "no member does this" and "no member can do this" is the whole of the risk. It
is unusual engines behaving reasonably that find this kind of thing, and they find it in
production.

Three ways to settle it, in increasing effort:

- Honour `EndSeqNo` when it is non-zero, which is what the specification asks for.
- Keep the current behaviour and say so deliberately in a comment, so the next reader knows it
  was considered rather than missed.
- Bound the replay regardless of what was asked for, which addresses the cost but not the
  conformance.

Whichever is chosen, `EndSeqNo` being absent from the code with no comment is the one state
that should not persist, because it cannot be told apart from an oversight.

**Fixed 2026-08-27**, by the first of the three: `EndSeqNo` is read, and a non-zero value falling
inside what the session has sent bounds both the reports asked of the sequencer and the numbers
replayed into. A value of zero, or one at or past the end of the stream, asks for nothing the
venue does not already give and is treated as the open-ended case it is.

**Closed without a test, and that was an omission rather than a tooling limit.** f8test can drive a
bounded request -- its `R` command reads `BeginSeqNo` and `EndSeqNo` from stdin -- which was not
checked before closing this. Scenario 40 was added the same day and confirms the bound is honoured.
It also reproduced [BUG-0053](#bug_0053): the bound is respected and the reports inside it are the
wrong ones.

The consequence worth naming is what happens above the bound. This gateway holds no store of what
it sent, so it replays by rewinding its live outbound number, and that number has to be returned
to where the session had reached before the next live report goes out. A gap-fill over the
unrequested remainder is the only in-band way to do it, so a member that asks for a bounded range
is told to skip everything above it. That is not the member being denied what it asked for -- it
asked for no more than the bound -- but it does mean the remainder has to be asked for before it
is gap-filled, rather than after. Both the bound and this consequence are commented at
`handle_resend_request`.

### BUG-0049: The docs build's strictest gate is silently off on RHEL8 {#bug_0049}

| | |
|---|---|
| Severity | medium |
| Found | 2026-08-24 |
| Recorded | 2026-08-24 |
| How | Testing a proposed `Doxyfile` change against the RHEL8 container before adopting it, and checking what else that Doxygen makes of the file |
| Impact | Documentation warnings fail the build on the development host and are ignored on the target platform, which is the one where they render as silent breakage |
| Fixed | 2026-08-24 -- `WARN_AS_ERROR = YES`, understood by both versions, and `scripts/check_doxyfile.py` to keep the file readable by 1.8.14 |

`Doxyfile` sets `WARN_AS_ERROR = FAIL_ON_WARNINGS`. That value was introduced in Doxygen 1.9.
RHEL8 ships **1.8.14**, where `WARN_AS_ERROR` is a boolean and the value is rejected:

```
warning: argument `FAIL_ON_WARNINGS' for option WARN_AS_ERROR is not a valid boolean value
Using the default: NO!
```

**So the gate falls back to off, on the platform that needs it most.** BUG-0004 records that 1.8.14
does not fail on an unresolved `\ref` -- it renders one as `href="../../"`, a bare directory link
that a browser opens as a directory listing. The one Doxygen that fails silently is the one running
without the setting meant to stop it.

A second tag is ignored there too: `MARKDOWN_ID_STYLE` is 1.9-only, so any reliance on
GitHub-style heading ids would work on the development host and quietly produce positional
`autotoc_md*` ids on RHEL8. That is why the bug list's summary links use explicit `{#bug_nnnn}`
labels, which both versions support, rather than heading slugs.

**Two things to settle, and they are separable.**

- `WARN_AS_ERROR = YES` is understood by both: 1.8.14 treats it as a boolean, and 1.9 accepts it
  as the equivalent of failing on a warning. Whether that is the right setting for this project
  is a judgement, but a value only one of the two versions understands is not.
- **Nothing checks that the `Doxyfile` stays readable by 1.8.14.** Both faults were found by
  running the container by hand. `docs/orientation/building.md` documents how to do that, and
  `release_check.py` has a Rocky stage, so the mechanism exists; nothing joins them up.

**Fixed 2026-08-24.** The value is now `YES`, which 1.8.14 accepts as a boolean and 1.9.8 accepts as the equivalent of failing on a warning. `scripts/check_doxyfile.py` checks every tag against the 1.8.14 vocabulary, captured in `scripts/doxygen_1_8_14_tags.txt`, and `--container` runs the real 1.8.14 against the file rather than the snapshot. Both fault classes were proved by reintroducing them.

Turning the gate on revealed why it had never been on: 1.8.14 cannot build this documentation at all under it. That is BUG-0050.

### BUG-0007: Metrics silently disabled when CPU pinning is off {#bug_0007}

| | |
|---|---|
| Severity | medium |
| Found | 2026-08-04 |
| Recorded | 2026-08-08 (8cc0ced) |
| How | Reading the configuration loaders while adding the Prometheus metrics |
| Fixed | 2026-08-08 |

`config.metrics_configuration = MetricsConfigurationLoader::load(toml)` sat **inside** the
`if (config.cpu_pinning_enabled)` block in the matching engine and both gateway loaders. A
component with pinning turned off exposed no metrics at all — no error, no warning, just an
endpoint that never appeared.

Left alone when first found because nothing depended on it. It became urgent when the test
harnesses moved their ground truth onto those counters, at which point a silent disable would make
them pass while verifying nothing. **This is exactly the defect that motivated this file.**

### BUG-0008: A growing hash map stalls the reactor callback thread for over a second {#bug_0008}

| | |
|---|---|
| Severity | high |
| Found | 2026-08-08 |
| Recorded | 2026-08-08 (8cc0ced) |
| How | The first clean compressed-trading-day load run — the reactor's own watchdog logged it, and the profile confirmed the cause |
| Impact | p99 of 733 ms at 4.2M orders, over 1 s at 8.4M, after which the pipeline did not recover and 1,167,392 of 9,556,000 orders were never accepted |
| Fixed | 2026-08-21 (68ab2b5) |

The matching engine's order book is a `tsl::robin_map` with `power_of_two_growth_policy<2ul>`. It
doubles, and each doubling rehashes the whole table **on the callback thread**. Stalls land only on
exact powers of two — all five stalls of 200 ms or more did, and none occurred elsewhere.

**This is a framework requirement, not an application bug.** Calling `reserve()` in the matching
engine would remove the symptom from a stub and teach nothing. `pubsub_itc_fw` offers slab and pool
allocators for messages *in flight* and nothing at all for long-lived hot-path state that *grows* —
so every application that keeps state (an order book, a session table, a subscription registry)
will grow a container on a reactor callback thread and eventually stall it.

See [Compressed Trading Day Load Profile](operations/trading_day_load.md).

**The framework half is done, 2026-08-11.** `IncrementalRehashMap` is the growable structure the
design note asked for: when the table must grow it allocates a second one and moves the entries
eight slots per mutating operation, searching both while the move is in flight. The worst case for
any one operation is a probe plus eight moves, whatever the size of the map -- where the old cost
was O(capacity) in the operation that crossed the threshold, and the capacity is what kept
doubling. Rehashing on a background thread was the alternative and was rejected: it puts two
threads on one table, which means a lock on the hot path or a lock-free open-addressed table with
tombstones. Migrating a few slots at a time keeps the structure thread-confined, as the container
it replaces already was -- and confinement is now checked rather than assumed, under
`PUBSUB_ITC_FW_THREAD_CHECKS`.

What remains O(capacity) is clearing the new table's state array: one byte per slot, a memset of
about 8 MB at the size that stalled for over a second, which measures under a millisecond. That is
recorded in the header rather than glossed over.

47 unit tests, counting the ones added for `GrowthReportingAllocator` alongside it: differential
testing against `std::unordered_map` over three seeds and three workload shapes, the migration
boundaries driven one step at a time through a template parameter, adversarial hashes that put
every key in one probe chain, lifetime accounting proving every construction is matched by a
destruction, and -- the property the class exists for -- a bound on entries moved per operation
that is **counted rather than timed**, so a loaded machine cannot make it flaky and a return to
one-pass rehashing fails it immediately. Clean under ASan, and under the C++23 dialect.

**Cured, and measured — see "The measurement that closes this entry" below.** The order book is
on `IncrementalRehashMap`, and a trading-day run on 2026-08-21 shows peak p99 flat at 1.78–4.27 ms
across six growth steps from 305 MB to 9760 MB, against 96 ms, 733 ms and over a second for the
same steps under `tsl::robin_map`.

**Committed, so this entry moves to Fixed.** It previously read: this entry stays under Open
only because the change is uncommitted. It moves to Fixed when the
order-book swap lands. Nothing else is outstanding on it.

What the container does *not* do is reduce peak memory — it holds two tables for *longer*, across
the whole migration rather than one operation. That is now the binding constraint rather than
latency, and it has its own entry: BUG-0028.

#### The 2026-08-16 run measured the old book, not the swap

A trading-day run was made on 2026-08-16 intending to measure the order book on
`IncrementalRehashMap`. It did not: the binary it exercised was the `tsl::robin_map` build
installed on 2026-08-11. The swap was compiled into `build/` at 16:59 that day and never
deployed, and `perf_run.py` takes an install prefix and runs what is already there -- it neither
builds nor deploys. So the run is a second `tsl::robin_map` measurement, and says nothing about
the container it was meant to test.

Three independent confirmations, recorded so this is not re-argued:

1. **Symbols.** `installed/bin/matching_engine`, dated 2026-08-11 20:29, carries 13 `robin_map`
   symbols and no `IncrementalRehashMap` symbol. `build/applications/matching_engine/matching_engine`,
   dated 2026-08-16 16:59, is the reverse: no `robin_map`, 7 `IncrementalRehashMap`.
2. **The log line number.** Every growth report in `matching_engine_primary.log` names
   `MatchingEngineThread.cpp:162`. That is where the `PUBSUB_LOG` sits in the committed source;
   the swap adds comment lines above it and moves it to 164.
3. **Bytes per slot.** The reports are 156, 312, 624, 1248, 2496 and 4992 MB, and 4992 MiB over
   2^24 buckets is exactly 312 bytes each. `sizeof(std::pair<OrderKey, OrderEntry>)` is 304
   (`OrderKey` 134, `OrderEntry` 168), and robin_map adds an 8-byte distance-and-hash word per
   bucket: 312. `IncrementalRehashMap` reports `state_bytes + entry_bytes`, one extra byte per
   slot, which would have logged 5008 MB rather than 4992.

**Correcting a figure this entry carried.** The table did not cost 624 bytes per slot. It cost
**312 bytes per bucket**, and the six growth reports correspond to bucket arrays of 2^19 through
2^24 -- reached, at robin_map's half-full growth policy, somewhere around 2^18 to 2^23 live
orders. The earlier "2^21 / 2^22 / 2^23" labels were counting entries rather than buckets, which
is consistent, but the per-slot figure was double the truth and any sizing argument built on it
was wrong by a factor of two.

**What the run does establish**, all of it about `tsl::robin_map`:

| allocation | buckets | growth step at | p99 then | first run, 2026-08-08 |
|---|---|---|---|---|
| 1248 MB | 2^22 | 18:38:32 | **84.94 ms** | 96 ms |
| 2496 MB | 2^23 | 18:48:37 | **378.32 ms** | 733 ms |
| 4992 MB | 2^24 | 19:08:47 | **997.20 ms** | over 1 s |

Median p99 in each window was ~0.9 ms, so the tail-excursion signature reproduces: the bulk of
orders are untouched and a few are delayed catastrophically. The spread against the first run --
733 ms against 378 ms at the same step, on the same container -- is run-to-run variation, and is
itself worth knowing, because it sets how large a difference the re-run must show before it means
anything.

**The profile is off-CPU.** `perf record` on `matching_engine_primary` across all six growth steps
gives a flat profile for the reactor thread: the top symbol is `handle_new_order_single` at 0.66%,
and no allocation or rehash symbol appears near the top. A one-second *CPU* stall would be
impossible to miss at that sampling rate, so the thread is blocked rather than spinning, and
cycle-based sampling cannot attribute it. This holds for the robin_map book and is the most useful
thing the run produced.

**The allocation hypothesis is untested, not supported.** The idea that the stall is the single
large `operator new` for the entry array was framed around `allocate_table`, which is
`IncrementalRehashMap`'s function and was not in the binary. robin_map's own bucket-array
reallocation is the same shape and the off-CPU evidence is compatible with it, but nothing here
distinguishes it from any other blocking cause. If the re-run with the swap deployed still shows
spikes that grow with table size, timing the two `operator new` calls in `allocate_table`
directly -- logged at Warning, so it survives the load run's log level -- settles it cheaply.

**Status: superseded.** The re-run was made on 2026-08-21, deployed first and with the documented
`--clients 4`. See the next section.

#### The measurement that closes this entry, 2026-08-21

Trading-day run with the order book on `IncrementalRehashMap`, deployed first this time, and with
the documented `--gateway binary --clients 4`. Prometheus started on its own beforehand and load
confirmed reaching it within 20 seconds. p99 is the worst 15-second sample in a window of -90s to
+240s around each growth step, taken from `order_round_trip_nanoseconds_bucket` on
`binary_order_gateway_a`; step times come from the growth-report lines in
`matching_engine_primary.log`.

| buckets | table | peak p99 | median p99 | robin_map 2026-08-16 | first run 2026-08-08 |
|---|---|---|---|---|---|
| 2^20 | 305 MB | 2.50 ms | 1.25 ms | -- | -- |
| 2^21 | 610 MB | 1.78 ms | 0.99 ms | -- | -- |
| 2^22 | 1220 MB | **2.43 ms** | 0.99 ms | 84.94 ms | 96 ms |
| 2^23 | 2440 MB | **4.27 ms** | 2.34 ms | 378.32 ms | 733 ms |
| 2^24 | 4880 MB | **2.44 ms** | 0.98 ms | 997.20 ms | over 1 s, pipeline collapsed |
| 2^25 | 9760 MB | **2.43 ms** | 0.99 ms | never reached | never reached |

**Flat across a 32-fold range of table size.** That is the property the container exists for, and
it is the part that could not be argued from unit tests. The failure signature was a tail
excursion -- median p99 around 0.9 ms while the peak went to 85 ms and then to a second -- and the
median is unchanged at 0.98-0.99 ms while the peak now sits beside it. The excursion is gone
rather than reduced. The 4.27 ms at 2^23 is not the start of a trend: the next doubling came back
to 2.44 ms.

Delivery was 12,735,099 of 13,016,000 orders offered, a 2.16% shortfall entirely inside the OOM
window described in BUG-0028. The first run lost 12.22% to the stall itself.

**The profile shows the migration doing its work.** `step_migration()` at **0.44%** of the reactor
thread, `find()` at 0.52%, `insert_entry()` at 0.41%. Compare the 2026-08-16 profile, where the
whole thread was flat at 0.66% top symbol because it was blocked rather than working.

#### The allocation hypothesis was wrong

Recorded because it was the leading theory for a fortnight and the refutation is one comparison:

| | allocation in one `operator new` | stall |
|---|---|---|
| `tsl::robin_map` at its worst step | 4992 MB | ~1 s |
| `IncrementalRehashMap` at the same step | 4880 MB | 2.44 ms |

Same machine, same profile, near-identical request size. `operator new` reads 0.02% in the profile
and `memset` 0.00%, and `node_pressure_memory_stalled_seconds_total` was 0.000 at every step but
the last, where it reached 0.001 with the machine nearly full. The cost was always the rehash.

Two things follow. The `allocate_table` timing experiment proposed above is not needed. And the
reasoning that the table's bytes-per-slot was the thing to attack was chasing the wrong quantity --
the size of the allocation was never what stalled the thread.

### BUG-0012: Orphaned build directories break coverage capture {#bug_0012}

| | |
|---|---|
| Severity | low |
| Found | 2026-08-08 |
| Recorded | 2026-08-08 (8cc0ced) |
| How | Every incremental coverage build was failing; the error named standard-library headers and pointed nowhere near the cause |
| Fixed | `10f4578` |

`applications/binary_gateway/` was renamed on 2026-08-01 and CMake never removed the old target
tree. gcovr searches the whole build tree *before* the report-level excludes apply, found `.gcno`
files with no `.gcda`, could not resolve their compilation directory, searched upward and aborted
at `/`.

### BUG-0013: Trading-day phases reused ClOrdIDs {#bug_0013}

| | |
|---|---|
| Severity | medium |
| Found | 2026-08-08 |
| Recorded | 2026-08-08 (8cc0ced) |
| How | The first trading-day run; `perf_run.py`'s own loss accounting reported the mismatch |
| Fixed | `d47112e` |

The phase scheduler restarts the load client per phase and every invocation numbered its orders
from 1, so 5,256,000 of 8,632,000 orders were rejected as duplicates and four of seven phases
recorded no latency at all. Quiet in the worst way: rejected orders still travel through the
gateway and sequencer, so the load looked real while measuring nothing.

### BUG-0019: A FIX logon arriving before the gateway's sequencer links are up is delayed five seconds {#bug_0019}

| | |
|---|---|
| Severity | medium |
| Found | 2026-08-09 |
| Recorded | 2026-08-09 (b51f84c) |
| How | `release_check.py`'s ha stage: scenario 23 (`inflight_gateway_death`) failed with "FIX logon timed out after 3s" |
| Fixed | 2026-08-10 -- `on_connection_established()` re-announces every session still awaiting its numbering down the link that has just come up |

The client authenticated successfully and was left hanging anyway. From the logs of one run:

- `23:01:57.324` the gateway receives the authentication request
- `23:01:57.326` the authentication service answers `Granted`, and the gateway announces the
  session and arms cancel-on-disconnect
- `23:01:57.326` two warnings, together: *primary sequencer not connected -- PDU not sent*, and
  the same for the secondary
- `23:01:57.367` both sequencer connections come up, 40ms later
- `23:02:00` the test gives up and tears the venue down

`complete_session_establishment()` sends the Logon reply only once the session's sequence state
is settled, which the gateway learns by asking the sequencer. That request is a PDU, and at that
instant there was no sequencer to send it to, so it was dropped with a warning and nothing
retried it when the links came up 40ms later.

**The session is not lost.** `sequence_state_timeout` is 5 seconds; when it fires the gateway
warns and calls `complete_session_establishment()` regardless, so the member gets its Logon after
five seconds instead of two milliseconds. `ha_test.py` waits three (`FIX8_LOGON_WAIT = 3.0`) and
gives up two seconds before the gateway would have recovered — which is the whole reason the
scenario reports a failure.

Confirmed by experiment: raising that constant to 9.0 makes scenario 23 pass. The change was
reverted, not kept — 3s versus a 5s fallback is a real defect in the test, but making the test
wait longer would only make it bless the degraded path.

Two separate things needed deciding, and they were the reason the entry stayed open:

- **The test cannot pass when this path is taken.** Its budget is shorter than the recovery it is
  waiting for, so the outcome is decided before the venue has finished trying.
- **The recovery path is degraded, by its own account.** The warning it emits reads: *"opening
  the session at {} anyway; a member expecting a higher number will see a low sequence and
  disconnect."* So the fallback is not a fix — it opens a session with numbering the sequencer
  never confirmed, and the code itself says that can cost the member its connection. The window
  wants closing at the source: retry the sequence-state request when a sequencer connection is
  established, or refuse logons until the gateway can service them.

The gateway had already announced the session and armed cancel-on-disconnect before discovering
it could not finish, so its own view of the session ran ahead of the member's.

Same family as the venue accepting orders with no matching engine and telling nobody: a PDU
dropped on an absent link, a warning in a log nobody is reading, and no retry.

**Where the delay comes from.** The gateway's outbound sequencer connections are retried on a
cadence — `ReactorConfiguration::connect_retry_interval_`, default 2 seconds — and its first
attempts fail because the sequencer is not accepting until leader election completes. From the
run at 23:07:

| time | event |
|---|---|
| `23:07:37.481` | gateway begins dialling both sequencers |
| `23:07:40.0`, `23:07:40.9` | sequencer execution-report *inbound* connections arrive (sequencer → gateway) |
| `23:07:42.384` | logon → *primary sequencer not connected -- PDU not sent* |
| `23:07:42.452` | primary and secondary sequencer connections established, **68ms later** |

Just under five seconds from first dial to success: the initial attempt plus two retries. The
client logged on 68ms before the last one landed.

So the gateway holds its FIX listener open while its upstream link is down, and a member that
logs on during a retry gap has its sequence-state request dropped. That the execution-report inbound
connections were already up at 23:07:40 makes it worse — the gateway looks connected to a
sequencer, in the direction that does not serve this request.

Not yet understood: it passed twice earlier the same evening under `--scenario all`, then failed
under `--scenario all` and three times standalone. A clean rebuild happened in between, but the
deployed configs were checked and contain no unexpanded placeholders, so that is not the
difference. The window is a race, so what matters is that it exists at all; but what moved the
timing enough to change the outcome consistently is not established.

**The fix, 2026-08-10.** `retry_pending_session_binds()` is called from
`on_connection_established()` for each sequencer link as it comes up, and re-sends `SessionBound`
for every session whose `awaiting_sequence_state` is still true. The first of the two options
above; refusing logons was not taken, because holding the FIX listener open and serving the
session a moment later costs the member nothing, whereas a rejection costs it a reconnect.

It re-sends down the one link that has just come up, deliberately, rather than to both
sequencers. A `SessionBound` arriving at a leader that already holds the binding is read as the
previous session having died, and the resume figure is then raised by `ers_since_report` plus the
admin allowance — right for a real failover, wrong for a retry. A link that already took the
announcement must therefore not be sent it twice.

Proved by A/B against a deterministic reproduction (the venue started with no sequencer at all, a
member logged on into the gap, the sequencers started 1.5s later), same venue and same script,
only the binary differing:

| | pre-fix binary | with the fix |
|---|---|---|
| re-announce on connect | never | 14µs after the link came up |
| 5s degraded fallback | ran — *"opening the session at 1 anyway"* | never ran |
| session established | ~5s after logon, on numbering the venue never confirmed | 756µs after the link came up |

Scenarios 1, 22 and 23 pass, as do the 30 `fix_order_gateway_tests`. Scenario 1 is the one worth
naming: it fails a sequencer over, so a link comes back up long after the sessions on it were
settled, and the retry correctly finds nothing to do.

**The test's budget, fixed the same day.** `FIX8_LOGON_WAIT` was 3.0 against a 5s fallback, so a
logon that took the fallback was failed two seconds before the venue had finished trying — and
reported as a timeout, which points at the gateway or the auth service rather than at what
actually happened.

Raising the constant was rejected on 2026-08-09 for a good reason: on its own it only teaches the
test to bless the degraded path. So both halves were done together.

- `FIX8_LOGON_WAIT` is now 8.0, and is documented as an upper bound rather than an expectation.
  It costs a passing run nothing — `wait_for_fix_logon()` returns the moment it sees an outcome,
  so scenario 23 still completes in about 31 seconds.
- `wait_for_fix_logon()` gained a fourth outcome, `degraded`, set when
  `_GW_SEQ_STATE_FALLBACK` appears before the session is established. The scenario now dies
  naming the fallback instead of passing quietly.

That last part is the point of the change. Before it, a session opened on unconfirmed numbering
was indistinguishable from a healthy one at the instant of logon: the run looked clean, and the
two diverged only later, when the member saw a sequence below the one it expected and dropped the
session. The test could not have caught the original bug even with a longer wait; now the longer
wait is safe, because taking the fallback is itself the failure.

The gateway warning it keys on carries a `TEST CONTRACT` comment, like the other lines `ha_test.py`
matches.

Detection proved against synthetic logs rather than a real venue, the degraded path not being
something that can be produced on demand: a clean logon reads `ok`; the fallback warning followed
by establishment reads `degraded`, including when the two land in separate reads of a log still
being written; and a previous session's fallback sitting before `from_byte` is correctly ignored.
Both marker strings were checked to appear verbatim in `FixOrderGatewayThread.cpp`, so a reworded
log line fails the check rather than silently ceasing to match.

### BUG-0020: The Rocky container deployed its gcc-8.5 binaries over the host's install tree {#bug_0020}

| | |
|---|---|
| Severity | high |
| Found | 2026-08-09 |
| Recorded | 2026-08-09 (5ab42ad) |
| How | A full `release_check.py` run in which `ha` scored 0/23 and `perf` died, immediately after the `rocky` stage passed for the first time |
| Fixed | 2026-08-09 |

`devsetup.py` runs build, release, stop and deploy. The first three qualify their directory by
target platform, so a Rocky build stages to `installed-rocky8/` and cannot overwrite the host's
gcc-13 tree. The fourth did not: `deploy.py` takes its destination from the env TOML, where
`install_dir = "installed"` is unqualified — and correct, because a real target host deploys to
that name whatever compiler built the artefact. Right for `deploy.py` alone, wrong as the last
step of a sequence whose other three had agreed on a different directory.

The Rocky stage bind-mounts the repository, so the container's deploy wrote gcc-8.5 binaries
into the host's `installed/`. They carry `RPATH=/opt/deps/...`, a path that exists only inside
the container, so every one of them failed to start with exit 127.

Two things hid it. The stage that caused the damage **passed** — it did its own job correctly
and corrupted a tree it was not testing — and the failures appeared in `ha` and `perf`, which
had passed an hour earlier and contain nothing to do with containers. The reason it had never
been seen is that the Rocky stage had never before run to completion: it had been failing early
on a permissions error, and the corrupting step came after that.

`devsetup.py` now resolves the staging directory once, from `build.platform_tag()`, and passes
it to `deploy.py` as `--install-dir`. On the development host it resolves to `installed`, so
nothing there changes.

The same fault had a second half, fixed the same day at the user's instruction. The release
artefact name carried **no platform tag**, so the container wrote a tarball into the bind-mounted
`release/` that could not be told from a host build — and both `devsetup.py` and
`build-release-deploy.py` chose "the newest tarball", which after a release check is the
container's. `release.py` now appends the platform tag, and both selectors filter by it rather
than by date. `build-release-deploy.py` was worse than its sibling: it hardcoded `installed` and
`rmtree`s it before building, so run in the container it would have deleted the host's tree
before replacing it.

The name is unchanged on the development host, whose tag is empty; only a cross-compiled
artefact is qualified, exactly as the staging directory works.

The third part, also fixed the same day: nothing verified that what a stage was about to test
could start. `release_check.py` gained a `runnable` stage, between `deploy` and `ha`, which runs
`ldd` over every installed binary with the same library path `devenv.py` launches components
under, and fails with the offending names and the `readelf -d` command to confirm the cause. It
takes 0.2s and guards `perf` as well, since that follows `ha`.

Checked in both directions before being trusted: it passes on the repaired tree, and pointed at
`installed-rocky8/` — the actual gcc-8.5 binaries that caused this entry, unmodified — it fails
20 of 23. That is what makes it worth having: `ha` answered the same question by reporting 0 of
23 scenarios failed, which reads as a catastrophic regression in the high-availability code and
was nothing of the kind.

### BUG-0021: `pubsub_metrics.py` built query labels from a module global, and fell back to a table describing the wrong venue {#bug_0021}

| | |
|---|---|
| Severity | medium |
| Found | 2026-08-09 |
| Recorded | 2026-08-09 (126df5d) |
| How | Fixing `--application`, which had the same shape and was silently inert; the user then asked why a static table existed at all when the tool does discovery |
| Fixed | 2026-08-09 |

Two faults with one cause — a value decided at import rather than passed to the code that
uses it.

**The global.** `APPLICATION` was a module-level name that `main()` reassigned, and the label
builders read it rather than being given it. That is exactly what made `--application` inert,
and it would have misled again the moment anything read the global before `main()` ran — a
module imported for its functions rather than executed, which is how a test would use it. The
application is now a parameter throughout and the global is gone.

**The fallback was worse.** When discovery found no series, the tool fell back to a built-in
table describing *this* venue and listed it as though it belonged to the application asked
for:

```
$ pubsub_metrics.py --application nosuchapp --list
note: ... returned no series for application=nosuchapp; using the static table
components (static table):
  binary_order_gateway_a
  fix_order_gateway_a
  ...
```

Those components do not exist for `nosuchapp`. The same would happen pointing the tool at any
other application: a confident, specific, wrong answer in the tool's own voice — and it bought
nothing, because if Prometheus cannot be reached for discovery the queries that follow cannot
reach it either.

Discovery is now the only source for anything that will be queried, and failure is an error
naming what was asked for. The built-in table survives solely for `--demo`, which synthesises
data and never queries, and is now named `build_demo_component_config()` so its one purpose is
visible at the call site.

### BUG-0022: `--application` never reached metric discovery {#bug_0022}

| | |
|---|---|
| Severity | low |
| Found | 2026-08-09 |
| Recorded | 2026-08-09 (63bddac) |
| How | The user challenged a claim that the metric registry was pubsub-specific; checking it showed discovery was correct and the flag feeding it was not |
| Fixed | 2026-08-09 |

`discover_component_config(prom_url, application=APPLICATION)` bound its default **when the
module was imported**, so it was always `"pubsub"`. `--application` set the global, was
reported faithfully in every message, and never reached the query. Passing
`--application nosuchapp` discovered pubsub's components and listed them.

The failure was invisible in the one place someone would look: the "returned no series for
application=X" message read the global and named the application the caller asked for, while
the query had used a different one.

Fixed by resolving the application when the function runs rather than when it is defined, and
threading it explicitly from `main()` through `resolve_component_config()` and
`_add_comparison_views()` instead of letting them read a global that `--application` mutates
after import.

**Also narrowed the fallback's `except Exception`.** It reported a `NameError` introduced while
making this very fix as "could not discover from http://localhost:9090" — a defect in the
discovery code wearing a connection failure's clothes. It now catches `RequestException`,
`ValueError` and `KeyError`, so an unreachable Prometheus or a malformed response still falls
back to the static table while a programming error surfaces as itself.

### BUG-0023: The band chart drew a flat line across periods with no data {#bug_0023}

| | |
|---|---|
| Severity | medium |
| Found | 2026-08-09 |
| Recorded | 2026-08-09 (5975a90) |
| How | First run of `--metrics bands` against a live Prometheus, over a window spanning two load runs and the three idle hours between them |
| Fixed | 2026-08-09 |

Those three idle hours — when no venue existed at all — were drawn as a steady p99 at about
1 ms: the most reassuring part of the chart, describing the interval with no data behind it.

**The cause was not interpolation between known points.** Prometheus omits empty regions from a
range query rather than returning them as null, so the percentile query returned *no series at
all* across the gap. The fetched timestamps jumped straight from 11:48 to 14:43, and those two
points are adjacent samples as far as the plot is concerned — the line between them is what
`step()` draws between any two neighbours. The renderer already broke lines on `None`; there
were simply no `None` values to break on.

Fixed by `align_to_step_grid()`, which places every fetched series on the full step grid of the
requested window and marks absent steps `None`. The rule is then exact — a step with no sample
is a hole — rather than a heuristic about which gaps look suspicious. The existing `None`
handling in `draw_latency_bands()` does the rest.

It also fixes a latent misalignment: the percentile tracks, the breach counts and the
observation counts are fetched by separate queries and indexed as parallel lists, so a point
present in one and missing from another shifted a track against its own breach bars. All three
now share one grid.

**`--from` / `--to` added at the same time**, because `--since MINUTES` could only frame a run by
arithmetic against the current time, and the breach total then described everything in view
rather than one run. Both accept `HH:MM` for today, `YYYY-MM-DD HH:MM`, a date alone, or an
epoch second, in local time — the axis is local and so is the reader. Scoped to run 8 alone the
count reads 831,554 orders over the 2.5 ms ceiling, which is a statement about a run.

### BUG-0024: The slab allocator had a hard message ceiling, below the performance target {#bug_0024}

| | |
|---|---|
| Severity | high |
| Found | 2026-08-09 |
| Recorded | 2026-08-09 (d9b38e5) |
| How | Writing a drain-loop test that forced continuous slab rotation; it failed on a limit nobody was looking for, and a follow-up measurement quantified it |
| Fixed | 2026-08-09 |

`ExpandableSlabAllocator` issued registry slots monotonically and never reused them. The slot
indexes a fixed two-level directory — fixed because deallocating threads read it without a lock
and it must never reallocate under them — so `1024 pages × 256 slots` was a hard ceiling on slab
**rotations for the lifetime of the process**, and therefore on the bytes it could ever receive:
**16 GiB at the then-default 64 KB slab.** Run 8 put ~14.3M messages through one gateway in 113
minutes, close enough that the next failure could have been `allocate()` throwing.

Consumption was throughput, not concurrency: `allocate()` resets the current slab in place only
when it is completely idle at that instant, and under sustained load a PDU is always in flight,
so every slab-full cost a slot.

**Fixed by a generation-tagged handle.** `SlabHandle` packs the slot in the low 32 bits and a
generation in the high 32. Slots are recycled through an **intrusive** free list threaded via the
slots themselves — no allocation, and no synchronisation, because both ends are reactor-only.
Releasing a slot bumps its generation, so a handle outliving its slab is rejected rather than
freeing into the slab that now owns the slot.

Measured, at a 64 KB slab and 1 KB PDUs:

| in-flight depth | slots for 200,000 messages, before | after |
|---|---|---|
| 1 | 3,125 | **3** |
| 8 | 3,125 | **3** |
| 64 | 3,125 | **3** |
| 256 | 3,125 | **6** |

Consumption now scales with in-flight depth rather than throughput, and
`SlabSlotsAreRecycledSoConsumptionIsBoundedByInFlightDepth` pins the property that matters:
**100,000 messages and 1,000,000 messages both cost 3 slots.** The registry now bounds how many
slabs may be live at once, not how many a process may handle.

`SlabHandle` is an `enum class`, not a `using` alias, and that was not cosmetic: as an alias it
converted implicitly to `int`, and the first cut of this change compiled cleanly with the
generation being silently truncated at four stages between `PduParser` and `deallocate()` — under
`-Wall -Wextra -pedantic -Werror`. It would have indexed the right slot and then been rejected as
stale the moment a slot was recycled. A distinct type makes each of those a compile error.

**Also raised `inbound_slab_size` from 64 KB to 256 KB** (`ReactorConfiguration`), which was the
interim mitigation and remains a better default: a slab stays mapped while any one of its chunks
is outstanding, so worst-case retention rises with slab size, and 256 KB keeps that four times
lower than 1 MB.

### BUG-0025: Reactor queue pool sizes were not configurable in any environment {#bug_0025}

| | |
|---|---|
| Severity | medium |
| Found | 2026-08-09 |
| Recorded | 2026-08-09 (d9b38e5) |
| How | `mep_primary.log` filled with `MepCommandPool exhausted: chaining new pool slab (1024 objects)` during a load run |
| Fixed | 2026-08-09 — awaiting a `cmake --install` + `deploy.py` to verify, which cannot run until the trading-day run in flight finishes |

Every application template carried its `[event_queue_pool]` and `[command_queue_pool]` sizes as
**literals**, so no environment could tune them. Only `open_order_pool` was ever templated. The
publisher was the component that made this visible because it was left at the framework default
of 1024 while its peers had been raised by hand — the sequencer to 81,920/1,500,000, the FIX
gateway to 80,000/1,000,000 — which is exactly the drift that having no single place to set them
produces.

All sixteen templates now take `${placeholder}` values seeded from what they held, so the change
is behaviour-neutral: 60 values unchanged, verified by comparing each placeholder's value against
the literal it replaced in `git HEAD`.

**Four values changed deliberately**, both publisher instances:

| | was | now | matched to |
|---|---|---|---|
| `event_queue_pool.objects_per_slab` | 1,024 | 81,920 | the sequencer that feeds it |
| `command_queue_pool.objects_per_slab` | 1,024 | 500,000 | the matching engine that feeds it |

**Still open, and worth a look when someone is next in there:** `binary_order_gateway_*` has a
command pool of 4,096 where `fix_order_gateway_*` has 1,000,000. The two gateways do the same job
over different protocols, so one of those two numbers is wrong, and 4,096 is the suspicious one.

### BUG-0026: Five ways 0.3.0 would not build or run on an RHEL8 target host {#bug_0026}

| | |
|---|---|
| Severity | high |
| Found | 2026-08-11 |
| Recorded | 2026-08-11 (9aafd07) |
| How | Installing the 0.3.0 release tarball on an RHEL8 target host -- the first time a published artefact was built anywhere other than this machine or the Rocky container |
| Fixed | 2026-08-11 |

Every one of them is a property of that host rather than of the source, which is why the
`rocky` stage of `release_check.py` passed and none of them showed up before. That stage
reproduces the **compiler** -- gcc 8.5 -- and nothing else. It does not reproduce the host's
Python toolchain, its filesystem, its third-party tree layout, its database, or the condition
of having started from a release tarball rather than a git checkout. `release_check.py`'s own
docstring says as much for the Python half, having been written after 0.2.0 failed the same
way; it said it about pylint versions and pytest, and it was right both times.

**1. The pylint gate failed on a message it had not asked for.** The errors-only invocation
(`--disable=all --enable=E`) reported `R0022 useless-option-value` for two inline disables in
`sca.py` naming `bad-whitespace`, a check pylint removed years ago. A message filter cannot
suppress a complaint about the filter itself, so the gate stopped a build over two stale words
in a comment. Fixed at both ends: the stale disables are gone, and `run_command` grew
`tolerated_exit_bits` so a gate that asked for errors is no longer failed by pylint's
warning, refactor and convention exit bits. pylint's exit status is a bitmask, not a verdict.

The versions are the mechanism: this machine has pylint 3.0.3 on Python 3.12, the Rocky image
installs whatever `pip3 install pylint` resolved to at image build time on Python 3.8, and the
work host has a newer one again. Three machines, three linters.

**2. Twenty-two DSL tests failed on NFS, having passed.** `compile_and_load()` builds a `.so`,
dlopens it, and lets `TemporaryDirectory` clean up -- unlinking a file that is still mapped.
On a local filesystem the inode outlives the name and nothing notices. On NFS the server
silly-renames it to `.nfsXXXX` in the same directory, so the following `rmdir` fails with
ENOTEMPTY and the error surfaces from the `with` block **after every assertion has passed**.
The scratch build sits inside the project tree because RHEL8 mounts `/tmp` noexec; on such a host
the project tree is itself the NFS one. Cleanup is now allowed to fail without failing a test, and
leftovers are reaped on the next run.

**3. prometheus-cpp was not found.** The work third-party tree holds it directly under
`THIRDPARTY_DIR` and names the directory after the metric system rather than after the C++
client, so the single prefix `CMakeLists.txt` offered did not exist there. Both prefixes are
now offered, and the comment records that `prometheus-cpp_DIR` in the environment covers any
third layout.

**4. The PostgreSQL port was hardcoded in four places.** `deploy.py` and `devenv.py` pass
`[db].port` from the environment file, which is why this looked plumbed. `perf_run.py`,
`callgrind_run.py` and three `psql` calls in `ha_test.py` passed the literal `"5432"`, and
`db/liquibase.properties` a literal URL. `create_db.py` and `export_credentials.py` now take
their defaults from libpq's own `PGHOST`/`PGPORT`, which covers every script that has no
environment file to read.

**`PGPORT` does not cover a deploy, and that is the case that failed.** `deploy.py` and
`devenv.py` pass `[db].port` from the environment file explicitly, and an explicit argument
beats an environment default -- so exporting `PGPORT` would have changed nothing about the
credential export that broke on RHEL8. Both now take `--db-port`, forwarded by `devsetup.py`
and `build-release-deploy.py` so that the flag survives a build+release+deploy run, which is
how the venue is actually deployed. It is applied to the parsed environment before anything
reads it, so `create_db.py`, `export_credentials.py`, the `${db_port}` placeholder and the
admin service's JDBC URL all move together. Overriding only the psql calls would have left the
Java service pointed at a port with nothing on it: a deploy that succeeds and fails later. The liquibase properties file cannot honour an
environment default -- liquibase substitutes `${env.VAR}` but has no syntax for a fallback --
so it says so in a comment instead.

Found while fixing it: `[admin_service] db_url` repeats the host, port and database name from
the `[db]` section, held together by nothing but a comment in each environment file. On a host
that is not on 5432 both must change, and missing one gives a deploy that succeeds while the
Java admin service alone cannot connect. `deploy.py` now checks them and refuses to deploy on
a mismatch.

**5. A failing step reported an exit code and nothing else.** `--sudo-postgres` failed with no
detail, because `create_db.py`'s database-exists probe captures output and its handler printed
only the status, and because `devsetup.py` exited bare -- so the last thing on screen was the
banner of the step that was *starting*. Both now name the command and repeat what it said.
`devenv.py` additionally names where the connection details came from, since psql reports the
host and port it could not reach but not that they came from the environment file.

### BUG-0027: Dismissed: the gateway's per-session `OpenOrderMap` is not the shape the order book was {#bug_0027}

| | |
|---|---|
| Severity | low |
| Found | 2026-08-16 |
| Recorded | 2026-08-21 (875259f) |
| How | Swept for other instances of the growing-hash-map pattern after moving the order book off `tsl::robin_map` |
| Impact | **None. Measured 2026-08-21 and dismissed** -- see the closing paragraph |
| Dismissed | 2026-08-21 (875259f) -- measured, and found not to be a problem |

`open_orders::OpenOrderMap` (`applications/fix_common/OpenOrderEntry.hpp`) is a `tsl::robin_map`
held per session (`FixSession::open_orders`) and mutated on the gateway's reactor callback thread
— `insert_or_assign` on each non-terminal execution report, `erase` on each terminal one. That is the same
structure, on the same kind of thread, with the same power-of-two growth policy as the order book
that stalled for over a second.

**Why it is weaker than the order book was.** It is scoped per session rather than venue-wide, and
entries leave on terminal execution reports rather than resting indefinitely. A venue with many small sessions
never grows any one map large enough to matter.

**Why it is not dismissible.** The bound is the number of orders one session leaves resting, and
nothing caps that. Under the trading-day profile it is worse than the general case, not better:
the load client runs a single session, so one `OpenOrderMap` accumulates every resting order in
the run — the same millions the order book holds, in the same growth steps, on the gateway's
callback thread instead of the engine's. A member that rests a large book behaves the same way.

**Settled 2026-08-21: it is not a problem, and the resemblance was superficial.**
`robin_hash::rehash_impl` on the gateway's callback thread reads **0.06%** of a trading-day
profile. The reason is in the type, which the sweep that raised this had not looked at closely
enough: `OpenOrderMap` is `robin_map<std::string_view, OpenOrderEntry*>`, so a bucket is about 32
bytes and the entries live in a pool. The order book stored `OrderEntry` inline at 304 bytes a
slot. A doubling here copies pointers; a doubling there copied the book. Nothing needs doing.

Worth keeping rather than deleting, for two reasons. It records that the pattern was looked for
elsewhere and where it was found, so the sweep is not repeated. And it is a reminder that "same
container, same thread, same growth policy" is not enough to make two structures behave alike --
what mattered was what each bucket held.

**Correction, 2026-08-23. The dismissal was right about the cost and wrong about one of its
premises.** Left above as it was written, because how it was reached matters.

The premise that failed is this one: *"entries leave on terminal execution reports rather than resting
indefinitely."* They did not. A cancel report names the retired order in OrigClOrdID and the
cancel request in ClOrdID, and the gateway looked up the latter -- a key that had never been
filed. Nothing was ever removed. Measured on a trading-day run: the map grew at 1,925 entries a
second against an order rate of 1,925 a second, with a profile specifying a tenth of orders
cancelled. Fixed the same day; see "remove the order a cancel names, not the cancel itself" in
the git history.

So the map was unbounded for any session, not only for the single-session load client the entry
worried about. The reasoning that "a venue with many small sessions never grows any one map
large enough to matter" did not hold, because every session grew without limit.

The second correction is about what was measured rather than what was assumed. `rehash_impl`
reading 0.06% of a profile is still true and still not the whole cost. A rehash was observed
taking p99 from 0.99 ms to about 2.3 ms for eleven minutes, with the load unchanged, recovering
to exactly 0.99 ms afterwards -- three times, at 2Mi, 4Mi and 8Mi entries, each step predicted
before it happened. A CPU profile cannot see it: the cycles in `rehash_impl` really are
negligible, and the cost is in the memory system afterwards, most likely huge pages being
re-formed under a freshly mapped table.

**What survives.** The conclusion that a doubling here copies pointers rather than order records
stands, and 32 bytes a bucket against 304 is still the reason this was not the order book's
problem. The entry's closing point survives too, and gains a second half: what each bucket held
was what mattered -- and a dismissal resting on a claim about behaviour needs that claim checked
against the code, not against what the code was meant to do.

### BUG-0031: Rejoin after a promotion re-runs the cold-start tie-break {#bug_0031}

| | |
|---|---|
| Severity | high |
| Found | 2026-08-22, designing process supervision |
| Recorded | 2026-08-22 (4ba3314) |
| How | Reading the arbiter against the rule a restarted primary needs to follow |
| Impact | A restarted primary -- which by definition has just lost its book -- would be handed leadership back from a healthy secondary that has it |
| Fixed | 2026-08-22 (81300d9) |

**Two different questions are answered by one rule.** "Which instance should lead when neither
is leading yet?" is a cold-start tie-break, and lowest instance id is the right answer for it:
the two can start in either order with a delay between them, and the preference makes that
deterministic. "Which instance should lead when one of them already is?" is a different
question, and lowest id is the wrong answer to it.

`decide_and_broadcast` (`ArbiterThread.cpp:550`) answers both the same way:

```cpp
const bool peer_connected = component_connections_.count(peer_key) > 0;
const int64_t leader_id = peer_connected ? std::min(self_instance_id, peer_instance_id) : self_instance_id;
```

It writes `leadership_state_` and never reads it. The arbiter therefore has no notion of an
incumbent, and recomputes leadership from scratch every time it is asked.

**Why that is dangerous rather than merely untidy.** The primary always holds the lower id. So
after the secondary has been promoted, a primary that restarts and triggers arbitration is
handed leadership back -- and a restarted primary has lost its book, because losing it is why
it restarted. The venue would move leadership from an instance holding the book to one holding
nothing.

**The rule agreed 2026-08-22**, which distinguishes the two questions:

```
if (an incumbent leader is recorded for this group AND that instance is connected)
    keep the incumbent
else
    lowest instance id wins          // cold start, or the incumbent is gone
```

| situation | incumbent | connected | outcome |
|---|---|---|---|
| cold start, either order | none | -- | lowest id: primary leads |
| primary restarts, secondary leads | secondary | yes | **primary becomes follower** |
| primary restarts, secondary's machine died | secondary | no | falls through: primary leads |
| primary restarts, secondary never promoted | primary | yes | primary leads again |

The last row is the *common* case once a supervisor exists, not an exotic one: the follower's
grace period is there so that a quick local restart happens before any promotion does.

**What the fix needs.**

1. **Re-key `leadership_state_` by group.** It is currently keyed `(group, instance_id)`
   (`ArbiterThread.hpp:84`), so "who leads the matching engine?" is not a single lookup and the
   incumbent cannot be consulted.
2. **Consult it in `decide_and_broadcast`**, with the connectivity check above.
3. **Gate resumption of leadership on reconciliation, not on the decision arriving.** A
   restarted primary that becomes leader again with an empty book is a leader that does not
   know what is resting: a member's cancel for a live order is rejected and the venue has
   silently lost state it still notionally holds. The `Reconciling` state and WAL catch-up
   exist; the ordering is what matters.
4. **Learn the current epoch from the arbiter before emitting anything.** Epochs are checked on
   every PDU, so a node rejoining with a stale one has its traffic discarded -- the right
   outcome, but it should be deliberate.

**Not traced:** what the code does on rejoin today, end to end. The reconnect path
(`ArbiterThread.cpp:509`) sends a *stored* decision keyed by the connecting instance's own id,
and after a promotion nothing is stored under the primary's key. Whether that yields silence, a
stale decision or a fresh arbitration was not followed through. It does not change the design
above, which is needed either way.

#### Fixed 2026-08-22

The rule is now `applications/arbiter/LeadershipDecision.hpp`, a pure function that reads no
state and sends nothing, so it is tested directly rather than through a running venue.
`ArbiterThread` gathers the inputs from its connection tables and carries the decision out.

`leadership_state_` is re-keyed by group. That fixed a second fault found on the way: the
reconnect path looked the stored decision up under *the connecting instance's own id*, so an
instance rejoining after a promotion found nothing recorded against itself and was told nothing
at all. Any instance reconnecting is now told who leads its group.

The epoch advances only when leadership actually changes. Confirming an incumbent returns the
epoch already in force, because epochs are checked on every PDU and superseding a leader's own
epoch would have its traffic discarded while it was doing nothing wrong. When the epoch does
advance it is taken from the higher of the arbiter's record and the reporter's, so an instance
that has been away cannot wind the sequence backwards with a stale report.

12 tests in `applications/arbiter/tests/LeadershipDecisionTest.cpp`, including a sweep asserting
that leader and follower are always the two distinct instances whatever the inputs. The rejoin
tests were checked against the previous rule and fail on it, so they catch the defect rather
than merely describing the new behaviour.

**Still to do: the integration scenario.** The unit tests prove the rule; they do not prove the
arbiter is asked at the right moments. That needs the `ha_test.py` scenario recorded in "Every HA
scenario models machine death; none models process death".

### BUG-0032: Ninety application tests were built, installed, and never run {#bug_0032}

| | |
|---|---|
| Severity | high |
| Found | 2026-08-22, after adding a test target to the arbiter and noticing it did not appear in the build output |
| Recorded | 2026-08-22 (fbce587) |
| How | `build.py` names each test binary it runs, and application binaries were never added to that list |
| Impact | Five suites, 90 tests, built and shipped by every release without once being executed |
| Fixed | 2026-08-22 (fbce587) |

`build.py` ran five test binaries by name -- the framework's unit and integration suites,
`scram_crypto`, and the two `fix_codec` ones. Every application suite was built, installed to
`bin/`, and then ignored:

| suite | tests |
|---|---|
| `fix_order_gateway_tests` | 39 |
| `matching_engine_tests` | 18 |
| `sequencer_tests` | 12 |
| `binary_order_gateway_tests` | 9 |
| `arbiter_tests` | 12 (added the same day) |

`ctest` was never invoked either, so the `gtest_discover_tests` registrations went nowhere. All
90 passed when run by hand, so nothing was being hidden -- but nothing was being checked, and a
regression in any of them would have reached a release unremarked.

**Fixed 2026-08-22.** `run_application_tests()` runs them, and the standard build goes from 893
tests to 983.

**The binaries are discovered, not listed**, and that is the point of the fix rather than an
implementation detail. A list is how the gap arose: each new test target had to be remembered in
`build.py`, and across five targets nobody remembered once. Discovery means a test target in any
application gates from the moment it builds.

**Finding nothing is an error, not a pass.** A glob that has stopped matching looks exactly like
a suite in which everything succeeded, which is the failure the gate exists to prevent -- so the
gate must not be able to fail that way itself. Both behaviours were checked: five suites found
and run, and exit code 1 with a message naming the directory when the glob matches nothing.

### BUG-0033: A restarted arbiter forgets who leads, and reverts to the cold-start rule {#bug_0033}

| | |
|---|---|
| Severity | high |
| Found | 2026-08-22, building the restart coverage matrix |
| Recorded | 2026-08-22 (63889b8) |
| How | Asked what the arbiter's new leadership state depends on, and what happens when it is lost |
| Impact | Undoes the incumbent-wins fix: a restarted arbiter hands leadership back to a restarted primary |
| Fixed | 2026-08-22 (c7b0d5d) |

`leadership_state_` lives only in memory. There is no snapshot, no WAL, and nothing reads
anything back at startup. It is replicated to the peer arbiter as decisions are taken
(`ArbiterStateRecord`), but that is a live feed and not a catch-up: an arbiter that starts, or
restarts, has an empty map and is told nothing about decisions made while it was away.

**Why that is worse than it was yesterday.** Before the incumbent rule, the arbiter was
stateless in effect -- it recomputed leadership from instance ids every time, so forgetting cost
nothing. Now the map is the thing that stops a restarted primary taking leadership back from a
working secondary. An arbiter with an empty map finds no incumbent, falls back to the cold-start
tie-break, and answers exactly as the code did before the fix.

The exposure is real but narrow while both arbiters do not restart together: the peer holds the
state and is the one that answers. It becomes live the moment the surviving arbiter is the one
that restarted, or both are.

**What it needs, and it is a design question rather than a patch.** Either the state is
recovered on startup -- from the peer, by asking, in the way a restarting sequencer syncs its
sequence number from its peer through StatusResponse -- or it is durable. The peer-sync route
looks the better fit: it is a pattern the venue already has, it needs no new persistence, and an
arbiter with no reachable peer genuinely has nothing to go on and is right to fall back to the
cold-start rule.

#### Fixed 2026-08-22

The arbiter is now told rather than remembering. `LeadershipLease` (id 118) carries what
`Heartbeat` used to imply -- an assertion of leadership by the instance holding it, at a stated
epoch -- and a restarted arbiter rebuilds its map from the leases it receives. Peer replay on a
link coming up is kept as an accelerator, not the mechanism, so the case with no peer to ask
still works. Full reasoning in `docs/availability/design_notes.md#ha_arbiter_relearns`.

Scenario 25 covers it, and the ordering inside that scenario is the substance of it: both
arbiters must be killed before either is restarted, or the state survives through the peer and
nothing is exercised. Demonstrated:

```
10:48:44  both arbiters dead
10:48:48  group=matching_engine is led by instance 2 at epoch 1 (learned from its lease)
10:49:07  replayed 1 leadership record(s) to peer
```

**An expectation this disproved, recorded because the reasoning looked sound.** The lease
interval is 30s and the arbiter's learning window 10s, which appeared to leave twenty seconds in
which a restarted arbiter would stop declining and start guessing before it could have been told
anything. It cannot arise: an arbiter restarting is what closes its components' connections, and
a leader sends a lease immediately on connecting rather than waiting for the timer. The interval
was left alone rather than shortened on principle.

### BUG-0034: The pair does not survive a second failure: replication is wired by identity, not by role {#bug_0034}

| | |
|---|---|
| Severity | high |
| Found | 2026-08-22, by scenario 32 |
| Recorded | 2026-08-22 (32060cc) |
| How | Failover, rejoin, then fail the new leader -- the surviving instance never takes over |
| Impact | After one failover the pair is finished. A second failure leaves the venue with no matching engine leader at all |
| Fixed | 2026-08-22 (5a47932) |

`docs/availability/design_notes.md#ha_restart_role` says the pair must be able to swap repeatedly: if the
secondary is promoted and later dies, the primary -- by then a follower -- must take over again.
It does not.

**Replication is hard-wired primary to secondary.** The primary connects out to
`me_secondary_replication`; the secondary listens. Nothing about that follows the role. So after
a failover the leader is the secondary, which has no replication channel *to* the primary, and
the primary has no channel *from* a leader.

**Failure detection inherits the same wiring.** A follower arms its promotion timer when
`primary_replication_conn_id_` is lost -- the connection it receives replication *on*. A primary
acting as a follower has no such connection, so nothing arms. It does notice the socket go, and
draws the wrong conclusion from it:

```
ME-secondary replication connection 3 lost: peer closed connection on service
'me_secondary_replication' -- book updates paused until secondary reconnects
```

It reads the leader's death as "my secondary has gone, book updates paused". Nothing promotes,
and the venue has no matching engine leader.

**This is the same class as the sequencer routing defect fixed earlier the same day**, and the
same omission behind all of them: *primary* and *leader* were interchangeable while nothing ever
restarted, so channels were named after identities. Once roles move, every channel named for an
identity is pointing at the wrong instance half the time.

#### Fixed 2026-08-22: both directions held permanently

Replication is now symmetric. Each instance listens on its own port and dials its peer's, both
connections stand at all times, and which one carries book updates follows the role because the
leader is the sender. A role change therefore needs no connection work at the moment the venue
can least afford it -- the alternative, having the leader dial the follower and re-establish on
every change, puts a reconnect on the failover path and leaves a newly promoted leader serving
without a replica while it completes.

Failure detection then works in both directions without special handling, which is the point.
A follower arms its promotion timer on losing the connection it *receives* replication on, and
with both directions held, whichever instance is the follower always has one.

**Three places had to stop branching on the configured identity**, and each was somewhere the
design had quietly baked in "primary means leader":

* the environment and both TOML templates, which gave the primary only a dial and the secondary
  only a listener;
* `MatchingEngineConfigurationLoader`, which read the two halves under an `is_secondary` test
  and now reads both for both roles;
* `MatchingEngine`, which registered the replication listener only on the secondary.

All three `is_secondary` checks are gone. Scenario 32 passes: leadership moves primary to
secondary, the primary rejoins as follower, and when the secondary then dies the primary takes
it back -- 15.2 s each way, the promotion timeout in both cases.

**A smaller fault visible in the same logs.** An ArbitrationDecision arrived as
`leader=2 follower=0`, and the instance correctly ignored a decision that did not mention it. The
zero comes from a leadership record rebuilt from a lease, which names only the leader; the
follower field is filled with 0. Harmless today because a correct decision follows immediately,
but it is a wrong value on the wire.

**Scenario 32 fails on this and is left failing**, as scenario 24 was before its defect was
fixed. It says so in place.

### BUG-0035: Role announcements were routed to the wrong socket, and then to a dead one {#bug_0035}

| | |
|---|---|
| Severity | high |
| Found | 2026-08-22, by scenario 26, in the routing added earlier the same day |
| Recorded | 2026-08-22 (abf7914) |
| How | A supervised restart made the ordering visible; the failover scenarios had hidden it |
| Impact | Orders sent down the wrong channel, and then to the connection of a process that had just died |
| Fixed | 2026-08-22 (4339a47) |

Two faults in the sequencer's handling of `RoleAnnouncement`, both introduced when routing was
first made role-aware and neither caught by the failover scenarios.

**The announcement arrives on the wrong socket to route by.** A matching engine opens an execution-report
connection *to* the sequencer and announces on it. Orders travel the other way, on a connection
the sequencer opens *to* the engine. Routing by the connection an announcement arrived on
therefore aimed orders down the execution-report channel. The announcement names an instance, so the fix is to
map that to this sequencer's own order connection for that instance.

**And an ordering race the first fault concealed.** A restarted engine announces immediately on
a connection it opens itself, while the sequencer's order connection to it is re-established a
couple of seconds later. So the announcement arrived while the only order connection on record
for that instance was the one belonging to the process that had just died:

```
11:09:32  instance 1 leads at epoch 1 -- orders now route to connection 15   <- dead process
11:09:34  matching engine order connection 25 established                    <- the live one
```

The announced leader is now remembered, so the order connection to that instance is routed to
when it appears:

```
11:09:34  order connection 25 to instance 1, which had already announced leadership -- routing there
```

**Why the failover scenarios missed both.** In a failover the promoted instance already holds a
pre-warmed standby connection, so the socket exists before the announcement and the ordering
never arises. It takes a restart -- where the connection is genuinely absent and then appears --
to expose it. That is the argument for the restart cells of the matrix in one paragraph.

### BUG-0036: With no arbiter reachable, a starting engine never promoted and never said so {#bug_0036}

| | |
|---|---|
| Severity | high |
| Found | 2026-08-22, by scenario 35 |
| Recorded | 2026-08-22 (eb2acec) |
| How | Killed both arbiters, then restarted the matching engine that had been leading |
| Impact | The venue has no matching engine leader, indefinitely, announced by nothing but connection-refused retries |
| Fixed | 2026-08-22 (eb2acec) |

`docs/availability/design_notes.md` is explicit that a two-node system with no arbiter falls back to
"lowest instance id wins". The fallback existed, and was unreachable.

The degraded self-promotion lives inside `send_arbitration_report()`, which was only ever called
from `request_startup_arbitration()` -- and that was called **when an arbiter connection came
up**. With the arbiter pool down, no connection comes up, so a starting engine never asked, never
degraded, and sat in `UNKNOWN` forever:

```
HA enabled, starting as UNKNOWN (instance_id=1, configured primary)
service 'arbiter_primary' failed to connect; retrying every 2000ms
service 'arbiter_secondary' failed to connect; retrying every 2000ms
```

Nothing else logged. The venue was not serving and said so only by omission -- the failure mode
section 13 of the design notes describes as the one that does not announce itself.

**Fixed 2026-08-22.** The startup arbitration timer is armed when the thread starts rather than
when an arbiter connects. It fires, finds the role still unresolved, and applies the instance-id
rule.

**Only the primary arms it**, which is worth stating because it is what makes the fallback
correct rather than merely present. The secondary starts as a follower and waits, so the instance
that promotes unilaterally is always the lower id -- the rule the design specifies, satisfied by
construction rather than by a comparison that could be written the wrong way round. Scenario 35
asserts both halves: the primary degrades and says so, and the secondary does not also promote.

### BUG-0042: A restarted primary matching engine promotes itself, producing two leaders {#bug_0042}

| | |
|---|---|
| Severity | high |
| Found | 2026-08-22, by the ha_test.py scenario written to prove the arbiter fix |
| Recorded | 2026-08-22 (c7b3459) |
| How | Scenario 24 restarts the primary after the secondary has been promoted, and asserts it comes back a follower |
| Impact | Two instances hold LEADER at once, and the venue stays that way -- the correct decision is discarded as a duplicate |
| Fixed | 2026-08-22 (193ad36) |

`MatchingEngineThread::on_connected` (`MatchingEngineThread.cpp:225`) does this when the first
arbiter connection comes up:

```cpp
if (first_arbiter && is_primary_) {
    // Primary holds leadership by heartbeating the arbiter to renew its lease.
    adopt_leader_role();
}
```

**The primary declares itself leader before asking anyone.** On a cold start that is harmless,
because the arbiter would name the same instance anyway. On a restart it is not: the secondary
may already be leading and serving traffic.

**And the correct answer is then thrown away.** `handle_arbitration_decision` suppresses a
decision that arrives when the instance is already Leader or Reconciling, which exists to stop a
second reply from both arbiters re-running reconciliation. It cannot tell that reply apart from
the first authoritative answer this instance has ever received. Observed:

```
08:54:10.598297  arbiter-primary connection 4 established
08:54:10.598297  adopting LEADER role (epoch=0)
08:54:10.598385  ArbitrationDecision received (group=matching_engine leader=2 follower=1 epoch=1)
08:54:10.598386  already LEADER -- ignoring duplicate ArbitrationDecision
```

The secondary had been promoted two seconds earlier and holds the book. The restarted primary
holds nothing, believes it leads, and has just discarded the message telling it otherwise.

**This is the failure the design says cannot happen.** `docs/availability/design_notes.md#ha_restart_role`: "The
arbiter decides in every case. A restarting node never promotes itself, so no sequence of
restarts can produce two leaders." The code does the opposite.

**The arbiter is not at fault, and the fix above is working.** The same run shows it answering
correctly, and answering at all only because `leadership_state_` is now keyed by group:

```
08:54:10  component group=matching_engine instance_id=1 registered on connection 9
08:54:10  ArbitrationDecision sent to connection 9 (group=matching_engine leader=2 follower=1 epoch=1)
```

**The shape of the fix.** On first arbiter connection a primary should send an
ArbitrationReport rather than adopt a role, and wait. The arbiter already answers both cases --
lowest id when no incumbent is recorded, the connected incumbent when one is -- so nothing new
is needed there. The duplicate guard needs to distinguish a genuine repeat from the first answer
received, which the epoch already carries.

**Blast radius, which is why this is recorded rather than immediately changed.** It alters
startup for the matching engine: a cold-start primary would become leader a round trip later
than it does today, and every scenario that waits on "adopting LEADER role" would see it at a
different moment. The sequencer pair should be checked for the same pattern before either is
touched.

**Scenario 24 fails against the current code**, which is what it is for.

**Restored 2026-08-24.** This entry was deleted in `63889b8`, when three defects found by
scenario 24 were folded into the restart-coverage matrix. Its substance did not survive the fold:
nothing in the matrix records the self-promotion, and three comments in `MatchingEngineThread.cpp`
and `.hpp` went on citing it by title. That is what ids and `check_bug_list.py` exist to prevent.

### BUG-0043: A cold-start primary routed through reconciliation strands the venue {#bug_0043}

| | |
|---|---|
| Severity | high |
| Found | 2026-08-22, uncovered by the fix for BUG-0042 |
| Recorded | 2026-08-24, written from `193ad36` after the code comment citing it was found to name an entry that had never existed |
| How | Reading what a starting instance does when the arbiter names it leader, having just changed what a restarting one does |
| Impact | A venue starting cold gets no matching engine leader at all: the instance waits on a connection it will never be given |
| Fixed | 2026-08-22 (193ad36) |

Reconciliation exists for a **promotion**. A follower that is told to lead already holds a replica
book and an open replication connection to the instance it is taking over from, and reconciliation
uses that connection to close the gap between the replica and the truth.

A cold start has neither. There is no peer serving, no replica to reconcile against, and no
connection to wait on -- so an instance routed through reconciliation on the way to leadership
waits for something that will not arrive, and the venue comes up with no matching engine leading.

The fix is stated as a rule rather than a special case: **reconciliation is entered only when
promoting from Follower.** A start is not a promotion, so it adopts the leader role directly.
`MatchingEngineThread.cpp` carries the reasoning at the branch that decides between the two.

**Why this had no entry until 2026-08-24.** It was found and fixed inside the change for BUG-0042
and recorded only in that commit's message. A comment at `MatchingEngineThread.cpp:1119` cited it
as though it were in this file, naming *"A cold-start primary that is told it leads should not
reconcile"* -- a title `git log -S` finds nowhere except in the commit that wrote the comment. A
citation to an entry nobody wrote is indistinguishable from a citation to one that was deleted,
which is the argument for checking citations mechanically.
