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
#  carries that in its Logon (35=A) and SenderCompID; the two messages below are
#  the binary equivalent, and are all this protocol adds.
#
#  Sequence:
#
#    Client                                  Binary Gateway
#       |                                          |
#       |--- Logon --------------------------------->|  comp_id
#       |<-- LogonAck -------------------------------|  outcome
#       |                                          |
#       |--- NewOrderSingle ----------------------->|  (from fix_orders.dsl)
#       |<-- ExecutionReport ------------------------|  (from fix_orders.dsl)
#
#  Deliberately absent, and why:
#
#    - No password or SCRAM exchange. The comp id establishes session identity
#      for execution-report routing and for the audit trail, which is what the
#      pipeline needs. Authenticating a second transport would re-tread ground
#      the FIX gateway already covers against the authentication service, and
#      teach us nothing new about the framework.
#    - No heartbeats or sequence numbers. The framework's PDU transport already
#      detects a dead connection, and every PDU is framed and ordered by it.
#      FIX needs those because it must work over a bare byte stream.
#
#  ID range:
#    700-709 reserved for the binary session protocol. 100-401 leader/follower,
#    500-519 authentication service and its admin channel, 600-619 matching
#    engine replication, 1000+ the DD-derived order messages.
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
    Accepted        = 0
    MissingCompId   = 1
    DuplicateCompId = 2
    AlreadyLoggedOn = 3
}

# ---------------------------------------------------------------------------
# Logon (id=700)
#
# First PDU on a new connection. Anything else before it is refused, so that a
# session always has an identity before it can place an order.
# ---------------------------------------------------------------------------

message Logon (id=700, version=1)
    string comp_id    # the client's identity, as SenderCompID is in FIX
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
