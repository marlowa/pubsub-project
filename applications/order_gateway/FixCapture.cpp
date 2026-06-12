// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "FixCapture.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>

#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>

namespace order_gateway {

namespace {

void write_uint32_le(FILE* file, uint32_t value) {
    uint8_t buffer[4];
    buffer[0] = static_cast<uint8_t>(value         & 0xFFU);
    buffer[1] = static_cast<uint8_t>((value >>  8) & 0xFFU);
    buffer[2] = static_cast<uint8_t>((value >> 16) & 0xFFU);
    buffer[3] = static_cast<uint8_t>((value >> 24) & 0xFFU);
    std::fwrite(buffer, 1, sizeof(buffer), file);
}

void write_int64_le(FILE* file, int64_t value) {
    uint8_t buffer[8];
    const auto u = static_cast<uint64_t>(value);
    buffer[0] = static_cast<uint8_t>(u         & 0xFFU);
    buffer[1] = static_cast<uint8_t>((u >>  8) & 0xFFU);
    buffer[2] = static_cast<uint8_t>((u >> 16) & 0xFFU);
    buffer[3] = static_cast<uint8_t>((u >> 24) & 0xFFU);
    buffer[4] = static_cast<uint8_t>((u >> 32) & 0xFFU);
    buffer[5] = static_cast<uint8_t>((u >> 40) & 0xFFU);
    buffer[6] = static_cast<uint8_t>((u >> 48) & 0xFFU);
    buffer[7] = static_cast<uint8_t>((u >> 56) & 0xFFU);
    std::fwrite(buffer, 1, sizeof(buffer), file);
}

} // un-named namespace

FixCapture::FixCapture(const std::string& file_path, pubsub_itc_fw::QuillLogger& logger,
                       size_t ring_bytes)
    : file_path_(file_path)
    , logger_(logger)
    , ring_(ring_bytes, 0)
    , capacity_(ring_bytes) {
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info,
               "FixCapture: capture started, writing to {} (ring {} MB)",
               file_path_, ring_bytes / (1024 * 1024));
    writer_thread_.start([this]() { writer_loop(); });
}

FixCapture::~FixCapture() {
    shutdown_.store(true, std::memory_order_release);
    writer_thread_.join();
}

void FixCapture::capture(Direction direction, const uint8_t* data, size_t size,
                         int64_t timestamp_ns) {
    // Total ring space needed: header + payload, padded to 4-byte alignment.
    // Plus header_bytes for a possible sentinel if the record doesn't fit
    // before the end of the ring (worst case: write a sentinel then wrap).
    const size_t needed = slot_bytes(size) + header_bytes;

    const size_t write = write_offset_.load(std::memory_order_relaxed);
    const size_t read  = read_offset_.load(std::memory_order_acquire);

    if (write - read + needed > capacity_) {
        PUBSUB_LOG_STR(logger_, pubsub_itc_fw::FwLogLevel::Warning, "FixCapture: ring buffer full -- dropping record");
        return;
    }

    size_t pos = write % capacity_;

    // If the record doesn't fit contiguously before the end of the ring,
    // write a sentinel at the current position and wrap to the start.
    if (pos + slot_bytes(size) > capacity_) {
        // Write sentinel header at pos (payload_size = SENTINEL, rest don't matter).
        uint8_t* p = ring_.data() + pos;
        const uint32_t s = sentinel_size;
        std::memcpy(p, &s, 4);
        // Advance write_offset to skip the remaining bytes at the end of the ring.
        const size_t skip = capacity_ - pos;
        pos = 0;
        write_offset_.store(write + skip, std::memory_order_relaxed);
    }

    // Pack the record header directly into the ring.
    uint8_t* slot = ring_.data() + pos;

    const auto payload_size = static_cast<uint32_t>(size);
    std::memcpy(slot,      &payload_size,  4);
    std::memcpy(slot +  4, &timestamp_ns,  8);
    slot[12] = static_cast<uint8_t>(direction);
    // slot[13..15]: padding (already zeroed from construction)

    std::memcpy(slot + header_bytes, data, size);

    write_offset_.store(write_offset_.load(std::memory_order_relaxed) + slot_bytes(size),
                        std::memory_order_release);
}

void FixCapture::writer_loop() {
    auto file_deleter = [](FILE* f) { if (f != nullptr) { std::fclose(f); } };
    std::unique_ptr<FILE, decltype(file_deleter)> file(
        std::fopen(file_path_.c_str(), "wb"), file_deleter);
    if (!file) {
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Error,
                   "FixCapture: failed to open capture file: {}", file_path_);
        return;
    }

    constexpr auto flush_interval = std::chrono::milliseconds{200};
    auto last_flush = std::chrono::steady_clock::now();
    bool dirty = false;

    auto drain = [&]() {
        size_t read        = read_offset_.load(std::memory_order_relaxed);
        const size_t write = write_offset_.load(std::memory_order_acquire);

        while (read < write) {
            const size_t pos = read % capacity_;
            const uint8_t* slot = ring_.data() + pos;

            uint32_t payload_size{};
            std::memcpy(&payload_size, slot, 4);

            if (payload_size == sentinel_size) {
                // Sentinel: skip remaining bytes at end of ring, wrap to start.
                const size_t skip = capacity_ - pos;
                read += skip;
                read_offset_.store(read, std::memory_order_release);
                continue;
            }

            int64_t timestamp_ns{};
            std::memcpy(&timestamp_ns, slot + 4, 8);
            const uint8_t direction = slot[12];
            const uint8_t* payload  = slot + header_bytes;

            write_uint32_le(file.get(), payload_size);
            write_int64_le(file.get(), timestamp_ns);
            std::fwrite(&direction, 1, 1, file.get());
            std::fwrite(payload, 1, static_cast<size_t>(payload_size), file.get());

            read += slot_bytes(static_cast<size_t>(payload_size));
            read_offset_.store(read, std::memory_order_release);
            dirty = true;
        }
    };

    for (;;) {
        drain();

        const auto now = std::chrono::steady_clock::now();
        if (dirty && (now - last_flush) >= flush_interval) {
            std::fflush(file.get());
            last_flush = now;
            dirty = false;
        }

        if (shutdown_.load(std::memory_order_acquire)) {
            drain();
            std::fflush(file.get());
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }

    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info,
               "FixCapture: capture file closed: {}", file_path_);
}

} // namespaces
