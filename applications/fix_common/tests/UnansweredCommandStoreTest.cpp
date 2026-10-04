// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <deque>
#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "UnansweredCommandStore.hpp"

#include <pubsub_itc_fw/PreconditionAssertion.hpp>

using fix_common::UnansweredCommandStore;

namespace {

// An envelope of the given size whose bytes say which command it is, so that a copy handed back from
// the wrong place, or overwritten in part, does not match.
std::vector<uint8_t> envelope_for(const std::string& cl_ord_id, size_t size) {
    std::vector<uint8_t> bytes(size);
    for (size_t index = 0; index < size; ++index) {
        bytes[index] = static_cast<uint8_t>(cl_ord_id[index % cl_ord_id.size()] + index);
    }
    return bytes;
}

UnansweredCommandStore::Kept keep(UnansweredCommandStore& store, const std::string& comp_id, const std::string& cl_ord_id, size_t size = 50) {
    const std::vector<uint8_t> envelope = envelope_for(cl_ord_id, size);
    return store.keep(comp_id, cl_ord_id, envelope.data(), envelope.size());
}

// The envelopes the store would send again, in order, checked against what was kept.
std::vector<std::vector<uint8_t>> unanswered(const UnansweredCommandStore& store) {
    std::vector<std::vector<uint8_t>> result;
    store.for_each_unanswered([&result](const uint8_t* bytes, size_t size) { result.emplace_back(bytes, bytes + size); });
    return result;
}

} // namespaces

TEST(UnansweredCommandStoreTest, ACommandIsHeldUntilItIsAnswered) {
    UnansweredCommandStore store(4096, 16);
    EXPECT_EQ(keep(store, "MEMBER", "order-1"), UnansweredCommandStore::Kept::kept);
    EXPECT_TRUE(store.holds("MEMBER", "order-1"));
    EXPECT_EQ(store.unanswered(), 1U);
    EXPECT_TRUE(store.answered("MEMBER", "order-1"));
    EXPECT_FALSE(store.holds("MEMBER", "order-1"));
    EXPECT_EQ(store.unanswered(), 0U);
    EXPECT_FALSE(store.answered("MEMBER", "order-1")) << "a second report for the same command finds nothing held";
}

TEST(UnansweredCommandStoreTest, TheUnansweredAreHandedBackInTheOrderTheyWereKept) {
    UnansweredCommandStore store(4096, 16);
    keep(store, "MEMBER", "order-1");
    keep(store, "MEMBER", "order-2");
    keep(store, "MEMBER", "order-3");
    store.answered("MEMBER", "order-2");
    const auto copies = unanswered(store);
    ASSERT_EQ(copies.size(), 2U);
    EXPECT_EQ(copies[0], envelope_for("order-1", 50));
    EXPECT_EQ(copies[1], envelope_for("order-3", 50));
}

TEST(UnansweredCommandStoreTest, TheCompIdIsPartOfTheIdentity) {
    UnansweredCommandStore store(4096, 16);
    keep(store, "MEMBER1", "order-1");
    EXPECT_EQ(keep(store, "MEMBER2", "order-1"), UnansweredCommandStore::Kept::kept);
    EXPECT_TRUE(store.answered("MEMBER2", "order-1"));
    EXPECT_TRUE(store.holds("MEMBER1", "order-1"));
    // A comp id that is the start of another, with the remainder moved into the ClOrdID, is a different command.
    EXPECT_FALSE(store.holds("MEMBER", "1order-1"));
}

TEST(UnansweredCommandStoreTest, AResubmissionOfAnUnansweredCommandIsNotKeptTwice) {
    UnansweredCommandStore store(4096, 16);
    keep(store, "MEMBER", "order-1");
    EXPECT_EQ(keep(store, "MEMBER", "order-1"), UnansweredCommandStore::Kept::already_held);
    EXPECT_EQ(store.unanswered(), 1U);
    EXPECT_EQ(unanswered(store).size(), 1U);
}

