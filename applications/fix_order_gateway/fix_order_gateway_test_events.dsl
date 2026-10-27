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
#  Message ids 190 to 209 are the FIX gateway's, within the range 170 to
#  229 set aside for applications' test events.
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

# ------------------------------------------------------------
#  200 -- SessionProvisioned
#  A member that is provisioned for particular gateway instances logged
#  on to one of them, and the gateway admitted it. A member provisioned
#  for no instance may log on anywhere, and this is not sent for it.
# ------------------------------------------------------------
message SessionProvisioned (id=200, version=1)
    string comp_id
    i16    primary_instance    # the gateway instance the member is provisioned for
    i16    backup_instance     # the second instance it may use; 0 when it has none
    i16    this_instance       # the instance that admitted it
end

# ------------------------------------------------------------
#  Why the gateway refused a logon.
# ------------------------------------------------------------
enum LogonRefusal : i8 {
    AuthenticationFailed = 1    # the authentication service did not grant the logon
    SignatureMismatch = 2       # the authentication service could not prove it is the genuine one
    NotProvisionedHere = 3      # the member is provisioned for other gateway instances
    LimitsOutOfRange = 4        # the member's throttle limits are outside the range the gateway can apply
}

# ------------------------------------------------------------
#  201 -- LogonRefused
#  The gateway refused a member's logon, sent it a Logout, and closed
#  its connection.
# ------------------------------------------------------------
message LogonRefused (id=201, version=1)
    string       comp_id
    i32          connection
    LogonRefusal reason
    i16          primary_instance    # when not provisioned here, the instance the member is provisioned for; otherwise 0
    i16          backup_instance     # when not provisioned here, its second instance; otherwise 0, as when it has none
end

# ------------------------------------------------------------
#  202 -- SessionEstablished
#  The gateway sent a member its Logon reply, and the session is open.
# ------------------------------------------------------------
message SessionEstablished (id=202, version=1)
    string comp_id
    i32    connection
    bool   numbering_confirmed    # false when the sequencer did not report the session's numbering in time, so the gateway opened the session without it
end

# ------------------------------------------------------------
#  203 -- ThrottleApplied
#  The gateway applied a member's limits on commands per second to its
#  session, as the authentication service sent them. 0 means no limit.
# ------------------------------------------------------------
message ThrottleApplied (id=203, version=1)
    string comp_id
    i32    new_orders_per_second
    i32    amends_per_second
    i32    cancels_per_second
end

# ------------------------------------------------------------
#  The kind of command a throttle limits.
# ------------------------------------------------------------
enum ThrottledKind : i8 {
    NewOrder = 1
    Amend = 2
    Cancel = 3
}

# ------------------------------------------------------------
#  204 -- ThrottleStarted
#  The gateway refused a member's command because its limit per second
#  was reached. It is sent for the first refusal of a run, not for each.
# ------------------------------------------------------------
message ThrottleStarted (id=204, version=1)
    string        comp_id
    ThrottledKind kind
    i32           limit_per_second
end

# ------------------------------------------------------------
#  Why a run of refusals ended.
# ------------------------------------------------------------
enum ThrottleEnd : i8 {
    AcceptedAgain = 1   # a command of that kind was accepted again
    SessionEnded = 2    # the member's session ended while it was being refused
}

# ------------------------------------------------------------
#  205 -- ThrottleStopped
#  A run of refusals for one kind of command ended.
# ------------------------------------------------------------
message ThrottleStopped (id=205, version=1)
    string        comp_id
    ThrottledKind kind
    i64           refused     # how many commands of that kind were refused in the run
    ThrottleEnd   ended_by
end

# ------------------------------------------------------------
#  206 -- ProgressReported
#  The gateway logged its progress line, as it does every five seconds,
#  and after a set number of orders, carrying the same figures. It is
#  sent each time the line is logged, not for each order.
# ------------------------------------------------------------
message ProgressReported (id=206, version=1)
    i64 accounted          # orders accounted for: execution reports sent or dropped, and orders refused
    i64 sent               # execution reports sent to members
    i64 dropped            # execution reports that could not be delivered
    i64 orders_received    # new orders taken from members
    i64 awaiting           # orders taken and not yet accounted for
    i64 refused            # new orders refused because the venue was not accepting orders
    i64 refused_cancels    # cancels refused because the venue could not process them
end

# ------------------------------------------------------------
#  207 -- OrderAcceptanceChanged
#  What the leading sequencer says about whether the venue takes
#  orders has changed (OrderAcceptance): whether it is accepting,
#  and whether it is not because trading is halted while no matching
#  engine could be trading within the longest recovery the venue
#  does by itself. Sent on the change, not on each arrival.
# ------------------------------------------------------------
message OrderAcceptanceChanged (id=207, version=1)
    bool accepting              # true: the gateway passes orders on again
    bool halted_for_recovery    # true: refused because the venue is halted while it recovers (RecoveryTooLong)
end
