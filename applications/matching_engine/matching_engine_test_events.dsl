# ============================================================
#  Matching engine test events -- message definitions
# ============================================================
#
#  The matching engine's own test events, for the venue's ideas the
#  library does not name. They travel on the same connection as the
#  library's test events (pubsub_itc_fw/test_events.dsl), sent by the
#  engine's TestEventChannel, and only when a test started the engine
#  with a [test_events] section. The design is
#  docs/availability/test_events.md.
#
#  The outcome of an order is sent only for the comp ids the section
#  lists in report_orders_of: the orders whose reports reach no member,
#  such as those inject_order hands straight to a sequencer, and those
#  whose check no member can make, such as that an order was applied
#  exactly once. Every other order is checked through the execution
#  reports its member receives.
#
#  Message ids 170 to 199 are set aside for applications' test events;
#  the library's take 140 to 169.
# ============================================================

# ------------------------------------------------------------
#  What the engine did with a new order or a cancel.
# ------------------------------------------------------------
enum OrderOutcomeKind : i8 {
    Accepted = 1               # a new order was accepted and rests on the book
    RefusedDuplicate = 2       # a new order was refused: its session already has a live order with that ClOrdID
    RefusedTooLong = 3         # a new order was refused: its ClOrdID is longer than the book holds
    CancelRefusedTooLong = 4   # a cancel was refused: its ClOrdID or OrigClOrdID is longer than the book holds
    Cancelled = 5              # a cancel was done: the order it names is off the book
    CancelRefusedUnknown = 6   # a cancel was refused: the session holds no live order with that OrigClOrdID
}

# ------------------------------------------------------------
#  170 -- OrderOutcome
#  The engine dealt with a new order or a cancel from a listed comp id.
#  Sent where the engine sends the order's execution report, so it says
#  what the member would have been told, had the report reached one.
# ------------------------------------------------------------
message OrderOutcome (id=170, version=1)
    string           comp_id          # the session's comp id
    string           cl_ord_id        # the ClOrdID of the order or cancel
    string           orig_cl_ord_id   # for a cancel, the ClOrdID of the order it names; empty for a new order
    OrderOutcomeKind outcome          # what the engine did
    bool             during_catch_up  # true when the engine dealt with it while catching up, and marked its report as a possible repeat
    i64              seq_no           # the sequence number the sequencer gave the command
end

# ------------------------------------------------------------
#  Where the engine stands in catching up from the sequencer's
#  WAL, before it may act.
# ------------------------------------------------------------
enum CatchUpStage : i8 {
    Started = 1             # it began catching up: it asked the sequencer for every record after the one it holds
    Incomplete = 2          # the sequencer said it had sent everything, and what arrived does not account for it: the engine asks again
    Completed = 3           # every record the sequencer sent arrived, once each and in order (R-0101)
    CurrentAndWaiting = 4   # caught up at startup, and waiting to be told whether it may lead
}

# ------------------------------------------------------------
#  171 -- CatchUp
#  The engine reached a stage of catching up. Sent whatever the comp
#  ids: a catch-up happens at a start or a change of leader, not for
#  each order.
# ------------------------------------------------------------
message CatchUp (id=171, version=1)
    CatchUpStage stage
    i64          through_seq_no    # Started: the last record the engine holds; otherwise the last record the sequencer sent
    i64          records_applied   # the records the engine applied in this catch-up so far
    i64          records_sent      # what the sequencer said it sent; 0 for Started
    i64          reports_sent      # the reports the engine sent members in this catch-up, each marked as a possible repeat
    i64          book_size         # the orders on the book at this stage
end
