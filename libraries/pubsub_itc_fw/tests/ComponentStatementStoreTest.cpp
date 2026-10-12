// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <fstream>
#include <string>

#include <unistd.h>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/ComponentStatementStore.hpp>

#include <pubsub_itc_fw/LeaderStatement.hpp>
#include <pubsub_itc_fw/tests_common/ScratchDirectory.hpp>

namespace {

// Two pairs, by the numbers their instances put on their lease messages. Any positive numbers will do.
constexpr int32_t first_pair = 1;
constexpr int32_t second_pair = 2;

// A directory of its own per test, removed afterwards, so a leftover record from one case cannot make
// another pass.
class ComponentStatementStoreTest : public ::testing::Test {
  protected:
    void SetUp() override {
        dir_ = pubsub_itc_fw::tests_common::make_scratch_directory("component_statement_store_test");
        path_ = dir_ + "/arbiter_promise.state.component_statements";
    }

    void TearDown() override {
        ::unlink(path_.c_str());
        ::unlink((path_ + ".tmp").c_str());
        ::rmdir(dir_.c_str());
    }

    std::string dir_;
    std::string path_;
};

} // un-named namespace

TEST_F(ComponentStatementStoreTest, StatementsAreReadBackForEachGroup) {
    const pubsub_itc_fw::ComponentStatementStore store(path_);
    pubsub_itc_fw::ComponentStatementStore::Statements statements;
    statements[first_pair] = pubsub_itc_fw::LeaderStatement{1, 5, 3, false};
    statements[second_pair] = pubsub_itc_fw::LeaderStatement{2, 6, 1, true};
    ASSERT_TRUE(store.save(statements));
    const pubsub_itc_fw::ComponentStatementStore::Statements read = store.load();
    ASSERT_EQ(read.size(), 2U);
    EXPECT_TRUE(read.at(first_pair).same_as(statements[first_pair]));
    EXPECT_FALSE(read.at(first_pair).peer_may_lead);
    EXPECT_TRUE(read.at(second_pair).same_as(statements[second_pair]));
    EXPECT_TRUE(read.at(second_pair).peer_may_lead);
}

TEST_F(ComponentStatementStoreTest, AMissingFileReadsAsNoStatements) {
    EXPECT_TRUE(pubsub_itc_fw::ComponentStatementStore(path_).load().empty());
}

TEST_F(ComponentStatementStoreTest, ADamagedLineIsSkippedAndTheRestRead) {
    std::ofstream(path_) << "1 3 5 1 0\n"
                         << "garbage\n"
                         << "2 1 6 2 1\n";
    const pubsub_itc_fw::ComponentStatementStore::Statements read = pubsub_itc_fw::ComponentStatementStore(path_).load();
    ASSERT_EQ(read.size(), 1U);
    EXPECT_EQ(read.count(first_pair), 0U) << "a statement naming instance 3 as its leader was believed";
    EXPECT_EQ(read.at(second_pair).leader_id, 1);
}

TEST_F(ComponentStatementStoreTest, WithNoPathNothingIsKept) {
    const pubsub_itc_fw::ComponentStatementStore store("");
    pubsub_itc_fw::ComponentStatementStore::Statements statements;
    statements[first_pair] = pubsub_itc_fw::LeaderStatement{1, 5, 3, false};
    EXPECT_FALSE(store.save(statements));
    EXPECT_TRUE(store.load().empty());
}
