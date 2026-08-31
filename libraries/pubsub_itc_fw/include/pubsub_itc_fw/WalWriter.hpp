#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

#include <pubsub_itc_fw/ThreadWithJoinTimeout.hpp>
#include <pubsub_itc_fw/WalPosition.hpp>

namespace pubsub_itc_fw {

/**
 * @brief Single-writer mmap'd segmented write-ahead log.
 *
 * Stores opaque application payloads in a series of fixed-size, memory-mapped
 * segment files (`wal_NNNNNN.log`, zero-padded to 6 digits). Each entry is
 * framed with a standard header and a trailing CRC32:
 *
 *   [ WalEntryHeader (24 bytes) | payload (payload_size bytes) | CRC32 (4 bytes) ]
 *
 * Wire layout of WalEntryHeader (host byte order; endian conversion):
 *   magic(4) | payload_size(4) | record_id(8) | filler(8)
 *
 * The `record_id` is application-supplied and monotonically increasing. For the
 * sequencer it is the sequence number; for the arbiter it may be an epoch counter.
 * The framework uses it for replication acking without interpreting it.
 *
 * Durability:
 *   No fsync per commit. The OS writes dirty mmap pages to disk in the background,
 *   providing crash-survivability for process crashes (kernel survives) but not
 *   power loss. Replication is the durability guarantee; local disk is
 *   the single-host recovery mechanism.
 *
 * Threading: single-writer -- every public method must be called from the same thread.
 *
 * One private helper thread exists, which creates the *next* segment while the writer is still
 * appending to the current one. Segments must have their blocks allocated before use, or the
 * first write to each page allocates one inside a page fault and can wait on the filesystem
 * journal for hundreds of milliseconds (BUG-0070). Allocating costs the same wherever it
 * happens, so the only question is which thread can afford to wait for it, and the answer is a
 * thread with nothing else to do. The writer and the helper share one slot and one atomic; they
 * touch different files, different descriptors and different mappings, so nothing on the append
 * path is shared at all.
 *
 * If the helper has not finished when the writer needs to roll, the writer creates the segment
 * itself rather than waiting or -- far worse -- appending into one whose blocks do not exist.
 * That case is counted by segments_filled_inline(): if it is ever non-zero under load, the
 * helper is not keeping pace and that is a measurement rather than a guess.
 *
 * Typical lifecycle:
 * @code
 *   WalPosition start = WalReader::replay(dir, {0,0}, cb); // returns end position
 *   WalWriter writer;
 *   writer.open(dir, segment_size, start);
 *   writer.append(seq_no, payload_ptr, payload_size);
 * @endcode
 */
class WalWriter {
  public:
    static constexpr uint32_t entry_magic = 0xFEEDFACEU;

    ~WalWriter();

    WalWriter() = default;

    WalWriter(const WalWriter&) = delete;
    WalWriter& operator=(const WalWriter&) = delete;

    /**
     * @brief Opens the WAL directory and positions the writer at `start`.
     *
     * The directory is created if it does not exist. The segment file at
     * `start.segment` is opened (or created) and mapped. The writer will
     * append from `start.offset` within that segment.
     *
     * @param[in] directory    Directory for segment files.
     * @param[in] segment_size Pre-allocation size of each segment in bytes.
     * @param[in] start        Position at which to begin writing (from WalReader::replay()).
     */
    void open(const std::string& directory, size_t segment_size, WalPosition start);

    /**
     * @brief Appends one record to the WAL (no fsync).
     *
     * Rolls to a new segment automatically when the current one is full.
     *
     * @param[in] record_id  Application-supplied monotonic record identifier.
     * @param[in] payload    Pointer to the payload bytes.
     * @param[in] size       Number of payload bytes.
     * @pre size must not exceed segment_size. Violating this throws PreconditionAssertion.
     */
    void append(int64_t record_id, const void* payload, size_t size);

    /**
     * @brief Returns the current write position (segment + offset).
     *
     * Applications store this in their snapshot so that WalReader::replay()
     * can start from the anchor rather than from segment 0 on the next open.
     */
    [[nodiscard]] WalPosition current_position() const {
        return {current_segment_, write_offset_};
    }

    [[nodiscard]] bool is_open() const {
        return mmap_ptr_ != nullptr;
    }

