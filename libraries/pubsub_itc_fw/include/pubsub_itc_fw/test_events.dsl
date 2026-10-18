# ============================================================
#  Test events -- message definitions
# ============================================================
#
#  A component whose configuration has a [test_events] section opens a
#  connection to the test that started it, and sends these messages on
#  it, so that the high availability tests can check what happened
#  inside the venue without reading the wording of its Quill logs. A
#  component with no such section sends nothing; a venue that members
#  trade on has none. The design is docs/availability/test_events.md;
#  the class that sends them is pubsub_itc_fw/TestEventChannel.hpp.
#
#  Each event is named in the library's terms. Events from one thread
#  arrive in the order that thread sent them, and the test keeps each
#  connection's events apart: there is no order between connections.
#
#  Message ids 140 to 199 are set aside for test events.

# ------------------------------------------------------------
#  The role an instance of a pair has, as the library's
#  ReplicaRole has it.
# ------------------------------------------------------------
enum LeadershipRole : i8 {
    Unknown = 0
    Follower = 1
    Leader = 2
}

# ------------------------------------------------------------
#  140 -- TestEventsHello
#  The first message on a connection: which component, which
#  instance, and which of its threads sends the events that follow.
# ------------------------------------------------------------
message TestEventsHello (id=140, version=1)
    string component    # the component's name, as its configuration gives it
    i64    instance     # the component's instance id
    string thread       # the thread that sends the events on this connection
end

# ------------------------------------------------------------
#  141 -- TestCheckpointRequest
#  Sent by the test. The thread answers with TestCheckpointReached once
#  it has dealt with everything it received before this, so every event
#  it sent before the answer has arrived by the time the answer does.
# ------------------------------------------------------------
message TestCheckpointRequest (id=141, version=1)
    i64 number          # chosen by the test, and returned in the answer
end

# ------------------------------------------------------------
#  142 -- TestCheckpointReached
#  The answer to TestCheckpointRequest.
# ------------------------------------------------------------
message TestCheckpointReached (id=142, version=1)
    i64 number          # the number of the request it answers
end

# ------------------------------------------------------------
#  143 -- RoleChanged
#  An instance of a pair changed its role: it took the lead, stopped
#  leading, or began following. When the connection to the test is
#  made, the owner reports the role it already has as a change from
#  Unknown, so that a test learns the role whenever it connects.
# ------------------------------------------------------------
message RoleChanged (id=143, version=1)
    string         owner      # the pair's owner, as the library's configuration names it
    i64            instance   # the instance's id
    LeadershipRole from_role  # the role it had
    LeadershipRole to_role    # the role it has now
    i32            epoch      # the epoch of the leadership it now has or follows
end
