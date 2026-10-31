// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include "VoidedCommandList.hpp"

namespace {

using fix_common::VoidedCommandList;
using fix_common::VoidedKind;
using fix_common::VoidedRange;
using pubsub_itc_fw_app::VoidReason;

VoidedRange void_at(int64_t void_seq_no, int64_t first, int64_t last, bool new_orders_only) {
    return VoidedRange{void_seq_no, first, last, new_orders_only, VoidReason::EngineDiedApplying};
}

TEST(VoidedCommandListTest, AnEmptyListVoidsNothing) {
    const VoidedCommandList list;
    EXPECT_TRUE(list.empty());
    EXPECT_EQ(list.voiding(1, VoidedKind::new_order), nullptr);
    EXPECT_EQ(list.voiding(1, VoidedKind::cancel), nullptr);
}

TEST(VoidedCommandListTest, AVoidCoversItsRangeAndNothingOutsideIt) {
    VoidedCommandList list;
    ASSERT_TRUE(list.add(void_at(50, 10, 12, false)));

    EXPECT_EQ(list.voiding(9, VoidedKind::new_order), nullptr);
    for (int64_t seq = 10; seq <= 12; ++seq) {
        ASSERT_NE(list.voiding(seq, VoidedKind::new_order), nullptr) << "command " << seq;
        EXPECT_EQ(list.voiding(seq, VoidedKind::new_order)->void_seq_no, 50);
        EXPECT_NE(list.voiding(seq, VoidedKind::cancel), nullptr) << "command " << seq;
    }
    EXPECT_EQ(list.voiding(13, VoidedKind::new_order), nullptr);
}

TEST(VoidedCommandListTest, AVoidOfNewOrdersOnlyLeavesCancelsToBeApplied) {
    VoidedCommandList list;
    ASSERT_TRUE(list.add(void_at(50, 10, 20, true)));

    EXPECT_NE(list.voiding(15, VoidedKind::new_order), nullptr);
    EXPECT_EQ(list.voiding(15, VoidedKind::cancel), nullptr) << "a cancel applied late still does what its member asked";
}

TEST(VoidedCommandListTest, AVoidAlreadyHeldIsNotAddedTwice) {
    VoidedCommandList list;
    ASSERT_TRUE(list.add(void_at(50, 10, 10, false)));
    EXPECT_FALSE(list.add(void_at(50, 10, 10, false)));
    EXPECT_EQ(list.size(), 1U);
}

TEST(VoidedCommandListTest, VoidsAreKeptInTheOrderOfTheirPositions) {
    VoidedCommandList list;
    ASSERT_TRUE(list.add(void_at(90, 80, 80, false)));
    ASSERT_TRUE(list.add(void_at(30, 20, 20, false)));
    ASSERT_TRUE(list.add(void_at(60, 40, 45, true)));

    ASSERT_EQ(list.size(), 3U);
    EXPECT_EQ(list.voids()[0].void_seq_no, 30);
    EXPECT_EQ(list.voids()[1].void_seq_no, 60);
    EXPECT_EQ(list.voids()[2].void_seq_no, 90);
    ASSERT_NE(list.at(60), nullptr);
    EXPECT_EQ(list.at(60)->first_seq_no, 40);
    EXPECT_EQ(list.at(61), nullptr);
}

TEST(VoidedCommandListTest, VoidsWrittenAfterADiscardedPositionGo) {
    VoidedCommandList list;
    ASSERT_TRUE(list.add(void_at(30, 20, 20, false)));
    ASSERT_TRUE(list.add(void_at(60, 40, 45, false)));
    list.discard_after(59);

    ASSERT_EQ(list.size(), 1U);
    EXPECT_EQ(list.voids()[0].void_seq_no, 30);
    EXPECT_EQ(list.voiding(42, VoidedKind::new_order), nullptr);
}

} // namespaces
