// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <string>
#include <thread>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/PreconditionAssertion.hpp>
#include <pubsub_itc_fw/ThreadConfinementCheck.hpp>

using pubsub_itc_fw::PreconditionAssertion;
using pubsub_itc_fw::ThreadConfinementCheck;

namespace {

// Calls check() on another thread and says whether it was refused.
bool refused_on_another_thread(const ThreadConfinementCheck& confinement) {
    bool refused = false;
    std::thread other([&confinement, &refused]() {
        try {
            confinement.check();
        } catch (const PreconditionAssertion&) {
            refused = true;
        }
    });
    other.join();
    return refused;
}

} // un-named namespace

TEST(ThreadConfinementCheckTest, TheFirstThreadToCheckOwnsTheStateAndMayCheckAgain) {
    const ThreadConfinementCheck confinement("the state under test");
    confinement.check();
    EXPECT_NO_THROW(confinement.check());
    EXPECT_EQ(confinement.known_breach_count(), 0);
}

TEST(ThreadConfinementCheckTest, ASecondThreadIsRefused) {
    const ThreadConfinementCheck confinement("the state under test");
    confinement.check();
    EXPECT_TRUE(refused_on_another_thread(confinement)) << "a second thread was allowed to touch state owned by one thread";
}

TEST(ThreadConfinementCheckTest, ARefusalNamesWhatWasTouched) {
    const ThreadConfinementCheck confinement("the state under test");
    confinement.check();
    std::string message;
    std::thread other([&confinement, &message]() {
        try {
            confinement.check();
        } catch (const PreconditionAssertion& e) {
            message = e.what();
        }
    });
    other.join();
    EXPECT_NE(message.find("the state under test"), std::string::npos) << message;
}

TEST(ThreadConfinementCheckTest, UnderAKnownBreachASecondThreadIsCountedNotRefused) {
    ThreadConfinementCheck confinement("the state under test");
    confinement.mark_known_breach("a known breach, removed in a later phase");
    confinement.check();
    EXPECT_FALSE(refused_on_another_thread(confinement));
    EXPECT_FALSE(refused_on_another_thread(confinement));
    EXPECT_EQ(confinement.known_breach_count(), 2);
    EXPECT_STREQ(confinement.known_breach(), "a known breach, removed in a later phase");
}

TEST(ThreadConfinementCheckTest, UnderAKnownBreachTheOwnerIsNotCounted) {
    ThreadConfinementCheck confinement("the state under test");
    confinement.mark_known_breach("a known breach, removed in a later phase");
    confinement.check();
    confinement.check();
    confinement.check();
    EXPECT_EQ(confinement.known_breach_count(), 0);
}

TEST(ThreadConfinementCheckTest, WithNoKnownBreachThereIsNoneToReport) {
    const ThreadConfinementCheck confinement("the state under test");
    EXPECT_EQ(confinement.known_breach(), nullptr);
}
