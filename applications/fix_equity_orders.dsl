# ---------------------------------------------------------------------------
# GENERATED FILE -- do not edit by hand.
# Produced by python/dd_to_dsl from a FIX data dictionary and a selection spec.
# Field types and enum values come from the FIX DD; the field selection and
# PDU ids come from the spec. Regenerate after changing either.
# ---------------------------------------------------------------------------

enum PduId : i16 {
    NewOrderSingle = 1000
    OrderCancelRequest = 1001
    ExecutionReport = 1002
}

# OrdType (tag 40)
enum OrdType : char {
    Market = '1'
    Limit = '2'
    Stop = '3'
    StopLimit = '4'
    MarketOnClose = '5'
    WithOrWithout = '6'
    LimitOrBetter = '7'
    LimitWithOrWithout = '8'
    OnBasis = '9'
    OnClose = 'A'
    LimitOnClose = 'B'
    ForexMarket = 'C'
    PreviouslyQuoted = 'D'
    PreviouslyIndicated = 'E'
    ForexLimit = 'F'
    ForexSwap = 'G'
    ForexPreviouslyQuoted = 'H'
    Funari = 'I'
    MarketIfTouched = 'J'
    MarketWithLeftOverAsLimit = 'K'
    PreviousFundValuationPoint = 'L'
    NextFundValuationPoint = 'M'
    Pegged = 'P'
    CounterOrderSelection = 'Q'
    StopOnBidOrOffer = 'R'
    StopLimitOnBidOrOffer = 'S'
}

# Side (tag 54)
enum Side : char {
    Buy = '1'
    Sell = '2'
    BuyMinus = '3'
    SellPlus = '4'
    SellShort = '5'
    SellShortExempt = '6'
    Undisclosed = '7'
    Cross = '8'
    CrossShort = '9'
    CrossShortExempt = 'A'
    AsDefined = 'B'
    Opposite = 'C'
    Subscribe = 'D'
    Redeem = 'E'
    Lend = 'F'
    Borrow = 'G'
    SellUndisclosed = 'H'
}

# TimeInForce (tag 59)
enum TimeInForce : char {
    Day = '0'
    GoodTillCancel = '1'
    AtTheOpening = '2'
    ImmediateOrCancel = '3'
    FillOrKill = '4'
    GoodTillCrossing = '5'
    GoodTillDate = '6'
    AtTheClose = '7'
    GoodThroughCrossing = '8'
    AtCrossing = '9'
    GoodForTime = 'A'
    GoodForAuction = 'B'
    GoodForMonth = 'C'
}

# OrdStatus (tag 39)
enum OrdStatus : char {
    New = '0'
    PartiallyFilled = '1'
    Filled = '2'
    DoneForDay = '3'
    Canceled = '4'
    Replaced = '5'
    PendingCancel = '6'
    Stopped = '7'
    Rejected = '8'
    Suspended = '9'
    PendingNew = 'A'
    Calculated = 'B'
    Expired = 'C'
    AcceptedForBidding = 'D'
    PendingReplace = 'E'
}

# ExecType (tag 150)
enum ExecType : char {
    New = '0'
    DoneForDay = '3'
    Canceled = '4'
    Replaced = '5'
    PendingCancel = '6'
    Stopped = '7'
    Rejected = '8'
    Suspended = '9'
    PendingNew = 'A'
    Calculated = 'B'
    Expired = 'C'
    Restated = 'D'
    PendingReplace = 'E'
    Trade = 'F'
    TradeCorrect = 'G'
    TradeCancel = 'H'
    OrderStatus = 'I'
    TradeInAClearingHold = 'J'
    TradeHasBeenReleasedToClearing = 'K'
    TriggeredOrActivatedBySystem = 'L'
    Locked = 'M'
    Released = 'N'
}

