#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <pubsub_itc_fw/CpuPinning.hpp>
#include <pubsub_itc_fw/ThreadWithJoinTimeout.hpp>
#include <pubsub_itc_fw/WalPosition.hpp>
#ifdef PUBSUB_ITC_FW_THREAD_CHECKS
#include <pubsub_itc_fw/ThreadConfinementCheck.hpp>
#endif

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
 * **Segments kept ready** (docs/durability/keeping_segments_ready.md). Segments must have their
 * blocks allocated before use, or the first write to each page allocates one inside a page fault
 * and can wait on the filesystem journal for hundreds of milliseconds (BUG-0070). So one private
 * helper thread fills segments with zeros ahead of the one being written, keeping up to
 * segments_kept_ready of them ready, four by default. Each is filled under a temporary name,
 * `filling_wal_NNNNNN.helper` or `.writer`, and linked to its final name only when it is full, so a
 * file named `wal_NNNNNN.log` is always complete: a process that dies while filling one leaves only
 * a temporary file, which the next open() deletes.
 *
 * When the current segment is full the writer takes the next one from the helper if it is ready,
 * and otherwise makes it itself, under the same rule, and reports that through the warning
 * function (set_warning_sink). It never waits for the helper: if both make the same segment,
 * whichever links its file first wins and the other opens that file. The writer and the helper
 * share a ring of slots, one for each ready segment, and two counters; each slot is handed from one
 * to the other with an atomic flag, so nothing on the append path takes a lock.
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

    /// How many segments the helper keeps ready ahead of the one being written, unless set otherwise.
    static constexpr size_t default_segments_kept_ready = 4;

    ~WalWriter();

    WalWriter() = default;

    WalWriter(const WalWriter&) = delete;
    WalWriter& operator=(const WalWriter&) = delete;

    /**
     * @brief Opens the WAL directory and positions the writer at `start`.
     *
     * The directory is created if it does not exist. Temporary files left by a segment being filled
     * when a process died are deleted, and a segment from `start.segment` on that is shorter than
     * the segment size, which only a writer that filled segments under their final names can have
     * left, is completed with zeros, keeping any records in it. The segment file at `start.segment`
     * is then opened (or created) and mapped, and the writer appends from `start.offset` within it.
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
     * @brief Closes the current segment and stops the segment-preparing helper, so that the log can
     *        be changed on disk and the writer opened again with open().
     *
     * Used by Wal::truncate_after(). Segments the helper had made ready are released; their files
     * stay on disk for the caller to keep or delete.
     */
    void close();

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
     * @brief How many segments the writer had to make itself because the helper had none ready.
     *
     * Expected to be one, for the first segment, which nothing can prepare in advance, and zero
     * thereafter. Each one after the first is also reported through the warning function.
     */
    [[nodiscard]] uint64_t segments_filled_inline() const {
        return segments_filled_inline_;
    }

    /**
     * @brief Waits until segments_kept_ready segments are ready ahead of the current one, or the
     *        timeout expires. For tests.
     *
     * Production never waits: the writer either finds a ready segment or makes one. This exists so
     * a test can assert on ready segments without sleeping and hoping.
     *
     * @return true if they are all ready.
     */
    [[nodiscard]] bool wait_until_prepared(std::chrono::milliseconds timeout);

    /**
     * @brief Turns off preparation, so every segment is made by the writer. For tests.
     *
     * Must be called before open().
     */
    void disable_preparation() {
        preparation_enabled_ = false;
    }

    /// For tests only: the helper waits this long before making each segment, as a disk that stalled
    /// would make it. Must be called before open().
    void delay_helper_for_tests(std::chrono::milliseconds delay) {
        helper_delay_for_tests_ = delay;
    }

    /**
     * @brief How many segments the helper keeps ready ahead of the one being written; at least one.
     *        Must be called before open(). From `[wal] segments_kept_ready` in a component's file.
     */
    void set_segments_kept_ready(size_t count);

    /**
     * @brief Where the writer says it had to make a segment itself, or found a short segment at
     *        open(), which are incidents rather than routine; the owner logs it at Warning. Called on
     *        the writing thread. Must be set before open() to report what open() finds.
     */
    void set_warning_sink(std::function<void(const std::string&)> sink) {
        warn_ = std::move(sink);
    }

    /**
     * @brief Says which processors the segment-preparing helper thread may run on.
     *
     * @param[in] cores Processors the helper may use. Empty leaves it where it lands, which is
     *                  on whichever processor the thread that opens the log was pinned to.
     *
     * Must be called before open(), which is what starts the helper. Calling it afterwards
     * does nothing, because the helper reads this once as it starts.
     */
    void set_helper_cores(const std::vector<CpuId>& cores) {
        helper_cores_ = cores;
    }

  private:
    // Minimum space for any entry (header + 1 byte payload + CRC32).
    static constexpr size_t min_entry_bytes = 24 + 1 + sizeof(uint32_t);

    /// A segment's temporary name while it is filled is `filling_wal_NNNNNN.` followed by one of these:
    /// different for the helper and the writer, so that the two never write the same file. It starts
    /// `filling_` rather than `wal_` because some readers find segments with sscanf("wal_%06" ...
    /// ".log"), which matches any name that merely begins like a segment's: a name such as
    /// `wal_000001.log.preparing` was taken for segment 1, and opening segment 1 then failed.
    static constexpr const char* temporary_prefix = "filling_";
    static constexpr const char* helper_temporary_ending = "helper";
    static constexpr const char* writer_temporary_ending = "writer";

    /// The temporary name segment @p seg_num is filled under, by the thread whose ending is @p ending.
    [[nodiscard]] std::string temporary_path(uint64_t seg_num, const char* ending) const;

    [[nodiscard]] std::string segment_path(uint64_t seg_num) const;
    /// Writes a newly created segment out in full, so that its blocks exist before anything
    /// is appended. See the definition for why ftruncate is not enough and fallocate is not
    /// either.
    void fill_new_segment(int fd, const std::string& path);

    /// How much is written at a time while filling a new segment. Large enough that the write
    /// is sequential and few in number, small enough that the buffer is not worth avoiding.
    static constexpr size_t fill_chunk_bytes = 1U << 20;

    void close_segment();
    void ensure_capacity(size_t bytes_needed);

    /// One segment's descriptor and mapping.
    struct OpenedSegment {
        uint64_t seg_num{0};
        int fd{-1};
        uint8_t* mmap_ptr{nullptr};
    };

    /// Opens segment @p seg_num complete and mapped: the existing file if there is one, otherwise a
    /// new one filled under a temporary name ending @p temporary_ending and linked to its final name
    /// when full. If another thread links its own copy first, that one is opened instead. No thread
    /// in it, so that the writer and the helper can both use it.
    [[nodiscard]] OpenedSegment create_and_map(uint64_t seg_num, const char* temporary_ending);

    /// Frees one segment's mapping and descriptor.
    void release(OpenedSegment& segment);

    /// Adopts an already-opened segment as the one being written to, closing the previous one.
    void adopt(const OpenedSegment& segment);

    /// Deletes the temporary files of segments whose filling was cut short. Before the helper starts.
    void remove_temporary_files();

    /// Completes with zeros every segment numbered @p from or later that is shorter than the segment
    /// size, keeping what it holds. Before the helper starts.
    void complete_short_segments(uint64_t from);

    /// Says something on the warning function, if there is one.
    void warn(const std::string& message) const;

    void start_helper();
    void stop_helper();
    void helper_loop();
    void wake_helper();

    /// Frees every ready segment the writer has passed, numbered @p through or lower. Writer only.
    void release_passed_slots(uint64_t through);

    /// Frees every ready segment. Only once the helper has been joined.
    void release_all_slots();

    std::string directory_;
    size_t segment_size_{0};
    uint64_t current_segment_{0};
    size_t write_offset_{0};

    uint8_t* mmap_ptr_{nullptr};
    int fd_{-1};

    bool preparation_enabled_{true};
    std::chrono::milliseconds helper_delay_for_tests_{0};
    size_t segments_kept_ready_{default_segments_kept_ready};
    uint64_t segments_filled_inline_{0};
    std::function<void(const std::string&)> warn_;

