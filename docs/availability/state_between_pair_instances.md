# State sent between the two instances of a pair

Every component that runs as a pair keeps the two instances agreeing through the sequencer's WAL. The WAL numbers its
records one after another with no gaps, so an instance that misses a record knows it has, and is sent it again. State
that one instance sends the other in any other way has no such numbering: a message lost on the way can leave the
receiver wrong without either instance knowing.

This document lists every message that travels between the two instances of a pair outside the WAL, says what each
carries, and says whether a lost, late or repeated copy can leave the receiver wrong without its knowing. It is the
check asked for by phase 11 of the [recovery plan](../recovery_plan.md).

The pairs are the two sequencers, the two matching engines, the two matching engine publishers, the two arbiters, the
two FIX gateways and the two authentication services. Each was checked by reading the code that handles messages on its
connections to its peer: `SequencerThread::handle_peer_pdu`, the peer branch of `MatchingEngineThread`'s message
handler, `MatchingEnginePublisherThread::handle_peer_pdu` and `ArbiterThread::handle_peer_pdu`.

## The lease messages, in every pair

`LeaseRequest`, `LeaseGrant` and `LeaseRefusal` go between the two instances of every pair with a peer connection, and
to the third voter. They decide which instance leads ([majority_leases.md](majority_leases.md)). A request is sent
again every renewal interval, a lease runs out unless it is renewed, and a voter records a newer epoch and ignores an
older one, so a lost message delays a decision and a late one is ignored. Both are checked by the model in
[tla/findings.md](tla/findings.md), section 11, in which any message may be lost or arrive late.

In the sequencer and matching engine pairs, a lease request also carries the leader's statement about whether its peer
may lead (rule 11 of [majority_leases.md](majority_leases.md)). Each statement is numbered, a voter keeps only the
newest it has seen, and the leader sends it on every request until a grant echoes it, so a lost or late copy cannot undo
a newer one. This is checked by the model in [tla/findings.md](tla/findings.md), section 12. The one place a copy of a
statement can be lost without anyone knowing is between the two arbiters, below.

## The sequencers

| Message | What it carries | If lost, late or repeated |
|---|---|---|
| `ReplicatedRecord`, `WalAck`, `LogPositionRequest`, `LogPositionReply`, and the records the leader sends a rejoining follower | The WAL itself, and how far the follower holds it | The WAL's own numbering finds every gap ([follower_log_repair.md](follower_log_repair.md)) |
| `StatusQuery`, `StatusResponse` | Each instance's role, epoch and next sequence number | Nothing acts on them: the receiver logs them and sets a field that nothing reads ([BUG-0232](../bug_list.md#bug_0232)) |

## The matching engines

The follower engine applies every command from the WAL itself, so no copy of the book travels between the two engines.

| Message | What it carries | If lost, late or repeated |
|---|---|---|
| `EngineSettings` | The two settings the engines must share: the open-order region's capacity and a fingerprint of the identifier secret | Sent on the connection's coming up, and a repeat is harmless, since the receiver compares values. But it is sent only on the engine's own connection to its peer's listener, so a leader whose peer reaches it only the other way never learns the peer's settings ([BUG-0231](../bug_list.md#bug_0231)) |
| `AppliedThroughQuery`, `AppliedThrough` | An engine that died applying a command asks how far its peer has applied ([voided_commands.md](voided_commands.md), section 4.2) | A lost answer runs out a timer of one lease period, and the engine asks the leading sequencer to void the command instead, which is the safe course |
| `RoleAnnouncement` | The engine's role and epoch | The peer drops it: leases decide the roles ([BUG-0232](../bug_list.md#bug_0232)) |

## The matching engine publishers

Only the lease messages. Each publisher follows the WAL from the leading sequencer.

## The arbiters

| Message | What it carries | If lost, late or repeated |
|---|---|---|
| `ArbiterStateRecord` | For each pair the arbiters vote in, the highest epoch granted and the leader's newest statement about whether its peer may lead | A repeat or a late copy is harmless: the receiver keeps the higher epoch and the newer statement. A lost copy is made up when the link between the arbiters comes up again, but only if the active arbiter is still running then. Nothing acknowledges it, and the active arbiter echoes a statement before the passive one has it, so a change of active arbiter can lose a statement the leader is relying on ([BUG-0230](../bug_list.md#bug_0230)). A lost epoch costs only availability ([tla/findings.md](tla/findings.md), section 11.5) |

## The FIX gateways

The two gateways have no connection to each other. They share each member session's numbering through the sequencers:
each gateway sends `SessionSequenceUpdate` to both sequencers every two seconds, and `SessionUnbound` when a session
ends, and a session that moves to the other gateway is resumed from what the sequencer holds
([session_binding.md](session_binding.md)). A clean end leaves the figures exact. After a gateway dies, they are up to
two seconds old, and the design makes that loss noticed and made up rather than silent: the outbound number is resumed
deliberately high, so the member sees a gap and asks for a resend, which the venue answers; the inbound number is
resumed as last reported, so the member sends again the messages the dead gateway took, and the check against
sequencing a command twice ([one_check_against_sequencing_twice.md](one_check_against_sequencing_twice.md)) finds any
the venue already holds.

The binary gateways have no numbering to share, since the binary protocol has none.

## The authentication services

The two instances have no connection to each other, and share nothing.

---

Back to [High availability](../availability/README.md).