# ExecInst (tag 18)
enum ExecInst : char {
    StayOnOfferSide = '0'
    NotHeld = '1'
    Work = '2'
    GoAlong = '3'
    OverTheDay = '4'
    Held = '5'
    ParticipateDoNotInitiate = '6'
    StrictScale = '7'
    TryToScale = '8'
    StayOnBidSide = '9'
    NoCross = 'A'
    OkToCross = 'B'
    CallFirst = 'C'
    PercentOfVolume = 'D'
    DoNotIncrease = 'E'
    DoNotReduce = 'F'
    AllOrNone = 'G'
    ReinstateOnSystemFailure = 'H'
    InstitutionsOnly = 'I'
    ReinstateOnTradingHalt = 'J'
    CancelOnTradingHalt = 'K'
    LastPeg = 'L'
    MidPricePeg = 'M'
    NonNegotiable = 'N'
    OpeningPeg = 'O'
    MarketPeg = 'P'
    CancelOnSystemFailure = 'Q'
    PrimaryPeg = 'R'
    Suspend = 'S'
    FixedPegToLocalBestBid = 'T'
    CustomerDisplayInstruction = 'U'
    Netting = 'V'
    PegToVwap = 'W'
    TradeAlong = 'X'
    TryToStop = 'Y'
    CancelIfNotBest = 'Z'
    TrailingStopPeg = 'a'
    StrictLimit = 'b'
    IgnorePriceValidityChecks = 'c'
    PegToLimitPrice = 'd'
    WorkToTargetStrategy = 'e'
    IntermarketSweep = 'f'
    ExternalRoutingAllowed = 'g'
    ExternalRoutingNotAllowed = 'h'
    ImbalanceOnly = 'i'
    SingleExecutionRequestedForBlockTrade = 'j'
    BestExecution = 'k'
    SuspendOnSystemFailure = 'l'
    SuspendOnTradingHalt = 'm'
    ReinstateOnConnectionLoss = 'n'
    CancelOnConnectionLoss = 'o'
    SuspendOnConnectionLoss = 'p'
    Release = 'q'
    ExecuteAsDeltaNeutral = 'r'
    ExecuteAsDurationNeutral = 's'
    ExecuteAsFxNeutral = 't'
    MinGuaranteedFillEligible = 'u'
    BypassNonDisplayLiquidity = 'v'
    Lock = 'w'
    IgnoreNotionalValueChecks = 'x'
    TrdAtRefPx = 'y'
    AllowFacilitation = 'z'
}

# OrdRejReason (tag 103)
enum OrdRejReason : i32 {
    BrokerCredit = 0
    UnknownSymbol = 1
    ExchangeClosed = 2
    OrderExceedsLimit = 3
    TooLateToEnter = 4
    UnknownOrder = 5
    DuplicateOrder = 6
    DuplicateOfAVerballyCommunicatedOrder = 7
    StaleOrder = 8
    TradeAlongRequired = 9
    InvalidInvestorId = 10
    UnsupportedOrderCharacteristic = 11
    SurveillanceOption = 12
    IncorrectQuantity = 13
    IncorrectAllocatedQuantity = 14
    UnknownAccount = 15
    PriceExceedsCurrentPriceBand = 16
    InvalidPriceIncrement = 18
    ReferencePriceNotAvailable = 19
    NotionalValueExceedsThreshold = 20
    AlgorithmRiskThresholdBreached = 21
    ShortSellNotPermitted = 22
    ShortSellSecurityPreBorrowRestriction = 23
    ShortSellAccountPreBorrowRestriction = 24
    InsufficientCreditLimit = 25
    ExceededClipSizeLimit = 26
    ExceededMaxNotionalOrderAmt = 27
    ExceededDv01Pv01Limit = 28
    ExceededCs01Limit = 29
    Other = 99
}

