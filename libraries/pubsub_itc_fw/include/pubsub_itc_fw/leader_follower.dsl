# ============================================================
#  Leader-Follower Protocol — PDU Definitions
# ============================================================
#
#  WHICH INSTANCE LEADS
#  ---------------------
#  An instance of a pair leads only while a majority of three voters
#  has granted it a lease that has not run out. For a component pair
#  (the sequencers, the matching engines, the matching engine
#  publishers) the voters are the two instances and the arbiter pool,
#  which votes through whichever arbiter is active. For the arbiters
#  themselves the voters are the two arbiters and the witness. An
#  instance's own vote is one of the three, so one grant from either
#  other voter is enough. LeaseRequest, LeaseGrant and LeaseRefusal
#  (130-132) carry the protocol. The rules, and the model checking
#  that shows each is needed, are in docs/availability/majority_leases.md;
#  the code that applies them is fix_common/PairLeaseAgent.hpp.
#
#  EPOCH SEMANTICS
#  ---------------
#  The epoch is a generation counter, one value for each leadership
#  generation. It travels on the lease messages and on RoleAnnouncement,
#  not on orders or reports. A voter refuses a request for an epoch
#  below the highest it has granted, and the sequencer refuses a
#  matching engine's announcement older than one it has accepted. What
#  keeps a deposed leader's orders and reports from being acted on is
#  its own lease ending, which stops it sending.
#
#  Rules:
#    1. A node that has never taken part starts with epoch 0.
#    2. The value of every epoch records which instance leads in it:
#       its remainder on division by 4 is that instance's id. An
#       instance asking to lead asks for the next such epoch above the
#       highest it knows (fix_common/LeaderEpoch.hpp), so two
#       instances never lead at the same epoch.
#    3. A voter never grants an epoch below the highest it has
#       granted, and says what that is when it refuses. An instance
#       that learns of a higher epoch than its own stops leading, or
#       asking, and asks again above it.
#
#  TOPOLOGY
#  --------
#  Three machines in the arbiter pool: arbiter-primary,
#  arbiter-secondary, witness. The arbiter pool needs two of the three
#  to have an active arbiter.
#
#  Components open connections to BOTH arbiters and send each lease
#  request to both. The active arbiter answers; the passive one stays
#  silent, because a refusal from it would cancel the request the
#  active arbiter is answering under the same id. An arbiter that
#  becomes active grants nothing to any component for one lease
#  period, because it does not know what the previously active
#  arbiter promised. The active arbiter tells the passive one the
#  highest epoch granted in each group with ArbiterStateRecord.
#
#  The witness keeps nothing. It answers the arbiters' lease requests
#  and never becomes leader, follower, active or passive.
#
#  See pubsub_itc_fw_topology.puml and docs/framework/topology.md
#  for the authoritative deployment diagram.
#
#  Role enum
#  Whether an instance leads. Between the arbiters, leader means
#  active. The arbiter value is reserved and not used at runtime.
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
    arbiter                   = 4    # the two arbiters, deciding which of them is active
}

# ------------------------------------------------------------
#  LeaseRefusalReason
#  Why a voter refused a LeaseRequest. It changes nothing about
#  what the asker does -- a refusal is a refusal -- but a log that
#  says why is what lets someone reading it tell a voter that has
#  just restarted from one whose vote is taken.
# ------------------------------------------------------------
enum LeaseRefusalReason : i32 {
    unknown            = 0
    restarting         = 1    # the voter started less than one lease period ago
    promised_elsewhere = 2    # its vote is promised to another instance, or to its own
    epoch_behind       = 3    # it has already granted a higher epoch
    may_not_lead = 4 # it holds a leader's statement that the asker may not lead, because the asker lacks commands the matching engine acted on
}

