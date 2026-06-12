#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>

namespace pubsub_itc_fw {

/**
 * @brief Incremental CRC32 (IEEE polynomial, 0xEDB88320 reflected).
 *
 * Typical use -- compute over multiple non-contiguous buffers:
 * @code
 *   Crc32 crc;
 *   crc.feed(header_ptr, header_size);
 *   crc.feed(payload_ptr, payload_size);
 *   uint32_t result = crc.finalize();
 * @endcode
 *
 * For a single contiguous buffer use the static helper:
 * @code
 *   uint32_t result = Crc32::compute(data, size);
 * @endcode
 */
class Crc32 {
  public:
    Crc32() = default;

    /** Feed more bytes into the running checksum. */
    void feed(const void* data, size_t length);

    /** Return the final checksum and reset state to initial. */
    [[nodiscard]] uint32_t finalize();

    /** Convenience: compute CRC32 of a single contiguous buffer. */
    [[nodiscard]] static uint32_t compute(const void* data, size_t length);

  private:
    uint32_t state_{0xFFFFFFFFU};
};

} // namespaces
