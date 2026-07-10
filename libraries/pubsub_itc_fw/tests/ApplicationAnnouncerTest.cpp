// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/ApplicationAnnouncer.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>

using namespace pubsub_itc_fw;

namespace {

bool any_record_contains(const std::vector<std::string>& records, const std::string& text) {
    for (const auto& record : records) {
        if (record.find(text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

} // namespaces

TEST(ApplicationAnnouncerTest, AnnounceLogsOneRecordWithBuildInfo) {
    std::vector<std::string> records;
    QuillLogger logger(FwLogLevel::Info, [&records](const std::string& record) { records.push_back(record); });

    ApplicationAnnouncer::announce(logger, "test_app");

    ASSERT_EQ(records.size(), 1u);
    EXPECT_TRUE(any_record_contains(records, "test_app"));
    EXPECT_TRUE(any_record_contains(records, "version="));
    EXPECT_TRUE(any_record_contains(records, "pid="));
    EXPECT_TRUE(any_record_contains(records, "built="));
    EXPECT_TRUE(any_record_contains(records, "branch="));
    EXPECT_TRUE(any_record_contains(records, "sha="));
    EXPECT_TRUE(any_record_contains(records, "host="));
}

TEST(ApplicationAnnouncerTest, AnnounceIsSuppressedBelowThreshold) {
    std::vector<std::string> records;
    QuillLogger logger(FwLogLevel::Error, [&records](const std::string& record) { records.push_back(record); });

    ApplicationAnnouncer::announce(logger, "test_app");

    EXPECT_TRUE(records.empty());
}
