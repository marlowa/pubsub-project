# External documentation

Documents published by other exchanges, and by the FIX standard's maintainers, describing functionality and behaviour
those exchanges document publicly. They are kept here as references for design decisions in this project, so that a
decision that follows what other venues do can point at what they actually publish rather than at anyone's memory of
it.

Nothing here describes this venue. Each document is the work of its publisher, who holds the copyright, and each was
downloaded from the address given below, where the publisher makes it available to the public. A newer issue may have
replaced it there since.

**The documents themselves are not in the repository.** The repository is public, and redistributing another
organisation's documents from it is not ours to do, so `.gitignore` keeps every file in this directory but this README
out of git. A working copy that lacks them can download each from the address in the table.

## The documents

All downloaded on 10 November 2026.

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
| `fix_5_0_sp2_ordercancelreject_b2bits.html` | B2BITS FIXopaedia: the FIX 5.0 SP2 OrderCancelReject message | [b2bits.com](https://www.b2bits.com/fixopaedia/fixdic50-sp2/message_Order_Cancel_Reject_9_.html) |

The London Stock Exchange's MIT202 at docs.londonstockexchange.com, Issues 17.0 and 13.1, and Turquoise's TQ201, refused
the download ("Access Denied"); the copy above is the one London Stock Exchange Group publishes on lseg.com.

## What they are used for

**How an exchange answers a message its gateway refuses**, for the design in
[gateway_refusals.md](../docs/venue/gateway_refusals.md):

- The FIX standard's OrderCancelReject requires an OrderID, and defines "NONE" for it only when the reason is that the
  order is unknown (the two FIX 5.0 SP2 pages; the Cboe page shows a venue following it).
- The London Stock Exchange answers a message over a user's rate limit with a Business Message Reject, and disconnects
  a user that exceeds it repeatedly (MIT201, "message rate"); it answers an Order Cancel Request its gateway rejects for
  a missing field with a Business Message Reject too, and fills in the OrderID on the Order Cancel Reject its trading
  system sends (MIT202).
- Euronext answers a message over the throttling limit with a session-level Reject, and a message rejected for
  technical reasons is not processed by the matching engine, so it carries no Order ID (Optiq client specifications).
- CME answers messages over a session's messaging-control threshold with a Business Message Reject (messaging controls).
- Deutsche Börse's T7 FIX LF interface lists no OrderCancelReject among its messages, and answers a refused request with
  a Business Message Reject (cash message reference).
- Nasdaq's OUCH identifies every order by the member's own token, with no exchange order identifier in the messages
  that cancel it (OUCH 4.2).
