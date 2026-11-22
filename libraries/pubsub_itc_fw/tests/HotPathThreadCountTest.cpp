// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/HotPathThreadCount.hpp>

namespace {

// Runs the query with the given arguments, returning whether it was answered and what it printed.
std::pair<bool, std::string> ask(std::vector<std::string> arguments) {
    std::vector<char*> values;
    values.reserve(arguments.size());
    for (std::string& argument : arguments) {
        values.push_back(argument.data());
    }
    std::ostringstream captured;
    std::streambuf* const original = std::cout.rdbuf(captured.rdbuf());
    const bool answered = pubsub_itc_fw::answer_hot_path_thread_count_query(static_cast<int>(values.size()), values.data(), 3);
    std::cout.rdbuf(original);
    return {answered, captured.str()};
}

TEST(HotPathThreadCountTest, TheQueryIsAnsweredWithTheCountAndNothingElse) {
    const auto [answered, printed] = ask({"sequencer", "--hot-path-thread-count"});
    EXPECT_TRUE(answered);
    EXPECT_EQ(printed, "3\n");
}

TEST(HotPathThreadCountTest, AnythingElseIsLeftToTheComponent) {
    EXPECT_EQ(ask({"sequencer"}), std::make_pair(false, std::string{})) << "no argument";
    EXPECT_EQ(ask({"sequencer", "--config"}), std::make_pair(false, std::string{})) << "another argument";
    EXPECT_EQ(ask({"sequencer", "--hot-path-thread-count", "extra"}), std::make_pair(false, std::string{})) << "the query with more after it";
}

} // namespaces
