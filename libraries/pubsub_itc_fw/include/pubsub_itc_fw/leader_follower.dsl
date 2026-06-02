# ============================================================
#  Leader-Follower Protocol — PDU Definitions
# ============================================================
#
#  DESIGN RATIONALE
#  ----------------
#  This is an intentionally simple, bespoke protocol. There is
#  no need for a full consensus algorithm such as Raft or Paxos.
#  Leader election is deterministic — the node with the lowest
#  instance_id wins. The arbiter pool provides authoritative
#  lease-grant decisions; the witness breaks ties within the
#  arbiter pool itself.
#
#  EPOCH SEMANTICS
#  ---------------
#  The epoch is a generation counter. It exists to detect stale
#  nodes from a previous leadership cycle.
#
#  Rules:
#    1. A node that has never participated in an election starts
#       with epoch 0.
#    2. When arbiter arbitration is used, the arbiter assigns the
#       epoch in ArbitrationDecision. Both nodes adopt this value.
#    3. When a follower detects leader death and promotes itself
#       to leader without arbiter contact, it increments its own
#       epoch by 1. This is the sole mechanism for local epoch
#       advancement.
#    4. When a restarting node connects and receives a
#       StatusResponse, it compares epochs. If the peer's epoch
#       is higher, the restarting node is stale: it adopts the
#       follower role immediately without contacting the arbiter.
#    5. A heartbeat carrying an epoch lower than the receiver's
#       own epoch indicates a stale sender; the receiver logs a
#       warning and ignores the heartbeat.
#
#  TOPOLOGY
#  --------
#  Three machines in the arbiter pool: arbiter-primary,
#  arbiter-secondary, witness. Three votes, majority is two.
#  Tolerates any single-machine failure.
#
#  Components (sequencer pair, ME pair) each open connections to
#  BOTH arbiter machines. Heartbeats and lease-renewal requests
#  flow from the component to the active arbiter; lease grants
#  and ArbitrationDecision PDUs flow back.  The passive arbiter
#  accepts the connection but drops component requests with a log
#  warning (redirect support is a future enhancement).
#
#  The two arbiter instances each hold a copy of the
#  leadership-state map. They elect one active and one passive
#  using StatusQuery/StatusResponse/Heartbeat among themselves,
#  consulting the witness via ArbiterVoteRequest when both are
#  undecided. The active arbiter replicates decisions to the
#  passive via ArbiterStateRecord/ArbiterStateAck.
#
#  The witness holds NO state. It accepts connections from both
#  arbiters and responds to ArbiterVoteRequest PDUs with an
#  ArbiterVoteResponse. The witness never becomes leader,
#  follower, active, or passive in any component sense.
#
#  See pubsub_itc_fw_topology.puml and pubsub_itc_fw_topology.md
#  for the authoritative deployment diagram.
#
#  Role enum
#  Lowest instance_id wins → leader
#  Other becomes follower
#  arbiter role value reserved; not used at runtime
# ------------------------------------------------------------
enum Role : i32 {
    unknown  = 0
    leader   = 1
    follower = 2
    arbiter  = 3
}

# ------------------------------------------------------------
#  100 — StatusQuery
#  Sent A ↔ B immediately after TCP connect.
#  Purpose:
#    - Announce identity
#    - Announce current epoch
#    - Trigger peer to reply with StatusResponse
# ------------------------------------------------------------
message StatusQuery (id=100, version=1)
    i64 instance_id        # unique per node, configured
    i32 epoch              # node's current generation number
end

# ------------------------------------------------------------
#  101 — StatusResponse
#  Reply to StatusQuery.
#  Purpose:
#    - Confirm identity of responder
#    - Echo back what responder believes about the peer
#    - Communicate responder's epoch
#    - Communicate responder's current role so that a restarting node
#      can immediately adopt follower role if the peer is already leader,
#      bypassing arbitration entirely
#  Notes:
#    - No sequence number needed because request/response is synchronous
#    - If current_role is Role::leader, the querying node becomes follower
#      without contacting the arbiter
#    - If current_role is Role::unknown, both sides proceed to arbitration
# ------------------------------------------------------------
message StatusResponse (id=101, version=1)
    i64 self_instance_id       # identity of responder
    i64 peer_instance_id       # identity responder believes it is talking to
    i32 epoch                  # responder's current epoch
    Role current_role          # responder's current role; unknown if not yet elected
    i64 next_sequence_number   # responder's current next_sequence_number_; restarting follower uses this to sync its counter after WAL recovery
end

# ------------------------------------------------------------
#  102 — Heartbeat
#  Sent peer ↔ peer (sequencer-to-sequencer or arbiter-to-arbiter).
#  Purpose:
#    - Liveness detection
#    - Epoch propagation (detect stale nodes)
#  Notes:
#    - No heartbeat counter needed because TCP is ordered and reliable
#    - A heartbeat with epoch lower than the receiver's epoch indicates
#      a stale sender; receiver logs a warning and ignores it
#    - Heartbeat loss triggers leader/follower death detection; see
#      epoch rule 3 in the file header for follower-promotion behaviour
# ------------------------------------------------------------
message Heartbeat (id=102, version=1)
    i64 instance_id        # sender identity
    i32 epoch              # sender's current epoch