# ------------------------------------------------------------
#  100 — StatusQuery
#  Sent sequencer to sequencer immediately after TCP connect.
#  Purpose:
#    - Announce identity
#    - Announce current epoch
#    - Trigger peer to reply with StatusResponse
#  It plays no part in deciding which instance leads, which is
#  settled by leases.
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
#    - Communicate responder's epoch and current role, for the log
#    - Tell a restarting sequencer how far the sequence has reached,
#      so that it does not stamp numbers already used
#  Notes:
#    - No sequence number needed because request/response is synchronous
# ------------------------------------------------------------
message StatusResponse (id=101, version=1)
    i64 self_instance_id       # identity of responder
    i64 peer_instance_id       # identity responder believes it is talking to
    i32 epoch                  # responder's current epoch
    Role current_role          # responder's current role; unknown if not yet elected
    i64 next_sequence_number   # responder's current next_sequence_number_; restarting follower uses this to sync its counter after WAL recovery
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
    # True when this execution report repeats one whose subject the member may already have
    # been told about. A matching engine that catches up on the sequencer's record cannot tell
    # a record the dead engine already reported from one no engine ever saw -- an order the
    # sequencer deferred while none was running -- so it reports every one of them and marks
    # them all. The gateway writes the mark as PossResend (tag 97), which is the standard
    # field for application content that may have been sent before under a different sequence
    # number. It rides here rather than in the report itself because tag 97 is a FIX header
    # field and the report PDU is a message body derived from the data dictionary.
    #
    # This is R-0122, and the reason a deferred order reaches its member at all: without the
    # report there is no answer, and without the mark an answer the member already had reads
    # as a second event.
    bool poss_resend
    # The epoch of the leadership that sequenced this record. Two logs that hold a record with
    # the same sequence number and the same epoch hold the same record, which is how a rejoining
    # follower finds the last record its log and its leader's agree on
    # (docs/availability/follower_log_repair.md). Optional, and last, so that records written
    # before it existed still decode: such a record lacks it and is taken to be from epoch zero.
    optional i32 leader_epoch
    # The command's ClOrdID, copied onto the envelope by the gateway so that the sequencer can keep
    # its record of the identifiers in its log without decoding the command itself. Absent on a
    # report, and on a record written before the field existed.
    # See docs/availability/commands_during_a_change_of_leader.md, section 3.4.
    optional string cl_ord_id
    # True on a command a gateway sends again after a change of sequencer leader, because it was
    # still unanswered. The new leader sequences it only if its log does not already hold it.
    optional bool sent_again
    # On an execution report from the matching engine: where it stands in the reports the engine
    # sends. The epoch of the engine leadership that sent it, and its number within that leadership,
    # counted from 1.
    optional i32 report_engine_epoch
    optional i64 report_number
    # On every record a leading sequencer writes: the position up to which it has forwarded every
    # report from the engine to its gateway. A follower uses it, read from its log, to discard the
    # copies of reports it keeps in case it takes the lead (docs/bug_list.md, BUG-0116).
    optional i32 reports_forwarded_through_epoch
    optional i64 reports_forwarded_through_number
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
#  107 -- LogPositionRequest
#  Sent by a sequencer follower to its leader when it connects, when
#  it starts following, and again every second until the two logs
#  agree. It names the follower's last record and that record's
#  epoch. See docs/availability/follower_log_repair.md.
# ------------------------------------------------------------
message LogPositionRequest (id=107, version=1)
    i64 last_seq_no # the follower's last record, or zero for an empty log
    i32 last_epoch # the epoch of the leadership that wrote it, or zero
end

# ------------------------------------------------------------
#  108 -- LogPositionReply
#  The leader's answer. seq_no is the last record, at or below the
#  follower's last, that the leader's log holds with an epoch no later
#  than the follower's last epoch, and epoch is that record's epoch in
#  the leader's log. The follower discards every record after seq_no,
#  and asks again. When agreed is true, the follower's last record is
#  seq_no with that epoch, the two logs agree up to it, and the leader
#  sends every record after it, followed by live records.
#
#  When the leader stops sending live records to its follower, because
#  a peer connection opened or closed or it has just taken the lead, it
#  sends a reply with ask_again set and nothing else: the follower
#  forgets that the logs agree and asks again.
# ------------------------------------------------------------
message LogPositionReply (id=108, version=1)
    i64 seq_no # the record the follower keeps its log through
    i32 epoch # that record's epoch in the leader's log
    bool agreed # true: the logs agree, and the records after seq_no follow
    bool ask_again # true: the leader has stopped sending; ask again, and ignore the other fields
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
    # Why the instance is asking, which only the instance knows. A promotion must move the
    # sequencer's ME order connection to the asker -- the request is what tells the sequencer
    # where to route, because the arbiter's decision does not reach it. A start must not: an
    # instance that is merely becoming current may be about to learn that its peer leads, and
    # taking the routing from a working leader stops the venue matching. See BUG-0077.
    bool asking_to_lead    # true: this catch-up is part of a promotion, not a start
