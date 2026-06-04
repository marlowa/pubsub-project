// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <FixCapture.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/tests_common/LoggerWithSink.hpp>

using order_gateway::FixCapture;

namespace {

struct ParsedRecord {
    uint32_t payload_size{0};
    int64_t  timestamp_ns{0};
    uint8_t  direction{0};
    std::vector<uint8_t> bytes;
};

std::vector<ParsedRecord> read_all_records(const std::string& path) {
    std::vector<ParsedRecord> records;
    FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return records;
    }
    for (;;) {
        uint8_t header[13]; // uint32_t(4) + int64_t(8) + uint8_t(1)
        if (std::fread(header, 1, sizeof(header), file) < sizeof(header)) {
            break;
        }
        ParsedRecord rec;
        std::memcpy(&rec.payload_size,  header,     4);
        std::memcpy(&rec.timestamp_ns,  header + 4, 8);
        rec.direction = header[12];
        if (rec.payload_size > 0) {
            rec.bytes.resize(rec.payload_size);
            if (std::fread(rec.bytes.data(), 1, rec.payload_size, file) < rec.payload_size) {
                break;
            }
        }
        records.push_back(std::move(rec));
    }
    std::fclose(file);
    return records;
}

} // namespace

class FixCaptureTest : public ::testing::Test {
  protected:
    void SetUp() override {
        capture_file_ = std::string(testing::TempDir()) + "fix_capture_test.bin";
        std::remove(capture_file_.c_str());
    }

    void TearDown() override {
        std::remove(capture_file_.c_str());
    }

    std::string capture_file_;
    pubsub_itc_fw::LoggerWithSink logger_;
};

TEST_F(FixCaptureTest, WritesRecordWithCorrectBinaryFormat) {
    const std::string data = "8=FIX.4.2\x01" "35=A\x01" "10=100\x01";
    const int64_t timestamp = 1700000000000000000LL;

    {
        FixCapture capture(capture_file_, logger_.logger, 1000);
        capture.capture(FixCapture::Direction::Inbound,
                        reinterpret_cast<const uint8_t*>(data.data()),
                        data.size(), timestamp);
    }

    const auto records = read_all_records(capture_file_);
    ASSERT_EQ(records.size(), 1U);

    const auto& rec = records[0];
    EXPECT_EQ(rec.payload_size, static_cast<uint32_t>(data.size()));
    EXPECT_EQ(rec.timestamp_ns, timestamp);
    EXPECT_EQ(rec.direction, static_cast<uint8_t>(FixCapture::Direction::Inbound));
    ASSERT_EQ(rec.bytes.size(), data.size());
    EXPECT_EQ(std::memcmp(rec.bytes.data(), data.data(), data.size()), 0);
}

TEST_F(FixCaptureTest, InboundAndOutboundDirectionBytesAreCorrect) {
    const std::string inbound  = "8=FIX.4.2\x01" "35=D\x01" "10=001\x01";
    const std::string outbound = "8=FIX.4.2\x01" "35=8\x01" "10=002\x01";
    const int64_t timestamp = 999000000000LL;

    {
        FixCapture capture(capture_file_, logger_.logger, 1000);
        capture.capture(FixCapture::Direction::Inbound,
                        reinterpret_cast<const uint8_t*>(inbound.data()),
                        inbound.size(), timestamp);
        capture.capture(FixCapture::Direction::Outbound,
                        reinterpret_cast<const uint8_t*>(outbound.data()),
                        outbound.size(), timestamp + 1);
    }

    const auto records = read_all_records(capture_file_);
    ASSERT_EQ(records.size(), 2U);
    EXPECT_EQ(records[0].direction, static_cast<uint8_t>(FixCapture::Direction::Inbound));
    EXPECT_EQ(records[1].direction, static_cast<uint8_t>(FixCapture::Direction::Outbound));
}

TEST_F(FixCaptureTest, MultipleRecordsWrittenInOrder) {
    constexpr int record_count = 10;

    {
        FixCapture capture(capture_file_, logger_.logger, 1000);
        for (int i = 0; i < record_count; ++i) {
            const std::string data = "record_" + std::to_string(i);
            capture.capture(FixCapture::Direction::Inbound,
                            reinterpret_cast<const uint8_t*>(data.data()),
                            data.size(),
                            static_cast<int64_t>(i) * 1'000'000'000LL);
        }
    }

    const auto records = read_all_records(capture_file_);
    ASSERT_EQ(records.size(), static_cast<size_t>(record_count));
    for (int i = 0; i < record_count; ++i) {
        const std::string expected = "record_" + std::to_string(i);
        ASSERT_EQ(records[i].bytes.size(), expected.size()) << "record " << i;
        EXPECT_EQ(std::memcmp(records[i].bytes.data(), expected.data(), expected.size()), 0)
            << "record " << i;
        EXPECT_EQ(records[i].timestamp_ns, static_cast<int64_t>(i) * 1'000'000'000LL)
            << "record " << i;
    }
}

TEST_F(FixCaptureTest, FileIsTruncatedWhenNewInstanceCreated) {
    const std::string first  = "first_message";
    const std::string second = "second_message";

    {
        FixCapture capture(capture_file_, logger_.logger, 1000);
        capture.capture(FixCapture::Direction::Inbound,
                        reinterpret_cast<const uint8_t*>(first.data()),
                        first.size(), 1LL);
    }
    {
        FixCapture capture(capture_file_, logger_.logger, 1000);
        capture.capture(FixCapture::Direction::Inbound,
                        reinterpret_cast<const uint8_t*>(second.data()),
                        second.size(), 2LL);
    }

    const auto records = read_all_records(capture_file_);
    ASSERT_EQ(records.size(), 1U);
    EXPECT_EQ(records[0].bytes.size(), second.size());
    EXPECT_EQ(std::memcmp(records[0].bytes.data(), second.data(), second.size()), 0);
}

TEST_F(FixCaptureTest, EmptyFileWhenNothingCaptured) {
    {
        FixCapture capture(capture_file_, logger_.logger, 1000);
    }

    const auto records = read_all_records(capture_file_);
    EXPECT_TRUE(records.empty());
}

TEST_F(FixCaptureTest, AllRecordsFlushedBeforeDestructorReturns) {
    constexpr int record_count = 500;

    {
        FixCapture capture(capture_file_, logger_.logger, record_count + 100);
        for (int i = 0; i < record_count; ++i) {
            const std::string data = "msg_" + std::to_string(i);
            capture.capture(FixCapture::Direction::Outbound,
                            reinterpret_cast<const uint8_t*>(data.data()),
                            data.size(), static_cast<int64_t>(i));
        }
    } // destructor must flush all records before returning

    const auto records = read_all_records(capture_file_);
    EXPECT_EQ(records.size(), static_cast<size_t>(record_count));
}
