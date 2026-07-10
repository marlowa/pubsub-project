// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <stdexcept>
#include <string>

#include <fcntl.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/ConsoleCaptureCrashDump.hpp>

using namespace pubsub_itc_fw;

namespace {

std::string read_whole_file(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY);
    std::string content;
    if (fd < 0) {
        return content;
    }
    char buffer[4096];
    ssize_t n;
    while ((n = ::read(fd, buffer, sizeof(buffer))) > 0) {
        content.append(buffer, static_cast<size_t>(n));
    }
    ::close(fd);
    return content;
}

// Fills a fresh pipe with the given bytes and returns its read end (write end
// already closed, so a drain reads the bytes then sees EOF).
int pipe_holding(const std::string& bytes) {
    int fds[2] = {-1, -1};
    if (::pipe(fds) != 0) {
        return -1;
    }
    if (!bytes.empty()) {
        const ssize_t written = ::write(fds[1], bytes.data(), bytes.size());
        if (written != static_cast<ssize_t>(bytes.size())) {
            ::close(fds[1]);
            ::close(fds[0]);
            return -1;
        }
    }
    ::close(fds[1]);
    return fds[0];
}

} // namespaces

class ConsoleCaptureCrashDumpTest : public ::testing::Test {
  protected:
    void SetUp() override {
        dump_path_ = "/dev/shm/console_capture_crash_dump_test.dump";
        ::unlink(dump_path_.c_str());
        dump_fd_ = ::open(dump_path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        ASSERT_GE(dump_fd_, 0);
    }

    void TearDown() override {
        if (dump_fd_ >= 0) {
            ::close(dump_fd_);
        }
        ::unlink(dump_path_.c_str());
    }

    std::string dump_contents() {
        return read_whole_file(dump_path_);
    }

    std::string dump_path_{};
    int dump_fd_ = -1;
};

TEST_F(ConsoleCaptureCrashDumpTest, WriteAllRawWritesTheEntireBuffer) {
    const std::string payload = "raw bytes to the dump";
    console_capture_detail::write_all_raw(dump_fd_, payload.data(), payload.size());
    ::close(dump_fd_);
    dump_fd_ = -1;

    EXPECT_EQ(dump_contents(), payload);
}

TEST_F(ConsoleCaptureCrashDumpTest, DrainPipeToFdCopiesBufferedBytes) {
    const int read_fd = pipe_holding("bytes still in the pipe");
    ASSERT_GE(read_fd, 0);

    console_capture_detail::drain_pipe_to_fd(read_fd, dump_fd_);
    ::close(read_fd);
    ::close(dump_fd_);
    dump_fd_ = -1;

    EXPECT_EQ(dump_contents(), "bytes still in the pipe");
}

TEST_F(ConsoleCaptureCrashDumpTest, DrainPipeToFdWithInvalidFdsIsANoOp) {
    console_capture_detail::drain_pipe_to_fd(-1, dump_fd_);
    console_capture_detail::drain_pipe_to_fd(0, -1);
    ::close(dump_fd_);
    dump_fd_ = -1;

    EXPECT_TRUE(dump_contents().empty());
}

TEST_F(ConsoleCaptureCrashDumpTest, TerminateDumpRecordsActiveExceptionAndDrainsPipe) {
    const int read_fd = pipe_holding("last console line before terminate");
    ASSERT_GE(read_fd, 0);

    try {
        throw std::runtime_error("intentional terminate reason");
    } catch (...) {
        console_capture_detail::write_terminate_crash_dump(dump_fd_, read_fd);
    }
    ::close(read_fd);
    ::close(dump_fd_);
    dump_fd_ = -1;

    const std::string contents = dump_contents();
    EXPECT_NE(contents.find("std::terminate"), std::string::npos);
    EXPECT_NE(contents.find("intentional terminate reason"), std::string::npos);
    EXPECT_NE(contents.find("last console line before terminate"), std::string::npos);
}

TEST_F(ConsoleCaptureCrashDumpTest, TerminateDumpRecordsWhenNoActiveException) {
    console_capture_detail::write_terminate_crash_dump(dump_fd_, -1);
    ::close(dump_fd_);
    dump_fd_ = -1;

    EXPECT_NE(dump_contents().find("no active exception"), std::string::npos);
}

TEST_F(ConsoleCaptureCrashDumpTest, TerminateDumpRecordsNonStandardException) {
    try {
        throw 42;
    } catch (...) {
        console_capture_detail::write_terminate_crash_dump(dump_fd_, -1);
    }
    ::close(dump_fd_);
    dump_fd_ = -1;

    EXPECT_NE(dump_contents().find("unhandled non-standard exception"), std::string::npos);
}

TEST_F(ConsoleCaptureCrashDumpTest, SignalDumpWritesMarkerAndDrainsPipe) {
    const int read_fd = pipe_holding("console line before the signal");
    ASSERT_GE(read_fd, 0);

    console_capture_detail::write_signal_crash_dump(dump_fd_, read_fd);
    ::close(read_fd);
    ::close(dump_fd_);
    dump_fd_ = -1;

    const std::string contents = dump_contents();
    EXPECT_NE(contents.find("fatal signal captured"), std::string::npos);
    EXPECT_NE(contents.find("console line before the signal"), std::string::npos);
}
