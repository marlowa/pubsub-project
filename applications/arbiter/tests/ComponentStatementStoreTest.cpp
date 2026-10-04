// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <fstream>
#include <string>

#include <unistd.h>

#include <gtest/gtest.h>

#include "ComponentStatementStore.hpp"

#include <LeaderStatement.hpp>
#include <leader_follower.hpp>
#include <pubsub_itc_fw/tests_common/ScratchDirectory.hpp>

namespace {

constexpr auto sequencer = pubsub_itc_fw_app::ComponentGroup::sequencer;
constexpr auto matching_engine = pubsub_itc_fw_app::ComponentGroup::matching_engine;

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
    const arbiter::ComponentStatementStore store(path_);
    arbiter::ComponentStatementStore::Statements statements;
    statements[sequencer] = fix_common::LeaderStatement{1, 5, 3, false};
    statements[matching_engine] = fix_common::LeaderStatement{2, 6, 1, true};
    ASSERT_TRUE(store.save(statements));
    const arbiter::ComponentStatementStore::Statements read = store.load();
    ASSERT_EQ(read.size(), 2U);
    EXPECT_TRUE(read.at(sequencer).same_as(statements[sequencer]));
    EXPECT_FALSE(read.at(sequencer).peer_may_lead);
    EXPECT_TRUE(read.at(matching_engine).same_as(statements[matching_engine]));
    EXPECT_TRUE(read.at(matching_engine).peer_may_lead);
}

TEST_F(ComponentStatementStoreTest, AMissingFileReadsAsNoStatements) {
    EXPECT_TRUE(arbiter::ComponentStatementStore(path_).load().empty());
}

TEST_F(ComponentStatementStoreTest, ADamagedLineIsSkippedAndTheRestRead) {
    std::ofstream(path_) << "1 3 5 1 0\n"
                         << "garbage\n"
                         << "2 1 6 2 1\n";
    const arbiter::ComponentStatementStore::Statements read = arbiter::ComponentStatementStore(path_).load();
    ASSERT_EQ(read.size(), 1U);
    EXPECT_EQ(read.count(sequencer), 0U) << "a statement naming instance 3 as its leader was believed";
    EXPECT_EQ(read.at(matching_engine).leader_id, 1);
}

TEST_F(ComponentStatementStoreTest, WithNoPathNothingIsKept) {
    const arbiter::ComponentStatementStore store("");
    arbiter::ComponentStatementStore::Statements statements;
    statements[sequencer] = fix_common::LeaderStatement{1, 5, 3, false};
    EXPECT_FALSE(store.save(statements));
    EXPECT_TRUE(store.load().empty());
}
