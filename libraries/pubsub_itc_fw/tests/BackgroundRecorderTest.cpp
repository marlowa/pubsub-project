// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <thread>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/BackgroundRecorder.hpp>

namespace pubsub_itc_fw::tests {

namespace {

// A fixed-size record, as the arbiter's record of statements is.
struct Numbers {
    std::array<int64_t, 4> values{};
    int count{0};
};

// What the write function sees and does, shared with it so that it outlives a recorder whose write is stuck.
struct Disk {
    std::atomic<bool> block{false};
    std::atomic<bool> succeed{true};
    std::atomic<int> writes{0};
    std::atomic<int64_t> last_first_value{0};
};

BackgroundRecorder<Numbers>::Write writes_to(const std::shared_ptr<Disk>& disk) {
    return [disk](const Numbers& record) {
        while (disk->block.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        disk->last_first_value.store(record.values[0]);
        disk->writes.fetch_add(1);
        return disk->succeed.load();
    };
}

std::optional<bool> wait_for_result(BackgroundRecorder<Numbers>& recorder, std::chrono::milliseconds limit = std::chrono::milliseconds{5000}) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (std::optional<bool> outcome = recorder.result()) {
            return outcome;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return std::nullopt;
}

} // namespaces

TEST(BackgroundRecorderTest, AWriteIsStartedAndItsOutcomeReadLater) {
    auto disk = std::make_shared<Disk>();
    BackgroundRecorder<Numbers> recorder(writes_to(disk), "TestRecorder");
    Numbers record;
    record.values[0] = 42;
    record.count = 1;
    ASSERT_TRUE(recorder.start(record));
    const std::optional<bool> outcome = wait_for_result(recorder);
    ASSERT_TRUE(outcome.has_value());
    EXPECT_TRUE(*outcome);
    EXPECT_EQ(disk->last_first_value.load(), 42);
    EXPECT_FALSE(recorder.result().has_value()) << "an outcome is reported once";
}

TEST(BackgroundRecorderTest, WhileAWriteIsStuckTheAskingThreadIsNotHeldAndASecondWriteIsRefused) {
    auto disk = std::make_shared<Disk>();
    disk->block.store(true);
    BackgroundRecorder<Numbers> recorder(writes_to(disk), "TestRecorder");
    Numbers first;
    first.values[0] = 1;
    const auto before = std::chrono::steady_clock::now();
    ASSERT_TRUE(recorder.start(first));
    EXPECT_LT(std::chrono::steady_clock::now() - before, std::chrono::milliseconds{50}) << "starting a write waited for it";
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    EXPECT_TRUE(recorder.in_progress());
    EXPECT_FALSE(recorder.result().has_value());
    Numbers second;
    second.values[0] = 2;
    EXPECT_FALSE(recorder.start(second)) << "a second write was started while the first was in progress";
    disk->block.store(false);
    ASSERT_TRUE(wait_for_result(recorder).has_value());
    EXPECT_EQ(disk->last_first_value.load(), 1);
    EXPECT_FALSE(recorder.in_progress());
    ASSERT_TRUE(recorder.start(second));
    ASSERT_TRUE(wait_for_result(recorder).has_value());
    EXPECT_EQ(disk->last_first_value.load(), 2);
}

TEST(BackgroundRecorderTest, AFailedWriteIsReportedAsFailed) {
    auto disk = std::make_shared<Disk>();
    disk->succeed.store(false);
    BackgroundRecorder<Numbers> recorder(writes_to(disk), "TestRecorder");
    ASSERT_TRUE(recorder.start(Numbers{}));
    const std::optional<bool> outcome = wait_for_result(recorder);
    ASSERT_TRUE(outcome.has_value());
    EXPECT_FALSE(*outcome);
}

TEST(BackgroundRecorderTest, TheRecordIsCopiedWhenTheWriteStartsSoChangingItAfterwardsChangesNothing) {
    auto disk = std::make_shared<Disk>();
    disk->block.store(true);
    BackgroundRecorder<Numbers> recorder(writes_to(disk), "TestRecorder");
    Numbers record;
    record.values[0] = 7;
    ASSERT_TRUE(recorder.start(record));
    record.values[0] = 8;
    disk->block.store(false);
    ASSERT_TRUE(wait_for_result(recorder).has_value());
    EXPECT_EQ(disk->last_first_value.load(), 7);
}

TEST(BackgroundRecorderTest, ARecorderWhoseWriteIsStuckIsDestroyedWithinAboutASecond) {
    auto disk = std::make_shared<Disk>();
    disk->block.store(true);
    const auto before = std::chrono::steady_clock::now();
    {
        BackgroundRecorder<Numbers> recorder(writes_to(disk), "TestRecorder");
        ASSERT_TRUE(recorder.start(Numbers{}));
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    EXPECT_LT(std::chrono::steady_clock::now() - before, std::chrono::milliseconds{3000});
    disk->block.store(false);
}

} // namespaces
