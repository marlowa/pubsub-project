# -----------------------------------------------------------------------------
#  binary_session.dsl
#
#  Session-layer protocol for the binary gateway.
#
#  The binary gateway offers the same venue as the ASCII FIX gateway, but its
#  clients speak the internal PDUs directly instead of FIX. Orders and execution
#  reports therefore need no definitions here -- a client sends the very
#  NewOrderSingle from fix_orders.dsl that the pipeline already carries, and
#  receives the very ExecutionReport. That is the whole point of the gateway:
#  there is nothing to translate.
#
#  What a client cannot get from fix_orders.dsl is a way to say who it is. FIX
#  carries that in its Logon (35=A) and SenderCompID; Logon and LogonAck below
#  are the binary equivalent. The protocol adds RequestRefused, the gateway's
#  answer to a request it refuses itself before the request reaches the
#  matching engine: the counterpart of a FIX session-level Reject. And it adds
#  TradingSessionStatus and TradingSessionStatusRequest, the counterparts of
#  FIX 35=h and 35=g, which tell a member what the venue is doing. And it adds
#  Logout, the counterpart of a FIX Logout (35=5) sent by the venue, which
#  tells a member why its session is ending.
#
#  Sequence:
#
#    Client                        Binary Gateway            Auth Service
#       |                                |                        |
#       |--- Logon --------------------->|  comp_id + password     |
#       |                                |--- SCRAM exchange ----->|
#       |                                |<-- granted or refused --|
#       |<-- LogonAck -------------------|  outcome
#       |                                          |
#       |--- NewOrderSingle ----------------------->|  (from fix_orders.dsl)
#       |<-- ExecutionReport ------------------------|  (from fix_orders.dsl)
#
#  Deliberately absent, and why:
#
#    - No heartbeats or sequence numbers. The framework's PDU transport already
#      detects a dead connection, and every PDU is framed and ordered by it.
#      FIX needs those because it must work over a bare byte stream.
#
#  Authentication is SCRAM-SHA-256, the same exchange the FIX gateway runs against
#  the authentication service. The password in Logon never leaves the gateway
#  process: it is used to derive a proof and then zeroed. An order-entry port that
#  anyone reachable could trade through would not be worth demonstrating, whatever
#  the transport.
#
#  ID range:
#    700-709 reserved for the binary session protocol (700 Logon, 701 LogonAck,
#    702 RequestRefused, 703 TradingSessionStatus, 704 TradingSessionStatusRequest). 100-401 leader/follower, 500-519 authentication
#    service and its admin channel, 600-619 matching engine replication, 1000+
#    the DD-derived order messages.
# -----------------------------------------------------------------------------

# Wire-level framing constants, so a client can frame these PDUs without
# hard-coding the header layout. Emitted by the Java generator as outer-class
# constants for PduChannel, exactly as the admin channel uses them, and must
# match the C++ framework's PduHeader.
framing {
    pdu_header_size = 24
    pdu_canary      = 0xC0FFEE00
    pdu_version     = 1
}

# ---------------------------------------------------------------------------
# LogonOutcome
#
# Why a logon was refused. Rejected is deliberately coarse: a client that gets
# its own identity wrong has a bug, not a condition to recover from.
# ---------------------------------------------------------------------------

enum LogonOutcome : i32 {
    Accepted            = 0
    MissingCompId       = 1
    DuplicateCompId     = 2
    AlreadyLoggedOn     = 3
    AuthenticationFailed = 4   # bad password, unknown user, locked account
    AuthenticationTimeout = 5  # the authentication service did not answer in time
    AuthenticationUnavailable = 6  # no authentication service connected
    WrongTargetCompId    = 7   # the client is talking to a venue it did not mean to reach
    # The credential was good but this session is provisioned against other gateway
    # instances. Its own outcome rather than AuthenticationFailed, which would send a
    # member off rotating a password that was never the problem; the text names the
    # instances it should be using instead.
    NotProvisionedForInstance = 8
    # This gateway instance already serves as many members as it is configured to, so that what its members can have
    # waiting in it stays bounded (docs/availability/bounding_what_a_gateway_has_read.md). The text names the limit; the
    # member logs on to another instance.
    NoRoomForMoreMembers = 9
}

# ---------------------------------------------------------------------------
# Logon (id=700)
#
# First PDU on a new connection. Anything else before it is refused, so that a
# session always has an identity before it can place an order.
# ---------------------------------------------------------------------------

message Logon (id=700, version=1)
    string comp_id         # the client's identity, as SenderCompID is in FIX
    string password        # verified by SCRAM-SHA-256; never travels beyond the gateway
    # The venue the client believes it is reaching. Checked against the gateway's own
    # identity rather than merely recorded: a client that has connected somewhere it did
    # not intend should be told so, not quietly traded. Leave it empty to skip the check.
    string target_comp_id
end

