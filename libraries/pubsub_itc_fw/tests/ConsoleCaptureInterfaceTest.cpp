// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/ConsoleCaptureInterface.hpp>

using namespace pubsub_itc_fw;

namespace {

constexpr size_t default_max_line_length = 8U << 10;

// Intercepts completed records instead of logging them, and exposes the protected
// assembly entry points so a test can drive them directly -- exactly the seam the
// interface exists to provide.
class RecordingConsoleCapture : public ConsoleCaptureInterface {
  public:
    explicit RecordingConsoleCapture(size_t max_line_length = default_max_line_length) : ConsoleCaptureInterface(max_line_length) {}

    using ConsoleCaptureInterface::feed_bytes;
    using ConsoleCaptureInterface::flush_pending;

    std::vector<std::string> lines{};

  private:
    void log_line(const std::string& line) override {
        lines.push_back(line);
    }
};

void feed(RecordingConsoleCapture& capture, const std::string& bytes) {
    capture.feed_bytes(bytes.data(), bytes.size());
}

} // namespaces

TEST(ConsoleCaptureInterfaceTest, SingleLineEmittedOnNewline) {
    RecordingConsoleCapture capture;
    feed(capture, "hello\n");

    EXPECT_EQ(capture.lines, (std::vector<std::string>{"hello"}));
}

TEST(ConsoleCaptureInterfaceTest, MultipleLinesInOneBuffer) {
    RecordingConsoleCapture capture;
    feed(capture, "one\ntwo\nthree\n");

    EXPECT_EQ(capture.lines, (std::vector<std::string>{"one", "two", "three"}));
}

TEST(ConsoleCaptureInterfaceTest, LineSplitAcrossTwoFeedsIsReassembled) {
    RecordingConsoleCapture capture;
    feed(capture, "hel");
    EXPECT_TRUE(capture.lines.empty());

    feed(capture, "lo\n");
    EXPECT_EQ(capture.lines, (std::vector<std::string>{"hello"}));
}

TEST(ConsoleCaptureInterfaceTest, NewlinelessRunIsBufferedUntilFlush) {
    RecordingConsoleCapture capture;
    feed(capture, "partial");
    EXPECT_TRUE(capture.lines.empty());

    capture.flush_pending();
    EXPECT_EQ(capture.lines, (std::vector<std::string>{"partial"}));
}

TEST(ConsoleCaptureInterfaceTest, EmptyLinesArePreserved) {
    RecordingConsoleCapture capture;
    feed(capture, "\n\n");

    EXPECT_EQ(capture.lines, (std::vector<std::string>{"", ""}));
}

TEST(ConsoleCaptureInterfaceTest, ForceFlushAtMaxLineLength) {
    RecordingConsoleCapture capture(4);
    feed(capture, "abcdefg"); // no newline

    EXPECT_EQ(capture.lines, (std::vector<std::string>{"abcd"}));

    capture.flush_pending();
    EXPECT_EQ(capture.lines, (std::vector<std::string>{"abcd", "efg"}));
}

TEST(ConsoleCaptureInterfaceTest, CarriageReturnIsKeptInTheRecord) {
    RecordingConsoleCapture capture;
    feed(capture, "line\r\n");

    EXPECT_EQ(capture.lines, (std::vector<std::string>{"line\r"}));
}

TEST(ConsoleCaptureInterfaceTest, FlushAfterCompleteLineDoesNotEmitEmptyRecord) {
    RecordingConsoleCapture capture;
    feed(capture, "done\n");
    capture.flush_pending();

    EXPECT_EQ(capture.lines, (std::vector<std::string>{"done"}));
}

TEST(ConsoleCaptureInterfaceTest, FlushWithNothingBufferedIsANoOp) {
    RecordingConsoleCapture capture;
    capture.flush_pending();

    EXPECT_TRUE(capture.lines.empty());
}