#ifdef PUBSUB_ITC_FW_THREAD_CHECKS
    ThreadConfinementCheck writer_thread_{"WalWriter"};
#endif

    /// A ready segment, handed from the helper to the writer. The helper fills an empty slot and then
    /// sets `full` with a release store; the writer reads a slot only after seeing `full` with an
    /// acquire load, and empties it by clearing `full` with a release store. That pairing is the whole
    /// of the synchronisation for the slot.
    struct Slot {
        OpenedSegment segment{};
        std::atomic<bool> full{false};
    };
    /// Segment n is in slot n modulo segments_kept_ready.
    std::unique_ptr<Slot[]> slots_;

    /// The highest segment the helper has dealt with: made ready, or found the writer already past.
    /// Written by the helper only.
    std::atomic<uint64_t> prepared_through_{0};
    /// The segment being written. Written by the writer only; the helper keeps at most
    /// segments_kept_ready segments ahead of it.
    std::atomic<uint64_t> writing_{0};
    std::atomic<bool> stopping_{false};

    ThreadWithJoinTimeout helper_;
    bool helper_running_{false};
    int helper_wake_fd_{-1};

    // Where the helper thread should run, or empty to leave it wherever it lands.
    //
    // It needs saying because of how it lands. A new thread inherits the processor mask of the
    // thread that created it, and the thread that opens the log is the component's application
    // thread, by then pinned to a processor reserved for the order path. So without this the
    // helper does its file opening, truncating and memory mapping on that processor, sharing it
    // with the one thread the venue most needs to be free.
    //
    // Set by the component from the layout's background processors, before open() is called.
    // Left empty by anything that does not care -- the tests, and any use with no layout.
    std::vector<CpuId> helper_cores_;

    // Set by the helper if it could not move itself. Reported rather than thrown: a log that
    // works from the wrong processor is better than a component that will not start.
    std::atomic<bool> helper_placement_failed_{false};

    // Why the helper last failed to make a segment, for the writer's warning; written by the helper
    // before it sets helper_failed_, read by the writer after it sees it.
    std::string helper_failure_;
    std::atomic<bool> helper_failed_{false};
};

} // namespaces
