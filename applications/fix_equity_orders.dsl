# -----------------------------------------------------------------------------
#  fix_equity_orders.dsl
#
#  FIX 5.0 SP2 equity order topic registry for pubsub_itc_fw.
#
#  Three pub/sub topics are defined:
#    NewOrderSingle     (FIX MsgType=D)  -- buy-side to sell-side
#    OrderCancelRequest (FIX MsgType=F)  -- buy-side to sell-side
#    ExecutionReport    (FIX MsgType=8)  -- sell-side to buy-side
#
#  Design notes:
#    - Enum values use FIX character literals (e.g. '1', 'A') matching the
#      FIX 5.0 SP2 specification exactly, making future validation against
#      FIX50SP2.xml straightforward.
#    - Prices and quantities are strings to avoid binary/decimal point
#      representation issues. Callers may use fixed-point integers internally
#      but the wire format remains format-agnostic.
#    - TransactTime uses datetime_ns.
#    - Optional fields are used for fields that are conditionally required
#      by the FIX specification (e.g. Price only for limit orders).
#    - Topic IDs start at 1000 to leave the 1-999 range for framework
#      internal PDUs (leader-follower protocol uses 100-201).
# -----------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# Topics enum -- one entry per pub/sub topic.
# The validator requires id=Topics.X in topics mode.
# ---------------------------------------------------------------------------

enum Topics : i16 {
    NewOrderSingle     = 1000
    OrderCancelRequest = 1001
    ExecutionReport    = 1002
}

# ---------------------------------------------------------------------------
# OrdType (tag 40)
# Active FIX 5.0 SP2 values; deprecated on-close and forex variants omitted.
# ---------------------------------------------------------------------------

enum OrdType : char {
    Market                     = '1'
    Limit                      = '2'
    Stop                       = '3'
    StopLimit                  = '4'
    WithOrWithout              = '6'
    LimitOrBetter              = '7'
    LimitWithOrWithout         = '8'
    OnBasis                    = '9'
    PreviouslyQuoted           = 'D'
    PreviouslyIndicated        = 'E'
    ForexSwap                  = 'G'
    Funari                     = 'I'
    MarketIfTouched            = 'J'
    MarketWithLeftOverAsLimit  = 'K'
    PreviousFundValuationPoint = 'L'
    NextFundValuationPoint     = 'M'
    Pegged                     = 'P'
    CounterOrderSelection      = 'Q'
}

# ---------------------------------------------------------------------------
# Side (tag 54)
# ---------------------------------------------------------------------------

enum Side : char {
    Buy              = '1'
    Sell             = '2'
    BuyMinus         = '3'
    SellPlus         = '4'
    SellShort        = '5'
    SellShortExempt  = '6'
    Undisclosed      = '7'
    Cross            = '8'
    CrossShort       = '9'
    CrossShortExempt = 'A'
    AsDefined        = 'B'
    Opposite         = 'C'
    Subscribe        = 'D'
    Redeem           = 'E'
    Lend             = 'F'
    Borrow           = 'G'
}

# ---------------------------------------------------------------------------
# TimeInForce (tag 59)
# ---------------------------------------------------------------------------

enum TimeInForce : char {
    Day                 = '0'
    GoodTillCancel      = '1'
    AtTheOpening        = '2'
    ImmediateOrCancel   = '3'
    FillOrKill          = '4'
    GoodTillCrossing    = '5'
    GoodTillDate        = '6'
    AtTheClose          = '7'
    GoodThroughCrossing = '8'
    AtCrossing          = '9'
}

# ---------------------------------------------------------------------------
# OrdStatus (tag 39)
# ---------------------------------------------------------------------------

enum OrdStatus : char {
    New                = '0'
    PartiallyFilled    = '1'
    Filled             = '2'
    DoneForDay         = '3'
    Canceled           = '4'
    Replaced           = '5'
    PendingCancel      = '6'
    Stopped            = '7'
    Rejected           = '8'
    Suspended          = '9'
    PendingNew         = 'A'
    Calculated         = 'B'
    Expired            = 'C'
    AcceptedForBidding = 'D'
    PendingReplace     = 'E'
}