end

# ------------------------------------------------------------
#  116 -- MePositionAck
#  Sent by the sequencer to the matching engine once WAL catch-up
#  streaming is complete.  It carries the sequencer's current WAL
#  head; on receipt the ME considers its book reconciled and
#  transitions to LEADER state.
#
#  It also carries the EARLIEST record the sequencer still holds.
#  The log is truncated as it is consumed, so a replay from zero
#  does not necessarily reach the start of the day. An engine that
#  has lost its own record of what it held falls back to this one,
#  and must be able to tell whether it established what was open or
#  merely got as far back as the log goes -- because resuming on an
#  incomplete answer conceals exactly the loss it is recovering
#  from. See R-0123.
#
#  And it carries HOW MANY records were streamed, so that the engine
#  can establish its catch-up was complete before it acts (R-0101).
#  The count is needed because contiguity cannot be used: the stream
#  is filtered -- only NewOrderSingle and OrderCancelRequest are
#  forwarded -- so the engine receives a subset of the numbers in the
#  range and a gap between them is ordinary. It is the count of what
#  was actually SENT, not of what was walked past: the filtering
#  happens after the walk, and counting the walk would have the engine
#  expect records the sequencer deliberately withheld.
# ------------------------------------------------------------
message MePositionAck (id=116, version=1)
    i64 last_seq_no        # sequencer's current WAL head at catch-up completion
    i64 first_seq_no       # earliest record the sequencer still retains, or 0 if it holds none
    i64 records_sent       # how many records were streamed for this request, after filtering
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
#  behind and the claim is refused. The authority rests with the lease
#  rules, because an instance leads in an epoch only once a majority
#  has granted it; the sequencer never has to ask anyone anything.
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
#  118 -- EnginePositionQuery
#  Sent by a sequencer that has just taken the lead to the leading
#  matching engine, to ask for the highest sequence number of an
#  order or cancel the engine has acted on.
#
#  The new leader's log can hold orders the engine never received:
#  the follower wrote and acknowledged them, and the old leader died
#  before the acknowledgement reached it, so it never sent them on.
#  Nothing else sends them, because an engine asks to catch up only
#  when it starts or is promoted. The answer tells the new leader
#  which orders to send from its log before any new one. See
#  docs/availability/commands_during_a_change_of_leader.md, 3.5.
# ------------------------------------------------------------
message EnginePositionQuery (id=118, version=1)
    i64 request_id         # echoed on the answer, so a late answer to an earlier query is recognised
end