TEST(UnansweredCommandStoreTest, OneUnansweredCommandHoldsTheSpaceBehindItAndTheStoreFills) {
    // Room for four copies of 100 bytes including their keys.
    UnansweredCommandStore store(4 * (100 + 7 + 1 + 7), 64);
    EXPECT_EQ(keep(store, "MEMBER", "order-1", 100), UnansweredCommandStore::Kept::kept);
    for (int number = 2; number <= 4; ++number) {
        EXPECT_EQ(keep(store, "MEMBER", "order-" + std::to_string(number), 100), UnansweredCommandStore::Kept::kept);
        store.answered("MEMBER", "order-" + std::to_string(number));
    }
    EXPECT_EQ(keep(store, "MEMBER", "order-5", 100), UnansweredCommandStore::Kept::full) << "order-1 is unanswered, so nothing behind it is reused";
    store.answered("MEMBER", "order-1");
    EXPECT_EQ(keep(store, "MEMBER", "order-5", 100), UnansweredCommandStore::Kept::kept);
}

TEST(UnansweredCommandStoreTest, TheListOfCopiesHasAFixedLength) {
    UnansweredCommandStore store(1 << 20, 3);
    keep(store, "M", "1");
    keep(store, "M", "2");
    keep(store, "M", "3");
    EXPECT_EQ(keep(store, "M", "4"), UnansweredCommandStore::Kept::full);
    store.answered("M", "1");
    EXPECT_EQ(keep(store, "M", "4"), UnansweredCommandStore::Kept::kept);
}

TEST(UnansweredCommandStoreTest, DroppingASessionForgetsOnlyItsCommands) {
    UnansweredCommandStore store(4096, 16);
    keep(store, "MEMBER", "order-1");
    keep(store, "MEMBER2", "order-2");
    keep(store, "MEMBER", "order-3");
    EXPECT_EQ(store.drop_session("MEMBER"), 2U);
    EXPECT_EQ(store.unanswered(), 1U);
    EXPECT_TRUE(store.holds("MEMBER2", "order-2"));
    EXPECT_FALSE(store.holds("MEMBER", "order-1"));
}

TEST(UnansweredCommandStoreTest, ZeroCapacitiesAreRefused) {
    EXPECT_THROW(UnansweredCommandStore(0, 10), pubsub_itc_fw::PreconditionAssertion);
    EXPECT_THROW(UnansweredCommandStore(10, 0), pubsub_itc_fw::PreconditionAssertion);
}

// Many commands of random sizes, answered in a random order, against a plain list of what should be
// held. Every few hundred, what the store would send again is compared with the list.
TEST(UnansweredCommandStoreTest, RandomCommandsAndAnswersAgreeWithASimpleModel) {
    UnansweredCommandStore store(8192, 64);
    struct Held {
        std::string cl_ord_id;
        size_t size;
    };
    std::deque<Held> model;
    std::mt19937 random(2026);
    std::uniform_int_distribution<size_t> sizes(1, 300);
    int kept_count = 0;
    int refused_count = 0;
    for (int number = 1; number <= 20000; ++number) {
        const std::string cl_ord_id = "order-" + std::to_string(number);
        const size_t size = sizes(random);
        if (keep(store, "MEMBER", cl_ord_id, size) == UnansweredCommandStore::Kept::kept) {
            model.push_back(Held{cl_ord_id, size});
            ++kept_count;
        } else {
            ++refused_count;
        }
        // Answer about as many as are kept, choosing which at random.
        while (!model.empty() && random() % 3 != 0) {
            const size_t which = random() % model.size();
            ASSERT_TRUE(store.answered("MEMBER", model[which].cl_ord_id)) << model[which].cl_ord_id;
            model.erase(model.begin() + static_cast<std::ptrdiff_t>(which));
        }
        ASSERT_EQ(store.unanswered(), model.size());
        if (number % 337 == 0) {
            const std::vector<std::vector<uint8_t>> sent_again = unanswered(store);
            ASSERT_EQ(sent_again.size(), model.size());
            for (size_t index = 0; index < model.size(); ++index) {
                ASSERT_EQ(sent_again[index], envelope_for(model[index].cl_ord_id, model[index].size)) << "after command " << number;
            }
        }
    }
    // Both outcomes must have happened, or the test exercised only one of them.
    EXPECT_GT(kept_count, 1000);
    EXPECT_GT(refused_count, 0);
}
