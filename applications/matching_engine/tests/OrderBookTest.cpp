// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file OrderBookTest.cpp
 * @brief Unit tests for matching_engine::OrderBook.
 *
 * The book keeps the venue's open orders where a successor process can find them, so most of
 * these close the book and open it again on the same file -- which is what a restart does,
 * minus the process death -- and then check what the region holds. What is stored has to come
 * back byte for byte, because a member's order is what the venue is holding it to.
 */

#include <OrderBook.hpp>

#include <cstdlib>
#include <string>
#include <unistd.h>

#include <filesystem>
#include <set>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/MappedSlotStore.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>
#include <pubsub_itc_fw/PubSubItcException.hpp>

namespace matching_engine::tests {

namespace {

using SlotIndex = OrderBook::SlotIndex;

constexpr SlotIndex region_capacity = 8;
constexpr size_t map_capacity = 16;

OrderKey key_for(const char* comp_id, const char* cl_ord_id) {
    return OrderKey::make(fix_common::SessionIdentity::make(comp_id, 1), cl_ord_id);
}

OrderEntry entry_for(int64_t order_id_num, const char* symbol, const char* quantity) {
    OrderEntry entry{};
    entry.order_id_num = order_id_num;
    entry.session = fix_common::SessionIdentity::make("MEMBER-A", 1);
    entry.side = pubsub_itc_fw_app::Side::Buy;
    entry.ord_type = pubsub_itc_fw_app::OrdType::Limit;
    entry.set_symbol(symbol);
    entry.set_order_qty(quantity);
    return entry;
}

class OrderBookTest : public ::testing::Test {
  protected:
    void SetUp() override {
        // Under the working directory rather than a machine-wide location: two builds on one
        // machine must not contend, and the region is meant to be read back off a real
        // filesystem, which is the thing being tested.
        char tmpl[] = "order_book_test_XXXXXX";
        ASSERT_NE(::mkdtemp(tmpl), nullptr);
        dir_ = tmpl;
        path_ = dir_ + "/open_orders.region";
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    std::string dir_;
    std::string path_;
};

// ---- what the book does while the process lives ----------------------------------------

TEST_F(OrderBookTest, OpeningCreatesTheRegionAndSaysSo) {
    OrderBook book;
    EXPECT_FALSE(book.open(path_, region_capacity, map_capacity));
    EXPECT_TRUE(book.is_open());
    EXPECT_EQ(book.region_capacity(), region_capacity);
    EXPECT_EQ(book.size(), 0U);
}

TEST_F(OrderBookTest, OpeningAgainSaysTheRegionWasAlreadyThere) {
    {
        OrderBook first;
        ASSERT_FALSE(first.open(path_, region_capacity, map_capacity));
    }
    OrderBook second;
    EXPECT_TRUE(second.open(path_, region_capacity, map_capacity));
}

TEST_F(OrderBookTest, ABookNeedsAPath) {
    OrderBook book;
    EXPECT_THROW(static_cast<void>(book.open("", region_capacity, map_capacity)), pubsub_itc_fw::PreconditionAssertion);
}

TEST_F(OrderBookTest, AnOrderIsFoundByTheIdentityItWasFiledUnder) {
    OrderBook book;
    ASSERT_FALSE(book.open(path_, region_capacity, map_capacity));

    const OrderKey key = key_for("MEMBER-A", "ORD-001");
    ASSERT_TRUE(book.add(key, entry_for(1, "BHP", "100"), 10));

    EXPECT_TRUE(book.contains(key));
    const OrderEntry* found = book.find(key);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->order_id_num, 1);
    EXPECT_EQ(found->get_symbol(), "BHP");
    EXPECT_EQ(found->get_order_qty(), "100");
}

TEST_F(OrderBookTest, AnOrderNobodyPlacedIsNotFound) {
    OrderBook book;
    ASSERT_FALSE(book.open(path_, region_capacity, map_capacity));
    ASSERT_TRUE(book.add(key_for("MEMBER-A", "ORD-001"), entry_for(1, "BHP", "100"), 10));

    EXPECT_FALSE(book.contains(key_for("MEMBER-A", "ORD-002")));
    EXPECT_EQ(book.find(key_for("MEMBER-A", "ORD-002")), nullptr);
}

TEST_F(OrderBookTest, TwoMembersMayUseTheSameOrderIdentifier) {
    OrderBook book;
    ASSERT_FALSE(book.open(path_, region_capacity, map_capacity));

    ASSERT_TRUE(book.add(key_for("MEMBER-A", "ORD-001"), entry_for(1, "BHP", "100"), 10));
    ASSERT_TRUE(book.add(key_for("MEMBER-B", "ORD-001"), entry_for(2, "RIO", "200"), 11));

    EXPECT_EQ(book.size(), 2U);
    EXPECT_EQ(book.find(key_for("MEMBER-A", "ORD-001"))->get_symbol(), "BHP");
    EXPECT_EQ(book.find(key_for("MEMBER-B", "ORD-001"))->get_symbol(), "RIO");
}

TEST_F(OrderBookTest, RemovingAnOrderTakesItOutOfTheBook) {
    OrderBook book;
    ASSERT_FALSE(book.open(path_, region_capacity, map_capacity));
    const OrderKey key = key_for("MEMBER-A", "ORD-001");
    ASSERT_TRUE(book.add(key, entry_for(1, "BHP", "100"), 10));

    EXPECT_TRUE(book.remove(key));
    EXPECT_FALSE(book.contains(key));
    EXPECT_EQ(book.size(), 0U);
}

TEST_F(OrderBookTest, RemovingAnOrderThatIsNotThereSaysSo) {
    OrderBook book;
    ASSERT_FALSE(book.open(path_, region_capacity, map_capacity));

    EXPECT_FALSE(book.remove(key_for("MEMBER-A", "ORD-001")));
}

TEST_F(OrderBookTest, ARemovedOrdersRecordIsUsedAgain) {
    OrderBook book;
    ASSERT_FALSE(book.open(path_, region_capacity, map_capacity));

    // Fill the region, empty it, and fill it again. If the records were not released, the
    // second round would find the region full.
    for (SlotIndex i = 0; i < region_capacity; ++i) {
        ASSERT_TRUE(book.add(key_for("MEMBER-A", ("ORD-" + std::to_string(i)).c_str()), entry_for(i, "BHP", "100"), 10 + i));
    }
    for (SlotIndex i = 0; i < region_capacity; ++i) {
        ASSERT_TRUE(book.remove(key_for("MEMBER-A", ("ORD-" + std::to_string(i)).c_str())));
    }
    for (SlotIndex i = 0; i < region_capacity; ++i) {
        EXPECT_TRUE(book.add(key_for("MEMBER-B", ("ORD-" + std::to_string(i)).c_str()), entry_for(i, "RIO", "200"), 100 + i));
    }
    EXPECT_EQ(book.size(), region_capacity);
}

TEST_F(OrderBookTest, ClearingEmptiesTheBookAndGivesEveryRecordBack) {
    OrderBook book;
    ASSERT_FALSE(book.open(path_, region_capacity, map_capacity));
    for (SlotIndex i = 0; i < region_capacity; ++i) {
        ASSERT_TRUE(book.add(key_for("MEMBER-A", ("ORD-" + std::to_string(i)).c_str()), entry_for(i, "BHP", "100"), 10 + i));
    }

    book.clear();

    EXPECT_EQ(book.size(), 0U);
    for (SlotIndex i = 0; i < region_capacity; ++i) {
        EXPECT_TRUE(book.add(key_for("MEMBER-B", ("ORD-" + std::to_string(i)).c_str()), entry_for(i, "RIO", "200"), 100 + i));
    }
}

TEST_F(OrderBookTest, ReplacingAnOrderKeepsOneRecordNotTwo) {
    OrderBook book;
    ASSERT_FALSE(book.open(path_, region_capacity, map_capacity));
    const OrderKey key = key_for("MEMBER-A", "ORD-001");

    ASSERT_TRUE(book.add_or_replace(key, entry_for(1, "BHP", "100"), 10));
    ASSERT_TRUE(book.add_or_replace(key, entry_for(2, "RIO", "200"), 11));

    EXPECT_EQ(book.size(), 1U);
    EXPECT_EQ(book.find(key)->order_id_num, 2);
    EXPECT_EQ(book.find(key)->get_symbol(), "RIO");
}

TEST_F(OrderBookTest, EveryOrderIsVisitedOnce) {
    OrderBook book;
    ASSERT_FALSE(book.open(path_, region_capacity, map_capacity));
    for (SlotIndex i = 0; i < 5; ++i) {
        ASSERT_TRUE(book.add(key_for("MEMBER-A", ("ORD-" + std::to_string(i)).c_str()), entry_for(i, "BHP", "100"), 10 + i));
    }

    std::set<int64_t> seen;
    size_t visits = 0;
    book.for_each([&](const OrderKey& key, const OrderEntry& entry) {
        EXPECT_EQ(key.session.comp_id_view(), "MEMBER-A");
        seen.insert(entry.order_id_num);
        ++visits;
    });

    EXPECT_EQ(visits, 5U);
    EXPECT_EQ(seen.size(), 5U);
}

// ---- what a full region does -----------------------------------------------------------

TEST_F(OrderBookTest, AnOrderArrivingWithNoRecordLeftIsRefused) {
    OrderBook book;
    ASSERT_FALSE(book.open(path_, region_capacity, map_capacity));
    for (SlotIndex i = 0; i < region_capacity; ++i) {
        ASSERT_TRUE(book.add(key_for("MEMBER-A", ("ORD-" + std::to_string(i)).c_str()), entry_for(i, "BHP", "100"), 10 + i));
    }

    // Refused rather than admitted, because the venue must not hold an order it could not
    // account for after a restart.
    EXPECT_FALSE(book.add(key_for("MEMBER-A", "ONE-TOO-MANY"), entry_for(99, "BHP", "100"), 99));
    EXPECT_EQ(book.size(), region_capacity);
    EXPECT_FALSE(book.contains(key_for("MEMBER-A", "ONE-TOO-MANY")));
}

TEST_F(OrderBookTest, ReplacingAnOrderStillWorksWhenTheRegionIsFull) {
    OrderBook book;
    ASSERT_FALSE(book.open(path_, region_capacity, map_capacity));
    for (SlotIndex i = 0; i < region_capacity; ++i) {
        ASSERT_TRUE(book.add(key_for("MEMBER-A", ("ORD-" + std::to_string(i)).c_str()), entry_for(i, "BHP", "100"), 10 + i));
    }

    // The replica applies these, and an update to an order it already holds needs no record
    // it does not already have.
    const OrderKey existing = key_for("MEMBER-A", "ORD-0");
    EXPECT_TRUE(book.add_or_replace(existing, entry_for(500, "RIO", "200"), 500));
    EXPECT_EQ(book.find(existing)->order_id_num, 500);
    EXPECT_FALSE(book.add_or_replace(key_for("MEMBER-A", "ONE-TOO-MANY"), entry_for(99, "BHP", "100"), 99));
}

// ---- what a successor process finds -----------------------------------------------------

TEST_F(OrderBookTest, TheOrdersAreStillInTheRegionAfterTheBookIsClosed) {
    const OrderKey key = key_for("MEMBER-A", "ORD-001");
    {
        OrderBook book;
        ASSERT_FALSE(book.open(path_, region_capacity, map_capacity));
        OrderEntry entry = entry_for(7, "BHP", "100");
        entry.has_price = true;
        entry.set_price("42.50");
        entry.has_time_in_force = true;
        entry.time_in_force = pubsub_itc_fw_app::TimeInForce::GoodTillCancel;
        ASSERT_TRUE(book.add(key, entry, 10));
        book.publish(10);
    }

    // The book maps the region again but does not read it back: that is recovery, which is
    // not built yet. What can be checked here is that the record survived, which the region
    // is asked directly.
    pubsub_itc_fw::MappedSlotStore store;
    ASSERT_TRUE(store.open(path_, static_cast<uint32_t>(sizeof(OrderEntry)), region_capacity));
    EXPECT_EQ(store.published(), 10);

    size_t recoverable = 0;
    for (SlotIndex i = 0; i < region_capacity; ++i) {
        if (!store.is_recoverable(i)) {
            continue;
        }
        ++recoverable;
        const auto* entry = reinterpret_cast<const OrderEntry*>(store.payload(i));
        EXPECT_EQ(entry->order_id_num, 7);
        EXPECT_EQ(entry->get_symbol(), "BHP");
        EXPECT_EQ(entry->get_order_qty(), "100");
        EXPECT_EQ(entry->get_price(), "42.50");
        EXPECT_TRUE(entry->has_time_in_force);
        EXPECT_EQ(entry->time_in_force, pubsub_itc_fw_app::TimeInForce::GoodTillCancel);
        EXPECT_EQ(entry->session.comp_id_view(), "MEMBER-A");
    }
    EXPECT_EQ(recoverable, 1U);
}

TEST_F(OrderBookTest, AnOrderCancelledBeforeTheProcessDiedIsNotInTheRegion) {
    {
        OrderBook book;
        ASSERT_FALSE(book.open(path_, region_capacity, map_capacity));
        ASSERT_TRUE(book.add(key_for("MEMBER-A", "ORD-001"), entry_for(1, "BHP", "100"), 10));
        ASSERT_TRUE(book.add(key_for("MEMBER-A", "ORD-002"), entry_for(2, "RIO", "200"), 11));
        book.publish(11);
        book.remove(key_for("MEMBER-A", "ORD-001"));
        book.publish(12);
    }

    pubsub_itc_fw::MappedSlotStore store;
    ASSERT_TRUE(store.open(path_, static_cast<uint32_t>(sizeof(OrderEntry)), region_capacity));

    size_t recoverable = 0;
    int64_t surviving_order = 0;
    for (SlotIndex i = 0; i < region_capacity; ++i) {
        if (store.is_recoverable(i)) {
            ++recoverable;
            surviving_order = reinterpret_cast<const OrderEntry*>(store.payload(i))->order_id_num;
        }
    }
    EXPECT_EQ(recoverable, 1U);
    EXPECT_EQ(surviving_order, 2);
}

TEST_F(OrderBookTest, AnOrderNotYetPublishedIsAboveWhatTheRegionVouchesFor) {
    {
        OrderBook book;
        ASSERT_FALSE(book.open(path_, region_capacity, map_capacity));
        ASSERT_TRUE(book.add(key_for("MEMBER-A", "ORD-001"), entry_for(1, "BHP", "100"), 10));
        book.publish(10);
        // The second order is written but the process dies before its report leaves, so
        // nothing publishes it. The sequencer's tail will run that order again.
        ASSERT_TRUE(book.add(key_for("MEMBER-A", "ORD-002"), entry_for(2, "RIO", "200"), 11));
    }

    pubsub_itc_fw::MappedSlotStore store;
    ASSERT_TRUE(store.open(path_, static_cast<uint32_t>(sizeof(OrderEntry)), region_capacity));
    EXPECT_EQ(store.published(), 10);

    size_t recoverable = 0;
    for (SlotIndex i = 0; i < region_capacity; ++i) {
        recoverable += store.is_recoverable(i) ? 1 : 0;
    }
    EXPECT_EQ(recoverable, 1U) << "the order written after the last publish must not be recovered";
}

TEST_F(OrderBookTest, ARegionWrittenForADifferentOrderRecordIsRefused) {
    {
        pubsub_itc_fw::MappedSlotStore store;
        ASSERT_FALSE(store.open(path_, static_cast<uint32_t>(sizeof(OrderEntry)) + 8, region_capacity));
    }
    OrderBook book;
    // Refused rather than reinterpreted: read with the wrong record size it would produce
    // orders that are wrong in ways nothing downstream can detect.
    EXPECT_THROW(static_cast<void>(book.open(path_, region_capacity, map_capacity)), pubsub_itc_fw::PubSubItcException);
}

TEST_F(OrderBookTest, ARegionWrittenForADifferentNumberOfOrdersIsRefused) {
    {
        OrderBook first;
        ASSERT_FALSE(first.open(path_, region_capacity, map_capacity));
    }
    OrderBook second;
    EXPECT_THROW(static_cast<void>(second.open(path_, region_capacity * 2, map_capacity)), pubsub_itc_fw::PubSubItcException);
}

// ---- what a successor process recovers --------------------------------------------------

TEST_F(OrderBookTest, RecoveringARegionNobodyWroteFindsNothing) {
    OrderBook book;
    ASSERT_FALSE(book.open(path_, region_capacity, map_capacity));

    const OrderBook::Recovery found = book.recover();

    EXPECT_EQ(found.orders, 0U);
    EXPECT_EQ(found.discarded, 0U);
    EXPECT_EQ(found.published, 0);
    EXPECT_EQ(book.size(), 0U);
}

TEST_F(OrderBookTest, AnOrderOpenBeforeTheRestartIsOpenAfterIt) {
    const OrderKey key = key_for("MEMBER-A", "ORD-001");
    {
        OrderBook before;
        ASSERT_FALSE(before.open(path_, region_capacity, map_capacity));
        OrderEntry entry = entry_for(7, "BHP", "100");
        entry.has_price = true;
        entry.set_price("42.50");
        entry.has_time_in_force = true;
        entry.time_in_force = pubsub_itc_fw_app::TimeInForce::GoodTillCancel;
        ASSERT_TRUE(before.add(key, entry, 10));
        before.publish(10);
    }

    OrderBook after;
    ASSERT_TRUE(after.open(path_, region_capacity, map_capacity));
    const OrderBook::Recovery found = after.recover();

    EXPECT_EQ(found.orders, 1U);
    EXPECT_EQ(found.published, 10);
    EXPECT_EQ(found.highest_order_id_num, 7);

    // The member's question is whether its order is still cancellable, which means the record
    // has to be filed under the identity the member will name it by.
    ASSERT_TRUE(after.contains(key));
    const OrderEntry* recovered = after.find(key);
    ASSERT_NE(recovered, nullptr);
    EXPECT_EQ(recovered->order_id_num, 7);
    EXPECT_EQ(recovered->get_cl_ord_id(), "ORD-001");
    EXPECT_EQ(recovered->session.comp_id_view(), "MEMBER-A");
    EXPECT_EQ(recovered->get_symbol(), "BHP");
    EXPECT_EQ(recovered->get_order_qty(), "100");
    EXPECT_EQ(recovered->get_price(), "42.50");
    EXPECT_EQ(recovered->time_in_force, pubsub_itc_fw_app::TimeInForce::GoodTillCancel);
    EXPECT_TRUE(after.remove(key));
}

TEST_F(OrderBookTest, EveryMembersOrdersComeBackUnderTheirOwnIdentities) {
    {
        OrderBook before;
        ASSERT_FALSE(before.open(path_, region_capacity, map_capacity));
        ASSERT_TRUE(before.add(key_for("MEMBER-A", "ORD-001"), entry_for(1, "BHP", "100"), 10));
        ASSERT_TRUE(before.add(key_for("MEMBER-B", "ORD-001"), entry_for(2, "RIO", "200"), 11));
        ASSERT_TRUE(before.add(key_for("MEMBER-A", "ORD-002"), entry_for(3, "CBA", "300"), 12));
        before.publish(12);
    }

    OrderBook after;
    ASSERT_TRUE(after.open(path_, region_capacity, map_capacity));
    EXPECT_EQ(after.recover().orders, 3U);

    // Two members using the same identifier must not become one order.
    EXPECT_EQ(after.find(key_for("MEMBER-A", "ORD-001"))->get_symbol(), "BHP");
    EXPECT_EQ(after.find(key_for("MEMBER-B", "ORD-001"))->get_symbol(), "RIO");
    EXPECT_EQ(after.find(key_for("MEMBER-A", "ORD-002"))->get_symbol(), "CBA");
}

TEST_F(OrderBookTest, AnOrderCancelledBeforeTheRestartDoesNotComeBack) {
    {
        OrderBook before;
        ASSERT_FALSE(before.open(path_, region_capacity, map_capacity));
        ASSERT_TRUE(before.add(key_for("MEMBER-A", "ORD-001"), entry_for(1, "BHP", "100"), 10));
        ASSERT_TRUE(before.add(key_for("MEMBER-A", "ORD-002"), entry_for(2, "RIO", "200"), 11));
        before.publish(11);
        before.remove(key_for("MEMBER-A", "ORD-001"));
        before.publish(12);
    }

    OrderBook after;
    ASSERT_TRUE(after.open(path_, region_capacity, map_capacity));
    EXPECT_EQ(after.recover().orders, 1U);
    EXPECT_FALSE(after.contains(key_for("MEMBER-A", "ORD-001")));
    EXPECT_TRUE(after.contains(key_for("MEMBER-A", "ORD-002")));
}

TEST_F(OrderBookTest, AnOrderWrittenAfterTheLastPublishIsDiscarded) {
    {
        OrderBook before;
        ASSERT_FALSE(before.open(path_, region_capacity, map_capacity));
        ASSERT_TRUE(before.add(key_for("MEMBER-A", "ORD-001"), entry_for(1, "BHP", "100"), 10));
        before.publish(10);
        // Written, but the process dies before the report leaves, so nothing publishes it.
        // The sequencer's tail holds this order and will run it again.
        ASSERT_TRUE(before.add(key_for("MEMBER-A", "ORD-002"), entry_for(2, "RIO", "200"), 11));
    }

    OrderBook after;
    ASSERT_TRUE(after.open(path_, region_capacity, map_capacity));
    const OrderBook::Recovery found = after.recover();

    EXPECT_EQ(found.orders, 1U);
    EXPECT_EQ(found.discarded, 1U) << "the unfinished record must be reported, not silently dropped";
    EXPECT_EQ(found.published, 10);
    EXPECT_TRUE(after.contains(key_for("MEMBER-A", "ORD-001")));
    EXPECT_FALSE(after.contains(key_for("MEMBER-A", "ORD-002")));
}

TEST_F(OrderBookTest, ARecoveredBookCanTakeNewOrdersInEveryRecordItFreed) {
    {
        OrderBook before;
        ASSERT_FALSE(before.open(path_, region_capacity, map_capacity));
        for (SlotIndex i = 0; i < region_capacity; ++i) {
            ASSERT_TRUE(before.add(key_for("MEMBER-A", ("ORD-" + std::to_string(i)).c_str()), entry_for(i + 1, "BHP", "100"), 10 + i));
        }
        before.publish(10 + region_capacity);
        // Two of them cancelled, so the region holds two free records that the free list in
        // the file may or may not be telling the truth about.
        before.remove(key_for("MEMBER-A", "ORD-0"));
        before.remove(key_for("MEMBER-A", "ORD-3"));
    }

    OrderBook after;
    ASSERT_TRUE(after.open(path_, region_capacity, map_capacity));
    EXPECT_EQ(after.recover().orders, region_capacity - 2);

    // The free list is rebuilt from what the scan found rather than trusted, so exactly the
    // two released records are available and no more.
    EXPECT_TRUE(after.add(key_for("MEMBER-B", "NEW-1"), entry_for(101, "RIO", "200"), 200));
    EXPECT_TRUE(after.add(key_for("MEMBER-B", "NEW-2"), entry_for(102, "RIO", "200"), 201));
    EXPECT_FALSE(after.add(key_for("MEMBER-B", "NEW-3"), entry_for(103, "RIO", "200"), 202));
    EXPECT_EQ(after.size(), region_capacity);
}

TEST_F(OrderBookTest, TheHighestOrderNumberComesBackSoASuccessorDoesNotReissueIt) {
    {
        OrderBook before;
        ASSERT_FALSE(before.open(path_, region_capacity, map_capacity));
        ASSERT_TRUE(before.add(key_for("MEMBER-A", "ORD-001"), entry_for(41, "BHP", "100"), 10));
        ASSERT_TRUE(before.add(key_for("MEMBER-A", "ORD-002"), entry_for(99, "RIO", "200"), 11));
        ASSERT_TRUE(before.add(key_for("MEMBER-A", "ORD-003"), entry_for(70, "CBA", "300"), 12));
        before.publish(12);
    }

    OrderBook after;
    ASSERT_TRUE(after.open(path_, region_capacity, map_capacity));
    EXPECT_EQ(after.recover().highest_order_id_num, 99);
}

TEST_F(OrderBookTest, RecoveryLeavesNothingForWarmingToDo) {
    {
        OrderBook before;
        ASSERT_FALSE(before.open(path_, region_capacity, map_capacity));
        ASSERT_TRUE(before.add(key_for("MEMBER-A", "ORD-001"), entry_for(1, "BHP", "100"), 10));
        before.publish(10);
    }

    OrderBook after;
    ASSERT_TRUE(after.open(path_, region_capacity, map_capacity));
    EXPECT_EQ(after.recover().orders, 1U);

    // Warming after a recovery is allowed and does nothing that matters, because the scan has
    // already touched every page. It must not disturb what was recovered.
    after.warm();
    EXPECT_EQ(after.size(), 1U);
    EXPECT_EQ(after.find(key_for("MEMBER-A", "ORD-001"))->get_symbol(), "BHP");
}

TEST_F(OrderBookTest, WarmingLeavesTheOrdersAlone) {
    OrderBook book;
    ASSERT_FALSE(book.open(path_, region_capacity, map_capacity));
    const OrderKey key = key_for("MEMBER-A", "ORD-001");
    ASSERT_TRUE(book.add(key, entry_for(1, "BHP", "100"), 10));

    book.warm();

    EXPECT_EQ(book.size(), 1U);
    EXPECT_EQ(book.find(key)->get_symbol(), "BHP");
}

TEST_F(OrderBookTest, ThePublishedPositionIsWhatWasLastPublished) {
    OrderBook book;
    ASSERT_FALSE(book.open(path_, region_capacity, map_capacity));

    EXPECT_EQ(book.published(), 0);
    book.publish(42);
    EXPECT_EQ(book.published(), 42);
}

} // namespaces

} // namespaces