# ------------------------------------------------------------
#  119 -- EnginePosition
#  The matching engine's answer to EnginePositionQuery. Sent only
#  by an engine that is acting on orders: an engine that is still
#  catching up as part of a promotion does not answer, and the
#  sequencer asks again until it does.
# ------------------------------------------------------------
message EnginePosition (id=119, version=1)
    i64 request_id         # the request_id of the query this answers
    i64 highest_applied    # the highest sequence number of an order or cancel the engine has acted on
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
    # The member asked, on its Logon, for both sides to restart at 1 (ResetSeqNumFlag=Y).
    #
    # The sequencer has to be told, because everything it remembers about this session describes
    # a numbering the member has just discarded: both sequence numbers, and the record of which
    # outbound numbers held execution reports. Kept, they would be handed to the next gateway to
    # bind the session as though they still described it.
    #
    # A member is entitled to do this at any logon, and clients make it easy -- the venue's own
    # Java test client offers it, and it is the default in the stock fix8 configuration. So this
    # is the ordinary path, not an edge case.
    bool   reset_seq_nums         # the member asked to restart at 1; forget what is remembered
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
    i32    outbound_seq_num        # next number the venue would send to this member
    # Next number the venue EXPECTS FROM this member: the highest MsgSeqNum it has seen on the
    # session plus one. The counterpart of the field above, and it travels for the same reason --
    # the series belongs to the session, not to the connection, so it has to survive the member
    # moving between gateway instances.
    #
    # It is resumed DIFFERENTLY, and that is the trap of having the two side by side. The
    # outbound number is resumed deliberately HIGH after an unclean death, because too low sends
    # the member a number below what it expects, which FIX treats as fatal. Every term of that
    # reverses here: resuming the inbound number too high makes the VENUE treat an innocent
    # member as having gone backwards, which is also fatal, and this time to a member that has
    # done nothing wrong. So this one is resumed exactly as reported, with no allowance added --
    # the reported figure is already a lower bound, since a member can only have sent more since.
    #
    # See docs/fix/inbound_sequence_checking.md.
    i32    inbound_seq_num         # next number the venue expects FROM this member
    # Which of this session's outbound numbers held an execution report, for the part of the
    # stream this gateway has not already reported on SessionSequenceUpdate. Everything not
    # covered by these -- and by what earlier updates carried -- held something the venue
    # cannot replay, and a resend gap-fills it. See SeqNumRange above.
    list<SeqNumRange> report_seq_nums
    # The log sequence number of the last execution report this gateway delivered to the member, so
    # that the reports produced after it, while the member was away, can be delivered when it binds
    # again (R-0005, docs/bug_list.md BUG-0088).
    optional i64 last_report_delivered
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
    # Next number to expect FROM the member. Handed back exactly as the venue remembers it, with
    # no allowance added -- see the note at SessionUnbound for why this one must not be biased the
    # way the number above it is.
    i32    inbound_seq_num         # next number to expect from the member
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
    # Next number this gateway expects from the member, reported for the same reason as the one
    # above: a gateway that is killed sends no unbind, so a figure that only ever travelled at
    # unbind would be missing in exactly the case it exists for.
    i32    inbound_seq_num         # next number this gateway expects from the member
    # Which outbound numbers have held an execution report since the last update, so the
    # sequencer's record keeps pace with the session rather than arriving only at unbind.
    #
    # Sent here for the same reason the number above is: a gateway that is killed sends no
    # unbind, and a record that only ever travelled at unbind would be empty in exactly the
    # case it exists for. Incremental rather than the whole history, so an update stays small
    # however long the session has been up.
    list<SeqNumRange> report_seq_nums
    # As on SessionUnbound: the log sequence number of the last execution report delivered to the
    # member. Sent periodically, so the sequencer still knows roughly how far the member was served if
    # the gateway dies without unbinding the session.
    optional i64 last_report_delivered
end

# ------------------------------------------------------------
#  128 -- UndeliveredReportsRequest
#  Sent by a gateway to the sequencers once a member's session is
#  established, asking for the execution reports produced for that
#  session while it had no connection. The leading sequencer sends
#  each such report from its log, as an ordinary report, to wherever
#  the session is now bound. R-0005: a report produced while the
#  session was unbound is delivered when it binds again, without the
#  member having to ask. See docs/bug_list.md, BUG-0088.
#
#  Asked for by the gateway, once the session's numbering has been
#  restored, rather than sent by the sequencer when the session binds:
#  the reports travel on a different connection from the reply that
#  restores the numbering, so sent unasked they could reach the
#  gateway first and be numbered wrongly.
# ------------------------------------------------------------
message UndeliveredReportsRequest (id=128, version=1)
    string comp_id
    i16    gateway_protocol_id
    i16    gateway_instance_id
    i32    gateway_session_conn_id
end

