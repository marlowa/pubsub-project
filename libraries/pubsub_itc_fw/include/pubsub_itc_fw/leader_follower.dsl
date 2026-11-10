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
#  (130-132) carry the protocol; they are the library's own messages,
#  defined in lease_messages.dsl. The rules, and the model checking
#  that shows each is needed, are in docs/availability/majority_leases.md;
#  the code that applies them is pubsub_itc_fw/PairLeaseAgent.hpp.
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
#       highest it knows (pubsub_itc_fw/LeaderEpoch.hpp), so two
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
#  highest epoch granted in each group with ArbiterStateRecord
#  (lease_messages.dsl).
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
#  The venue's pairs. The library's lease messages (lease_messages.dsl)
#  name a pair by a plain number, pair_id, and the venue's number for
#  each pair is its value here (fix_common/LeasePairNumbers.hpp). The
#  arbiters and the witness are configured with the same numbers, as
#  [[voting_pair]], [own_pair] and [voting_pair].
# ------------------------------------------------------------
enum ComponentGroup : i32 {
    unknown                   = 0
    sequencer                 = 1
    matching_engine           = 2
    matching_engine_publisher = 3
    arbiter                   = 4    # the two arbiters, deciding which of them is active
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
    # On an execution report a sequencer writes to its WAL: the sequence number of the command the report answers.
    # Absent on a report no command produced, such as a cancel of every order on promotion. With it, a program that
    # reads the WAL can pair each command with the reports the engine sent for it
    # (docs/availability/deterministic_matching_engine.md, section 7.2).
    optional i64 answers_seq_no
    # On a new order or a cancel the leading sequencer writes to its WAL: the trading date, as the number yyyymmdd, the
    # UTC date of the first record in the day's WAL. The matching engine derives the keys its identifiers are made with
    # from it, so every engine and every replay of the WAL makes the same identifiers, even on a day that runs past
    # midnight (docs/availability/deterministic_matching_engine.md, section 3.3).
    optional i32 trading_date
    # On a command from the FIX gateway: the MsgSeqNum (tag 34) of the member's FIX message that carried it. If the
    # sequencer refuses the command, it returns the number on CommandRefused, so that the gateway can answer with a
    # session-level Reject naming the refused message by it, as FIX requires (docs/venue/gateway_refusals.md). Absent
    # on a command from the binary gateway, whose protocol has no sequence numbers, and on every other record.
    optional i64 member_msg_seq_num
end

# ------------------------------------------------------------
#  104, 107 and 108 -- WalAck, LogPositionRequest and LogPositionReply
#  These are the library's own messages, in replication_messages.dsl,
#  which the replicated WAL uses between the two instances of a pair.
# ------------------------------------------------------------

# ------------------------------------------------------------
#  105, 106, 115 and 116 are not used, and are not to be given to
#  another message. A matching engine publisher and a matching
#  engine follow the sequencer's WAL with the library's messages,
#  WalFollowRequest, WalFollowCaughtUp, WalFollowDeclined and
#  WalLeaderChanged (replication_messages.dsl, 134-137).
# ------------------------------------------------------------

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
    # The session was established before, and is still connected here; this is not a logon.
    #
    # Sent for every session a gateway holds when one of its links to a sequencer comes up. A
    # sequencer keeps where each session is connected only in memory, so one that has restarted
    # knows of no session until it is told, and drops every report for a session it does not
    # know of. A link can also come up again without the sequencer having restarted.
    #
    # The sequencer records where the session is connected and does nothing else: it sends no
    # SessionBoundAck, since the session's numbering is settled already, and it does not read
    # the binding as the previous connection having died, which would raise the number the
    # session resumes from. See docs/availability/session_binding.md.
    bool   still_connected        # an established session, announced again; not a logon
    # The member's instruction for its resting orders when its connection ends, as the authentication service gave it to
    # the gateway at this logon: whether they are to be cancelled, and how many seconds to wait first. Each is absent when
    # the member said nothing, which is not the same as saying no: the venue's default then applies. The sequencer keeps
    # them with the session, so that a gateway taking the session on is handed them back on SessionBoundAck, and so that
    # the venue can carry the instruction out when the gateway cannot. See
    # docs/availability/cancel_on_disconnect_where_the_orders_are.md.
    optional bool cancel_on_disconnect_enabled
    optional i32  cancel_on_disconnect_grace_period_seconds
    # What the gateway binding the session applies to a member that has not said: its own [cancel_on_disconnect]
    # settings. The venue's default is configured for each gateway instance, so the sequencer, carrying the instruction
    # out where the gateway cannot, applies the default of the gateway that last bound the session.
    bool          default_cancel_on_disconnect_enabled
    i32           default_cancel_on_disconnect_grace_period_seconds
end

# ------------------------------------------------------------
#  129 -- SessionsAnnounced
#  Sent by a gateway down a link to a sequencer that has just come
#  up, straight after the SessionBound it sends for each session it
#  holds, and sent even when it holds none.
#
#  It tells the sequencer that this gateway instance has now told it
#  of every session it holds. A sequencer keeps where each session is
#  connected only in memory, so one that has restarted knows of none
#  until the gateways tell it. Until every configured gateway instance
#  has sent this, a leading sequencer holds a report for a session it
#  does not know of rather than dropping it, because the session may
#  be connected after all. See
#  docs/availability/reports_held_until_sessions_are_announced.md.
# ------------------------------------------------------------
message SessionsAnnounced (id=129, version=1)
    i16    gateway_protocol_id    # which order-entry protocol: see GatewayIds.hpp
    i16    gateway_instance_id    # which instance of that protocol
    i32    sessions_announced     # how many sessions it announced
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
    # The member logged out cleanly, rather than its connection dropping. A member that logs out has said it is going, so
    # its resting orders are cancelled at once if it asked for cancel on disconnect, with no grace period; one whose
    # connection dropped is given the grace period to come back.
    bool   logged_out
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
    # The member's instruction for its resting orders when its connection ends, as the sequencer holds it: the latest
    # each part was stated on a SessionBound for this session. Each is absent when the member has never said, which is
    # not the same as saying no.
    optional bool cancel_on_disconnect_enabled
    optional i32  cancel_on_disconnect_grace_period_seconds
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
#
#  declined = true means the sequencer that sent it does not lead,
#  so it has sent no records and read nothing from its WAL. The
#  gateway sends every request to both sequencers, because it does
#  not know which one leads, and only the leader answers: a
#  follower's WAL may not yet hold the newest reports, so counting
#  back through it from the newest would find the wrong ones, and
#  two answers placed on the same numbers as they arrive would mix
#  (BUG-0148). A gateway that has only been declined asks again
#  when a new leader announces itself.
# ------------------------------------------------------------

message SessionReplayComplete (id=125, version=1)
    i64    request_id
    i32    record_count            # records sent in this reply
    i64    last_seq_no             # WAL sequence of the last record sent, or from_seq_no if none
    bool   truncated               # more records remain beyond last_seq_no
    bool   declined                # the sender does not lead, and has sent no records
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
#  accepting=false does NOT mean orders are being lost. The
#  orders held while no matching engine leads are in the WAL, and
#  are released to the engine that next leads. It means the venue
#  will no longer take on new ones it cannot act upon, because a
#  member holding an order the venue has not answered is worse off
#  than one whose order was refused
#  (docs/availability/voided_commands.md, section 5.2).
#
#  The counts travel so a gateway can say something specific to
#  a member and to a log reader, rather than only "no". They are
#  a snapshot at send time, not a running total the gateway is
#  expected to reconcile.
# ------------------------------------------------------------

# Why the venue is not accepting orders, sent with OrderAcceptance so that a gateway can tell the member.
enum OrderAcceptanceReason : i8 {
    Accepting = 0          # the venue is accepting orders
    NoMatchingEngine = 1   # no matching engine has led for longer than orders are held
    RecoveryTooLong = 2    # no matching engine could be trading within the longest recovery the venue does by itself; trading is halted
    Halted = 3             # trading is halted: new orders are refused, and cancels are passed on, because the engine carries them out while halted
}

message OrderAcceptance (id=127, version=1)
    bool accepting                  # false: the venue will not take new orders it cannot process
    i64  held_order_count           # orders and cancels held since no matching engine led, at send time; 0 while one leads
    i32  no_engine_for_seconds      # how long no matching engine has led, 0 while one leads
    OrderAcceptanceReason reason    # why the venue is not accepting, or Accepting when it is
    # The epoch of the leadership that sent this. A gateway that sees a higher epoch than any before
    # knows a new instance leads, and sends it again every command it is still holding unanswered
    # (docs/availability/commands_during_a_change_of_leader.md, section 3.2).
    optional i32 leader_epoch
end

# ============================================================
#  Decisions as commands in the WAL
#
#  A decision that changes the matching engine's book is a
#  command in the WAL, written by the leading sequencer, so that
#  every engine applies it at the same position, including one
#  that catches up later. See
#  docs/availability/deterministic_matching_engine.md, section 4.
# ============================================================

# Why a cancel of the book was written.
enum CancelBookReason : i8 {
    RecoveryTooLong = 3   # no matching engine could be trading within the longest recovery the venue does by itself (R-0117)
}

# ------------------------------------------------------------
#  139 -- CancelBook
#  A command in the WAL, carried as the payload of a WalRecord
#  envelope like a new order or a cancel: cancel every order on
#  the book placed at or before orders_placed_through, in the
#  order they were placed, reporting each to its member. Orders
#  placed after it stay on the book. The cancel written when no
#  engine could be trading within the longest recovery the venue
#  does by itself carries the largest i64, so that it cancels every
#  order on the book, and is followed by a TechnicalEvent that halts
#  trading (docs/availability/voided_commands.md, section 5.3).
# ------------------------------------------------------------
message CancelBook (id=139, version=1)
    CancelBookReason reason
    i64 orders_placed_through
end

# ------------------------------------------------------------
#  165 -- CancelSessionOrders
#  A command in the WAL, carried as the payload of a WalRecord
#  envelope like a new order or a cancel: cancel every order the
#  session has resting, except a GoodTillCancel or GoodTillDate
#  order, which the member placed to outlive its connection,
#  reporting each to the member. Written by the leading sequencer
#  when the grace period a member asked for after its connection
#  ended runs out without it binding again. See
#  docs/availability/cancel_on_disconnect_where_the_orders_are.md.
# ------------------------------------------------------------
message CancelSessionOrders (id=165, version=1)
    string comp_id                # the session identity, with the protocol below
    i16    gateway_protocol_id
end

# ------------------------------------------------------------
#  166 -- SessionEnded
#  A record in the WAL, carried as the payload of a WalRecord
#  envelope, and not a command: no engine applies it. Written by
#  the leading sequencer when a member's session ends and the
#  member asked for its resting orders to be cancelled then: its
#  gateway reported the session gone, or the gateway itself was
#  lost. The orders are due to be cancelled grace_period_seconds
#  after the envelope's time, unless the session binds again
#  first (SessionEndWithdrawn). Kept in the WAL so that a
#  sequencer that takes the lead finds the cancel still pending,
#  and goes on waiting from the time recorded.
# ------------------------------------------------------------
message SessionEnded (id=166, version=1)
    string comp_id                # the session identity, with the protocol below
    i16    gateway_protocol_id
    i32    grace_period_seconds   # zero after a clean Logout, or when the member asked for no wait
    bool   logged_out             # the member logged out cleanly; false when the connection dropped or the gateway was lost
end

# ------------------------------------------------------------
#  167 -- SessionEndWithdrawn
#  A record in the WAL, and not a command. Written by the leading
#  sequencer when a session whose end was recorded (SessionEnded)
#  binds again before its grace period runs out: its orders are no
#  longer to be cancelled.
# ------------------------------------------------------------
message SessionEndWithdrawn (id=167, version=1)
    string comp_id                # the session identity, with the protocol below
    i16    gateway_protocol_id
end

# The venue's trading phase. See docs/venue/trading_phases.md. Until
# the start and the end of the trading day are built, the venue is
# always open.
enum TradingPhase : i8 {
    StartOfDay = 1   # the venue is preparing and is not trading
    PreOpen = 2      # members may enter orders; nothing is matched yet
    Open = 3         # continuous trading
    Closed = 4       # trading has finished for the day
    EndOfDay = 5     # work that belongs after trading
}

# Why a technical event was written.
enum TechnicalEventReason : i8 {
    CannotAccountForBook = 1   # a matching engine could not say what orders the venue held, so trading halts without cancelling (R-0123)
    LiftedByOperator = 2       # a person lifted a halt (R-0023)
    RecoveryTooLong = 3        # no matching engine could be trading within the longest recovery the venue does by itself, so a person decides when trading resumes
    DeclaredByOperator = 4     # a person halted trading; nothing is cancelled, and a person lifts it
}

# ------------------------------------------------------------
#  153 -- TechnicalEvent
#  A command in the WAL, carried as the payload of a WalRecord
#  envelope like a new order or a cancel. It says the state the
#  venue is now in, not only what changed: the phase, whether
#  trading is halted, and the trading day. A matching engine that
#  applies one halted refuses every new order until one that is
#  not halted, and still carries out cancels. One whose trading
#  day is older than the trading day the engine holds is refused.
#  See docs/venue/trading_phases.md.
# ------------------------------------------------------------
message TechnicalEvent (id=153, version=1)
    TradingPhase phase
    bool halted
    i32 trading_day                 # YYYYMMDD
    TechnicalEventReason reason
end

# ------------------------------------------------------------
#  154 -- HaltRequest
#  Sent by a matching engine that is about to lead, and cannot
#  account for the orders the venue holds, to each sequencer it has
#  an order connection to. The leading sequencer writes a
#  TechnicalEvent that halts trading, with the reason given; a
#  follower ignores it. The engine sends it again every second
#  until it has applied a halt, so a request lost to a change of
#  leader is not lost for good, and a second event finds trading
#  halted already. See R-0123 and
#  docs/availability/deterministic_matching_engine.md, decision 8.
# ------------------------------------------------------------
message HaltRequest (id=154, version=1)
    i64 instance_id                 # the engine asking, for the Quill log
    i32 epoch                       # the epoch it leads in
    TechnicalEventReason reason
end

# Why commands were voided.
enum VoidReason : i8 {
    EngineDiedApplying = 1      # the matching engines died applying the command, and no engine applied it and went on
    HeldWhenTradingHalted = 2   # the order was waiting for a matching engine to lead when trading halted
}

# ------------------------------------------------------------
#  156 -- VoidCommands
#  A command in the WAL, carried as the payload of a WalRecord
#  envelope like a new order or a cancel, and written only by the
#  leading sequencer. The commands from first_seq_no to
#  last_seq_no, both included, all before this record, are not
#  applied: each is answered with a rejection instead. With
#  new_orders_only, only the new orders among them are, and every
#  other command in the range is applied as usual. A command the
#  sequencer wrote itself is never voided.
#
#  An engine reaches the commands before it reaches this record,
#  so it is told of every void beforehand, by VoidedCommands. See
#  docs/availability/voided_commands.md.
# ------------------------------------------------------------
message VoidCommands (id=156, version=1)
    i64 first_seq_no
    i64 last_seq_no
    bool new_orders_only
    VoidReason reason
end

# ------------------------------------------------------------
#  158 -- VoidedCommand
#  One void, as VoidedCommands carries it. Nested only, and never
#  sent on its own. It has an id of its own because no two messages
#  in a file may share one, and SeqNumRange already has 0.
# ------------------------------------------------------------
message VoidedCommand (id=158, version=1)
    i64 void_seq_no      # the position of the VoidCommands record
    i64 first_seq_no
    i64 last_seq_no
    bool new_orders_only
    VoidReason reason
end

# ------------------------------------------------------------
#  157 -- VoidedCommands
#  Sent by the leading sequencer to a matching engine: every void
#  in its WAL, at the start of every catch-up before WalFollowStarted,
#  and to every engine connected to it whenever it writes a new one.
#  An engine learns of each void this way before it reaches the
#  commands the void names. See docs/availability/voided_commands.md,
#  section 3.3.
# ------------------------------------------------------------
message VoidedCommands (id=157, version=1)
    list<VoidedCommand> voids
end

# ------------------------------------------------------------
#  159 -- VoidRequest
#  Sent by a matching engine that finds, on starting, that it died
#  applying a command, and whose peer has not applied it, to each
#  sequencer it has an order connection to. The leading sequencer
#  writes a VoidCommands command voiding it, if that is safe, or
#  answers VoidDeclined; a sequencer that does not lead ignores it.
#  The engine asks again every second until it is sent a
#  VoidedCommands list that voids the command, or VoidDeclined. See
#  docs/availability/voided_commands.md, section 4.
# ------------------------------------------------------------
message VoidRequest (id=159, version=1)
    i64 instance_id                 # the engine asking, for the Quill log, and to tell it from another engine that leads
    i32 epoch                       # its epoch, for the Quill log
    i64 seq_no                      # the command it died applying
end

# Why the leading sequencer did not void a command a matching engine asked it to.
enum VoidDeclinedReason : i8 {
    NotAMembersCommand = 1      # the command is not a member's new order or cancel: one the sequencer wrote itself is never voided
    AnsweredAlready = 2         # the WAL holds a report answering it or a later command, so an engine applied it and went on
    AnotherEngineLeads = 3      # the other engine leads, so it has applied the command, and its reports of it are on their way
    NotInWal = 4                # the WAL does not hold the command
}

# ------------------------------------------------------------
#  160 -- VoidDeclined
#  The leading sequencer's answer to a VoidRequest it will not act
#  on. The engine then applies the command as usual. See
#  docs/availability/voided_commands.md, section 4.3.
# ------------------------------------------------------------
message VoidDeclined (id=160, version=1)
    i64 seq_no                      # the command the request named
    VoidDeclinedReason reason
end

# ------------------------------------------------------------
#  161 -- OperatorHaltRequest
#  Sent by the operator's command-line tool, scripts/operator_halt.py,
#  to each sequencer, over the sequencer's listener for operators,
#  which accepts only TLS with a client certificate signed by the
#  operator's certificate authority. Asks the leading sequencer to
#  halt trading, or to lift a halt. A sequencer that does not lead
#  declines. Every request is answered with OperatorHaltAnswer. See
#  docs/availability/voided_commands.md, section 6.2.
# ------------------------------------------------------------
message OperatorHaltRequest (id=161, version=1)
    bool halt                       # true: halt trading; false: lift the halt
    string operator_name            # who asked, for the Quill log
end

# What the sequencer did with an OperatorHaltRequest.
enum OperatorHaltOutcome : i8 {
    Halted = 1               # a TechnicalEvent that halts trading was written
    Lifted = 2               # a TechnicalEvent that lifts the halt was written
    AlreadyHalted = 3        # trading was halted already, so nothing was written
    NotHalted = 4            # trading was not halted, so there was nothing to lift
    NotLeader = 5            # this sequencer does not lead, so it wrote nothing; the one that leads acts
    NoEngineLeads = 6        # no matching engine leads, so the halt cannot be lifted yet
    ReportsOutstanding = 7   # the WAL does not yet hold a report of every order and cancel written before the halt
    NotWritten = 8           # the TechnicalEvent could not be encoded, so nothing was written and trading is as it was
}

# ------------------------------------------------------------
#  162 -- OperatorHaltAnswer
#  A sequencer's answer to an OperatorHaltRequest: what it wrote, or
#  why it wrote nothing, so that the operator can try again.
# ------------------------------------------------------------
message OperatorHaltAnswer (id=162, version=1)
    OperatorHaltOutcome outcome
    bool halted                     # whether trading is halted, as this sequencer knows it, once the request is dealt with
    i64 technical_event_seq_no      # the position of the TechnicalEvent written, or 0 when none was
    string explanation              # the outcome in words, as the sequencer logged it
end

# Why the leading sequencer refused a member's command without writing it into the WAL.
enum CommandRefusedReason : i8 {
    IdentifierAlreadyUsed = 1   # the WAL already holds a command with the same comp id, protocol and ClOrdID, and this one was not marked as sent again
}

# ------------------------------------------------------------
#  164 -- CommandRefused
#  Sent by the leading sequencer to the gateway a member's command
#  came from, on the connection it came by, which the gateway dialled
#  and so reads whether or not the sequencer's own connection to it,
#  which carries reports, is open yet. Sent when the sequencer
#  refuses the command without writing it into the WAL. Nothing in
#  the WAL, the book or the matching engines changes. The gateway
#  answers the member with a rejection: an execution report for a
#  new order; for a cancel, a session-level Reject (FIX) or a
#  RequestRefused (binary). The message
#  carries the refused command as the gateway sent it, so that the
#  gateway can answer from it whether or not it kept a copy. See
#  docs/availability/one_check_against_sequencing_twice.md, section 4.
# ------------------------------------------------------------
message CommandRefused (id=164, version=1)
    i32 gateway_session_conn_id     # the member's session at the gateway, as the command's envelope carried it
    string sender_comp_id           # the member's comp id, as the command's envelope carried it
    string cl_ord_id                # the command's ClOrdID, as the command's envelope carried it
    i16 command_pdu_id              # what the command is: NewOrderSingle or OrderCancelRequest
    bytes command                   # the command, encoded, exactly as the envelope's payload carried it
    CommandRefusedReason reason
    i64 first_seq_no                # the position in the WAL of the command that used the identifier first, for the Quill log
    optional i64 member_msg_seq_num # the command's member_msg_seq_num, as its envelope carried it; absent when the envelope had none
end
