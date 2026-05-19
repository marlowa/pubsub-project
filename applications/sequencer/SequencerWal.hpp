#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <functional>
#include <string>

namespace sequencer {

/**
 * @brief On-disk WAL entry header.
 *
 * Immediately followed on disk by `payload_size` payload bytes, then a
 * uint32_t CRC32 checksum covering the header and payload.
 *
 * All fields are in host byte order (single-host, no cross-machine replication
 * yet). Slice 7 (network replication) will add endian conversion.
 *
 * Total entry size = sizeof(WalEntryHeader) + payload_size + sizeof(uint32_t).
 *
 * Layout (24 bytes):
 *   magic(4) | payload_size(4) | seq_no(8) | pdu_id(2) | filler_a(2) | filler_b(4)
 *   filler_b makes the struct a multiple of alignof(int64_t)=8 with no implicit padding.
 */
struct WalEntryHeader {
    uint32_t magic;        ///< Must equal SequencerWal::entry_magic on valid entries.
    uint32_t payload_size; ///< Byte count of the payload that follows.
    int64_t  seq_no;       ///< Sequencer-assigned monotonic sequence number.
    int16_t  pdu_id;       ///< DSL message type identifier.
    uint16_t filler_a;     ///< Reserved; always zero.
    uint32_t filler_b;     ///< Reserved; always zero. Makes sizeof == 24 with no implicit padding.
};
static_assert(sizeof(WalEntryHeader) == 24, "WalEntryHeader must be 24 bytes");

/**
 * @brief On-disk snapshot header written by SequencerWal::take_snapshot().
 *
 * Written atomically (write to .tmp then rename) to `snapshot.bin` in the WAL
 * directory. On startup, if a valid snapshot is found, WAL replay begins from
 * the snapshot's anchor position rather than from segment 0.
 *
 * Layout (48 bytes, no implicit padding):
 *   magic(4) | version(4) | last_seq_no(8) | record_count(8) |
 *   wal_segment(8) | wal_offset(8) | checksum(4) | filler(4)
 *
 * checksum = CRC32 over the first 40 bytes (everything before checksum).
 */
struct SnapshotHeader {
    uint32_t magic;        ///< Must equal SequencerWal::snapshot_magic.
    uint32_t version;      ///< Must equal SequencerWal::snapshot_version.
    int64_t  last_seq_no;  ///< Highest seq_no committed when snapshot was taken.
    uint64_t record_count; ///< Total records committed when snapshot was taken.
    uint64_t wal_segment;  ///< current_segment_ at snapshot time.
    uint64_t wal_offset;   ///< write_offset_ at snapshot time.
    uint32_t checksum;     ///< CRC32 over the 40 bytes above.
    uint32_t filler;       ///< Reserved; always zero.
};
static_assert(sizeof(SnapshotHeader) == 48, "SnapshotHeader must be 48 bytes");

/**
 * @brief mmap'd, segmented write-ahead log for the sequencer (Slices 3–4).
 *
 * Persists every committed order PDU to a series of fixed-size, memory-mapped
 * segment files. On startup the existing segments are replayed to recover
 * `next_sequence_number_` before the sequencer begins accepting new orders.
 *
 * Snapshots (Slice 4):
 *   take_snapshot() captures the current state to `snapshot.bin` (written
 *   via write-to-tmp-then-rename for atomicity) then deletes WAL segments
 *   that are fully covered by the snapshot. On the next open(), replay starts
 *   from the snapshot anchor rather than from segment 0, so restart time is
 *   bounded by post-snapshot WAL size rather than total WAL history.
 *
 * On-disk format:
 *   Segment files are named `wal_NNNNNN.log` (zero-padded to 6 digits).
 *   Each segment is pre-allocated to `segment_size` bytes via ftruncate (all
 *   zeros). Entries are written sequentially from offset 0. The first zero
 *   magic value marks the end of committed data within a segment.
 *
 *   Entry layout:
 *     [ WalEntryHeader (24 bytes) | payload (payload_size bytes) | CRC32 (4 bytes) ]
 *   CRC32 covers the header and payload bytes.
 *
 * Durability:
 *   No fsync is issued per commit. The OS writes dirty pages to disk in the
 *   background. This provides crash-survivability for normal process crashes
 *   (the kernel survives) but not power loss. Per the design: replication
 *   (slice 7) removes the need for fsync; the disk is a local recovery
 *   mechanism, not the durability guarantee.
 *
 * madvise:
 *   Each segment opened for replay calls madvise(MADV_WILLNEED) to pre-fault
 *   pages up-front, so cold-start MTTR is dominated by disk read time rather
 *   than lazy page faults scattered through the replay scan.
 *
 * Threading: single-writer — all methods must be called from the SequencerThread.
 */
class SequencerWal {
  public:
    static constexpr uint32_t entry_magic      = 0xFEEDFACEU;
    static constexpr uint32_t snapshot_magic   = 0xC0DEBA5EU;
    static constexpr uint32_t snapshot_version = 1;

