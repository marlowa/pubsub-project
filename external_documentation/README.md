# External documentation

Documents published by other exchanges, and by the FIX standard's maintainers, describing functionality and behaviour
those exchanges document publicly. They are kept here as references for design decisions in this project, so that a
decision that follows what other venues do can point at what they actually publish rather than at anyone's memory of
it.

Nothing here describes this venue. Each document is the work of its publisher, who holds the copyright, and each was
downloaded from the address given below, where the publisher makes it available to the public. A newer issue may have
replaced it there since.

**The documents themselves are not in the repository.** Each is its publisher's copyright, and the repository is
public, so `.gitignore` keeps every file in this directory except this README out of git. A working copy that lacks them
can download each from the address in the table.

## The documents

Downloaded on 10 November 2026, except those in the second table below, downloaded on 14 November 2026.

| File | Publisher, and what it is | Where it came from |
|---|---|---|
| `lse24_mit202_fix_trading_gateway_fix_5_0_issue_01.pdf` | London Stock Exchange Group: MIT202, the FIX Trading Gateway (FIX 5.0) specification for the London Stock Exchange's trading system | [lseg.com](https://www.lseg.com/content/dam/lseg/en_us/documents/londonstockexchange-backup/lse24-mit202-fix-trading-gateway-fix-5-0-issue-01.pdf) |
| `lse_mit201_guide_to_the_trading_system_15_8.pdf` | London Stock Exchange: MIT201, Guide to the Trading System, Issue 15.8 | [docs.londonstockexchange.com](https://docs.londonstockexchange.com/sites/default/files/documents/mit201-guide-to-the-trading-system-15-8-20260119_0.pdf) |
| `euronext_optiq_oeg_fix_5_0_messages_v5_353_0.pdf` | Euronext: Optiq Order Entry Gateway, FIX 5.0 messages, interface specification v5.353.0 | [connect.euronext.com](https://connect.euronext.com/sites/default/files/it-documentation/Optiq%20OEG%20FIX%205.0%20Messages%20-%20Interface%20Specification%20-%20Euronext%20Cash%20and%20Derivatives%20Markets%20-%20External%20-%20v5.353.0.pdf?VersionId=4plX7rv_SZ3QRJGIIcnwJngYIvymJcaP) |
| `euronext_optiq_oeg_client_specifications_fix_5_0_v4_12_0.pdf` | Euronext: Optiq Order Entry Gateway client specifications, FIX 5.0 interface, v4.12.0 | [connect2.euronext.com](https://connect2.euronext.com/sites/default/files/it-documentation/Euronext%20Markets%20-%20Optiq%20OEG%20Client%20Specifications%20-%20FIX%205.0%20Interface%20-%20v4.12.0.pdf) |
| `eurex_t7_r14_0_fix_lf_manual_v1.pdf` | Deutsche Börse Group / Eurex: T7 Release 14.0, FIX LF interface manual | [eurex.com](https://www.eurex.com/resource/blob/4597694/04238a237828a83c88541ca3e847bbae/data/T7_R.14.0_FIX_LF_Manual_Version_1.pdf) |
| `eurex_t7_r14_1_fix_lf_cash_message_reference_v2.pdf` | Deutsche Börse Group: T7 Release 14.1, FIX LF interface, cash market message reference | [eurex.com](https://www.eurex.com/resource/blob/5032548/0da80a8738b68a86fb19c6fca3c72a6f/data/T7_R.14.1_FIX_LF_Cash_Message_Reference_Version_2.pdf) |
| `nasdaq_ouch_4_2.pdf` | Nasdaq: OUCH 4.2, the binary order entry protocol | [nasdaqtrader.com](https://nasdaqtrader.com/content/technicalsupport/specifications/TradingProducts/OUCH4.2.pdf) |
| `cme_messaging_controls.html` | CME Group client systems wiki: messaging controls on iLink sessions | [cmegroupclientsite.atlassian.net](https://cmegroupclientsite.atlassian.net/wiki/spaces/EPICSANDBOX/pages/457317540/Messaging+Controls) |
| `cme_ilink_order_cancel_reject.html` | CME Group client systems wiki: the iLink Order Cancel Reject message | [cmegroupclientsite.atlassian.net](https://cmegroupclientsite.atlassian.net/wiki/spaces/EPICSANDBOX/pages/715423974/iLink+3+Order+Cancel+Reject) |
| `cboe_titanium_us_options_fix_cancel_reject.html` | Cboe: Titanium US Options FIX specification, the Cancel Reject message | [cboe.com](https://www.cboe.com/document/tech-spec/content/technical-specifications/cboe-titanium-u.s.-options-fix-specification/fix-messages/order-protocol---cboe-to-member/cancel-reject) |
| `fix_5_0_sp2_ordercancelreject_fixtrading.html` | FIX Trading Community: the FIX 5.0 SP2 OrderCancelReject message | [fiximate.fixtrading.org](https://fiximate.fixtrading.org/legacy/en/FIX.5.0SP2/body_494857.html) |
| `fix_5_0_sp2_sessionrejectreason_onixs.html` | OnixS FIX dictionary: SessionRejectReason (373), the reason a session-level Reject gives | [onixs.biz](https://onixs.biz/fix-dictionary/latest/tagNum_373.html) |
| `fix_5_0_sp2_ordercancelreject_b2bits.html` | B2BITS FIXopaedia: the FIX 5.0 SP2 OrderCancelReject message | [b2bits.com](https://www.b2bits.com/fixopaedia/fixdic50-sp2/message_Order_Cancel_Reject_9_.html) |

The London Stock Exchange's MIT202 at docs.londonstockexchange.com, Issues 17.0 and 13.1, and Turquoise's TQ201, refused
the download ("Access Denied"); the copy above is the one London Stock Exchange Group publishes on lseg.com.

### How sequenced systems handle time and the trading day

Downloaded on 14 November 2026. The four Aeron files are published under the Apache License 2.0 and could be kept in
the repository, but are kept out of it like the rest, so that this directory has one rule. The Aeron source files are
taken from the `master` branch on that date, so a later copy may differ.

| File | Publisher, and what it is | Where it came from |
|---|---|---|
| `aeron_cluster_tutorial_wiki.html` | Real Logic: the Aeron Cluster tutorial, on the Aeron project's wiki | [github.com](https://github.com/real-logic/aeron/wiki/Cluster-Tutorial) |
| `aeron_cluster_Cluster.java` | Real Logic: Aeron Cluster's `Cluster` interface, with its documentation of `scheduleTimer`, `cancelTimer` and `time` | [github.com](https://raw.githubusercontent.com/aeron-io/aeron/master/aeron-cluster/src/main/java/io/aeron/cluster/service/Cluster.java) |
| `aeron_cluster_ClusteredService.java` | Real Logic: Aeron Cluster's `ClusteredService` interface, with `onTimerEvent` | [github.com](https://raw.githubusercontent.com/aeron-io/aeron/master/aeron-cluster/src/main/java/io/aeron/cluster/service/ClusteredService.java) |
| `aeron_cluster_LogPublisher.java` | Real Logic: Aeron Cluster's `LogPublisher`, which writes timer events and administrative actions into the cluster's log | [github.com](https://raw.githubusercontent.com/aeron-io/aeron/master/aeron-cluster/src/main/java/io/aeron/cluster/LogPublisher.java) |
| `janestreet_how_to_build_an_exchange.html` | Jane Street: "How to Build an Exchange", a talk by Brian Nigito, with its transcript | [janestreet.com](https://www.janestreet.com/tech-talks/building-an-exchange/) |
| `infoq_leveraging_determinism_2022.html` | InfoQ: "Leveraging Determinism", a report of Frank Yu's talk on the Coinbase Derivatives Exchange | [infoq.com](https://www.infoq.com/news/2022/10/leveraging-determinism/) |
| `adaptive_aeron_sequencer_reference_overview.html` | Adaptive: "Aeron Sequencer: A reference overview", the landing page only; the guide itself is given out in return for contact details | [hub.weareadaptive.com](https://hub.weareadaptive.com/aeron-sequencer-a-reference-overview) |
| `adaptive_enabling_24_hour_trading.html` | Adaptive: "Enabling 24 hour trading", the landing page only, for the same reason | [hub.weareadaptive.com](https://hub.weareadaptive.com/enabling-24-hour-trading) |

The Javadoc of `Cluster` for Aeron 1.41.4 at javadoc.io, which says the same as `aeron_cluster_Cluster.java`, refused
the download (HTTP 404).

## What they are used for

**How an exchange answers a message its gateway refuses**, for the design in
[gateway_refusals.md](../docs/venue/gateway_refusals.md):

- The FIX standard's OrderCancelReject requires an OrderID, defines "NONE" for it only when the reason is that the order
  is unknown, and defines OrdStatus as the order's status after the reject, Rejected for an unknown order (the two FIX
  5.0 SP2 pages; the Cboe page shows a venue following it).
- The London Stock Exchange answers a message over a user's rate limit with a Business Message Reject, and disconnects
  a user that exceeds it repeatedly (MIT201, "message rate"); it answers an Order Cancel Request its gateway rejects for
  a missing field with a Business Message Reject too, and fills in the OrderID on the Order Cancel Reject its trading
  system sends (MIT202).
- Euronext answers each kind of request with an acknowledgement, a functional or technical rejection (an
  OrderCancelReject, for a cancel) or a session-level Reject (35=3); the Reject is for a message that is badly formed,
  missing a mandatory field, out of range or over the throttling limit, and carries no Order ID, since the matching
  engine never processed it (Optiq client specifications, sections 4.8 and 4.10). Its own SessionRejectReason values
  for throttling are 25, 26 and 27 (Optiq FIX 5.0 messages). Its OrderCancelReject carries OrdStatus as the order's
  current status (section 5.3.7).
- The FIX standard's SessionRejectReason values are 0 to 18 and 99 (Other), and the field accepts "any value conforming
  to the data type Reserved100Plus", values from 100 that the parties agree (the OnixS page).
- The London Stock Exchange's OrderCancelReject carries OrdStatus as the order's current status, Rejected if the order is
  unknown or the request cannot be processed, and OrderID "NONE" if the order is unknown (MIT202, section 6.4.6); a
  message missing a required tag gets a session-level Reject (MIT202, section 2.9).
- CME answers messages over a session's messaging-control threshold with a Business Message Reject (messaging controls).
- Deutsche Börse's T7 FIX LF interface lists no OrderCancelReject among its messages, and answers a refused request with
  a Business Message Reject (cash message reference).
- Nasdaq's OUCH identifies every order by the member's own token, with no exchange order identifier in the messages
  that cancel it (OUCH 4.2).

**How the trading day moves from one phase to the next**, for the design in
[trading_day_plan.md](../docs/venue/trading_day_plan.md):

- The London Stock Exchange's phases follow a published schedule, the trading cycles in its Business Parameters
  document, and can be overridden by "market control actions invoked by London Stock Exchange that override the normal
  schedule"; halt, suspend and resume are separate, unscheduled sessions. Each instrument has its own cycle, and its
  status is broadcast on the market data feed (MIT201, sections 4.4 and 10.5). Orders the exchange removes in the post
  close session generate "a corresponding cancellation message back to the specific participant" (MIT201, section
  6.8.2).
- Deutsche Börse's T7 sends `TradingSessionStatus` (35=h) on the order entry session "for all session related events",
  with `TradSesStatus` only Unknown, Open or Closed and the trading date as `UTradeDate` (cash message reference,
  section 8.17). Ending the restatement of orders at start of day starts trading for a product, and implies the
  cancellation of every day order with no notice for each (FIX LF manual, sections 3.8.11 and 3.8.16).
- Euronext sends the messages for expired day orders "at the beginning of the last Closed (Closing) phase at the end of
  the trading session" (Optiq client specifications, "Cancellation of Orders at the Close of Business").
- Aeron Cluster gives each message to the service with a timestamp to use "within your application state, so it will be
  consistent under replay" (tutorial). A service schedules a timer only while handling a message, a timer or a session
  opening or closing (`Cluster.java`), and the timer's expiry is written into the cluster's log as a `TimerEvent`
  carrying the leadership term (`LogPublisher.java`), which every member applies at the same position (`onTimerEvent`
  in `ClusteredService.java`). Administrative actions are written into the log in the same way, as
  `ClusterActionRequest` records.
- Jane Street's talk states the same rule for a sequenced exchange: a timer on one machine is not a valid cause of a
  change of state ("You can't just act on the timer on that box"), and every change "has to essentially come from that
  global totally ordered stream of messages", which every consumer, market data included, receives.
