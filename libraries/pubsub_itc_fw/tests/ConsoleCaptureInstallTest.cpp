// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

#include <unistd.h>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/ConsoleCapture.hpp>
#include <pubsub_itc_fw/ConsoleCaptureInterface.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>

using namespace pubsub_itc_fw;

namespace {

constexpr size_t default_max_line_length = 8U << 10;

// A capture sink that records completed lines instead of logging them, so the
// real capture engine can be installed and its output asserted without Quill.
class RecordingConsoleCapture : public ConsoleCaptureInterface {
  public:
    RecordingConsoleCapture() : ConsoleCaptureInterface(default_max_line_length) {}

    using ConsoleCaptureInterface::drain_fd_until_eof;

    bool contains(const std::string& text) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& line : lines_) {
            if (line.find(text) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    size_t count() {
        std::lock_guard<std::mutex> lock(mutex_);
        return lines_.size();
    }

  private:
    void log_line(const std::string& line) override {
        std::lock_guard<std::mutex> lock(mutex_);
        lines_.push_back(line);
    }

    std::mutex mutex_;
    std::vector<std::string> lines_{};
};

ConsoleCapture::Options options_without_handlers() {
    ConsoleCapture::Options options;
    options.install_terminate_handler = false;
    options.install_fatal_signal_handlers = false;
    return options;
}

} // namespaces

TEST(ConsoleCaptureInstallTest, DrainFdUntilEofReadsFromARealPipe) {
    int fds[2] = {-1, -1};
    ASSERT_EQ(::pipe(fds), 0);

    const char data[] = "alpha\nbeta\ngamma\n";
    ASSERT_EQ(::write(fds[1], data, sizeof(data) - 1), static_cast<ssize_t>(sizeof(data) - 1));
    ::close(fds[1]); // EOF for the reader

    RecordingConsoleCapture recorder;
    recorder.drain_fd_until_eof(fds[0]);
    ::close(fds[0]);

    EXPECT_EQ(recorder.count(), 3u);
    EXPECT_TRUE(recorder.contains("alpha"));
    EXPECT_TRUE(recorder.contains("beta"));
    EXPECT_TRUE(recorder.contains("gamma"));
}

TEST(ConsoleCaptureInstallTest, InstallEngineCapturesStdoutAndStderr) {
    RecordingConsoleCapture recorder;
    {
        auto handle = ConsoleCapture::install_engine(recorder, options_without_handlers());
        ASSERT_TRUE(handle.installed());

        const char out[] = "line on stdout\n";
        const char err[] = "line on stderr\n";
        ::write(STDOUT_FILENO, out, sizeof(out) - 1);
        ::write(STDERR_FILENO, err, sizeof(err) - 1);
        // Leaving this scope destroys the handle: fds restored, reader drained and joined.
    }

    EXPECT_TRUE(recorder.contains("line on stdout"));
    EXPECT_TRUE(recorder.contains("line on stderr"));
}

TEST(ConsoleCaptureInstallTest, SecondInstallWhileActiveReportsNotInstalled) {
    RecordingConsoleCapture first_sink;
    auto first = ConsoleCapture::install_engine(first_sink, options_without_handlers());
    ASSERT_TRUE(first.installed());

    RecordingConsoleCapture second_sink;
    auto second = ConsoleCapture::install_engine(second_sink, options_without_handlers());
    EXPECT_FALSE(second.installed());
}

TEST(ConsoleCaptureInstallTest, InstallEngineWithHandlersOpensCrashDumpAndRestores) {
    const std::string dump_path = "/dev/shm/console_capture_install_test.dump";
    ::unlink(dump_path.c_str());

    RecordingConsoleCapture recorder;
    {
        ConsoleCapture::Options options;
        options.install_terminate_handler = true;
        options.install_fatal_signal_handlers = true;
        options.crash_dump_file_path = dump_path;

        auto handle = ConsoleCapture::install_engine(recorder, options);
        ASSERT_TRUE(handle.installed());
        // Leaving this scope removes the handlers and restores fds 1 and 2.
    }

    // The crash-dump file is opened (O_CREAT) at install time.
    EXPECT_EQ(::access(dump_path.c_str(), F_OK), 0);
    ::unlink(dump_path.c_str());
}

TEST(ConsoleCaptureInstallTest, QuillInstallPathCapturesWithoutReadingTheLog) {
    const std::string log_path = "/dev/shm/console_capture_quill_install_test.log";
    ::unlink(log_path.c_str());
    {
        QuillLogger logger(log_path, FileOpenMode{FileOpenMode::Truncate}, FwLogLevel::Debug, FwLogLevel::Critical);

        auto capture = ConsoleCapture::install(logger, options_without_handlers());
        ASSERT_NE(capture, nullptr);

        const char line[] = "captured through the quill path\n";
        ::write(STDOUT_FILENO, line, sizeof(line) - 1);
        // Leaving this scope drains and logs via Quill (file sink), then destroys the
        // logger. The log is never read back, so no Quill flush is involved.
    }

    ::unlink(log_path.c_str());
}