# CxlRejReason (tag 102)
enum CxlRejReason : i32 {
    TooLateToCancel = 0
    UnknownOrder = 1
    BrokerCredit = 2
    OrderAlreadyInPendingStatus = 3
    UnableToProcessOrderMassCancel = 4
    OrigOrdModTime = 5
    DuplicateClOrdId = 6
    PriceExceedsCurrentPrice = 7
    PriceExceedsCurrentPriceBand = 8
    InvalidPriceIncrement = 18
    Other = 99
}

message NewOrderSingle (id=PduId.NewOrderSingle)
    string cl_ord_id  # tag 11 (ClOrdID)
    Side side  # tag 54 (Side)
    string symbol  # tag 55 (Symbol)
    OrdType ord_type  # tag 40 (OrdType)
    datetime_ns transact_time  # tag 60 (TransactTime)
    string order_qty  # tag 38 (OrderQty)
    optional string security_id  # tag 48 (SecurityID)
    optional string security_id_source  # tag 22 (SecurityIDSource)
    optional string price  # tag 44 (Price)
    optional string stop_px  # tag 99 (StopPx)
    optional TimeInForce time_in_force  # tag 59 (TimeInForce)
    optional string account  # tag 1 (Account)
    optional string ex_destination  # tag 100 (ExDestination)
    optional ExecInst exec_inst  # tag 18 (ExecInst)
    optional string min_qty  # tag 110 (MinQty)
    optional string max_floor  # tag 111 (MaxFloor)
    optional datetime_ns expire_time  # tag 126 (ExpireTime)
    optional string text  # tag 58 (Text)
end

message OrderCancelRequest (id=PduId.OrderCancelRequest)
    string orig_cl_ord_id  # tag 41 (OrigClOrdID)
    string cl_ord_id  # tag 11 (ClOrdID)
    Side side  # tag 54 (Side)
    string symbol  # tag 55 (Symbol)
    datetime_ns transact_time  # tag 60 (TransactTime)
    string order_qty  # tag 38 (OrderQty)
    optional string account  # tag 1 (Account)
    optional string text  # tag 58 (Text)
end

message ExecutionReport (id=PduId.ExecutionReport)
    string order_id  # tag 37 (OrderID)
    string exec_id  # tag 17 (ExecID)
    ExecType exec_type  # tag 150 (ExecType)
    OrdStatus ord_status  # tag 39 (OrdStatus)
    string symbol  # tag 55 (Symbol)
    Side side  # tag 54 (Side)
    string leaves_qty  # tag 151 (LeavesQty)
    string cum_qty  # tag 14 (CumQty)
    string avg_px  # tag 6 (AvgPx)
    datetime_ns transact_time  # tag 60 (TransactTime)
    optional string security_id  # tag 48 (SecurityID)
    optional string security_id_source  # tag 22 (SecurityIDSource)
    optional string cl_ord_id  # tag 11 (ClOrdID)
    optional string orig_cl_ord_id  # tag 41 (OrigClOrdID)
    optional OrdType ord_type  # tag 40 (OrdType)
    optional string price  # tag 44 (Price)
    optional string stop_px  # tag 99 (StopPx)
    optional string order_qty  # tag 38 (OrderQty)
    optional TimeInForce time_in_force  # tag 59 (TimeInForce)
    optional string account  # tag 1 (Account)
    optional string ex_destination  # tag 100 (ExDestination)
    optional ExecInst exec_inst  # tag 18 (ExecInst)
    optional string last_qty  # tag 32 (LastQty)
    optional string last_px  # tag 31 (LastPx)
    optional datetime_ns trade_date  # tag 75 (TradeDate)
    optional string exec_ref_id  # tag 19 (ExecRefID)
    optional OrdRejReason ord_rej_reason  # tag 103 (OrdRejReason)
    optional CxlRejReason cxl_rej_reason  # tag 102 (CxlRejReason)
    optional string text  # tag 58 (Text)
    optional string min_qty  # tag 110 (MinQty)
    optional string max_floor  # tag 111 (MaxFloor)
    optional datetime_ns expire_time  # tag 126 (ExpireTime)
end
