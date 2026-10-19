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
#  Message ids 180 to 189 are the sequencer's, within the range 170 to
#  199 set aside for applications' test events.
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
