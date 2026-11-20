// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <string>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>

namespace pubsub_itc_fw::tests {

// The service name is held in the object, so that copying a ConnectionID never allocates (BUG-0270).

TEST(ConnectionIDTest, HoldsANameOfTheMostCharactersAllowed) {
    const std::string longest(max_service_name_length, 'n');
    const ConnectionID id{7, std::string_view(longest)};
    EXPECT_EQ(id.service_name(), longest);
    EXPECT_EQ(id.get_value(), 7);
}

TEST(ConnectionIDTest, RefusesANameLongerThanAllowed) {
    const std::string too_long(max_service_name_length + 1, 'n');
    EXPECT_THROW(ConnectionID(7, std::string_view(too_long)), PreconditionAssertion);
}

TEST(ConnectionIDTest, ANameFromALiteralIsHeld) {
    const ConnectionID id{3, "sequencer_primary"};
    EXPECT_EQ(id.service_name(), "sequencer_primary");
}

TEST(ConnectionIDTest, ACopyKeepsTheNumberAndTheName) {
    const ConnectionID original{5, "inbound:11021"};
    const ConnectionID copy = original;
    EXPECT_EQ(copy, original);
    EXPECT_EQ(copy.service_name(), "inbound:11021");
}

TEST(ConnectionIDTest, ConnectionsAreTheSameWhenTheirNumbersAre) {
    EXPECT_EQ(ConnectionID(4, "a"), ConnectionID(4, "b"));
    EXPECT_NE(ConnectionID(4, "a"), ConnectionID(5, "a"));
}

TEST(ConnectionIDTest, AConnectionWithNoNameHasAnEmptyName) {
    EXPECT_TRUE(ConnectionID{9}.service_name().empty());
    EXPECT_FALSE(ConnectionID{}.is_valid());
}

} // namespaces
