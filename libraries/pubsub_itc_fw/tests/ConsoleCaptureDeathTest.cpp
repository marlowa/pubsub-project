// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <csignal>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <thread>

#include <fcntl.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/ConsoleCapture.hpp>
#include <pubsub_itc_fw/ConsoleCaptureInterface.hpp>

using namespace pubsub_itc_fw;

// These verify the crash-dump contract end to end: installing the real handlers and
// then actually crashing. They cannot raise the coverage figure -- a child that
// abort()s or dies from a re-raised signal never flushes gcov counters -- so the
// covered crash-dump logic is tested separately in ConsoleCaptureCrashDumpTest.

namespace {

class NullSink : public ConsoleCaptureInterface {
  public:
    NullSink() : ConsoleCaptureInterface(8U << 10) {}

  private:
    void log_line(const std::string&) override {}
};

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

} // namespaces

TEST(ConsoleCaptureDeathTest, TerminateHandlerWritesCrashDumpOnUncaughtException) {
    testing::GTEST_FLAG(death_test_style) = "threadsafe";
    const std::string dump_path = "/dev/shm/console_capture_death_terminate.dump";
    ::unlink(dump_path.c_str());

    EXPECT_EXIT(
        {
            ConsoleCapture::Options options;
            options.install_terminate_handler = true;
            options.crash_dump_file_path = dump_path;
            NullSink sink;
            auto handle = ConsoleCapture::install_engine(sink, options);
            // An exception escaping a thread's entry function calls std::terminate()
            // with the exception still active; gtest cannot intercept it (a plain
            // throw in the test body would be caught before terminate is reached).
            std::thread thrower([] { throw std::runtime_error("death-terminate-reason"); });
            thrower.join();
        },
        ::testing::KilledBySignal(SIGABRT), ".*");

    const std::string contents = read_whole_file(dump_path);
    EXPECT_NE(contents.find("std::terminate"), std::string::npos);
    EXPECT_NE(contents.find("death-terminate-reason"), std::string::npos);
    ::unlink(dump_path.c_str());
}

TEST(ConsoleCaptureDeathTest, SignalHandlerWritesCrashDumpOnRaisedSignal) {
    testing::GTEST_FLAG(death_test_style) = "threadsafe";
    const std::string dump_path = "/dev/shm/console_capture_death_signal.dump";
    ::unlink(dump_path.c_str());

    EXPECT_EXIT(
        {
            ConsoleCapture::Options options;
            options.install_terminate_handler = false;
            options.install_fatal_signal_handlers = true;
            options.crash_dump_file_path = dump_path;
            NullSink sink;
            auto handle = ConsoleCapture::install_engine(sink, options);
            ::raise(SIGSEGV);
        },
        ::testing::KilledBySignal(SIGSEGV), ".*");

    const std::string contents = read_whole_file(dump_path);
    EXPECT_NE(contents.find("fatal signal captured"), std::string::npos);
    ::unlink(dump_path.c_str());
}
