# ============================================================
#  Leader-Follower Protocol — PDU Definitions
# ============================================================
#
#  DESIGN RATIONALE
#  ----------------
#  This is an intentionally simple, bespoke protocol. There is
#  no need for a full consensus algorithm such as Raft or Paxos.
#  The deployment topology is fixed: exactly two active nodes
#  per site, with a third node (DR arbiter) available to break
#  ties at startup. Leader election is deterministic — the node
#  with the lowest instance_id wins. The arbiter never becomes
#  a leader or follower; it only resolves the startup ambiguity
#  when both nodes are undecided.
#
#  EPOCH SEMANTICS
#  ---------------
#  The epoch is a generation counter. It exists to detect stale
#  nodes from a previous leadership cycle.
#
#  Rules:
#    1. A node that has never participated in an election starts
#       with epoch 0.
#    2. At startup, when DR arbitration is used, the DR arbiter
#       assigns the epoch in ArbitrationDecision. Both nodes
#       adopt this value.
#    3. When a follower detects leader death and promotes itself
#       to leader (no DR contact), it increments its own epoch
#       by 1. This is the sole mechanism for local epoch
#       advancement.
#    4. When a restarting node connects and receives a
#       StatusResponse, it compares epochs. If the peer's epoch
#       is higher, the restarting node is stale: it adopts the
#       follower role immediately without contacting DR.
#    5. A heartbeat carrying an epoch lower than the receiver's
#       own epoch indicates a stale sender; the receiver logs a
#       warning and ignores the heartbeat.
#
#  TOPOLOGY
#  --------
#  Main site : node A (primary), node B (secondary)
#  DR site   : node C (primary-DR), node D (secondary-DR)
#
#  Main-site election uses DR-site nodes as arbiters.
#  DR-site election uses main-site nodes as arbiters.
#  Primary arbiter is tried first; secondary arbiter is the
#  fallback if the primary is unreachable.
#
#  Role enum
#  Lowest instance_id wins → leader
#  Other becomes follower
#  DR is a pure arbiter and never becomes leader/follower
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
#      without contacting DR
#    - If current_role is Role::unknown, both sides proceed to arbitration
# ------------------------------------------------------------
message StatusResponse (id=101, version=1)
    i64 self_instance_id   # identity of responder
    i64 peer_instance_id   # identity responder believes it is talking to
    i32 epoch              # responder's current epoch
    Role current_role      # responder's current role; unknown if not yet elected
end

# ------------------------------------------------------------
#  102 — Heartbeat
#  Sent A ↔ B only (never to DR).
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
#  200 — ArbitrationReport
#  Sent A → DR and B → DR when DR is reachable AND
#  arbitration is required (startup or partition).
#  Purpose:
#    - Tell DR what this node believes the world looks like
#    - DR uses this to break ties deterministically
#  Notes:
#    - peer_role_hint removed; DR does not need it
# ------------------------------------------------------------
message ArbitrationReport (id=200, version=1)
    i64 self_instance_id   # identity of sender
    i64 peer_instance_id   # identity of the other node
    i32 epoch              # sender's current epoch
    Role proposed_role     # leader or follower based on lowest-id rule
end

# ------------------------------------------------------------
#  201 — ArbitrationDecision
#  Sent DR → A and DR → B.
#  Purpose:
#    - Final authoritative tie-break
#    - Assigns the epoch for this leadership generation (see
#      epoch rule 2 in the file header)
#  Notes:
#    - No ack required; DR is fire-and-forget
#    - After receiving this, both nodes close the DR connection;
#      it is not kept open between elections
# ------------------------------------------------------------
message ArbitrationDecision (id=201, version=1)
    i64 leader_instance_id     # node chosen as leader
    i64 follower_instance_id   # node chosen as follower
    i32 epoch                  # DR-assigned epoch for this generation
end
