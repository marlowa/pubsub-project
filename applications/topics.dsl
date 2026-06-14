# ============================================================
#  Topic Pub/Sub Protocol -- PDU Definitions
# ============================================================
#
#  GLOSSARY
#  --------
#  cursor: a sequence number marking how far a subscriber has consumed
#  the event stream. A subscriber presenting cursor N has already
#  received and processed all records with seq_no <= N and wishes to
#  receive records with seq_no > N next. The publisher uses each
#  subscriber's cursor to decide which old WAL segments are safe to
#  delete: no segment may be deleted while it still contains records
#  that any connected subscriber has not yet consumed.
#
#  DESIGN RATIONALE
#  ----------------
#  MEP (Matching Engine Publisher) fans out WAL records to
#  connected C++ subscriber applications. Delivery uses an
#  ack-based pagination protocol: the publisher sends a page
#  of up to TOPIC_PAGE_SIZE records; the subscriber sends
#  TopicAck after processing it; the publisher sends the next
#  page immediately (catch-up) or when new records arrive
#  (live). The page_number / total_pages fields (X of Y)
#  bound the delivery semantics at the protocol level and
#  provide meaningful context for logging.
#
#  All direct topic subscribers are C++ applications because
#  they require low latency. Non-latency-sensitive downstream
#  consumers subscribe to the enterprise bus via TAP.
#
#  FAILOVER
#  --------
#  Passive (non-leader) MEP instances reply with TopicNotLeader
#  to any TopicSubscribeRequest and close the connection.
#  TopicSubscriberChannel (the client-side library component)
#  uses this as an immediate redirect signal to try the other
#  MEP endpoint, making failover transparent to the subscriber
#  application.
#
#  PDU ID range: 107-115 reserved for this protocol.
#
# ============================================================

# ------------------------------------------------------------
#  TopicRecord -- one event payload within a TopicPage.
#  Inner message type; not dispatched as a standalone PDU.
#  id=0 is a sentinel meaning "inner type, no PDU dispatch".
#
#  payload: raw encoded DSL message payload (NOS, OCR, ER, etc.).
#  Variable-length; carries exactly the bytes from the WAL record.
#  Mirrors the WalRecord.payload field design.
# ------------------------------------------------------------
message TopicRecord (id=0, version=1)
    i64         seq_no
    i16         pdu_id
    datetime_ns wall_time_ns
    bytes       payload
end

# ------------------------------------------------------------
#  107 -- TopicSubscribeRequest
#  Sent by a subscriber to the publisher immediately after the
#  TCP connection is established.
#
#  subscriber_id:  label for logging and per-connection cursor
#                  tracking. Not validated against any list;
#                  the topic API is open to any C++ application.
#  topic_name:     e.g. "orders" or "execution_reports".
#  from_seq_no:    starting cursor.
#    0  = start from publisher's oldest retained WAL record.
#   -1  = start from publisher's current WAL head.
#    N  = resume from seq_no N (reconnect after disconnect).
# ------------------------------------------------------------
message TopicSubscribeRequest (id=107, version=1)
    string subscriber_id
    string topic_name
    i64    from_seq_no
end

# ------------------------------------------------------------
#  108 -- TopicSubscribeAck
#  Sent by the publisher to the subscriber after
#  TopicSubscribeRequest. Streaming of TopicPage PDUs begins
#  immediately after this PDU is sent.
#
#  accepted_from_seq_no may differ from the requested cursor
#  if the request predates the publisher's oldest retained
#  record; in that case streaming starts from the oldest
#  available record and a warning is logged.
# ------------------------------------------------------------
message TopicSubscribeAck (id=108, version=1)
    i64 accepted_from_seq_no
end

# ------------------------------------------------------------
#  109 -- TopicPage
#  Sent by the publisher to a subscriber to deliver a batch
#  of records.
#
#  record_count:  number of records in this page
#                 (1..TOPIC_PAGE_SIZE). Equals list length.
#                 The publisher never sends more than
#                 TOPIC_PAGE_SIZE records per page.
#  page_number:   1-based index of this page within the
#                 current delivery cycle (X in "page X of Y").
#                 Fixed for the duration of the cycle.
#  total_pages:   total pages in this delivery cycle
#                 (Y in "page X of Y"). Calculated at the
#                 start of the cycle as
#                 ceil(pending_records / TOPIC_PAGE_SIZE)
#                 and held fixed even if new records arrive
#                 mid-cycle. Both fields equal 1 during live
#                 delivery (subscriber is caught up).
#  records:       the payload. Length equals record_count.
#
#  Flow control:
#    Subscriber sends TopicAck after processing each page.
#    page_number < total_pages: publisher sends next page
#    immediately on receiving the ack (catch-up path).
#    page_number == total_pages: publisher sends next page
#    when new records arrive (live path).
# ------------------------------------------------------------
message TopicPage (id=109, version=1)
    i16               record_count
    i16               page_number
    i16               total_pages
    list<TopicRecord> records
end

# ------------------------------------------------------------
#  110 -- TopicAck
#  Sent by the subscriber after processing a TopicPage.
#  last_seq_no: seq_no of the last record processed in the
#  acknowledged page. The publisher advances this subscriber's
#  cursor to last_seq_no + 1 and considers WAL truncation.
# ------------------------------------------------------------
message TopicAck (id=110, version=1)
    i64 last_seq_no
end

# ------------------------------------------------------------
#  111 -- TopicNotLeader
#  Sent by a passive (non-leader) publisher instance
#  immediately after receiving a TopicSubscribeRequest.
#  Signals to TopicSubscriberChannel to try the other
#  endpoint. The connection is closed by the publisher
#  after sending this PDU.
# ------------------------------------------------------------
message TopicNotLeader (id=111, version=1)
end
