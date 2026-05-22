// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <pubsub_itc_fw/Crc32.hpp>

namespace pubsub_itc_fw {

namespace {

uint32_t crc32_table[256];

bool build_table() noexcept
{
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        crc32_table[i] = c;
    }
    return true;
}

const bool table_ready = build_table();

} // anonymous namespace

void Crc32::feed(const void* data, size_t len) noexcept
{
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; ++i) {
        state_ = crc32_table[(state_ ^ p[i]) & 0xFFu] ^ (state_ >> 8);
    }
}

uint32_t Crc32::finalize() noexcept
{
    const uint32_t result = state_ ^ 0xFFFFFFFFu;
    state_ = 0xFFFFFFFFu;
    return result;
}

uint32_t Crc32::compute(const void* data, size_t len) noexcept
{
    Crc32 c;
    c.feed(data, len);
    return c.finalize();
}

} // namespace pubsub_itc_fw
