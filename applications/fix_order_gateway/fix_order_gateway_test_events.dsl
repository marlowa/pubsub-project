# ============================================================
#  FIX gateway test events -- message definitions
# ============================================================
#
#  The FIX gateway's own test events, for the venue's ideas the library
#  does not name. They travel on the same connection as the library's
#  test events (pubsub_itc_fw/test_events.dsl), sent by the gateway's
#  TestEventChannel, and only when a test started the gateway with a
#  [test_events] section. The design is docs/availability/test_events.md.
#
#  None is sent for each order: each says what happened to a member's
#  session.
#
#  Message ids 190 to 199 are the FIX gateway's, within the range 170 to
#  199 set aside for applications' test events.
# ============================================================

# ------------------------------------------------------------
#  190 -- SessionAnnounced
#  The gateway told the sequencers that a member's session is bound to
#  it, or is no longer.
# ------------------------------------------------------------
message SessionAnnounced (id=190, version=1)
    string comp_id         # the member's comp id
    i32    connection      # the framework's id for the member's connection
    bool   bound           # true when bound to this gateway, false when unbound from it
end

# ------------------------------------------------------------
#  191 -- ReportDroppedNoSession
#  An execution report arrived without the member connection it is
#  for, so the gateway could not deliver it.
# ------------------------------------------------------------
message ReportDroppedNoSession (id=191, version=1)
    string order_id
    string exec_id
end

# ------------------------------------------------------------
#  192 -- SessionDisconnectedWithOrders
#  A member's connection went with orders open. The gateway cancels
#  them at once, when the member logged out or its grace period is
#  zero; or holds them for the grace period, after which it cancels
#  them unless the member reconnects; or, when cancel-on-disconnect is
#  off for the member, leaves them resting.
# ------------------------------------------------------------
message SessionDisconnectedWithOrders (id=192, version=1)
    string comp_id         # the member's comp id; empty if the session never had one
    i32    connection      # the framework's id for the connection that went
    i64    open_orders     # the orders it had open
    bool   cancel_enabled  # whether they are to be cancelled once the grace period ends
    i64    grace_seconds   # the grace period they are held for; 0 when they are cancelled at once or left resting
end

# ------------------------------------------------------------
#  193 -- CancelsSent
#  The gateway finished sending the cancels for the orders of sessions
#  whose grace period ended.
# ------------------------------------------------------------
message CancelsSent (id=193, version=1)
    i64 count              # the cancels sent
end

# ------------------------------------------------------------
#  194 -- SessionReclaimed
#  A member reconnected within the grace period, so the orders held for
#  it are left resting and none is cancelled.
# ------------------------------------------------------------
message SessionReclaimed (id=194, version=1)
    string comp_id
    i64    orders_left_resting
end

# ------------------------------------------------------------
#  195 -- SessionResumed
#  A member's session resumed the numbering the venue remembered for
#  it, with the record of which outbound numbers held reports.
# ------------------------------------------------------------
message SessionResumed (id=195, version=1)
    string comp_id
    i64    outbound            # the outbound number the session resumes at
    i64    inbound             # the inbound number it expects next
    i64    report_numbers      # how many of the outbound numbers before outbound held a report
end

# ------------------------------------------------------------
#  196 -- ResendRequested
#  The gateway read a member's ResendRequest and asked the sequencers
#  for the session's reports.
# ------------------------------------------------------------
message ResendRequested (id=196, version=1)
    string comp_id
    i32    connection
    i64    begin_seq_no        # BeginSeqNo as the member sent it
    i64    end_seq_no          # EndSeqNo as the member sent it
    i64    request_id          # the gateway's identifier for its request to the sequencers
    i64    resume_at           # the outbound number the session resumes at once the resend is done
end

# ------------------------------------------------------------
#  197 -- ResendCompleted
#  The gateway finished a member's resend.
# ------------------------------------------------------------
message ResendCompleted (id=197, version=1)
    i32 connection
    i64 reports_resent
    i64 gap_from               # the first number gap-filled at the end; 0 when nothing was left to fill
    i64 gap_to                 # the number the gap-fill leads to; 0 when nothing was left to fill
end

# ------------------------------------------------------------
#  198 -- ReplayAskedAgain
#  A new sequencer took the lead while a resend waited for its
#  answer, so the gateway asked the new leader.
# ------------------------------------------------------------
message ReplayAskedAgain (id=198, version=1)
    string comp_id
    i32    connection
    i64    request_id          # the new request's identifier
end
