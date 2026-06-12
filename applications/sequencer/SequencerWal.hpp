#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <functional>
#include <string>

#include <pubsub_itc_fw/WalWriter.hpp>

namespace sequencer {

/**
 * @brief Controls whether SequencerWal::open() uses the snapshot as a
 *        starting anchor or replays all records from segment 0.
 */
class WalOpenMode {
  public:
    enum WalOpenModeTag {
        UseSnapshot = 0,   ///< Normal crash-recovery open; starts from the snapshot anchor.
        IgnoreSnapshot = 1 ///< Full replay; reads every record from segment 0 regardless of snapshot.
    };

    explicit WalOpenMode(WalOpenModeTag value) : value_{value} {}

    [[nodiscard]] bool is_equal(const WalOpenMode& rhs) const {
        return value_ == rhs.value_;
    }
    [[nodiscard]] bool is_equal(const WalOpenModeTag& rhs) const {
        return value_ == rhs;
    }
    [[nodiscard]] WalOpenModeTag value() const {
        return value_;
    }

  private:
    WalOpenModeTag value_;
};

inline bool operator==(const WalOpenMode& lhs, const WalOpenMode& rhs) {
    return lhs.is_equal(rhs);
}
inline bool operator==(const WalOpenMode& lhs, const WalOpenMode::WalOpenModeTag& rhs) {
    return lhs.is_equal(rhs);
}
inline bool operator==(const WalOpenMode::WalOpenModeTag& lhs, const WalOpenMode& rhs) {
    return rhs.is_equal(lhs);
}

/**
 * @brief On-disk snapshot header written by SequencerWal::take_snapshot().
 *
 * Written atomically (write to .tmp then rename) to `snapshot.bin` in the WAL
 * directory. On startup, if a valid snapshot is found, WAL replay begins from
 * the snapshot's WAL anchor rather than from segment 0.
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
    int64_t last_seq_no;   ///< Highest seq_no committed when snapshot was taken.
    uint64_t record_count; ///< Total records committed when snapshot was taken.
    uint64_t wal_segment;  ///< WalWriter::current_position().segment at snapshot time.
    uint64_t wal_offset;   ///< WalWriter::current_position().offset at snapshot time.
    uint32_t checksum;     ///< CRC32 over the 40 bytes above.
    uint32_t filler;       ///< Reserved; always zero.
};
static_assert(sizeof(SnapshotHeader) == 48, "SnapshotHeader must be 48 bytes");

/**
 * @brief Sequencer write-ahead log built on pubsub_itc_fw::WalWriter / WalReader.
 *
 * Extends the framework's generic WAL with the sequencer-specific payload
 * format and snapshot management.
 *
 * Payload format stored in each WAL entry:
 *   [ wall_time_ns (int64_t, 8 bytes) | pdu_id (int16_t, 2 bytes) | PDU payload bytes ]
 *
 * wall_time_ns is the wall time (nanoseconds since the Unix epoch) at which the
 * leader sequenced the record.  It is identical to the sequenced_at field stamped
 * on the NOS/OCR PDU forwarded to the matching engine, enabling exact timestamp
 * reconstruction during WAL replay.
 *
 * The framework WalEntryHeader carries seq_no as `record_id`; pdu_id travels
 * in the application payload so the framework header remains generic.
 *
 * Threading: single-writer at any given time.
 *   On the leader: all writes happen from SequencerThread.
 *   On the follower: writes for WalRecord PDUs happen from the reactor thread
 *   via the inline PDU handler installed in install_peer_wal_inline_handler().
 *   SequencerThread never writes on the follower (role guard in
 *   on_framework_pdu_message), so there is no concurrent access.
 */
class SequencerWal {
  public:
    static constexpr uint32_t snapshot_magic = 0xC0DEBA5EU;
    static constexpr uint32_t snapshot_version = 1;

    /**
     * @brief Callback invoked once per replayed WAL record during open().
     *
     * @param seq_no       Sequence number stamped by the sequencer.
     * @param pdu_id       DSL message type identifier.
     * @param payload      Pointer to the raw PDU payload bytes (valid only during this call).
     * @param payload_size Number of PDU payload bytes (excludes the header prefix).
     * @param wall_time_ns Wall time at which the leader sequenced this record (nanoseconds
     *                     since the Unix epoch). Feed into a ReplayClock before dispatching
     *                     the PDU to reproduce the original sequenced_at timestamps.
     */
    using ReplayCallback = std::function<void(int64_t seq_no, int16_t pdu_id, const uint8_t* payload, size_t payload_size, int64_t wall_time_ns)>;

    SequencerWal() = default;
    ~SequencerWal() = default;

    SequencerWal(const SequencerWal&) = delete;
    SequencerWal& operator=(const SequencerWal&) = delete;

    /**
     * @brief Opens (or creates) the WAL and replays any uncommitted records.
     *
     * Loads the snapshot (if present), replays post-snapshot WAL entries to
     * recover committed state, then opens the writer at the correct resume
     * position. For each replayed entry `replay_cb` is invoked (if provided).
     *
     * @param[in] directory    Directory for segment files. Created if absent.
     * @param[in] segment_size Pre-allocation size of each segment in bytes.
     * @param[in] replay_cb    Optional callback invoked per replayed record.
     * @param[in] open_mode    WalOpenMode::UseSnapshot (default) uses the snapshot
     *                         anchor for crash-recovery. WalOpenMode::IgnoreSnapshot
     *                         replays every record from segment 0; used by --replay mode.
     * @return Highest seq_no found during replay, or 0 if the WAL was empty.
     * @throws std::runtime_error on I/O failure.
     */
    int64_t open(const std::string& directory, size_t segment_size, ReplayCallback replay_cb = nullptr,
                 WalOpenMode open_mode = WalOpenMode{WalOpenMode::UseSnapshot});

    /**
     * @brief Appends one order record to the WAL.
     *
     * Payload stored: pdu_id (2 bytes) followed by the raw PDU payload bytes.
     * The framework WalWriter handles segmentation, mmap, and CRC.
     *
     * @param[in] seq_no       Stamped sequence number (stored as record_id).
     * @param[in] pdu_id       DSL message type.
     * @param[in] payload      Raw inbound PDU payload bytes.
     * @param[in] size         Number of payload bytes.
     * @param[in] wall_time_ns Wall time at which this record is being sequenced.
     *                         Must equal the sequenced_at value stamped on the outbound PDU.
     */
    void append(int64_t seq_no, int16_t pdu_id, const uint8_t* payload, int size, int64_t wall_time_ns);

    /**
     * @brief Writes a snapshot then deletes superseded WAL segments.
     *
     * Captures last_seq_no, record_count, and the current WalWriter position.
     * Written atomically via write-to-tmp-then-rename.
     *
     * @throws std::runtime_error on I/O failure.
     */
    void take_snapshot();

    [[nodiscard]] size_t record_count() const {
        return record_count_;
    }
    [[nodiscard]] int64_t last_seq_no() const {
        return last_seq_no_;
    }
    [[nodiscard]] bool is_open() const {
        return writer_.is_open();
    }

  private:
    static constexpr size_t snapshot_checksum_offset = 40;

    [[nodiscard]] std::string snapshot_path() const;
    [[nodiscard]] std::string segment_path_for_delete(uint64_t seg_num) const;
    bool load_snapshot(pubsub_itc_fw::WalPosition& out_pos);
    void delete_segments_before(uint64_t seg_num) const;

    std::string directory_;
    size_t segment_size_{0};

    pubsub_itc_fw::WalWriter writer_;

    size_t record_count_{0};
    int64_t last_seq_no_{0};
};

} // namespaces