end

# ------------------------------------------------------------
#  103 — WalRecord
#  Sent by the leader to the follower to replicate each WAL
#  entry as it is committed.  The follower appends the record
#  to its own WAL and replies with WalAck.  The leader gates
#  ER emission to the gateway on receipt of that ack, ensuring
#  the follower has durably recorded the order before the
#  client-visible fill notification is sent.
# ------------------------------------------------------------
message WalRecord (id=103, version=1)
    i64 seq_no      # sequence number assigned by the leader
    i16 pdu_id      # PDU type tag (e.g. NewOrderSingle = 1000)
    bytes payload   # complete encoded PDU payload (as stored in the WAL)
end

# ------------------------------------------------------------
#  104 — WalAck
#  Sent by the follower to the leader to confirm that the WAL
#  entry for seq_no has been durably written to the follower's
#  on-disk WAL.  Receipt of this PDU by the leader releases any
#  buffered ExecutionReport for the corresponding order.
# ------------------------------------------------------------
message WalAck (id=104, version=1)
    i64 seq_no      # sequence number echoed from the WalRecord
end

# ------------------------------------------------------------
#  200 — ArbitrationReport
#  Sent by a component (sequencer or ME) to the active arbiter
#  when arbitration is required (startup or after peer heartbeat
#  timeout).
#  Purpose:
#    - Tell the active arbiter what this node believes the world
#      looks like
#    - The arbiter uses this to make a deterministic decision
# ------------------------------------------------------------
message ArbitrationReport (id=200, version=1)
    i64 self_instance_id   # identity of sender
    i64 peer_instance_id   # identity of the other node
    i32 epoch              # sender's current epoch
    Role proposed_role     # leader or follower based on lowest-id rule
end

# ------------------------------------------------------------
#  201 — ArbitrationDecision
#  Sent by the active arbiter back to the requesting component,
#  and forwarded to both connected components.
#  Purpose:
#    - Final authoritative assignment of leader and follower
#    - Assigns the epoch for this leadership generation (see
#      epoch rule 2 in the file header)
#  Notes:
#    - No ack required; fire-and-forget
#    - Component connections are kept open between elections for
#      ongoing heartbeats and liveness detection
# ------------------------------------------------------------
message ArbitrationDecision (id=201, version=1)
    i64 leader_instance_id     # node chosen as leader
    i64 follower_instance_id   # node chosen as follower
    i32 epoch                  # arbiter-assigned epoch for this generation
end

# ------------------------------------------------------------
#  300 — ArbiterHeartbeat
#  Sent by an arbiter to the witness at a regular interval for
#  liveness detection.  The witness registers the arbiter on
#  first heartbeat and tracks whether it is reachable.
# ------------------------------------------------------------
message ArbiterHeartbeat (id=300, version=1)
    i64 instance_id        # arbiter identity (configured)
    i32 epoch              # arbiter's current epoch
end

# ------------------------------------------------------------
#  301 — ArbiterVoteRequest
#  Sent by an arbiter to the witness when it is contemplating
#  promotion to active arbiter (i.e. it has lost contact with
#  its peer and needs an independent tie-break vote).
#  Purpose:
#    - Ask the witness: "should I become the active arbiter?"
#    - The witness replies with ArbiterVoteResponse
# ------------------------------------------------------------
message ArbiterVoteRequest (id=301, version=1)
    i64 self_instance_id   # identity of requesting arbiter
    i64 peer_instance_id   # identity of the other arbiter
    i32 epoch              # requester's current epoch
end

# ------------------------------------------------------------
#  302 — ArbiterVoteResponse
#  Sent by the witness to an arbiter in reply to ArbiterVoteRequest.
#  The witness applies the same deterministic rule as component
#  arbitration: lower instance_id wins; if the peer is not
#  connected to the witness, the requester wins unconditionally.
# ------------------------------------------------------------
message ArbiterVoteResponse (id=302, version=1)
    i64 granted_to_instance_id   # which arbiter gets the active role
    i32 epoch                    # epoch for this arbiter generation
end

# ------------------------------------------------------------
#  400 — ArbiterStateRecord
#  Sent by the active arbiter to the passive arbiter to replicate
#  one entry of the leadership-state map.  The passive arbiter
#  stores this record and sends ArbiterStateAck.
# ------------------------------------------------------------
message ArbiterStateRecord (id=400, version=1)
    i64 component_instance_id    # which component's leader was assigned
    i64 leader_instance_id       # assigned leader for that component pair
    i32 epoch                    # leadership epoch for this component
end

# ------------------------------------------------------------
#  401 — ArbiterStateAck
#  Sent by the passive arbiter to the active arbiter to confirm
#  receipt of an ArbiterStateRecord.
# ------------------------------------------------------------
message ArbiterStateAck (id=401, version=1)
    i64 component_instance_id    # echoed from the record being acknowledged
    i32 epoch                    # echoed epoch
end