# ---------------------------------------------------------------------------
# ExecType (tag 150)
# ---------------------------------------------------------------------------

enum ExecType : char {
    New                     = '0'
    DoneForDay              = '3'
    Canceled                = '4'
    Replaced                = '5'
    PendingCancel           = '6'
    Stopped                 = '7'
    Rejected                = '8'
    Suspended               = '9'
    PendingNew              = 'A'
    Calculated              = 'B'
    Expired                 = 'C'
    Restated                = 'D'
    PendingReplace          = 'E'
    Trade                   = 'F'
    TradeCorrect            = 'G'
    TradeCancel             = 'H'
    OrderStatus             = 'I'
    TradeInClearingHold     = 'J'
    TradeReleasedToClearing = 'K'
    TriggeredOrActivated    = 'L'
}

# ---------------------------------------------------------------------------
# ExecInst (tag 18)
# Single-valued use only; multi-instruction cases are out of scope.
# ---------------------------------------------------------------------------

enum ExecInst : char {
    StayOnOfferSide              = '0'
    NotHeld                      = '1'
    Work                         = '2'
    GoAlong                      = '3'
    OverTheDay                   = '4'
    Held                         = '5'
    ParticipateDoNotInitiate     = '6'
    StrictScale                  = '7'
    TryToScale                   = '8'
    StayOnBidSide                = '9'
    NoCross                      = 'A'
    OkToCross                    = 'B'
    CallFirst                    = 'C'
    PercentOfVolume              = 'D'
    DoNotIncrease                = 'E'
    DoNotReduce                  = 'F'
    AllOrNone                    = 'G'
    ReinstateOnSystemFailure     = 'H'
    InstitutionsOnly             = 'I'
    ReinstateOnTradingHalt       = 'J'
    CancelOnTradingHalt          = 'K'
    LastPeg                      = 'L'
    MidPricePeg                  = 'M'
    NonNegotiable                = 'N'
    OpeningPeg                   = 'O'
    MarketPeg                    = 'P'
    CancelOnSystemFailure        = 'Q'
    PrimaryPeg                   = 'R'
    Suspend                      = 'S'
    FixedPegToLocalBestBid       = 'T'
    CustomerDisplayInstruction   = 'U'
    Netting                      = 'V'
    PegToVwap                    = 'W'
    TradeAlong                   = 'X'
    TryToStop                    = 'Y'
    CancelIfNotBest              = 'Z'
}

# ---------------------------------------------------------------------------
# OrdRejReason (tag 103)
# ---------------------------------------------------------------------------

enum OrdRejReason : i32 {
    BrokerOption                    = 0
    UnknownSymbol                   = 1
    ExchangeClosed                  = 2
    OrderExceedsLimit               = 3
    TooLateToEnter                  = 4
    UnknownOrder                    = 5
    DuplicateOrder                  = 6
    DuplicateOfVerballyCommunicated = 7
    StaleOrder                      = 8
    TradeAlongRequired              = 9
    InvalidInvestorId               = 10
    UnsupportedOrderCharacteristic  = 11
    SurveillanceOption              = 12
    IncorrectQuantity               = 13
    IncorrectAllocatedQuantity      = 14
    UnknownAccount                  = 15
    Other                           = 99
}

# ---------------------------------------------------------------------------
# CxlRejReason (tag 102)
# ---------------------------------------------------------------------------

enum CxlRejReason : i32 {
    TooLateToCancel                       = 0
    UnknownOrder                          = 1
    BrokerOption                          = 2
    OrderAlreadyInPendingStatus           = 3
    UnableToProcessOrderMassCancelRequest = 4
    OrigClOrdIdMismatch                   = 5
    DuplicateClOrdId                      = 6
}