# ------------------------------------------------------------
#  127 -- OrderAcceptance
#  Sent by the leading sequencer to every gateway it holds a
#  connection to, saying whether the venue can currently process
#  the orders it is being given.
#
#  It exists because the two facts lived in different processes.
#  The sequencer knows there is no matching engine; the gateway
#  holds the member relationship. With no path between them the
#  sequencer knew for seven minutes that nothing could be
#  processed while the gateway went on acknowledging orders and
#  reporting dropped=0. See BUG-0009 and
#  docs/availability/order_acceptance.md.
#
#  Sent on transition in both directions, repeated while the
#  venue is not accepting, and sent to a gateway when it
#  connects. That last case is the one a transition-only design
#  gets wrong: a gateway starting up during an outage would
#  otherwise assume the venue was fine, which is the default
#  that caused this bug.
#
#  accepting=false does NOT mean orders are being lost. Deferred
#  orders are WAL-committed and recovered on promotion. It means
#  the venue will no longer take on new ones it cannot act upon,
#  because a member holding an acknowledged order it cannot
#  cancel is worse off than one whose order was refused.
#
#  The counts travel so a gateway can say something specific to
#  a member and to a log reader, rather than only "no". They are
#  a snapshot at send time, not a running total the gateway is
#  expected to reconcile.
# ------------------------------------------------------------

message OrderAcceptance (id=127, version=1)
    bool accepting                  # false: the venue will not take new orders it cannot process
    i64  deferred_order_count       # orders accepted and not yet forwarded, at send time
    i32  degraded_for_seconds       # how long the venue has been unable to forward, 0 when accepting
    # The epoch of the leadership that sent this. A gateway that sees a higher epoch than any before
    # knows a new instance leads, and sends it again every command it is still holding unanswered
    # (docs/availability/commands_during_a_change_of_leader.md, section 3.2).
    optional i32 leader_epoch
end

# ------------------------------------------------------------
#  130 -- LeaseRequest
#  Sent by an instance to each of the other two voters, to ask to
#  lead or to renew the lease it leads under.
#
#  An instance leads only while a majority of three voters has
#  granted it a lease that has not run out. For a component pair
#  the voters are the two instances and the active arbiter; for the
#  arbiters they are the two arbiters and the witness. Its own vote
#  is one of the three, so one grant from either other voter is
#  enough. See docs/availability/majority_leases.md.
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
#  A leader of the sequencer pair also says, on every request,
#  whether its peer may lead. It says the peer may not while it has
#  the matching engine act on commands the peer does not hold. A
#  voter that grants the request records the statement if it is
#  newer than the one it holds, and refuses a lease to the instance
#  it names. See docs/availability/a_follower_behind_does_not_lead.md.
#  A candidate's request, and every request in other pairs, carries
#  no statement: statement_leader_id is zero.
# ------------------------------------------------------------
message LeaseRequest (id=130, version=1)
    i64 candidate_instance_id   # the instance asking
    ComponentGroup group        # the pair it belongs to
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
    ComponentGroup group
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
    ComponentGroup group
    i32 highest_epoch           # the highest epoch this voter has granted
    i64 request_id              # echoed from the request
    LeaseRefusalReason reason
end

# ------------------------------------------------------------
#  400 — ArbiterStateRecord
#  Sent by an arbiter to its peer to say the highest epoch granted in
#  one component group: whenever the active arbiter grants a higher
#  one, and for every group when the peer link comes up.
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
#  instance lacking commands the matching engine acted on from
#  leading.
# ------------------------------------------------------------
message ArbiterStateRecord (id=400, version=1)
    i64 component_instance_id    # the instance that leads in the epoch: its remainder on division by 4
    i64 leader_instance_id       # the same
    i32 epoch                    # the highest epoch granted in the group
    ComponentGroup group         # the component pair
    i64 statement_leader_id # the leader that made the newest statement held, or zero for none
    i32 statement_epoch # the epoch of the leadership that made it
    i64 statement_number # its number within that leadership
    bool peer_may_lead # false: the leader's peer may not lead
end

