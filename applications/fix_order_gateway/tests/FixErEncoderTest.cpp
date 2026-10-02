// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <FixErEncoder.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>

#include <gtest/gtest.h>

#include <fix_codec/FixMessageReader.hpp>
#include <fix_codec/fix_dictionary.hpp>
#include <fix_orders.hpp>
#include <pubsub_itc_fw/WallClock.hpp>

// Guards the ExecutionReport encoder after its move onto fix_codec::FixMessageWriter.
// The returned view does not start at buffer[0] (the header is framed backward into
// a reserved prefix), so parsing the view proves both the framing and that the
// caller must use view.data()/view.size(), not the buffer base.

using fix_order_gateway::encode_execution_report;
using fix_order_gateway::encode_order_cancel_reject;
using fix_order_gateway::is_cancel_rejection;
namespace tag = fix_codec::tag;

namespace {

// Nanoseconds since the Unix epoch for a UTC date and time.
//
// Spelled out rather than pasted in as a literal. The first version of these tests carried
// two hand-computed constants and BOTH were six days wrong: they asserted against dates
// nobody had actually calculated, and only failed because the encoder disagreed. A reader
// cannot check 1786381200000000000 by eye, so the test should not ask them to.
//
// days_from_civil is Howard Hinnant's algorithm: days since 1970-01-01 for any Gregorian
// date, using only integer arithmetic, so the whole thing folds at compile time.
constexpr int64_t days_from_civil(int64_t year, int64_t month, int64_t day) {
    year -= month <= 2;
    const int64_t era = (year >= 0 ? year : year - 399) / 400;
    const int64_t year_of_era = year - era * 400;
    const int64_t day_of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const int64_t day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return era * 146097 + day_of_era - 719468;
}

constexpr int64_t utc_nanos(int64_t year, int64_t month, int64_t day, int64_t hour, int64_t minute, int64_t second) {
    const int64_t seconds = (((days_from_civil(year, month, day) * 24 + hour) * 60 + minute) * 60) + second;
    return seconds * 1000000000LL;
}

/** @return 0 = Sunday .. 6 = Saturday. 1970-01-01 was a Thursday. */
constexpr int64_t weekday_from_civil(int64_t year, int64_t month, int64_t day) {
    return (days_from_civil(year, month, day) + 4) % 7;
}

// The premises of the tests below, checked by the compiler rather than asserted in a comment.
// A date that quietly stopped being a Saturday would leave the non-trading-day test passing
// while testing nothing at all.
static_assert(weekday_from_civil(2026, 8, 10) == 1, "the ordinary-expiry test needs a weekday");
static_assert(weekday_from_civil(2026, 8, 15) == 6, "the non-trading-day test needs a Saturday");

TEST(FixErEncoderTest, EncodesAValidExecutionReport) {
    pubsub_itc_fw_app::ExecutionReportView view{};
    view.order_id = "ME-ORD-1";
    view.exec_id = "ME-EXEC-1";
    view.exec_type = static_cast<pubsub_itc_fw_app::ExecType>('0');   // New
    view.ord_status = static_cast<pubsub_itc_fw_app::OrdStatus>('0'); // New
    view.symbol = "AAPL";
    view.side = static_cast<pubsub_itc_fw_app::Side>('1'); // Buy
    view.leaves_qty = "100";
    view.cum_qty = "0";
    view.has_cl_ord_id = true;
    view.cl_ord_id = "ORDER-1";
    view.has_order_qty = true;
    view.order_qty = "100";

    pubsub_itc_fw::ReplayClock clock(1700000000000000000LL);
    char buffer[fix_order_gateway::execution_report_initial_buffer_size];
    const std::string_view wire = encode_execution_report(view, "GATEWAY", "CLIENT", 5, clock, buffer, sizeof(buffer));

    ASSERT_FALSE(wire.empty());
    fix_codec::FixMessageReader reader(wire);
    ASSERT_TRUE(reader.is_valid()) << wire; // BodyLength and Checksum are correct
    EXPECT_EQ(reader.msg_type(), "8");
    EXPECT_EQ(reader.find(tag::ClOrdID).as_string_view(), "ORDER-1");
    EXPECT_EQ(reader.find(tag::OrderID).as_string_view(), "ME-ORD-1");
    EXPECT_EQ(reader.find(tag::ExecID).as_string_view(), "ME-EXEC-1");
    EXPECT_EQ(reader.find(tag::ExecType).as_char(), '0');
    EXPECT_EQ(reader.find(tag::OrdStatus).as_char(), '0');
    EXPECT_EQ(reader.find(tag::Symbol).as_string_view(), "AAPL");
    EXPECT_EQ(reader.find(tag::Side).as_char(), '1');
    EXPECT_EQ(reader.find(tag::OrderQty).as_string_view(), "100");
    EXPECT_EQ(reader.find(tag::LeavesQty).as_string_view(), "100");
}

// Expiry is echoed exactly as the member stated it.
//
// This is worth a test where "we do not alter the symbol" would not be, because adjusting a
// GTD expiry is a real and common venue behaviour -- truncating to a maximum lifetime,
// rounding to session close, resolving a date against a trading calendar. Venues that do it
// are not misbehaving. This one has chosen not to, and a choice between defensible
// alternatives is exactly the kind that erodes without something holding it in place.
//
// The report is where a member confirms which choice it is dealing with, so these pin the hop
// where a well-meaning conversion would be easiest to introduce.

TEST(FixErEncoderTest, EchoesTimeInForceAndExpiryForAGoodTillDateOrder) {
    pubsub_itc_fw_app::ExecutionReportView view{};
    view.order_id = "ME-ORD-1";
    view.exec_id = "ME-EXEC-1";
    view.exec_type = static_cast<pubsub_itc_fw_app::ExecType>('0');
    view.ord_status = static_cast<pubsub_itc_fw_app::OrdStatus>('0');
    view.symbol = "AAPL";
    view.side = static_cast<pubsub_itc_fw_app::Side>('1');
    view.leaves_qty = "100";
    view.cum_qty = "0";
    view.has_time_in_force = true;
    view.time_in_force = pubsub_itc_fw_app::TimeInForce::GoodTillDate;
    view.has_expire_time = true;
    // A Monday: an ordinary expiry with nothing a venue might want to "correct".
    view.expire_time = utc_nanos(2026, 8, 10, 17, 0, 0);

    pubsub_itc_fw::ReplayClock clock(1700000000000000000LL);
    char buffer[fix_order_gateway::execution_report_initial_buffer_size];
    const std::string_view wire = encode_execution_report(view, "GATEWAY", "CLIENT", 5, clock, buffer, sizeof(buffer));

    ASSERT_FALSE(wire.empty());
    fix_codec::FixMessageReader reader(wire);
    ASSERT_TRUE(reader.is_valid()) << wire;
    EXPECT_EQ(reader.find(tag::TimeInForce).as_char(), '6'); // GoodTillDate
    EXPECT_EQ(reader.find(tag::ExpireTime).as_string_view(), "20260810-17:00:00");
}

TEST(FixErEncoderTest, ExpiryOnANonTradingDayIsEmittedUnchanged) {
    // The case that distinguishes this venue's choice from the alternative. A venue that
    // moved this to the next business day would not be behaving unreasonably -- several do --
    // but it would be settling the terms of the order differently from the member, and the
    // member would find out only when the order outlived, or failed to reach, the moment it
    // expected.
    pubsub_itc_fw_app::ExecutionReportView view{};
    view.order_id = "ME-ORD-2";
    view.exec_id = "ME-EXEC-2";
    view.exec_type = static_cast<pubsub_itc_fw_app::ExecType>('0');
    view.ord_status = static_cast<pubsub_itc_fw_app::OrdStatus>('0');
    view.symbol = "AAPL";
    view.side = static_cast<pubsub_itc_fw_app::Side>('1');
    view.leaves_qty = "100";
    view.cum_qty = "0";
    view.has_time_in_force = true;
    view.time_in_force = pubsub_itc_fw_app::TimeInForce::GoodTillDate;
    view.has_expire_time = true;
    // 2026-08-15 12:00:00 UTC -- a Saturday.
    view.expire_time = utc_nanos(2026, 8, 15, 12, 0, 0); // a Saturday -- see the static_assert

    pubsub_itc_fw::ReplayClock clock(1700000000000000000LL);
    char buffer[fix_order_gateway::execution_report_initial_buffer_size];
    const std::string_view wire = encode_execution_report(view, "GATEWAY", "CLIENT", 5, clock, buffer, sizeof(buffer));

    ASSERT_FALSE(wire.empty());
    fix_codec::FixMessageReader reader(wire);
    ASSERT_TRUE(reader.is_valid()) << wire;
    EXPECT_EQ(reader.find(tag::ExpireTime).as_string_view(), "20260815-12:00:00") << "the expiry was moved off the weekend -- the venue must never adjust it";
}

TEST(FixErEncoderTest, AnOrderWithNoExpiryEmitsNoExpiryField) {
    // Absent must stay absent: inventing a default would tell the member its Day order dies
    // at a time it never asked for.
    pubsub_itc_fw_app::ExecutionReportView view{};
    view.order_id = "ME-ORD-3";
    view.exec_id = "ME-EXEC-3";
    view.exec_type = static_cast<pubsub_itc_fw_app::ExecType>('0');
    view.ord_status = static_cast<pubsub_itc_fw_app::OrdStatus>('0');
    view.symbol = "AAPL";
    view.side = static_cast<pubsub_itc_fw_app::Side>('1');
    view.leaves_qty = "100";
    view.cum_qty = "0";

    pubsub_itc_fw::ReplayClock clock(1700000000000000000LL);
    char buffer[fix_order_gateway::execution_report_initial_buffer_size];
    const std::string_view wire = encode_execution_report(view, "GATEWAY", "CLIENT", 5, clock, buffer, sizeof(buffer));

    ASSERT_FALSE(wire.empty());
    fix_codec::FixMessageReader reader(wire);
    ASSERT_TRUE(reader.is_valid()) << wire;
    EXPECT_TRUE(reader.find(tag::ExpireTime).empty());
    EXPECT_TRUE(reader.find(tag::TimeInForce).empty());
}

// A refused request to cancel must reach a FIX member as an OrderCancelReject, never as a
// rejected ExecutionReport, which in FIX says the order itself was rejected (R-0151, BUG-0099).
// The matching engine refuses a cancel for an order it does not hold with a rejected report that
// carries the OrigClOrdID of the order named; these build that report exactly as the engine does.

pubsub_itc_fw_app::ExecutionReportView engine_refusal_of_a_cancel() {
    pubsub_itc_fw_app::ExecutionReportView view{};
    view.order_id = "NONE";
    view.exec_id = "ME-EXEC-7";
    view.exec_type = pubsub_itc_fw_app::ExecType::Rejected;
    view.ord_status = pubsub_itc_fw_app::OrdStatus::Rejected;
    view.symbol = "AAPL";
    view.side = static_cast<pubsub_itc_fw_app::Side>('1');
    view.leaves_qty = "0";
    view.cum_qty = "0";
    view.has_cl_ord_id = true;
    view.cl_ord_id = "CANCEL-1";
    view.has_orig_cl_ord_id = true;
    view.orig_cl_ord_id = "ORDER-1";
    view.has_ord_rej_reason = true;
    view.ord_rej_reason = pubsub_itc_fw_app::OrdRejReason::UnknownOrder;
    return view;
}

TEST(FixErEncoderTest, TheEnginesRefusalOfACancelIsRecognised) {
    EXPECT_TRUE(is_cancel_rejection(engine_refusal_of_a_cancel()));
}

TEST(FixErEncoderTest, ARejectedNewOrderIsNotTakenForARefusedCancel) {
    pubsub_itc_fw_app::ExecutionReportView view = engine_refusal_of_a_cancel();
    view.has_orig_cl_ord_id = false;
    view.orig_cl_ord_id = {};
    EXPECT_FALSE(is_cancel_rejection(view));
}

TEST(FixErEncoderTest, ACancelledOrderIsNotTakenForARefusedCancel) {
    // A successful cancel also carries OrigClOrdID; it is the Rejected ExecType that marks a refusal.
    pubsub_itc_fw_app::ExecutionReportView view = engine_refusal_of_a_cancel();
    view.exec_type = pubsub_itc_fw_app::ExecType::Canceled;
    view.ord_status = pubsub_itc_fw_app::OrdStatus::Canceled;
    view.has_ord_rej_reason = false;
    EXPECT_FALSE(is_cancel_rejection(view));
}

TEST(FixErEncoderTest, TheEnginesRefusalOfACancelIsEncodedAsAnOrderCancelReject) {
    pubsub_itc_fw::ReplayClock clock(1700000000000000000LL);
    char buffer[fix_order_gateway::execution_report_initial_buffer_size];
    const std::string_view wire = encode_order_cancel_reject(engine_refusal_of_a_cancel(), "GATEWAY", "CLIENT", 9, clock, buffer, sizeof(buffer));

    ASSERT_FALSE(wire.empty());
    fix_codec::FixMessageReader reader(wire);
    ASSERT_TRUE(reader.is_valid()) << wire;
    EXPECT_EQ(reader.msg_type(), "9");
    EXPECT_EQ(reader.find(tag::OrderID).as_string_view(), "NONE");
    EXPECT_EQ(reader.find(tag::ClOrdID).as_string_view(), "CANCEL-1");
    EXPECT_EQ(reader.find(tag::OrigClOrdID).as_string_view(), "ORDER-1");
    EXPECT_EQ(reader.find(tag::OrdStatus).as_char(), '8');
    EXPECT_EQ(reader.find(tag::CxlRejResponseTo).as_char(), '1');    // a request to cancel
    EXPECT_EQ(reader.find(tag::CxlRejReason).as_string_view(), "1"); // unknown order
    // Nothing that belongs only to an ExecutionReport.
    EXPECT_TRUE(reader.find(tag::ExecType).empty());
    EXPECT_TRUE(reader.find(tag::ExecID).empty());
    EXPECT_TRUE(reader.find(tag::OrdRejReason).empty());
}

TEST(FixErEncoderTest, ARefusalForAnyOtherReasonIsReportedAsOtherWithTheEnginesText) {
    pubsub_itc_fw_app::ExecutionReportView view = engine_refusal_of_a_cancel();
    view.ord_rej_reason = static_cast<pubsub_itc_fw_app::OrdRejReason>(99); // Other
    view.has_text = true;
    view.text = "a reason of the engine's";

    pubsub_itc_fw::ReplayClock clock(1700000000000000000LL);
    char buffer[fix_order_gateway::execution_report_initial_buffer_size];
    const std::string_view wire = encode_order_cancel_reject(view, "GATEWAY", "CLIENT", 9, clock, buffer, sizeof(buffer));

    ASSERT_FALSE(wire.empty());
    fix_codec::FixMessageReader reader(wire);
    ASSERT_TRUE(reader.is_valid()) << wire;
    EXPECT_EQ(reader.find(tag::CxlRejReason).as_string_view(), "99");
    EXPECT_EQ(reader.find(tag::Text).as_string_view(), "a reason of the engine's");
}

TEST(FixErEncoderTest, ARefusedCancelThatIsResentCarriesTheResendFlags) {
    pubsub_itc_fw::ReplayClock clock(1700000000000000000LL);
    char buffer[fix_order_gateway::execution_report_initial_buffer_size];
    const std::string_view wire = encode_order_cancel_reject(engine_refusal_of_a_cancel(), "GATEWAY", "CLIENT", 9, clock, buffer, sizeof(buffer),
                                                             /*poss_dup=*/true, utc_nanos(2026, 8, 10, 9, 30, 0), /*poss_resend=*/true);

    ASSERT_FALSE(wire.empty());
    fix_codec::FixMessageReader reader(wire);
    ASSERT_TRUE(reader.is_valid()) << wire;
    EXPECT_EQ(reader.find(tag::PossDupFlag).as_char(), 'Y');
    EXPECT_EQ(reader.find(tag::OrigSendingTime).as_string_view(), "20260810-09:30:00");
    EXPECT_EQ(reader.find(tag::PossResend).as_char(), 'Y');
}

} // namespaces