    /**
     * @brief Callback invoked once per replayed WAL record during open().
     *
     * Called only for records at or after the snapshot anchor (i.e. records
     * that were not already accounted for by a loaded snapshot). Each call
     * delivers the raw PDU payload as stored at WAL-append time.
     *
     * @param seq_no       Sequence number stamped by the sequencer.
     * @param pdu_id       DSL message type identifier.
     * @param payload      Pointer to the raw payload bytes (into mmap memory;
     *                     valid only for the duration of this call).
     * @param payload_size Number of payload bytes.
     */
    using ReplayCallback = std::function<void(int64_t seq_no, int16_t pdu_id,
                                              const uint8_t* payload, std::size_t payload_size)>;

    SequencerWal() = default;
    ~SequencerWal();

    SequencerWal(const SequencerWal&)            = delete;
    SequencerWal& operator=(const SequencerWal&) = delete;

    /**
     * @brief Opens (or creates) the WAL.
     *
     * Loads the snapshot (if present), replays post-snapshot WAL segments to
     * recover committed state, then opens the current segment for writing at
     * the correct offset. For each replayed entry `replay_cb` is invoked (if
     * provided) so callers can rebuild derived in-memory state (e.g. a
     * cl_ord_id→comp_id routing map) without a second pass over the segments.
     *
     * @param[in] directory    Directory for segment files. Created if absent.
     * @param[in] segment_size Pre-allocation size of each segment in bytes.
     * @param[in] replay_cb    Optional callback invoked per replayed record.
     * @return Highest seq_no found during replay, or 0 if the WAL was empty.
     * @throws std::runtime_error on I/O failure.
     */
    int64_t open(const std::string& directory, std::size_t segment_size,
                 ReplayCallback replay_cb = nullptr);

    /**
     * @brief Appends an order record to the WAL (no fsync).
     *
     * The WAL append is the irreversible commit act. The ME forward must only
     * happen after this call returns. Rolls to a new segment automatically when
     * the current one is full.
     *
     * @param[in] seq_no   Stamped sequence number.
     * @param[in] pdu_id   DSL message type.
     * @param[in] payload  Raw inbound PDU payload bytes.
     * @param[in] size     Number of payload bytes.
     */
    void append(int64_t seq_no, int16_t pdu_id, const uint8_t* payload, int size);

    /**
     * @brief Writes a snapshot then deletes superseded WAL segments.
     *
     * The snapshot captures last_seq_no, record_count, and the current WAL
     * position. Written atomically via write-to-tmp-then-rename. After a
     * successful rename, WAL segments strictly before the snapshot's segment
     * are deleted (best-effort; ENOENT is silently ignored).
     *
     * On the next open(), replay will begin from the snapshot anchor rather
     * than from segment 0, bounding restart time to post-snapshot WAL size.
     *
     * @throws std::runtime_error on I/O failure writing or renaming the snapshot.
     */
    void take_snapshot();

    [[nodiscard]] std::size_t record_count() const noexcept { return record_count_; }
    [[nodiscard]] int64_t     last_seq_no()  const noexcept { return last_seq_no_; }
    [[nodiscard]] bool        is_open()      const noexcept { return mmap_ptr_ != nullptr; }

  private:
    // Minimum space required for any entry (header + 1 byte payload + checksum).
    static constexpr std::size_t min_entry_bytes =
        sizeof(WalEntryHeader) + 1 + sizeof(uint32_t);

    // Byte offset of the checksum field within SnapshotHeader (= 40).
    static constexpr std::size_t snapshot_checksum_offset = 40;

    // Build the absolute path for segment `seg_num`.
    std::string segment_path(std::size_t seg_num) const;

    // Absolute path to the snapshot file.
    std::string snapshot_path() const;

    // Open segment `seg_num` for writing. Creates and ftruncates if the file
    // does not yet exist.
    void open_write_segment(std::size_t seg_num);

    // munmap + close the current write segment.
    void close_write_segment() noexcept;

    // Replay one segment file starting from `start_offset`. Returns the byte
    // offset at which the scan stopped (= write offset to resume from if this
    // is the last segment). Invokes replay_cb for each valid entry.
    std::size_t replay_segment(std::size_t seg_num, std::size_t start_offset,
                               const ReplayCallback& replay_cb);

    // Load snapshot.bin. Sets last_seq_no_ and record_count_ from the snapshot
    // and returns the WAL anchor (segment + offset) to resume replay from.
    // Returns false (leaves out params at 0) when no valid snapshot exists.
    bool load_snapshot(std::size_t& out_seg, std::size_t& out_offset);

    // Delete WAL segment files with index strictly less than `seg_num`.
    // Best-effort: ENOENT and other I/O errors are silently ignored.
    void delete_segments_before(std::size_t seg_num) noexcept;

    // Ensure at least `bytes_needed` bytes remain in the current segment.
    // Rolls to a new segment when there is not enough room.
    void ensure_capacity(std::size_t bytes_needed);

    std::string directory_;
    std::size_t segment_size_{0};
    std::size_t current_segment_{0};
    std::size_t write_offset_{0};

    uint8_t* mmap_ptr_{nullptr};
    int      fd_{-1};

    std::size_t record_count_{0};
    int64_t     last_seq_no_{0};
};

} // namespace sequencer