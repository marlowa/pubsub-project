# ============================================================
#  Replicated WAL -- message definitions
# ============================================================
#
#  A pair of instances keeps two copies of one write-ahead log (WAL):
#  the leader numbers and writes each record and sends it to its
#  follower, which writes it and acknowledges it. A record is
#  committed once the follower has acknowledged it. The code that
#  does this is pubsub_itc_fw/ReplicatedWal.hpp; the design is
#  docs/availability/replicated_wal_in_the_library.md, and the repair
#  of a rejoining follower's WAL is docs/availability/follower_log_repair.md.
#
#  These are the library's own messages. A record's bytes are the
#  application's, and the library never reads them.

# ------------------------------------------------------------
#  104 -- WalAck
#  Sent by a follower to its leader to say that it has written the
#  record numbered seq_no to its WAL. The follower acknowledges
#  records in the order it receives them, so an acknowledgement also
#  covers every record before it.
#
#  A component that follows the venue's WAL from outside the pair,
#  the matching engine publisher, sends the same message to the
#  sequencer to say how far it has read.
# ------------------------------------------------------------
message WalAck (id=104, version=1)
    i64 seq_no      # the number of the record written
end

# ------------------------------------------------------------
#  107 -- LogPositionRequest
#  Sent by a follower to its leader when it connects, when it starts
#  following, and again every second until the two WALs agree. It
#  names the follower's last record and that record's epoch.
# ------------------------------------------------------------
message LogPositionRequest (id=107, version=1)
    i64 last_seq_no # the follower's last record, or zero for an empty WAL
    i32 last_epoch # the epoch of the leadership that wrote it, or zero
end

# ------------------------------------------------------------
#  108 -- LogPositionReply
#  The leader's answer. seq_no is the last record, at or below the
#  follower's last, that the leader's WAL holds with an epoch no later
#  than the follower's last epoch, and epoch is that record's epoch in
#  the leader's WAL. The follower discards every record after seq_no,
#  and asks again. When agreed is true, the follower's last record is
#  seq_no with that epoch, the two WALs agree up to it, and the leader
#  sends every record after it, followed by live records.
#
#  When the leader stops sending live records to its follower, because
#  a peer connection opened or closed or it has just taken the lead, it
#  sends a reply with ask_again set and nothing else: the follower
#  forgets that the WALs agree and asks again.
# ------------------------------------------------------------
message LogPositionReply (id=108, version=1)
    i64 seq_no # the record the follower keeps its WAL through
    i32 epoch # that record's epoch in the leader's WAL
    bool agreed # true: the WALs agree, and the records after seq_no follow
    bool ask_again # true: the leader has stopped sending; ask again, and ignore the other fields
end

# ------------------------------------------------------------
#  133 -- ReplicatedRecord
#  One record of the WAL, sent by the leader to its follower: the
#  fields the leader stored with it, and its bytes exactly as the
#  application gave them. The follower writes it under the same
#  number, with the same epoch, time and message type, so that the
#  two WALs hold the same entries.
# ------------------------------------------------------------
message ReplicatedRecord (id=133, version=1)
    i64 seq_no # the record's number
    i32 leader_epoch # the epoch of the leadership that numbered it
    i64 wall_time_ns # when the leader numbered it, in nanoseconds of the wall clock
    i16 pdu_id # the type of the message in bytes
    bytes payload # the record's bytes, exactly as the application gave them
end
