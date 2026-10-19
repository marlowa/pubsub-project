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
#  Message ids 140 to 199 are set aside for test events: 140 to 169 for
#  the library's, here, and 170 to 199 for applications' own, in DSL
#  files of their own.

# ------------------------------------------------------------
#  What happened to an instance's lease, as the library's
#  PairLeaseAgent decides it.
# ------------------------------------------------------------
enum LeaseChange : i8 {
    AskingToLead = 1    # it asked the other two voters to let it lead
    Granted = 2         # a voter granted it a lease, which with its own vote is a majority: it leads
    RanOut = 3          # its lease ran out without being renewed: it stops leading
    NoLeader = 4        # no voter granted a lease within a lease period of asking: the group has no leader
    ResumingLead = 5    # it restarted having been leading, so it asks to lead again at once
}

# ------------------------------------------------------------
#  Which voter a lease came from.
# ------------------------------------------------------------
enum Voter : i8 {
    NoVoter = 0
    Peer = 1            # the other instance of the pair
    ThirdVoter = 2      # the arbiter, or the witness
}

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
#  The process id lets the test tell a connection that closed
#  because the process ended from one that closed while it ran.
# ------------------------------------------------------------
message TestEventsHello (id=140, version=1)
    string component    # the component's name, as its configuration gives it
    i64    instance     # the component's instance id
    string thread       # the thread that sends the events on this connection
    i64    process_id   # the component's process id
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

# ------------------------------------------------------------
#  144 -- LeaseChanged
#  Something happened to an instance's lease (LeaseChange).
# ------------------------------------------------------------
message LeaseChanged (id=144, version=1)
    string      owner       # the lease agent's owner, as it names itself
    i64         instance    # the instance's id
    LeaseChange change      # what happened
    Voter       granted_by  # for Granted, the voter whose grant made the majority; otherwise NoVoter
    i32         epoch       # the epoch it asked for, leads at, or stopped leading at
end

# ------------------------------------------------------------
#  145 -- LeaseGrantedTo
#  A third voter (an arbiter, or the witness) granted an instance of a
#  pair a lease.
# ------------------------------------------------------------
message LeaseGrantedTo (id=145, version=1)
    i32 pair_id         # the pair, by the number every voter in it is configured with
    i64 instance        # the instance granted the lease
    i32 epoch           # the epoch of the lease
end

# ------------------------------------------------------------
#  146 -- ConnectionChanged
#  One of the thread's connections, other than the one to the test,
#  was made or lost. Only connections made or lost after the
#  connection to the test was made are reported.
# ------------------------------------------------------------
message ConnectionChanged (id=146, version=1)
    string service      # the service, as the framework names it: an outbound service's name, or "inbound:<port>"
    i32    connection   # the framework's id for the connection
    bool   made         # true when it was made, false when it was lost
end

# ------------------------------------------------------------
#  Which side of a replicated pair says its WAL agrees with the
#  other's.
# ------------------------------------------------------------
enum WalSide : i8 {
    Leader = 1          # the leader found the follower's WAL agrees with its own
    Follower = 2        # the follower found its WAL agrees with the leader's
}

# ------------------------------------------------------------
#  147 -- RunningAloneChanged
#  The library's ReplicatedWal began or stopped running as if alone:
#  committing records without waiting for a follower that is too far
#  behind, once a voter confirms that the follower may not lead.
# ------------------------------------------------------------
message RunningAloneChanged (id=147, version=1)
    string owner                # the pair's owner, as the library's configuration names it
    i64    instance             # the instance's id
    bool   running_alone        # true when it began running as if alone, false when it stopped
    string reason               # why, as the ReplicatedWal states it
    i64    acknowledged_through # the last record the follower has acknowledged
    i64    last_record          # the last record in this instance's WAL
end

# ------------------------------------------------------------
#  148 -- WalsAgree
#  The two WALs of a pair were found to agree through a record, after
#  which the leader sends the follower the records after it.
# ------------------------------------------------------------
message WalsAgree (id=148, version=1)
    string  owner       # the pair's owner
    i64     instance    # the instance that says so
    WalSide side        # whether the instance is the leader or the follower
    i64     through     # the last record the two WALs agree on
end

# ------------------------------------------------------------
#  149 -- RecordsDiscarded
#  A follower discarded records from its WAL that its leader does not
#  hold. They were never committed, so nothing acted on them.
# ------------------------------------------------------------
message RecordsDiscarded (id=149, version=1)
    string owner        # the pair's owner
    i64    instance     # the instance that discarded them
    i64    first        # the first record discarded
    i64    last         # the last record discarded
end

# ------------------------------------------------------------
#  150 -- PeerMayLeadChanged
#  A leader changed what it says about whether its follower may lead:
#  that it may not, because it is missing records; or that it may
#  again, because it holds every record.
# ------------------------------------------------------------
message PeerMayLeadChanged (id=150, version=1)
    string owner         # the pair's owner
    i64    instance      # the leader's id
    bool   peer_may_lead # what the leader now says
    i64    through       # when it may lead again, the last record the follower holds; otherwise 0
end

# ------------------------------------------------------------
#  151 -- StatementRecorded
#  An arbiter, starting, found a statement from a pair's leader on
#  disk that an instance may not lead, and will grant it no lease
#  until the leader says otherwise.
# ------------------------------------------------------------
message StatementRecorded (id=151, version=1)
    i32 pair_id                  # the pair
    i64 leader                   # the instance that made the statement
    i32 epoch                    # the epoch it was made at
    i64 instance_may_not_lead    # the instance it says may not lead
end
