# Knowing there is no matching engine {#ha_me_presence}

**Status: designed, not built. 2026-08-28; the startup race settled the same day.** It is the next step for
[BUG-0009](../bug_list.md#bug_0009) and is what makes
[BUG-0064](../bug_list.md#bug_0064)'s cold-start case tractable.

## The requirement

When a matching engine dies mid-session the sequencer buffers orders, expecting high availability
to put things right. That expectation is correct and it must not be open-ended. Today it is: the
sequencer retries the engine connection every two seconds, for ever, with a reminder in the log
every fifteen minutes and no other consequence.

**This is about a failure during the trading day, not a cold start.** A venue that comes up with no
matching engine has never traded and nobody is waiting on anything. A venue that loses its engines
at eleven in the morning has members holding orders it has taken.

## Why a timeout is the wrong instrument

The venue already has one — [Refusing orders the venue cannot process](order_acceptance.md) stops
accepting after a deferral outlives 45 seconds. That threshold exists for a single reason: **the
sequencer cannot tell a failover in progress from a matching engine service that no longer
exists.** Unable to distinguish them, it waits long enough for the first and calls it a policy.

The cost is paid twice. In the case where an engine is coming back, 45 seconds is longer than it
needs. In the case where none is coming back, 45 seconds is 45 seconds of orders taken from members
who will never hear about them, and no timer set on ignorance can do better. Shortening it trades
one failure for the other, which is why the number was hard to choose and why the choice never felt
settled.

**And a timer races.** A threshold that fires while a promotion is 200 milliseconds from completing
kills a venue that was about to be healthy. Any timer picked to avoid that is picked to be longer
than the worst promotion, which makes it too long for the case it exists to catch.

## The fact already exists

The arbiter tracks component registration per group, `matching_engine` among them. It must: it
cannot decide who leads a group without knowing who is in it. `ArbiterThread.cpp` records each
instance as it registers and drops it on disconnect, logging

```
ArbiterThread: component group=matching_engine instance_id=2 disconnected
```

and ceasing to treat a disconnected leader as the incumbent.

So "is there any matching engine at all?" is **a fact the venue already holds and throws away.** A
fact does not race. It is not a guess that gets better with a longer wait.

## The sequencer asks; the arbiter does not announce

**This is settled by [section 11b](design_notes.md#ha_arbiter_only_arbitrates) and the shape it
chose, not by a fresh decision.** That section considered making the arbiter the distributor of
leadership news and rejected it — a component that decides *and* announces acquires a second role,
a subscriber list, and knowledge of who depends on what. Its rule: the arbiter *"should answer the
question it is asked, by the party that asked it, and nothing more."*

Publishing engine presence would be the rejected shape wearing different clothes. **Asking is not.**

So: when the sequencer finds itself deferring, it asks the arbiter whether any matching engine is
registered, on the connection it already holds. One question, one answer, to the party that asked.
No subscribers, no push, no new channel, and the arbiter gains no new knowledge — it stops
discarding what it has.

### Does this make the arbiter a process monitor?

The concern is right to raise and the answer is no, provided one line is held.

**The arbiter reports registration. It never reports health, and it never acts.** It can say whether
an instance is present. It cannot say whether that instance is working, and it must never be asked
to — an engine that is connected and wedged is
[BUG-0010](../bug_list.md#bug_0010)'s territory and stays there. Nothing here lets the arbiter
start, stop, restart or judge a component; the sequencer decides what the answer means and what to
do about it.

Knowing is not monitoring. **Deciding and acting is**, and that is where
[section 12](design_notes.md#ha_supervisor_role)'s boundary belongs. Supervision remains
[process death](process_death.md)'s subject and is untouched by this.

## What the sequencer does with the answer

Three states, where today there is one:

| The arbiter says | What it means | The sequencer |
|---|---|---|
| An instance is registered | A failover is in progress, or an engine is about to reconnect | Defers, as now. This is the case deferral was designed for and it is correct |
| Every arbiter says `all_gone` | There is no matching engine service | **Refuses at once**, and says so |
| It cannot answer (`unknown`) | No arbiter is reachable, or none has heard of the group since starting | Falls back on the age threshold |

The 45-second threshold does not go away. **Where an arbiter exists it stops being the mechanism and
becomes the backstop**, which is the right job for a number chosen out of ignorance: it covers the
case where the fact is genuinely unavailable rather than standing in for a fact the venue had all
along. Where no arbiter exists it remains the only mechanism there is — see
[With high availability turned off](#ha_me_presence_no_ha).

The member's rejection can then say what is true from the first order, rather than after 45 seconds
of silence.

## The startup race, which was the hard part {#ha_me_presence_race}

**A freshly started arbiter cannot distinguish "no matching engine exists" from "nobody has checked
in with me yet".** Both look like an empty membership list, and
[section 11c](design_notes.md#ha_arbiter_relearns) makes this worse rather than better by design: an
arbiter that restarts is told who leads and deliberately does not remember. So after an arbiter
restart the list is empty for reasons that have nothing to do with the engines.

Getting this wrong is not a small error. **An arbiter restart would refuse every order on a
perfectly healthy venue.** That is worse than the bug being fixed, and it is the failure mode this
design has to be judged on.

### Decided 2026-08-28: a negative answer requires positive evidence

**The arbiter never infers absence from the passage of time. It reports absence only when it has
seen an instance and seen it go.**

Three answers, where the middle one is the whole point:

| Answer | Means |
|---|---|
| `registered` | An instance is connected to this arbiter now |
| `unknown` | This arbiter has heard nothing about the group since it started |
| `all_gone` | This arbiter saw one or more instances and every one has disconnected |

Only `all_gone` is evidence. `unknown` is declined, not negative, and the sequencer treats it
exactly as it treats an unreachable arbiter: fall back on the age threshold.

**Why this and not a settling timer.** A timer — "treat a negative from an arbiter younger than the
registration interval as unknown" — was the obvious option and is the wrong one, because it
reintroduces the thing this design exists to remove. It infers a negative from elapsed time, so it
needs a tuned duration, and it can still be wrong in the dangerous direction: an arbiter up for
longer than the interval whose engine is merely slow to register produces a false `all_gone`, and
the venue refuses orders it could have processed. **A rule that cannot produce a false negative by
construction beats one that makes false negatives unlikely.**

**This is not a new mechanism.** The arbiter already declines to speak about a group it has heard
nothing about since starting — `ArbiterThread.hpp` keeps `started_at_` for exactly this, and
[section 11c](design_notes.md#ha_arbiter_relearns) is the same reasoning applied to leadership: a
restarted arbiter that knows nothing must not decide as though it knew something. This question gets
the same answer because it is the same problem.

**Both arbiters must say `all_gone`.** A single arbiter's view is "is it connected to *me*", not
"does it exist" — an arbiter that has lost its own link to a healthy engine would report `all_gone`
truthfully and mislead completely. Requiring agreement makes the conservative direction the default:
any arbiter still seeing an instance means defer.

An earlier draft called this option "the most informative and the least reliable after a restart".
That was wrong, and the correction is the reason it was chosen. **It is less *available* after a
restart, not less reliable.** Its unavailability degrades to the age threshold, which is what the
venue does today, so the failure mode is "no improvement" rather than "wrong answer". The timer's
failure mode is a wrong answer.

### What the sequencer counts as evidence

Worth stating because it changes how strong the conclusion is. The sequencer is not asking a
disinterested question: **it only asks while it is deferring, which means it already cannot reach an
engine itself.** The arbiters supply a second and third independent observation of the same
component. Two or three independent failures to reach an engine is a far stronger basis for refusing
than any one of them alone, and it is why this can act immediately where a timer had to wait.

The rule, in full: **refuse at once when the sequencer cannot reach an engine, it can reach at least
one arbiter, and every arbiter it can reach answers `all_gone`.** Otherwise defer, with the age
threshold behind it.

## With high availability turned off {#ha_me_presence_no_ha}

Asked while this note was being written, and it is the case that decides how the threshold is
described. **With no arbiter there is no fact to ask for.** Every query resolves to "cannot answer"
and the venue falls back on the age threshold for the life of the deployment.

So the threshold is not a backstop there. **It is the entire mechanism**, running in production
every day, and it is how a member finds out. That settles a question the
[order acceptance](order_acceptance.md) design left slightly open: the 45 seconds can never be
removed in favour of the arbiter fact, however good that fact becomes, because a non-HA venue has
no arbiter to ask. It can only ever be demoted to second place where an arbiter exists.

Two things make the non-HA case worse rather than merely different.

**There is no second engine.** A dead matching engine stays dead until somebody starts one, and the
one they start is a cold start — so [BUG-0064](../bug_list.md#bug_0064) applies every time rather
than only after a total outage. With high availability off, orders deferred before the threshold
trips are always lost.

**And today the venue would not even reach the threshold.** Deferral is counted inside the leader
branch of the forward path: `SequencerThread.cpp` returns for `role_ != leader` before
`note_order_deferred` is called. A sequencer that never adopts leadership never defers, never
refuses, and tells nobody anything — which is precisely
[BUG-0061](../bug_list.md#bug_0061), where a venue with high availability disabled accepts orders,
forwards none, and looks healthy throughout. **So the answer to "how would the client ever know?"
is, today, that it would not** — not because the mechanism is missing, but because the code that
runs it is behind a role the venue never adopts.

That makes BUG-0061 a prerequisite for this note rather than a neighbour of it. Fixing it is what
puts the sequencer in the leader role with high availability off, and only then does the fallback
described here actually run.

## What this does not solve

- **Orders already deferred when the answer arrives.** They remain [BUG-0064](../bug_list.md#bug_0064)'s:
  a cold-starting engine never reconciles, so nothing applies them and the member is never told.
  Knowing sooner reduces how many join them and does not rescue the ones there.
- **An engine that is connected and not working.** Everything here keys on registration, and a
  wedged engine is registered. See [BUG-0010](../bug_list.md#bug_0010).
- **Whether the sequencer should ever stop.** It was proposed that a sequencer with no matching
  engine service should die, so the gateway's existing "Sequencer unavailable" rejection fires.
  Rejected here, on four grounds: it tells the member a false reason, since the sequencer was fine
  and the engine was not; it destroys the mechanism that recovers a returning engine, which is the
  sequencer sending what that engine has not applied; there is no supervisor that could restart it
  into anything but a crash loop, since it would find no engine and die again; and it replaces a
  state that heals itself, measured resuming with no operator action, with one that needs a person.
  A sequencer refusing every order, saying why, and ready to recover is a venue in a declared
  degraded state. A dead one is a venue that needs somebody.

  **The case is not closed for a narrower condition.** A sequencer that can reach no arbiter *and*
  has no engine knows nothing and can serve nobody. Whether that should stop is a real question,
  and it belongs with [section 12](design_notes.md#ha_supervisor_role) rather than here.

## See also

- [BUG-0009](../bug_list.md#bug_0009) — the venue taking orders it cannot process
- [BUG-0064](../bug_list.md#bug_0064) — deferred orders never recovered on a cold start
- [Refusing orders the venue cannot process](order_acceptance.md) — the threshold this demotes to a backstop
- [HA design notes 11b](design_notes.md#ha_arbiter_only_arbitrates) — the arbiter answers what it is asked
- [HA design notes 12](design_notes.md#ha_supervisor_role) — a supervisor starts processes; it does not decide
- [Process death](process_death.md) — supervision, which this does not touch

---

Back to [High availability](../availability/README.md).
