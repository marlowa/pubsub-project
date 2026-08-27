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
#  See pubsub_itc_fw_topology.puml and docs/framework/topology.md
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
#  ComponentGroup
#  Identifies which HA pair a component belongs to. The arbiter
#  pool is shared by several independent HA pairs (the sequencer
#  pair, the matching-engine pair, ...), each of which numbers its
#  own members instance_id 1 (primary) and 2 (secondary). Without
#  a group the two pairs alias onto the same {1,2} slots and one
#  pair's election contaminates the other's leadership state. The
#  arbiter keys its leadership-state and connection maps by
#  (group, instance_id); components stamp every ArbitrationReport,
#  ArbitrationDecision, Heartbeat and ArbiterStateRecord with their
#  group and reject decisions addressed to a different group.
# ------------------------------------------------------------
enum ComponentGroup : i32 {
    unknown                   = 0
    sequencer                 = 1
    matching_engine           = 2
    matching_engine_publisher = 3
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
#    - Liveness and epoch, and nothing else. Leadership is asserted by
#      LeadershipLease (117) and not by this message, which is why
#      BOTH instances of a pair send it: an arbiter needs to know that
#      a follower is there, not only that a leader is.
#    - Heartbeat loss triggers leader/follower death detection; see
#      epoch rule 3 in the file header for follower-promotion behaviour
# ------------------------------------------------------------
message Heartbeat (id=102, version=1)
    i64 instance_id        # sender identity
    i32 epoch              # sender's current epoch
    ComponentGroup group   # HA pair this sender belongs to (arbiter registration)
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
    i64 seq_no           # sequence number assigned by the leader
    i16 pdu_id           # PDU type tag (e.g. NewOrderSingle = 1000)
    bytes payload        # complete encoded PDU payload (as stored in the WAL)
    datetime_ns wall_time_ns  # wall time at which the leader sequenced this record; used for WAL replay clock
    # WalRecord doubles as the pipeline envelope: the routing metadata that must not live
    # inside the (DD-derived) FIX PDU rides here instead. FIX messages here are a genuine
    # FIX50SP2 subset, so venue-internal routing data cannot be smuggled in as an invented
    # tag; it belongs on the envelope. See docs/fix/pdu_generation.md.
    #
    # The four fields below are optional together, and for one shared reason: a WalRecord
    # does not always have an originating client session. Plain leader-to-follower
    # replication records do not, and neither do the ERs the matching engine emits with no
    # originating order (the seq_no==0 cancel-on-failover ERs). Optional says "there was no
    # client session here", which a receiver must be able to distinguish from a real session
    # that happens to be numbered zero.
    optional i32 gateway_session_conn_id  # originating client session; sequencer routes the ER back to it
    optional string sender_comp_id        # originating client comp id, retained for audit
    # Which gateway the order came from. gateway_session_conn_id is only unique within one
    # gateway -- each numbers its own client connections -- so with more than one gateway
    # (the ASCII FIX one and the binary one) the pair (origin_gateway_id, session conn id)
    # is what identifies a client session.
    optional i16 origin_gateway_id
    # Which *instance* of that gateway. origin_gateway_id names a protocol -- the ASCII FIX
    # gateway or the binary one -- and nothing else: the two are separate axes, because one
    # protocol can be served by several processes and the protocol id alone stopped
    # identifying a process the moment a second instance was started. Conflating them is a
    # mistake this project has already made at three separate layers; see
    # docs/availability/gateway_ha.md.
    #
    # The triple (origin_gateway_id, gateway_instance_id, gateway_session_conn_id)
    # identifies a client session venue-wide.
    #
    # Read sites currently substitute gateway_ids::default_when_absent and
    # gateway_ids::first_instance when these are missing. That substitution is a leftover:
    # it was there to let older records route, and since the records it protected no longer
    # exist it now only turns a genuinely absent origin into a plausible-looking wrong one.
    # Both fields are set exactly when gateway_session_conn_id is, so a reader that has
    # already checked that has nothing left to default.
    optional i16 gateway_instance_id
    # Wall-clock nanoseconds at which the originating gateway first read this order off the
    # client socket. Stamped by the gateway on the NOS/OCR envelope, remembered by the
    # sequencer against the order's seq_no, and stamped back onto the ER envelope so the
    # gateway can measure the whole round trip when it sends the ER. It exists solely to be
    # measured: nothing routes or matches on it.
    #
    # Optional because it is genuinely absent, not to spare any older reader:
    #   - plain leader-to-follower replication records have no originating client at all;
    #   - ERs the matching engine emits with no originating order (the seq_no==0
    #     cancel-on-failover ERs) never had an ingress time to remember;
    #   - an order replayed from the WAL carries the ingress time of the original client
    #     read, which is minutes or hours stale, so a consumer must be able to tell a
    #     missing value from a misleading one rather than reading a defaulted zero.
    # A recorded observation is therefore always a real measurement; see docs/operations/metrics.md.
    #
    # Wall clock rather than steady clock, because the two ends of the measurement are not
    # always stamped by the same process: after a gateway failover the ER is sent by the
    # instance that took the session over, whose steady clock shares no origin with the
    # instance that read the order. The gateway discards a negative delta for that reason.
    optional datetime_ns gateway_ingress_ns
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
#  External WAL subscriber protocol (105-106)
#
#  cursor: a sequence number marking how far an external subscriber
#  has consumed the WAL stream. A subscriber presenting cursor N
#  has already received and processed all records with seq_no <= N
#  and wishes to receive records with seq_no > N next. The
#  sequencer uses each subscriber's cursor when deciding which old
#  WAL segments are safe to delete.
#
# ------------------------------------------------------------
#  105 — WalSubscribeRequest
#  Sent by an external WAL subscriber (e.g. MEP primary or
#  MEP secondary) to the sequencer's external WAL subscriber
#  listener immediately after the TCP connection is established.
#  The sequencer replies with WalSubscribeAck and then streams
#  WalRecord PDUs from accepted_from_seq_no onward.
#
#  from_seq_no semantics:
#    0  = start from the sequencer's oldest retained WAL record
#         (full replay from the beginning).
#   -1  = start from the sequencer's current WAL head
#         (no replay; live stream only).
#    N  = resume from seq_no N (reconnect after disconnect).
# ------------------------------------------------------------
message WalSubscribeRequest (id=105, version=1)
    string subscriber_id    # stable identity; used for logging and cursor tracking
    i64    from_seq_no      # requested starting cursor
end

# ------------------------------------------------------------
#  106 — WalSubscribeAck
#  Sent by the sequencer to the external WAL subscriber in
#  reply to WalSubscribeRequest.  Streaming of WalRecord PDUs
#  begins immediately after this PDU is sent.
#
#  accepted_from_seq_no may differ from the requested cursor
#  if the request predates the sequencer's oldest retained
#  record; in that case streaming starts from the oldest
#  available record and a warning is logged.
# ------------------------------------------------------------
message WalSubscribeAck (id=106, version=1)
    i64 accepted_from_seq_no    # actual starting seq_no for the stream
end

# ------------------------------------------------------------
#  115 -- MePositionRequest
#  Sent by a matching-engine instance to the sequencer when it
#  begins WAL reconciliation (RECONCILING state).  It tells the
#  sequencer the seq_no of the last record the ME has already
#  applied (via book replication).  The sequencer then streams all
#  WAL records after that point so the ME can catch up before it
#  begins live processing as the new leader.
# ------------------------------------------------------------
message MePositionRequest (id=115, version=1)
    i64 last_seq_no        # last seq_no the ME has already applied
end

# ------------------------------------------------------------
#  116 -- MePositionAck
#  Sent by the sequencer to the matching engine once WAL catch-up
#  streaming is complete.  It carries the sequencer's current WAL
#  head; on receipt the ME considers its book reconciled and
#  transitions to LEADER state.
# ------------------------------------------------------------
message MePositionAck (id=116, version=1)
    i64 last_seq_no        # sequencer's current WAL head at catch-up completion
end

# ------------------------------------------------------------
#  118 -- LeadershipLease
#  Sent by the leader of an HA pair to the arbiter, repeatedly, for
#  as long as it leads.
#
#  This was carried by Heartbeat until 2026-08-22, which was a poor
#  name for it: a heartbeat says a sender is alive, and this says
#  something much stronger -- that the sender holds leadership of a
#  group, at a stated epoch, and is renewing it. The code had always
#  described it as a lease in its comments while calling it a
#  heartbeat on the wire.
#
#  Separating them matters for more than naming. Only a leader has
#  reason to renew a lease, so while the two were one message only
#  leaders ever reached the arbiter -- and the arbiter registers a
#  component when it hears from it, so it never knew a follower was
#  connected at all. Its own cold-start rule asks whether the peer is
#  connected, and it was asking about a map that could not contain
#  followers.
#
#  It is also how an arbiter that has restarted learns who leads. It
#  holds that knowledge only in memory and reads nothing back at
#  startup, so rather than persisting it, it is told: a lease renewal
#  is an assertion of leadership by the only party entitled to make
#  one, and the epoch settles any disagreement between two of them.
# ------------------------------------------------------------
message LeadershipLease (id=118, version=1)
    i64 instance_id        # the instance asserting leadership
    ComponentGroup group   # the HA pair it leads
    i32 epoch              # the epoch it holds leadership under
end

# ------------------------------------------------------------
#  117 -- RoleAnnouncement
#  Sent by a matching engine to the sequencer to say which role it
#  currently holds, and under which epoch it holds it.
#
#  It exists because the sequencer used to decide where to send orders
#  by which socket had connected: the connection from the primary was
#  "the matching engine" and the one from the secondary was a standby.
#  That was true only while primary and leader meant the same thing.
#  Once an instance can restart and rejoin as a follower, the sequencer
#  can be sending orders to an instance that discards them while the
#  leader sits on the connection it calls the standby.
#
#  The epoch is what makes the claim safe to believe. The sequencer
#  accepts an announcement only when its epoch is at least as new as
#  the last it accepted for that group, so an instance whose leadership
#  has since been superseded cannot reclaim routing -- its epoch is
#  behind and the claim is refused. The authority still rests with the
#  arbiter, because the epoch being quoted is one the arbiter issued;
#  the sequencer never has to ask it anything.
#
#  Sent on connecting to the sequencer, and again on every role change,
#  so a sequencer that restarts learns the current arrangement from the
#  next announcement rather than having to remember it.
# ------------------------------------------------------------
message RoleAnnouncement (id=117, version=1)
    i64 instance_id        # which instance is announcing
    ComponentGroup group   # the HA pair it belongs to
    Role current_role      # the role it now holds
    i32 epoch              # the epoch it holds that role under
end

# ------------------------------------------------------------
#  120 -- SessionBound
#  Sent by a gateway to the sequencer once a client session is
#  authenticated and established, and again on every reconnect.
#
#  It tells the sequencer where a session identity currently lives:
#  which gateway instance is holding it, on which connection. The
#  sequencer keys its routing on the identity and treats the
#  connection as a mutable destination, so a member that reconnects
#  -- to the same instance or to its backup -- has its execution
#  reports follow it without anything else being rewritten.
#
#  Before this existed, the sequencer's routing entry was keyed on
#  the connection the order arrived on, which is gateway-local and
#  dies with the socket. A reconnecting member could therefore not
#  be handed reports for orders it had already placed: the address
#  they were bound to no longer existed. See docs/availability/gateway_ha.md.
#
#  The identity is (comp_id, gateway_protocol_id), NOT the comp id
#  alone. A comp id gets one session per order-entry protocol: an
#  instance failover moves a session between instances of the SAME
#  protocol, which is the case that has to keep working, while a FIX
#  and a binary session sharing a comp id stay separate books with
#  separate reports.
#
#  Sent for every established session, not only for reconnects. The
#  sequencer cannot tell a first logon from a return, and a binding
#  it never received is one it cannot route to.
# ------------------------------------------------------------
message SessionBound (id=120, version=1)
    string comp_id                # the session identity, with the protocol below
    i16    gateway_protocol_id    # which order-entry protocol: see GatewayIds.hpp
    i16    gateway_instance_id    # which instance of that protocol now holds it
    i32    gateway_session_conn_id # the connection within that instance: the destination
end

# ------------------------------------------------------------
#  SeqNumRange -- an inclusive run of outbound sequence numbers.
#
#  Nested only; it is never a PDU in its own right, hence id=0.
#
#  Carried by the three session PDUs below to say which of a
#  member's outbound numbers held an execution report. A resend
#  refills a range of numbers from the WAL, and the WAL holds
#  reports and nothing else -- so every number that held a Logon,
#  a heartbeat or a reject has to be gap-filled instead, and the
#  venue can only do that if it knows which those were.
#
#  Ranges rather than a number apiece because reports come in
#  runs: a member sending orders is sent a contiguous block of
#  numbered reports, broken only when it goes quiet long enough
#  for a heartbeat to take one. A burst of ten thousand orders
#  is a single range.
#
#  See docs/availability/resend_provenance.md, and BUG-0051 for
#  what filling every number with a report instead does.
# ------------------------------------------------------------
message SeqNumRange (id=0, version=1)
    i32 from_seq_num
    i32 to_seq_num              # inclusive
end

# ------------------------------------------------------------
#  121 -- SessionUnbound
#  Sent by a gateway when a client session goes away, so the
#  sequencer stops addressing reports at a connection that is gone.
#
#  Unbinding is deliberately NOT the same as forgetting the session:
#  the identity and its orders outlive the connection, which is the
#  whole point of keying on the identity. An unbound session's
#  reports have nowhere to go until it binds again -- today they are
#  dropped, as they were before; step 6 is what makes them replayable.
#
#  Carries the connection id so a late unbind cannot tear down a
#  newer binding: a member that reconnects fast enough for its new
#  SessionBound to overtake the old connection's SessionUnbound would
#  otherwise be unbound immediately after binding. The sequencer
#  ignores an unbind naming a connection it is no longer bound to.
# ------------------------------------------------------------
message SessionUnbound (id=121, version=1)
    string comp_id
    i16    gateway_protocol_id
    i16    gateway_instance_id
    i32    gateway_session_conn_id # the connection going away; ignored if not the current one
    # Where the session's sequence numbers had reached when it ended, handed back so the
    # next gateway to hold this session can carry on from them rather than restarting at 1.
    # A member that saw its sequence reset on every reconnect would see a break it cannot
    # reconcile, which is the opposite of surviving a failover.
    #
    # Reported by the gateway rather than counted by the sequencer because the sequencer
    # cannot count them: the FIX outbound number covers every message sent to the member,
    # including the heartbeats, Logouts and Rejects that never come near the sequencer.
    #
    # This is therefore only as current as the last clean unbind. A gateway that is killed
    # sends none, so the stored numbers stay where they were and the returning member finds
    # the venue behind it -- which its own ResendRequest then resolves, and which is one of
    # the reasons resend has to work.
    #
    # Only the outbound number is carried. The gateway does not track what the member sends
    # it -- there is no inbound gap detection to hold a number for -- so a field for it would
    # be one nothing populates. When that is built, it belongs here beside this one.
    i32    outbound_seq_num        # next number the venue would send to this member
    # Which of this session's outbound numbers held an execution report, for the part of the
    # stream this gateway has not already reported on SessionSequenceUpdate. Everything not
    # covered by these -- and by what earlier updates carried -- held something the venue
    # cannot replay, and a resend gap-fills it. See SeqNumRange above.
    list<SeqNumRange> report_seq_nums
end

# ------------------------------------------------------------
#  122 -- SessionBoundAck
#  Sent by the sequencer in reply to SessionBound, handing the
#  gateway whatever the venue remembers about this session.
#
#  A session's sequence numbers belong to the session and not to
#  the connection carrying it, so a gateway that has just taken
#  one on cannot know where it had got to. The sequencer does,
#  because it is the one component every instance of every
#  protocol reports to.
#
#  known = false means the venue has never seen this session, so
#  the numbers are the defaults and the gateway starts fresh. It
#  is not an error: a member's first ever logon takes this path.
# ------------------------------------------------------------

message SessionBoundAck (id=122, version=1)
    string comp_id
    i16    gateway_protocol_id
    bool   known                   # false: first sight of this session, the number is the default
    i32    outbound_seq_num        # next number to send to the member
    # Which of this session's outbound numbers held an execution report, as far back as the
    # venue still remembers. This is what lets a gateway answer a resend for messages it did
    # not send: the instance that did send them is gone, and this is the only record of what
    # its numbering carried. Empty when known = false.
    list<SeqNumRange> report_seq_nums
end

# ------------------------------------------------------------
#  123 -- SessionReplayRequest
#  Sent by a gateway to ask for a session's execution reports
#  back, so it can answer a member that has asked for messages
#  it missed.
#
#  The reports are in the WAL already -- every one, with the
#  session that originated it on the envelope -- so nothing has
#  to be stored a second time to answer this. What the sequencer
#  does is walk its WAL and hand back the records belonging to
#  one session, which is the only part of recovery the venue did
#  not already have.
#
#  max_records bounds the answer. A member asking for everything
#  since the beginning of a long session would otherwise be
#  served a reply proportional to the whole trading day, on the
#  reactor thread that is also serving live order flow.
# ------------------------------------------------------------

message SessionReplayRequest (id=123, version=1)
    i64    request_id              # gateway-assigned; echoed on every record and on completion
    string comp_id
    i16    gateway_protocol_id
    i64    from_seq_no             # WAL sequence to resume after; 0 means from the beginning
    i32    max_records             # cap on records returned; 0 means the sequencer's own limit
    # How many of this session's most recent reports to pass over before starting to collect.
    #
    # Without it the sequencer can only return the most recent max_records reports, which is
    # right when the member is asking about the tail of its stream -- the usual case, after a
    # disconnect -- and wrong for any other range. A member asking about the middle of its
    # history was sent recent reports wearing the numbers it asked for, with every other
    # property of the reply correct. That is BUG-0053.
    #
    # The gateway can always compute it: the reports above the range being replayed are recent
    # ones, so they are covered by what it knows of the session's numbering.
    i32    skip_most_recent        # reports to skip, counting back from the newest; 0 for the tail
end

# ------------------------------------------------------------
#  124 -- SessionReplayRecord
#  One execution report from the replay, in WAL order.
#
#  A PDU of its own rather than the WalRecord a live report
#  arrives in, so that a gateway cannot mistake a replayed report
#  for a live one: the two need different treatment on the wire
#  (a replayed FIX report carries PossDupFlag=Y) and confusing
#  them would tell a member an old fill had just happened.
# ------------------------------------------------------------

message SessionReplayRecord (id=124, version=1)
    i64    request_id              # echoed from SessionReplayRequest
    i64    seq_no                  # the record's WAL sequence number
    i64    wall_time_ns            # when the venue originally sequenced it
    bytes  payload                 # the encoded ExecutionReport, exactly as stored
end

# ------------------------------------------------------------
#  125 -- SessionReplayComplete
#  Ends a replay. Sent even when no records matched, because the
#  gateway is waiting for it before it lets live traffic resume:
#  without a definite end it could not tell "nothing to send" from
#  "still coming".
#
#  truncated = true means max_records was reached and more remain
#  after last_seq_no. The gateway can ask again from there.
# ------------------------------------------------------------

message SessionReplayComplete (id=125, version=1)
    i64    request_id
    i32    record_count            # records sent in this reply
    i64    last_seq_no             # WAL sequence of the last record sent, or from_seq_no if none
    bool   truncated               # more records remain beyond last_seq_no
end

# ------------------------------------------------------------
#  126 -- SessionSequenceUpdate
#  Sent by a gateway on a timer, for each session it holds, to
#  keep the sequencer's record of where that session's numbering
#  has reached.
#
#  Reporting only at SessionUnbound was not enough, and the way
#  it failed is worth stating: a gateway that is KILLED reports
#  nothing, so the sequencer had no record at all and started the
#  returning member at 1. With a client whose own store had also
#  restarted, both sides sat at 1, no gap was visible, and the
#  member was silently resynchronised while thousands of its
#  orders were live on the book.
#
#  The sequencer cannot derive this number: it counts every
#  message the gateway sends the member, including the heartbeats
#  and session-level rejects that never reach the sequencer. So it
#  is reported, and reported often, because the value is only ever
#  useful when the process holding it has died without warning.
# ------------------------------------------------------------

message SessionSequenceUpdate (id=126, version=1)
    string comp_id
    i16    gateway_protocol_id
    i16    gateway_instance_id
    i32    gateway_session_conn_id
    i32    outbound_seq_num        # next number this gateway would send to the member
    # Which outbound numbers have held an execution report since the last update, so the
    # sequencer's record keeps pace with the session rather than arriving only at unbind.
    #
    # Sent here for the same reason the number above is: a gateway that is killed sends no
    # unbind, and a record that only ever travelled at unbind would be empty in exactly the
    # case it exists for. Incremental rather than the whole history, so an update stays small
    # however long the session has been up.
    list<SeqNumRange> report_seq_nums
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
    ComponentGroup group   # HA pair this report belongs to
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
    ComponentGroup group       # HA pair this decision is addressed to
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
    ComponentGroup group         # HA pair this record belongs to
end

# ------------------------------------------------------------
#  401 — ArbiterStateAck
#  Sent by the passive arbiter to the active arbiter to confirm
#  receipt of an ArbiterStateRecord.
# ------------------------------------------------------------
message ArbiterStateAck (id=401, version=1)
    i64 component_instance_id    # echoed from the record being acknowledged
    i32 epoch                    # echoed epoch
    ComponentGroup group         # echoed HA pair
end
