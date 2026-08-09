#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>

namespace pubsub_itc_fw {

/**
 * @brief Opaque reference to an allocated chunk's owning slab.
 *
 * Packs the slab's registry slot in the low 32 bits and a generation counter in the high 32.
 * The slot is what ExpandableSlabAllocator indexes; the generation is what makes reusing a
 * slot safe.
 *
 * Slots are recycled, so a slot number alone no longer identifies a slab: the slab that held
 * it may have been destroyed and the slot handed to a new one. The generation is incremented
 * when a slot is released, so a handle held across that boundary no longer matches and
 * deallocate() rejects it rather than freeing into whichever slab now owns the slot.
 *
 * In its own header, and 64 bits wide, so that every stage carrying a chunk from allocate()
 * to deallocate() can hold one.
 *
 * **An enum class rather than a using-alias, deliberately.** As an alias for uint64_t it
 * converts implicitly to and from int, so a stage that stored a handle in an int would
 * discard the generation, compile without a diagnostic under -Wall -Wextra -Werror, index
 * the correct slot, and then be rejected as stale the first time a slot was recycled. A
 * distinct type makes that a compile error at every stage instead of relying on a warning
 * flag being switched on. The three functions below are the only sanctioned way in or out.
 */
enum class SlabHandle : uint64_t {};

/**
 * @brief Sentinel for "this message owns no slab chunk".
 *
 * Not -1: the handle is unsigned, so the old `slab_id >= 0` idiom cannot express absence.
 * Comparisons must be against this value.
 */
inline constexpr SlabHandle invalid_slab_handle = static_cast<SlabHandle>(~uint64_t{0});

/**
 * @brief Builds a handle from a registry slot and its generation.
 */
[[nodiscard]] constexpr SlabHandle make_slab_handle(int slot, uint32_t generation) {
    return static_cast<SlabHandle>((static_cast<uint64_t>(generation) << 32) | static_cast<uint32_t>(slot));
}

/**
 * @brief The registry slot a handle refers to.
 */
[[nodiscard]] constexpr int slab_handle_slot(SlabHandle handle) {
    return static_cast<int>(static_cast<uint64_t>(handle) & 0xFFFFFFFFULL);
}

/**
 * @brief The generation a handle was issued in.
 */
[[nodiscard]] constexpr uint32_t slab_handle_generation(SlabHandle handle) {
    return static_cast<uint32_t>(static_cast<uint64_t>(handle) >> 32);
}

/**
 * @brief Lets fmt and quill print a handle, which is otherwise unformattable.
 *
 * Found by argument-dependent lookup. Prints the packed value; use slab_handle_slot() and
 * slab_handle_generation() when a message wants the two parts separately.
 */
[[nodiscard]] constexpr uint64_t format_as(SlabHandle handle) {
    return static_cast<uint64_t>(handle);
}

} // namespaces
