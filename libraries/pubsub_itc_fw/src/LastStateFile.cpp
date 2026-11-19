// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <pubsub_itc_fw/LastStateFile.hpp>

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>

#include <pubsub_itc_fw/PubSubItcException.hpp>
#include <pubsub_itc_fw/StringUtils.hpp>
#include <pubsub_itc_fw/TradingDayOfFile.hpp>

namespace pubsub_itc_fw {

namespace {

// Flushes the directory holding @p path, so that a rename into it survives a loss of power, not only of the process.
void sync_directory_of(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    std::string directory{"."};
    if (slash == 0) {
        directory = "/";
    } else if (slash != std::string::npos) {
        directory = path.substr(0, slash);
    }
    const int descriptor = ::open(directory.c_str(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        return;
    }
    ::fsync(descriptor);
    ::close(descriptor);
}

} // namespaces

LastStateFile::LastStateFile(std::string path, int32_t trading_day)
    : path_(std::move(path)), temp_path_(path_.empty() ? std::string{} : path_ + ".tmp"), trading_day_(trading_day) {}

void LastStateFile::record(std::string_view phase) const {
    if (path_.empty()) {
        return;
    }
    const std::string line = fmt::format("{}{}\n", trading_day_field_text(trading_day_), phase);
    const auto fail = [this](const std::string& what) {
        const std::string error = StringUtils::get_errno_string();
        ::unlink(temp_path_.c_str());
        throw PubSubItcException(fmt::format("LastStateFile: could not {} '{}': {}", what, path_, error));
    };

    const int descriptor = ::open(temp_path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (descriptor < 0) {
        fail("create the temporary file for");
    }
    size_t written = 0;
    while (written < line.size()) {
        const ssize_t count = ::write(descriptor, line.data() + written, line.size() - written);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            ::close(descriptor);
            fail("write");
        }
        written += static_cast<size_t>(count);
    }
    if (::fsync(descriptor) != 0) {
        ::close(descriptor);
        fail("flush");
    }
    if (::close(descriptor) != 0) {
        fail("close");
    }
    if (::rename(temp_path_.c_str(), path_.c_str()) != 0) {
        fail("rename the temporary file over");
    }
    sync_directory_of(path_);
}

std::optional<LastStateFile::Recorded> LastStateFile::read(const std::string& path) {
    if (path.empty()) {
        return std::nullopt;
    }
    std::ifstream file(path);
    if (!file) {
        return std::nullopt;
    }
    std::string line;
    if (!std::getline(file, line)) {
        return std::nullopt;
    }
    std::string_view text(line);
    const int32_t trading_day = take_trading_day(text);
    if (trading_day == 0 || text.empty()) {
        return std::nullopt;
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    return Recorded{trading_day, std::string(text)};
}

bool LastStateFile::says(std::string_view phase) const {
    const std::optional<Recorded> recorded = read(path_);
    return recorded.has_value() && recorded->trading_day == trading_day_ && recorded->phase == phase;
}

} // namespaces