# ---------------------------------------------------------------------------
# NewOrderSingle (FIX MsgType=D, tag 35=D)
#
# Mandatory FIX fields:
#   ClOrdID (11), Side (54), Symbol (55), OrdType (40),
#   TransactTime (60), OrderQty (38)
#
# Optional fields (conditionally required per FIX spec):
#   Price (44)         -- required for Limit, StopLimit
#   StopPx (99)        -- required for Stop, StopLimit
#   TimeInForce (59)   -- absence implies Day
#   Account (1)        -- often required by venue
#   ExDestination (100)-- routing destination
#   ExecInst (18)      -- execution instructions (single-valued)
#   MinQty (110)       -- minimum acceptable fill quantity
#   MaxFloor (111)     -- iceberg display/reserve quantity
#   ExpireTime (126)   -- required when TimeInForce=GoodTillDate
#   Text (58)          -- free text
# ---------------------------------------------------------------------------

message NewOrderSingle (id=Topics.NewOrderSingle)
    string cl_ord_id
    Side side
    string symbol
    OrdType ord_type
    datetime_ns transact_time
    string order_qty
    optional string price
    optional string stop_px
    optional TimeInForce time_in_force
    optional string account
    optional string ex_destination
    optional ExecInst exec_inst
    optional string min_qty
    optional string max_floor
    optional datetime_ns expire_time
    optional string text
end

# ---------------------------------------------------------------------------
# OrderCancelRequest (FIX MsgType=F, tag 35=F)
#
# Mandatory FIX fields:
#   OrigClOrdID (41), ClOrdID (11), Side (54), Symbol (55),
#   TransactTime (60), OrderQty (38)
#
# Optional:
#   Account (1), Text (58)
# ---------------------------------------------------------------------------

message OrderCancelRequest (id=Topics.OrderCancelRequest)
    string orig_cl_ord_id
    string cl_ord_id
    Side side
    string symbol
    datetime_ns transact_time
    string order_qty
    optional string account
    optional string text
end

# ---------------------------------------------------------------------------
# ExecutionReport (FIX MsgType=8, tag 35=8)
#
# Mandatory FIX fields:
#   OrderID (37), ExecID (17), ExecType (150), OrdStatus (39),
#   Symbol (55), Side (54), LeavesQty (151), CumQty (14),
#   AvgPx (6), TransactTime (60)
#
# Optional fields (conditionally required or commonly present):
#   ClOrdID (11)       -- echoed from order; absent on unsolicited reports
#   OrigClOrdID (41)   -- echoed on cancel/replace confirms
#   OrdType (40)       -- echoed from order
#   Price (44)         -- echoed for limit orders
#   StopPx (99)        -- echoed for stop orders
#   OrderQty (38)      -- echoed from order
#   TimeInForce (59)   -- echoed from order
#   Account (1)        -- echoed from order
#   ExDestination (100)
#   ExecInst (18)
#   LastQty (32)       -- fill qty on this execution; present on Trade reports
#   LastPx (31)        -- fill price on this execution; present on Trade reports
#   TradeDate (75)     -- date of execution; present on Trade reports
#   ExecRefID (19)     -- present on TradeCorrect and TradeCancel
#   OrdRejReason (103) -- present when OrdStatus=Rejected
#   CxlRejReason (102) -- present when cancel was rejected inline
#   Text (58)
#   MinQty (110)
#   MaxFloor (111)
#   ExpireTime (126)
# ---------------------------------------------------------------------------

message ExecutionReport (id=Topics.ExecutionReport)
    string order_id
    string exec_id
    ExecType exec_type
    OrdStatus ord_status
    string symbol
    Side side
    string leaves_qty
    string cum_qty
    string avg_px
    datetime_ns transact_time
    optional string cl_ord_id
    optional string orig_cl_ord_id
    optional OrdType ord_type
    optional string price
    optional string stop_px
    optional string order_qty
    optional TimeInForce time_in_force
    optional string account
    optional string ex_destination
    optional ExecInst exec_inst
    optional string last_qty
    optional string last_px
    optional datetime_ns trade_date
    optional string exec_ref_id
    optional OrdRejReason ord_rej_reason
    optional CxlRejReason cxl_rej_reason
    optional string text
    optional string min_qty
    optional string max_floor
    optional datetime_ns expire_time
end