    /**
     * @brief How many segments the writer had to create itself because the helper was not ready.
     *
     * Expected to be one for the first segment, which nothing can prepare in advance, and zero
     * thereafter. A rising count means preparation is not keeping pace with appends.
     */
    [[nodiscard]] uint64_t segments_filled_inline() const {
        return segments_filled_inline_;
    }

    /**
     * @brief How many rolls had to wait for a preparation that was still in progress.
     *
     * Expected to be zero: the helper has the whole life of a segment to prepare the next one.
     * A non-zero count means segments are being consumed faster than they can be created, which
     * is a property of the storage rather than of this class.
     */
    [[nodiscard]] uint64_t segments_waited_for() const {
        return segments_waited_for_;
    }

    /**
     * @brief Waits until the next segment is ready, or the timeout expires. For tests.
     *
     * Production never waits: the writer either finds a prepared segment or makes one. This
     * exists so a test can assert on a prepared segment without sleeping and hoping.
     *
     * @return true if a prepared segment is ready.
     */
    [[nodiscard]] bool wait_until_prepared(std::chrono::milliseconds timeout);

    /**
     * @brief Turns off preparation, so every segment is created by the writer. For tests.
     *
     * Must be called before open(). Gives a test the pre-BUG-0070 scheduling with the
     * post-BUG-0070 allocation, which is what the fallback path does in production.
     */
    void disable_preparation() {
        preparation_enabled_ = false;
    }

  private:
    // Minimum space for any entry (header + 1 byte payload + CRC32).
    static constexpr size_t min_entry_bytes = 24 + 1 + sizeof(uint32_t);

    [[nodiscard]] std::string segment_path(uint64_t seg_num) const;
    void open_segment(uint64_t seg_num);
    /// Writes a newly created segment out in full, so that its blocks exist before anything
    /// is appended. See the definition for why ftruncate is not enough and fallocate is not
    /// either.
    void fill_new_segment(int fd, const std::string& path);

    /// How much is written at a time while filling a new segment. Large enough that the write
    /// is sequential and few in number, small enough that the buffer is not worth avoiding.
    static constexpr size_t fill_chunk_bytes = 1U << 20;

    void close_segment();
    void ensure_capacity(size_t bytes_needed);

    /// Creates, fills and maps one segment. The whole mechanism, with no thread in it, so that
    /// the writer and the helper can both use it and it can be tested on its own.
    struct OpenedSegment {
        uint64_t seg_num{0};
        int fd{-1};
        uint8_t* mmap_ptr{nullptr};
    };
    [[nodiscard]] OpenedSegment create_and_map(uint64_t seg_num);

    /// Adopts an already-opened segment as the one being written to, closing the previous one.
    void adopt(const OpenedSegment& segment);

    void start_helper();
    void stop_helper();
    void helper_loop();
    void request_preparation(uint64_t seg_num);
    void await_helper();
    void wake_helper();
    /// Discards a prepared segment that will not be used, if the helper has finished with it.
    /// The file stays: replay stops at the first record whose magic does not match, so an unused
    /// segment reads as empty and is adopted, already filled, on the next start.
    void release_prepared_if_ready();

    /// Frees whatever is in prepared_, regardless of state. Only valid once the helper has been
    /// joined, which is why it is called from the destructor and nowhere else.
    void discard_prepared();

    /// What the helper has prepared, and whether it may be read.
    ///
    /// Idle      nothing asked for; Requested  helper is working; Ready  prepared_ may be read;
    /// Failed    the helper could not, and failure_ says why.
    ///
    /// The helper publishes prepared_ then release-stores Ready; the writer acquire-loads Ready
    /// then reads prepared_. That pairing is the whole of the synchronisation. No mutex: one on
    /// the writer's side would put back the blocking this exists to remove.
    enum class PrepState : uint8_t { Idle, Requested, Ready, Failed, Stopping };

    std::string directory_;
    size_t segment_size_{0};
    uint64_t current_segment_{0};
    size_t write_offset_{0};

    uint8_t* mmap_ptr_{nullptr};
    int fd_{-1};

    bool preparation_enabled_{true};
    uint64_t segments_filled_inline_{0};
    uint64_t segments_waited_for_{0};

    ThreadWithJoinTimeout helper_;
    bool helper_running_{false};
    int helper_wake_fd_{-1};

    std::atomic<PrepState> prep_state_{PrepState::Idle};
    OpenedSegment prepared_{};
    uint64_t requested_segment_{0};
    std::string failure_;
};

} // namespaces
