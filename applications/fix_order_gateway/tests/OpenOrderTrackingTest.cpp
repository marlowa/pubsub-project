// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

// Tests for open_orders::track_open_order, the one rule both gateways use to keep a session's record
// of its open orders up to date from the matching engine's reports.

#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/ExpandablePoolAllocator.hpp>
#include <pubsub_itc_fw/UseHugePagesFlag.hpp>

#include "OpenOrderTracking.hpp"

namespace {

using open_orders::OpenOrderEntry;
using open_orders::TrackOutcome;
using pubsub_itc_fw_app::ExecType;
using pubsub_itc_fw_app::OrdStatus;

class OpenOrderTrackingTest : public ::testing::Test {
  protected:
    OpenOrderTrackingTest()
        : pool_("TestOpenOrderPool", 16, 1, /*expansion_threshold_hint=*/0, /*handler_for_pool_exhausted=*/nullptr, /*handler_for_invalid_free=*/nullptr,
                /*handler_for_huge_pages_error=*/nullptr, pubsub_itc_fw::UseHugePagesFlag{pubsub_itc_fw::UseHugePagesFlag::DoNotUseHugePages}) {}

    // A report acknowledging a new order, as the matching engine sends it.
    static pubsub_itc_fw_app::ExecutionReportView acknowledgement(std::string_view cl_ord_id) {
        pubsub_itc_fw_app::ExecutionReportView report{};
        report.order_id = "ME-ORD-1";
        report.exec_id = "ME-EXEC-1";
        report.exec_type = ExecType::New;
        report.ord_status = OrdStatus::New;
        report.symbol = "AAPL";
        report.side = static_cast<pubsub_itc_fw_app::Side>('1');
        report.leaves_qty = "100";
        report.cum_qty = "0";
        report.has_cl_ord_id = true;
        report.cl_ord_id = cl_ord_id;
        report.has_order_qty = true;
        report.order_qty = "100";
        return report;
    }

    int allocated() const {
        return pool_.get_pool_statistics().number_of_allocated_objects_;
    }

    pubsub_itc_fw::ExpandablePoolAllocator<OpenOrderEntry> pool_;
    open_orders::OpenOrderMap orders_;
};

TEST_F(OpenOrderTrackingTest, AnAcknowledgedOrderIsPutOnFileWithItsTerms) {
    EXPECT_EQ(open_orders::track_open_order(orders_, pool_, acknowledgement("ORD-1")), TrackOutcome::Added);
    ASSERT_EQ(orders_.size(), 1U);
    const OpenOrderEntry& entry = *orders_.at("ORD-1");
    EXPECT_EQ(std::string_view(entry.symbol, entry.symbol_len), "AAPL");
    EXPECT_EQ(std::string_view(entry.order_qty, entry.order_qty_len), "100");
    EXPECT_EQ(std::string_view(entry.order_id, entry.order_id_len), "ME-ORD-1");
    EXPECT_EQ(entry.side, '1');
    EXPECT_EQ(allocated(), 1);
}

TEST_F(OpenOrderTrackingTest, ARepeatedReportUpdatesTheEntryInPlaceWithoutAllocatingAgain) {
    ASSERT_EQ(open_orders::track_open_order(orders_, pool_, acknowledgement("ORD-1")), TrackOutcome::Added);
    pubsub_itc_fw_app::ExecutionReportView later = acknowledgement("ORD-1");
    later.ord_status = OrdStatus::PartiallyFilled;
    later.order_qty = "60";
    EXPECT_EQ(open_orders::track_open_order(orders_, pool_, later), TrackOutcome::Updated);
    ASSERT_EQ(orders_.size(), 1U);
    const OpenOrderEntry& entry = *orders_.at("ORD-1");
    EXPECT_EQ(std::string_view(entry.order_qty, entry.order_qty_len), "60");
    // One entry, not two: a second allocation would be stranded in the pool for good.
    EXPECT_EQ(allocated(), 1);
}

TEST_F(OpenOrderTrackingTest, ACancelRetiresTheOrderItNamesAndReturnsItsEntry) {
    ASSERT_EQ(open_orders::track_open_order(orders_, pool_, acknowledgement("ORD-1")), TrackOutcome::Added);
    pubsub_itc_fw_app::ExecutionReportView cancelled = acknowledgement("CXL-1");
    cancelled.exec_type = ExecType::Canceled;
    cancelled.ord_status = OrdStatus::Canceled;
    cancelled.has_orig_cl_ord_id = true;
    cancelled.orig_cl_ord_id = "ORD-1";
    EXPECT_EQ(open_orders::track_open_order(orders_, pool_, cancelled), TrackOutcome::Removed);
    EXPECT_TRUE(orders_.empty());
    EXPECT_EQ(allocated(), 0);
}

TEST_F(OpenOrderTrackingTest, ARefusedCancelRetiresNothing) {
    ASSERT_EQ(open_orders::track_open_order(orders_, pool_, acknowledgement("ORD-1")), TrackOutcome::Added);
    pubsub_itc_fw_app::ExecutionReportView refused = acknowledgement("CXL-1");
    refused.exec_type = ExecType::Rejected;
    refused.ord_status = OrdStatus::Rejected;
    refused.has_orig_cl_ord_id = true;
    refused.orig_cl_ord_id = "ORD-1";
    EXPECT_EQ(open_orders::track_open_order(orders_, pool_, refused), TrackOutcome::NothingToRemove);
    EXPECT_EQ(orders_.size(), 1U);
    EXPECT_EQ(allocated(), 1);
}

TEST_F(OpenOrderTrackingTest, AnOverLongFieldIsRefusedAndNothingIsAllocated) {
    const std::string long_symbol(open_orders::max_supported_symbol_length + 1, 'S');
    pubsub_itc_fw_app::ExecutionReportView report = acknowledgement("ORD-1");
    report.symbol = long_symbol;
    EXPECT_EQ(open_orders::track_open_order(orders_, pool_, report), TrackOutcome::FieldTooLong);
    EXPECT_TRUE(orders_.empty());
    EXPECT_EQ(allocated(), 0);
}

TEST_F(OpenOrderTrackingTest, AReportNamingNoOrderIsIgnored) {
    pubsub_itc_fw_app::ExecutionReportView report = acknowledgement("ORD-1");
    report.has_cl_ord_id = false;
    EXPECT_EQ(open_orders::track_open_order(orders_, pool_, report), TrackOutcome::NoOrderNamed);
    EXPECT_TRUE(orders_.empty());
}

} // namespaces
