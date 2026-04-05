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
#  Notes:
#    - No sequence number needed because request/response is synchronous
#    - No role_hint needed because roles are deterministic
# ------------------------------------------------------------
message StatusResponse (id=101, version=1)
    i64 self_instance_id   # identity of responder
    i64 peer_instance_id   # identity responder believes it is talking to
    i32 epoch              # responder's current epoch
end

# ------------------------------------------------------------
#  102 — Heartbeat
#  Sent A ↔ B only (never to DR).
#  Purpose:
#    - Liveness detection
#    - Epoch propagation (detect stale leaders)
#  Notes:
#    - No heartbeat counter needed because TCP is ordered and reliable
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
#    - DR may bump epoch here
#  Notes:
#    - No ack required; DR is fire-and-forget
# ------------------------------------------------------------
message ArbitrationDecision (id=201, version=1)
    i64 leader_instance_id     # node chosen as leader
    i64 follower_instance_id   # node chosen as follower
    i32 epoch                  # DR-assigned epoch for this generation
end