# ---------------------------------------------------------------------------
# LogonAck (id=701)
#
# The gateway's answer. On anything but Accepted the gateway closes the
# connection after sending this, so the outcome is the client's only diagnosis.
# ---------------------------------------------------------------------------

message LogonAck (id=701, version=1)
    LogonOutcome outcome
    optional string text    # human-readable detail for logs; never parsed
end

# ---------------------------------------------------------------------------
# RefusalReason
#
# Why the gateway refused a request before it reached the matching engine. The
# numbers are the ones the FIX gateway gives in SessionRejectReason (373) for the
# same refusal: 1, 5 and 99 are the FIX standard's, and 100 and 101 are the
# venue's own, from the range the standard leaves to the parties
# (docs/venue/gateway_refusals.md).
# ---------------------------------------------------------------------------

enum RefusalReason : i32 {
    RequiredFieldMissing  = 1    # a field the venue requires is absent; field names it
    ValueIncorrect        = 5    # a field holds a value the venue does not accept, or is too long; field names it
    Other                 = 99   # any other reason, such as a ClOrdID used before; text says which
    ThrottleLimitExceeded = 100  # the session is over its limit for this kind of request; text names the limit
    VenueUnavailable      = 101  # the venue cannot take the request now: no sequencer, not taking commands, or too busy
}

# ---------------------------------------------------------------------------
# RequestRefused (id=702)
#
# The gateway's answer to a request it refuses itself, before the request
# reaches the matching engine: the binary counterpart of a FIX session-level
# Reject (35=3). It refuses the request and says nothing of any order: the
# venue never processed the request, so whatever order it named is as the
# member last knew it. A request the matching engine refuses is answered by the
# engine instead, with an OrderCancelReject that reports the order's status.
# ---------------------------------------------------------------------------

message RequestRefused (id=702, version=1)
    string        cl_ord_id      # the ClOrdID of the refused request, as the member sent it
    i32           refused_pdu_id # the message type of the refused request, such as OrderCancelRequest (1001)
    RefusalReason reason
    string        text           # the reason in words, naming the field or the limit
    optional string field        # the field at fault, for RequiredFieldMissing and ValueIncorrect; optional fields come last
end

# ---------------------------------------------------------------------------
# TradingSessionState
#
# The state of the trading session, as a member sees it. The numbers are the
# ones FIX gives in TradSesStatus (340), and both gateways work them out with
# the same function (fix_common/TradingSession.hpp), so the two cannot report
# the same moment differently. Pre-open and open are reported as themselves,
# or as Halted while trading is halted; the start of day, closed, the end of
# day, and the time before the trading day starts are reported as Closed.
# ---------------------------------------------------------------------------

enum TradingSessionState : i32 {
    Unknown = 0   # the gateway has not yet been told what the venue is doing
    Halted  = 1
    Open    = 2
    Closed  = 3
    PreOpen = 4
}

# ---------------------------------------------------------------------------
# TradingSessionStatus (id=703)
#
# What the venue is doing: the counterpart of FIX TradingSessionStatus (35=h).
# The gateway sends it to every logged-on member each time the phase, the
# trading day, or whether trading is halted changes, and to each member as it
# logs on, once the gateway knows. It also answers TradingSessionStatusRequest
# with it. Every member is sent every change: there is no list of which
# members asked (docs/venue/trading_day_plan.md, section 4.2).
# ---------------------------------------------------------------------------

message TradingSessionStatus (id=703, version=1)
    TradingSessionState state
    bool   day_started               # whether the trading day has started; when false, trading_day and phase say nothing
    i32    trading_day               # YYYYMMDD
    i32    phase                     # the venue's phase, numbered as TradingPhase in leader_follower.dsl: 1 start of day, 2 pre-open, 3 open, 4 closed, 5 end of day
    bool   halted                    # whether trading is halted
    bool   unsolicited               # true when sent because something changed or the member logged on; false in answer to a request
    string text                      # what the venue is doing, in words, as the FIX gateway gives it in Text (58)
    optional string request_id       # the request_id of the TradingSessionStatusRequest this answers
end

# ---------------------------------------------------------------------------
# TradingSessionStatusRequest (id=704)
#
# A member asks what the venue is doing: the counterpart of FIX
# TradingSessionStatusRequest (35=g). The gateway answers it itself, from what
# the sequencer last told it, so a member asking is never traffic on the
# sequencer.
# ---------------------------------------------------------------------------

message TradingSessionStatusRequest (id=704, version=1)
    string request_id                # echoed in the answer
end

# ---------------------------------------------------------------------------
# Logout (id=705)
#
# The gateway ends a member's session and says why: the counterpart of a FIX
# Logout (35=5) sent by the venue. The gateway sends it to every logged-on
# member at the end of the trading day, and then shuts down, which closes the
# connection. A member that receives it has nothing to answer
# (docs/venue/start_and_end_of_day.md, section 4.4, decision 13).
# ---------------------------------------------------------------------------

message Logout (id=705, version=1)
    string text                      # why the session ends, in words, such as "The trading day 20261119 has ended"
end
