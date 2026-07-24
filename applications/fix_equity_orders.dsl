# ---------------------------------------------------------------------------
# GENERATED FILE -- do not edit by hand.
# Produced by python/dd_to_dsl: each message is expanded in full from the FIX
# data dictionary (all fields, DD order, DD required/optional, components
# inlined, repeating groups as nested messages + list<>). Enums are the DD's
# <value> sets; only the PDU ids come from the spec. Regenerate after changes.
# ---------------------------------------------------------------------------

enum PduId : i16 {
    NewOrderSingle = 1000
    OrderCancelRequest = 1001
    ExecutionReport = 1002
    PartySubIDs = 2000  # repeating-group body
    PartyIDs = 2001  # repeating-group body
    Underlyings = 2002  # repeating-group body
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

# PartyIDSource (tag 447)
enum PartyIDSource : char {
    UkNationalInsuranceOrPensionNumber = '6'
    UsSocialSecurityNumber = '7'
    UsEmployerOrTaxIdNumber = '8'
    AustralianBusinessNumber = '9'
    AustralianTaxFileNumber = 'A'
    TaxId = 'J'
    KoreanInvestorId = '1'
    TaiwaneseForeignInvestorId = '2'
    TaiwaneseTradingAcct = '3'
    MalaysianCentralDepository = '4'
    ChineseInvestorId = '5'
    IsitcAcronym = 'I'
    Bic = 'B'
    GeneralIdentifier = 'C'
    Proprietary = 'D'
    IsoCountryCode = 'E'
    SettlementEntityLocation = 'F'
    Mic = 'G'
    CsdParticipant = 'H'
    AustralianCompanyNumber = 'K'
    AustralianRegisteredBodyNumber = 'L'
    CftcReportingFirmIdentifier = 'M'
    LegalEntityIdentifier = 'N'
    InterimIdentifier = 'O'
    ShortCodeIdentifier = 'P'
    NationalIdNaturalPerson = 'Q'
    IndiaPermanentAccountNumber = 'R'
    Fdid = 'S'
    Spsaid = 'T'
    MasterSpsaid = 'U'
}

# PartyRole (tag 452)
enum PartyRole : i32 {
    ExecutingFirm = 1
    BrokerOfCredit = 2
    ClientId = 3
    ClearingFirm = 4
    InvestorId = 5
    IntroducingFirm = 6
    EnteringFirm = 7
    Locate = 8
    FundManagerClientId = 9
    SettlementLocation = 10
    OrderOriginationTrader = 11
    ExecutingTrader = 12
    OrderOriginationFirm = 13
    GiveupClearingFirmDepr = 14
    CorrespondantClearingFirm = 15
    ExecutingSystem = 16
    ContraFirm = 17
    ContraClearingFirm = 18
    SponsoringFirm = 19
    UnderlyingContraFirm = 20
    ClearingOrganization = 21
    Exchange = 22
    CustomerAccount = 24
    CorrespondentClearingOrganization = 25
    CorrespondentBroker = 26
    Buyer = 27
    Custodian = 28
    Intermediary = 29
    Agent = 30
    SubCustodian = 31
    Beneficiary = 32
    InterestedParty = 33
    RegulatoryBody = 34
    LiquidityProvider = 35
    EnteringTrader = 36
    ContraTrader = 37
    PositionAccount = 38
    ContraInvestorId = 39
    TransferToFirm = 40
    ContraPositionAccount = 41
    ContraExchange = 42
    InternalCarryAccount = 43
    OrderEntryOperatorId = 44
    SecondaryAccountNumber = 45
    ForeignFirm = 46
    ThirdPartyAllocationFirm = 47
    ClaimingAccount = 48
    AssetManager = 49
    PledgorAccount = 50
    PledgeeAccount = 51
    LargeTraderReportableAccount = 52
    TraderMnemonic = 53
    SenderLocation = 54
    SessionId = 55
    AcceptableCounterparty = 56
    UnacceptableCounterparty = 57
    EnteringUnit = 58
    ExecutingUnit = 59
    IntroducingBroker = 60
    QuoteOriginator = 61
    ReportOriginator = 62
    SystematicInternaliser = 63
    MultilateralTradingFacility = 64
    RegulatedMarket = 65
    MarketMaker = 66
    InvestmentFirm = 67
    HostCompetentAuthority = 68
    HomeCompetentAuthority = 69
    CompetentAuthorityLiquidity = 70
    CompetentAuthorityTransactionVenue = 71
    ReportingIntermediary = 72
    ExecutionVenue = 73
    MarketDataEntryOriginator = 74
    LocationId = 75
    DeskId = 76
    MarketDataMarket = 77
    AllocationEntity = 78
    PrimeBroker = 79
    StepOutFirm = 80
    BrokerClearingId = 81
    CentralRegistrationDepository = 82
    ClearingAccount = 83
    AcceptableSettlingCounterparty = 84
    UnacceptableSettlingCounterparty = 85
    ClsMemberBank = 86
    InConcertGroup = 87
    InConcertControllingEntity = 88
    LargePositionsReportingAccount = 89
    SettlementFirm = 90
    SettlementAccount = 91
    ReportingMarketCenter = 92
    RelatedReportingMarketCenter = 93
    AwayMarket = 94
    GiveupTradingFirm = 95
    TakeupTradingFirm = 96
    GiveupClearingFirm = 97
    TakeupClearingFirm = 98
    OriginatingMarket = 99
    MarginAccount = 100
    CollateralAssetAccount = 101
    DataRepository = 102
    CalculationAgent = 103
    ExerciseNoticeSender = 104
    ExerciseNoticeReceiver = 105
    RateReferenceBank = 106
    Correspondent = 107
    BeneficiaryBank = 109
    Borrower = 110
    PrimaryObligator = 111
    Guarantor = 112
    ExcludedReferenceEntity = 113
    DeterminingParty = 114
    HedgingParty = 115
    ReportingEntity = 116
    SalesPerson = 117
    Operator = 118
    Csd = 119
    Icsd = 120
    TradingSubAccount = 121
    InvestmentDecisionMaker = 122
    PublishingIntermediary = 123
    CsdParticipant = 124
    Issuer = 125
    ContraCustomerAccount = 126
    ContraInvestmentDecisionMaker = 127
}

# PartySubIDType (tag 803)
enum PartySubIDType : i32 {
    Firm = 1
    Person = 2
    System = 3
    Application = 4
    FullLegalNameOfFirm = 5
    PostalAddress = 6
    PhoneNumber = 7
    EmailAddress = 8
    ContactName = 9
    SecuritiesAccountNumber = 10
    RegistrationNumber = 11
    RegisteredAddressForConfirmation = 12
    RegulatoryStatus = 13
    RegistrationName = 14
    CashAccountNumber = 15
    Bic = 16
    CsdParticipantMemberCode = 17
    RegisteredAddress = 18
    FundAccountName = 19
    TelexNumber = 20
    FaxNumber = 21
    SecuritiesAccountName = 22
    CashAccountName = 23
    Department = 24
    LocationDesk = 25
    PositionAccountType = 26
    SecurityLocateId = 27
    MarketMaker = 28
    EligibleCounterparty = 29
    ProfessionalClient = 30
    Location = 31
    ExecutionVenue = 32
    CurrencyDeliveryIdentifier = 33
    AddressCity = 34
    AddressStateOrProvince = 35
    AddressPostalCode = 36
    AddressStreet = 37
    AddressIsoCountryCode = 38
    IsoCountryCode = 39
    MarketSegment = 40
    CustomerAccountType = 41
    OmnibusAccount = 42
    FundsSegregationType = 43
    GuaranteeFund = 44
    SwapDealer = 45
    MajorParticipant = 46
    FinancialEntity = 47
    UsPerson = 48
    ReportingEntityIndicator = 49
    ElectedClearingRequirementException = 50
    BusinessCenter = 51
    ReferenceText = 52
    ShortMarkingExemptAccount = 53
    ParentFirmIdentifier = 54
    ParentFirmName = 55
    DealIdentifier = 56
    SystemTradeId = 57
    SystemTradeSubId = 58
    FcmCode = 59
    DlvryTrmlCode = 60
    VolntyRptEntity = 61
    RptObligJursdctn = 62
    VolntyRptJursdctn = 63
    CompanyActivities = 64
    EeAreaDomiciled = 65
    ContractLinked = 66
    ContractAbove = 67
    VolntyRptPty = 68
    EndUser = 69
    LocationOrJurisdiction = 70
    DerivativesDealer = 71
    Domicile = 72
    ExemptFromRecognition = 73
    Payer = 74
    Receiver = 75
    SystematicInternaliser = 76
    PublishingEntityIndicator = 77
    FirstName = 78
    Surname = 79
    DateOfBirth = 80
    OrderTransmittingFirm = 81
    OrderTransmittingFirmBuyer = 82
    OrderTransmitterSeller = 83
    LegalEntityIdentifier = 84
    SubSectorClassification = 85
    PartySide = 86
    LegalRegistrationCountry = 87
}

message PartySubIDs (id=PduId.PartySubIDs)  # repeating group 'NoPartySubIDs' (tag 802)
    optional string party_sub_id  # tag 523 (PartySubID)
    optional PartySubIDType party_sub_id_type  # tag 803 (PartySubIDType)
end

message PartyIDs (id=PduId.PartyIDs)  # repeating group 'NoPartyIDs' (tag 453)
    optional string party_id  # tag 448 (PartyID)
    optional PartyIDSource party_id_source  # tag 447 (PartyIDSource)
    optional PartyRole party_role  # tag 452 (PartyRole)
    list<PartySubIDs> no_party_sub_i_ds  # tag 802 (NoPartySubIDs) repeating group
end

message Underlyings (id=PduId.Underlyings)  # repeating group 'NoUnderlyings' (tag 711)
    optional string underlying_symbol  # tag 311 (UnderlyingSymbol)
    optional string underlying_security_id  # tag 309 (UnderlyingSecurityID)
    optional string underlying_qty  # tag 879 (UnderlyingQty)
end

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
    optional string exec_inst  # tag 18 (ExecInst)
    optional string min_qty  # tag 110 (MinQty)
    optional string max_floor  # tag 111 (MaxFloor)
    optional datetime_ns expire_time  # tag 126 (ExpireTime)
    optional string text  # tag 58 (Text)
    list<PartyIDs> no_party_i_ds  # tag 453 (NoPartyIDs) repeating group
    list<Underlyings> no_underlyings  # tag 711 (NoUnderlyings) repeating group
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
    optional string exec_inst  # tag 18 (ExecInst)
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
    list<PartyIDs> no_party_i_ds  # tag 453 (NoPartyIDs) repeating group
end
