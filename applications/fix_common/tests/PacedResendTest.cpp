// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "PacedResend.hpp"
#include "UnansweredCommandStore.hpp"

using fix_common::PacedResend;
using fix_common::UnansweredCommandStore;
using fix_common::UnansweredSnapshot;

namespace {

std::vector<uint8_t> envelope_for(const std::string& cl_ord_id) {
    return std::vector<uint8_t>(cl_ord_id.begin(), cl_ord_id.end());
}

void keep(UnansweredCommandStore& store, const std::string& cl_ord_id) {
    const std::vector<uint8_t> envelope = envelope_for(cl_ord_id);
    static_cast<void>(store.keep("MEMBER", cl_ord_id, envelope.data(), envelope.size()));
}

std::string command_text(const UnansweredSnapshot& snapshot, size_t index) {
    const auto [bytes, size] = snapshot.command(index);
    return std::string(reinterpret_cast<const char*>(bytes), size);
}

} // namespaces

TEST(PacedResendTest, ASnapshotHoldsTheUnansweredCommandsOldestFirst) {
    UnansweredCommandStore store(4096, 16);
    keep(store, "order-1");
    keep(store, "order-2");
    keep(store, "order-3");
    static_cast<void>(store.answered("MEMBER", "order-2"));
    UnansweredSnapshot snapshot;
    snapshot.take(store);
    ASSERT_EQ(snapshot.count(), 2U);
    EXPECT_EQ(command_text(snapshot, 0), "order-1");
    EXPECT_EQ(command_text(snapshot, 1), "order-3");
}

TEST(PacedResendTest, ASnapshotIsNotChangedByCommandsAnsweredAfterItWasTaken) {
    UnansweredCommandStore store(4096, 16);
    keep(store, "order-1");
    keep(store, "order-2");
    UnansweredSnapshot snapshot;
    snapshot.take(store);
    static_cast<void>(store.answered("MEMBER", "order-1"));
    keep(store, "order-3");
    ASSERT_EQ(snapshot.count(), 2U);
    EXPECT_EQ(command_text(snapshot, 0), "order-1");
    EXPECT_EQ(command_text(snapshot, 1), "order-2");
}

TEST(PacedResendTest, TakingASnapshotAgainReplacesTheOldOne) {
    UnansweredCommandStore store(4096, 16);
    keep(store, "order-1");
    UnansweredSnapshot snapshot;
    snapshot.take(store);
    static_cast<void>(store.answered("MEMBER", "order-1"));
    keep(store, "order-2");
    snapshot.take(store);
    ASSERT_EQ(snapshot.count(), 1U);
    EXPECT_EQ(command_text(snapshot, 0), "order-2");
}

TEST(PacedResendTest, EveryCommandIsSentOnceInBatchesOfAtMostTheBatchSize) {
    PacedResend resend;
    const size_t total = 2 * PacedResend::batch_size + 7;
    resend.start(total);
    std::vector<size_t> sent;
    std::vector<size_t> batch_sizes;
    bool more = true;
    while (more) {
        const size_t before = sent.size();
        more = resend.send_next_batch([&sent](size_t index) { sent.push_back(index); });
        batch_sizes.push_back(sent.size() - before);
    }
    ASSERT_EQ(sent.size(), total);
    for (size_t index = 0; index < total; ++index) {
        ASSERT_EQ(sent[index], index);
    }
    EXPECT_EQ(batch_sizes, (std::vector<size_t>{PacedResend::batch_size, PacedResend::batch_size, 7}));
    EXPECT_FALSE(resend.active());
}

TEST(PacedResendTest, NothingToSendIsNotAnActiveResend) {
    PacedResend resend;
    resend.start(0);
    EXPECT_FALSE(resend.active());
    EXPECT_FALSE(resend.send_next_batch([](size_t) { FAIL() << "nothing should be sent"; }));
}

TEST(PacedResendTest, AnAbandonedResendSendsNothingMore) {
    PacedResend resend;
    resend.start(1000);
    static_cast<void>(resend.send_next_batch([](size_t) {}));
    resend.abandon();
    EXPECT_FALSE(resend.active());
    EXPECT_FALSE(resend.send_next_batch([](size_t) { FAIL() << "an abandoned resend sent a command"; }));
    EXPECT_EQ(resend.sent(), PacedResend::batch_size);
}
