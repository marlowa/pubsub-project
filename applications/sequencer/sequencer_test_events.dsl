# ============================================================
#  Sequencer test events -- message definitions
# ============================================================
#
#  The sequencer's own test events, for the venue's ideas the library
#  does not name. They travel on the same connection as the library's
#  test events (pubsub_itc_fw/test_events.dsl), sent by the
#  sequencer's TestEventChannel, and only when a test started the
#  sequencer with a [test_events] section. The design is
#  docs/availability/test_events.md.
#
#  None is sent for each order: each says that the sequencer's way of
#  handling orders changed.
#
#  Message ids 180 to 189 and 210 to 219 are the sequencer's, within the
#  range 170 to 229 set aside for applications' test events.
# ============================================================

# ------------------------------------------------------------
#  180 -- HeldOrdersFull
#  The leader's storage for orders waiting for the follower to
#  acknowledge them is full. Later orders wait in the WAL instead, and
#  are sent to the matching engine from there when they are released.
# ------------------------------------------------------------
message HeldOrdersFull (id=180, version=1)
    i64 first_unheld_seq_no    # the first order that waits in the WAL rather than in the storage
end

# ------------------------------------------------------------
#  181 -- EngineServedFromWal
#  Orders that waited in the WAL are released, and the sequencer's
#  reading thread sends them to the matching engine from the WAL.
# ------------------------------------------------------------
message EngineServedFromWal (id=181, version=1)
    i32 engine_connection      # the framework's id for the connection to the matching engine
    i64 after_seq_no           # the reading thread sends every order after this one
end

# ------------------------------------------------------------
#  182 -- EngineHandedOver
#  The reading thread has sent the matching engine every order it
#  lacked, and orders go to it from the sequencer's own thread again.
# ------------------------------------------------------------
message EngineHandedOver (id=182, version=1)
    i32  engine_connection     # the framework's id for the connection to the matching engine
    i64  through_seq_no        # the last order the reading thread sent
    i64  records_sent          # how many it sent
    bool answered_catch_up     # true when the engine had asked to be caught up; false when it was sent orders that waited in the WAL
end

# ------------------------------------------------------------
#  183 -- CheckingLogForRepeats
#  The first command that may already be in the WAL arrived, and the
#  sequencer began reading the end of the WAL to check such commands
#  exactly, before its record of command identifiers is complete.
# ------------------------------------------------------------
message CheckingLogForRepeats (id=183, version=1)
    i64 last_record            # the last record in the WAL when it began
end

# ------------------------------------------------------------
#  184 -- ReportDroppedGatewayNotConnected
#  An execution report could not be sent, because the gateway instance
#  it is for is not connected.
# ------------------------------------------------------------
message ReportDroppedGatewayNotConnected (id=184, version=1)
    i16 gateway_protocol    # 1 for FIX, 2 for binary
    i16 gateway_instance
    i64 seq_no              # the report's sequence number
end

# ------------------------------------------------------------
#  185 -- SessionUnboundRecorded
#  A gateway said a member's session is no longer bound to it. Its
#  reports have nowhere to go until it binds again.
# ------------------------------------------------------------
message SessionUnboundRecorded (id=185, version=1)
    string comp_id
    i16    gateway_protocol
    i16    gateway_instance
    i32    connection       # the gateway's id for the member's connection
end

# ------------------------------------------------------------
#  186 -- UndeliveredReportsSent
#  A member's session bound again, and the sequencer sent it the reports
#  produced while it had no connection.
# ------------------------------------------------------------
message UndeliveredReportsSent (id=186, version=1)
    string comp_id
    i16    gateway_protocol
    i64    count            # the reports sent
end

# ------------------------------------------------------------
#  187 -- ReplayAnswered
#  The leader answered a gateway's request for a session's reports,
#  sent to replay a member's resend.
# ------------------------------------------------------------
message ReplayAnswered (id=187, version=1)
    i64    request_id      # the gateway's identifier for the request
    string comp_id
    i64    records         # the reports sent in answer
    bool   truncated       # true when the answer stopped at the sequencer's cap on records
end

# ------------------------------------------------------------
#  188 -- ReplayDeclined
#  A sequencer that does not lead declined a request for a session's
#  reports: only the leader answers.
# ------------------------------------------------------------
message ReplayDeclined (id=188, version=1)
    i64    request_id
    string comp_id
end

# ------------------------------------------------------------
#  189 -- KeptReportsForwarded
#  The sequencer took the lead, and forwarded the execution reports it
#  had kept while it was not leading, each marked as a possible repeat.
#  It first discarded those the old leader said it had forwarded.
# ------------------------------------------------------------
message KeptReportsForwarded (id=189, version=1)
    i64 discarded_as_forwarded   # kept reports discarded because the old leader said it had forwarded them
    i64 forwarded                # kept reports forwarded
    i64 size_in_bytes            # the size of the reports forwarded
end

# ------------------------------------------------------------
#  210 -- SessionAnnouncedAgain
#  A gateway's link to this sequencer came up, and the gateway said that
#  a member's session established before is still connected to it. The
#  sequencer recorded where the session is connected, so that its reports
#  reach it. A sequencer that has restarted knows of no session until it
#  is told this.
# ------------------------------------------------------------
message SessionAnnouncedAgain (id=210, version=1)
    string comp_id
    i16    gateway_protocol    # 1 for FIX, 2 for binary
    i16    gateway_instance
    i32    connection          # the gateway's id for the member's connection
    bool   leading             # whether this sequencer was leading when it was told
end

# ------------------------------------------------------------
#  211 -- ReportDroppedSessionNotConnected
#  An execution report was dropped because the member's session is
#  connected to no gateway that has told this sequencer so. Sent for the
#  first report dropped for a session since it was last connected, not
#  for every report.
# ------------------------------------------------------------
message ReportDroppedSessionNotConnected (id=211, version=1)
    string comp_id
    i16    gateway_protocol    # 1 for FIX, 2 for binary
    i64    seq_no              # the report's sequence number
end

# ------------------------------------------------------------
#  212 -- ReportsHeld
#  The leading sequencer held a report for a session it does not know
#  of, because some gateway instance has not yet announced its
#  sessions. Sent for the first report held since the sessions were
#  last known, not for every report.
# ------------------------------------------------------------
message ReportsHeld (id=212, version=1)
    string comp_id                  # the session of the first report held
    i16    gateway_protocol         # 1 for FIX, 2 for binary
    i32    gateways_not_announced   # how many configured gateway instances have not announced
end

# ------------------------------------------------------------
#  213 -- HeldReportsReleased
#  Held reports were dealt with: sent, because their session was
#  announced; or dropped, because every gateway instance has announced
#  and their sessions are connected nowhere. Sent once for each session
#  whose reports were sent, and once when the sessions become known.
# ------------------------------------------------------------
message HeldReportsReleased (id=213, version=1)
    string comp_id                  # the session whose reports were sent; empty when the sessions became known
    i16    gateway_protocol         # 1 for FIX, 2 for binary; 0 when the sessions became known
    i64    sent                     # how many held reports were sent
    i64    dropped                  # how many held reports were dropped
end
