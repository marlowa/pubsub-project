#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cerrno>
#include <cstddef>

#include <sys/mman.h>

#include <pubsub_itc_fw/AllocationGrowthReporter.hpp>
#include <pubsub_itc_fw/GrowthReportingAllocator.hpp>

namespace pubsub_itc_fw {

/**
 * @brief What a MemoryLockingAllocator has locked, and the first failure to lock, if there was one.
 *
 * Owned by whoever creates the container, and written by the thread that allocates for it. Another thread may read it
 * only once that thread's allocating is known to be over, for example after a release store it makes once the container
 * is built.
 */
struct MemoryLockRecord {
    /// The bytes of the allocations locked and not yet freed. Exact only while `error` is 0: once a lock has failed,
    /// an allocation freed afterwards may be one that was never locked, and the owner gives up on the container anyway.
    size_t locked_bytes{0};
    /// The errno of the first allocation that could not be locked, or 0 when every one was.
    int error{0};
};

/**
 * @brief A standard allocator, for a container whose storage is not ours, that locks each block it allocates into
 *        memory with mlock() and unlocks it when the block is freed, and reports large allocations as
 *        GrowthReportingAllocator does.
 *
 * Locked memory stays in memory: the kernel never swaps it out, so a thread that reads it never waits for a page to be
 * read back from disk. A component that must never pause on its order path locks its large tables at start for that
 * reason (docs/venue/the_sequencer_holds_its_memory_from_the_start.md). A container such as `tsl::robin_map` keeps its
 * storage in a vector it allocates itself and gives no way to reach that storage's address, so the only place to lock
 * it is the allocator it is given.
 *
 * A block that cannot be locked is still returned, so that the container is built as it would be without locking; the
 * failure is recorded in the MemoryLockRecord, and the owner reads it once the container is built and decides what to
 * do. Throwing from here instead would leave the container half built inside its own code. With no record, nothing is
 * locked, and this behaves exactly as GrowthReportingAllocator.
 *
 * The record and the reporter are not owned, and must outlive the container. Every copy and every rebound copy of the
 * allocator writes to the same ones, as a container copies and rebinds its allocator freely; see
 * GrowthReportingAllocator for why the converting constructor is not explicit.
 */
template <typename T> class MemoryLockingAllocator {
  public:
    using value_type = T;

    /**
     * @param[in] reporter    Told of large allocations; may be null.
     * @param[in] lock_record Where locking is recorded; null to lock nothing.
     */
    explicit MemoryLockingAllocator(AllocationGrowthReporter* reporter = nullptr, MemoryLockRecord* lock_record = nullptr)
        : reporting_(reporter), lock_record_(lock_record) {}

    /// Rebinding constructor: carries the reporter and the record across when a container rebinds this allocator to
    /// its own internal type.
    template <typename U> MemoryLockingAllocator(const MemoryLockingAllocator<U>& other) : reporting_(other.reporter()), lock_record_(other.lock_record()) {}

    [[nodiscard]] AllocationGrowthReporter* reporter() const {
        return reporting_.reporter();
    }

    [[nodiscard]] MemoryLockRecord* lock_record() const {
        return lock_record_;
    }

    [[nodiscard]] T* allocate(size_t element_count) {
        T* block = reporting_.allocate(element_count);
        if (lock_record_ != nullptr) {
            const size_t bytes = element_count * sizeof(T);
            if (::mlock(block, bytes) == 0) {
                lock_record_->locked_bytes += bytes;
            } else if (lock_record_->error == 0) {
                lock_record_->error = errno;
            }
        }
        return block;
    }

    void deallocate(T* block, size_t element_count) {
        if (lock_record_ != nullptr) {
            const size_t bytes = element_count * sizeof(T);
            // Unlocking memory that was never locked does no harm, so a block whose lock failed is unlocked too.
            static_cast<void>(::munlock(block, bytes));
            lock_record_->locked_bytes -= bytes <= lock_record_->locked_bytes ? bytes : lock_record_->locked_bytes;
        }
        reporting_.deallocate(block, element_count);
    }

    template <typename U> bool operator==(const MemoryLockingAllocator<U>& other) const {
        return reporter() == other.reporter() && lock_record_ == other.lock_record();
    }

    template <typename U> bool operator!=(const MemoryLockingAllocator<U>& other) const {
        return !(*this == other);
    }

  private:
    GrowthReportingAllocator<T> reporting_;
    MemoryLockRecord* lock_record_;
};

} // namespaces
