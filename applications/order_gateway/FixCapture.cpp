// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "FixCapture.hpp"

#include <cstdio>
#include <cstring>

#include <pubsub_itc_fw/LoggingMacros.hpp>

namespace order_gateway {

namespace {

void write_uint32_le(FILE* file, uint32_t value) {
    uint8_t buf[4];
    buf[0] = static_cast<uint8_t>(value & 0xFFU);
    buf[1] = static_cast<uint8_t>((value >> 8) & 0xFFU);
    buf[2] = static_cast<uint8_t>((value >> 16) & 0xFFU);
    buf[3] = static_cast<uint8_t>((value >> 24) & 0xFFU);
    std::fwrite(buf, 1, sizeof(buf), file);
}

void write_int64_le(FILE* file, int64_t value) {
    uint8_t buf[8];
    const auto u = static_cast<uint64_t>(value);
    buf[0] = static_cast<uint8_t>(u & 0xFFU);
    buf[1] = static_cast<uint8_t>((u >> 8) & 0xFFU);
    buf[2] = static_cast<uint8_t>((u >> 16) & 0xFFU);
    buf[3] = static_cast<uint8_t>((u >> 24) & 0xFFU);
    buf[4] = static_cast<uint8_t>((u >> 32) & 0xFFU);
    buf[5] = static_cast<uint8_t>((u >> 40) & 0xFFU);
    buf[6] = static_cast<uint8_t>((u >> 48) & 0xFFU);
    buf[7] = static_cast<uint8_t>((u >> 56) & 0xFFU);
    std::fwrite(buf, 1, sizeof(buf), file);
}

} // namespace

FixCapture::FixCapture(const std::string& file_path, pubsub_itc_fw::QuillLogger& logger, size_t queue_depth)
    : file_path_(file_path)
    , logger_(logger)
    , queue_depth_(queue_depth)
    , writer_thread_([this]() { writer_loop(); }) {
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info,
               "FixCapture: capture started, writing to {}", file_path_);
}

FixCapture::~FixCapture() {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        shutdown_ = true;
    }
    cv_.notify_one();
    writer_thread_.join();
}

void FixCapture::capture(Direction direction, const uint8_t* data, size_t size, int64_t timestamp_ns) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pending_.size() >= queue_depth_) {
        PUBSUB_LOG_STR(logger_, pubsub_itc_fw::FwLogLevel::Warning,
                       "FixCapture: queue full -- dropping record");
        return;
    }
    Record rec;
    rec.timestamp_ns = timestamp_ns;
    rec.direction = direction;
    rec.bytes.assign(data, data + size);
    pending_.push_back(std::move(rec));
    cv_.notify_one();
}

void FixCapture::writer_loop() {
    FILE* file = std::fopen(file_path_.c_str(), "wb");
    if (file == nullptr) {
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Error,
                   "FixCapture: failed to open capture file: {}", file_path_);
        return;
    }

    std::vector<Record> batch;
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this]() { return !pending_.empty() || shutdown_; });
            batch.swap(pending_);
        }

        for (const Record& rec : batch) {
            write_uint32_le(file, static_cast<uint32_t>(rec.bytes.size()));
            write_int64_le(file, rec.timestamp_ns);
            std::fwrite(&rec.direction, 1, 1, file);
            std::fwrite(rec.bytes.data(), 1, rec.bytes.size(), file);
        }
        if (!batch.empty()) {
            std::fflush(file);
        }
        batch.clear();

        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (shutdown_ && pending_.empty()) {
                break;
            }
        }
    }

    std::fclose(file);
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info,
               "FixCapture: capture file closed: {}", file_path_);
}

} // namespace order_gateway
