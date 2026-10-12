# ============================================================
#  Lease protocol -- message definitions
# ============================================================
#
#  An instance of a pair leads only while a majority of three voters
#  has granted it a lease that has not run out. The voters are the
#  two instances and a third voter that is never a candidate. An
#  instance's own vote is one of the three, so one grant from either
#  other voter is enough. The rules, and the model checking that shows
#  each is needed, are in docs/availability/majority_leases.md; the
#  code that applies them is pubsub_itc_fw/PairLeaseAgent.hpp.
#
#  These are the library's own messages. They name a pair by a plain
#  number, pair_id, which the application chooses and configures: the
#  library knows nothing of what the pairs are. Every voter in a pair
#  must be configured with the same number for it.
#
#  An epoch is a generation counter, one value for each leadership
#  generation; see pubsub_itc_fw/LeaderEpoch.hpp. A voter refuses a
#  request for an epoch below the highest it has granted.

# ------------------------------------------------------------
#  LeaseRefusalReason
#  Why a voter refused a LeaseRequest. A log that says why is what
#  lets someone reading it tell a voter that has just restarted from
#  one whose vote is taken. Only one reason changes what the asker
#  does: after recording_promise, a candidate asks again at its next
#  renewal interval instead of waiting out the lease period, because
#  the voter is about to grant.
# ------------------------------------------------------------
enum LeaseRefusalReason : i32 {
    unknown            = 0
    restarting         = 1    # the voter started less than one lease period ago
    promised_elsewhere = 2    # its vote is promised to another instance, or to its own
    epoch_behind       = 3    # it has already granted a higher epoch
    may_not_lead       = 4    # it holds a leader's statement that the asker may not lead, because the asker lacks commands the leader has acted on
    recording_promise  = 5    # it would grant, but the promise of its vote is not yet on disk; it is being written, and the asker should ask again shortly
}

# ------------------------------------------------------------
#  130 -- LeaseRequest
#  Sent by an instance to each of the other two voters, to ask to
#  lead or to renew the lease it leads under.
#
#  An instance leads only while a majority of three voters has
#  granted it a lease that has not run out: the two instances of the
#  pair and a third voter. Its own vote is one of the three, so one
#  grant from either other voter is enough. See
#  docs/availability/majority_leases.md.
#
#  Asking to lead and renewing are the same request: the voter
#  applies the same rules to both. A leader sends it repeatedly,
#  well within the lease period, which is also how its follower
#  knows it is alive.
#
#  request_id is chosen by the asker and echoed on the reply. The
#  asker counts a lease from when it SENT the request, not from when
#  the grant arrived, and the id is how it finds that moment again.
#  Ids need only be unique within one process's lifetime: a reply
#  travels on the connection its request came in on, and a process
#  that restarts has new connections.
#
#  In a pair whose leader makes statements (PeerStatementsFlag), the
#  leader also says, on every request, whether its peer may lead. It
#  says the peer may not while it acts on commands the peer does not
#  hold. A voter that grants the request records the statement if it
#  is newer than the one it holds, and refuses a lease to the instance
#  it names. See docs/availability/a_follower_behind_does_not_lead.md.
#  A candidate's request, and every request in a pair whose leader
#  makes no statements, carries no statement: statement_leader_id is
#  zero.
# ------------------------------------------------------------
message LeaseRequest (id=130, version=1)
    i64 candidate_instance_id   # the instance asking
    i32 pair_id                 # the pair it belongs to, as a number every voter in the pair is configured with
    i32 epoch                   # the epoch it asks to lead in, or leads in
    i64 request_id              # echoed on the reply
    i64 statement_leader_id # the leader making the statement about its peer, or zero for none
    i32 statement_epoch # the epoch of the leadership that made it
    i64 statement_number # goes up by one each time that leadership changes what it says
    bool peer_may_lead # false: the leader's peer may not lead
end

# ------------------------------------------------------------
#  131 -- LeaseGrant
#  A voter's grant of a LeaseRequest. The voter has promised not to
#  grant a lease to anyone else for one lease period, counted from
#  the moment it granted this one.
# ------------------------------------------------------------
message LeaseGrant (id=131, version=1)
    i64 voter_instance_id       # the voter granting: 1 or 2 for an instance, 3 for the third voter
    i32 pair_id                 # the pair, as on the request
    i32 epoch                   # the epoch granted, as asked
    i64 request_id              # echoed from the request
    i64 echoed_statement_number # the number of the request's statement, if the voter now holds it; otherwise zero
end

# ------------------------------------------------------------
#  132 -- LeaseRefusal
#  A voter's refusal of a LeaseRequest. It carries the highest epoch
#  the voter has granted: an asker below it has been overtaken by a
#  newer generation, stops leading or asking, and asks again above
#  it.
# ------------------------------------------------------------
message LeaseRefusal (id=132, version=1)
    i64 voter_instance_id
    i32 pair_id                 # the pair, as on the request
    i32 highest_epoch           # the highest epoch this voter has granted
    i64 request_id              # echoed from the request
    LeaseRefusalReason reason
end

# ------------------------------------------------------------
#  400 -- ArbiterStateRecord
#  Sent by an arbiter to its peer to say the highest epoch granted in
#  one pair it votes in: whenever the active arbiter grants a higher
#  one, and for every pair when the peer link comes up.
#
#  An arbiter that becomes active knows nothing of what the previous
#  one promised, and waits out one lease period for that reason. It
#  would also not know the highest epoch granted, and could grant a
#  lower one, which receivers then ignore. This record is what it
#  knows instead.
#
#  It also carries the newest statement the arbiter holds from the
#  group's leader about whether the leader's peer may lead, and is
#  sent whenever that changes. Unlike a promise, a statement must not
#  be forgotten when the active arbiter changes: it is what keeps an
#  instance lacking commands its leader acted on from leading.
# ------------------------------------------------------------
message ArbiterStateRecord (id=400, version=1)
    i64 component_instance_id    # the instance that leads in the epoch: its remainder on division by 4
    i64 leader_instance_id       # the same
    i32 epoch                    # the highest epoch granted in the group
    i32 pair_id                  # the pair, as its instances put it on their lease messages
    i64 statement_leader_id # the leader that made the newest statement held, or zero for none
    i32 statement_epoch # the epoch of the leadership that made it
    i64 statement_number # its number within that leadership
    bool peer_may_lead # false: the leader's peer may not lead
end
