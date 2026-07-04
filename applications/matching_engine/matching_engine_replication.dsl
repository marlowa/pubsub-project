# ============================================================
#  Matching Engine Book Replication Protocol
# ============================================================
#
#  ME-primary streams BookUpdate PDUs to ME-secondary after
#  each order book change so the secondary maintains a live
#  replica.  On promotion, the secondary reconciles its replica
#  against the sequencer WAL before issuing cancel ERs.
#
#  PDU ID range: 600-619 (ME replication, internal)
# ============================================================

enum BookUpdateType : i8 {
    Add    = 1
    Remove = 2
}

# Sent by ME-primary to ME-secondary after every order book mutation.
# seq_no is the WAL sequence number of the triggering order; the
# secondary tracks this as its last_replicated_seq_no for WAL
# reconciliation during promotion.
#
# For Remove updates, order_id_num/side/ord_type/symbol/order_qty are
# sent as zero/empty -- the secondary uses only session_id+cl_ord_id to
# locate and erase the entry from its replica book.
# price is absent on Remove and on Add when the order has no limit price.
message BookUpdate(id=600)
    i64             seq_no
    i8              update_type
    i32             session_id
    string          cl_ord_id
    i64             order_id_num
    i8              side
    i8              ord_type
    string          symbol
    string          order_qty
    optional string price
end
