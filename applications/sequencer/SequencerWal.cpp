// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "SequencerWal.hpp"

#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <pubsub_itc_fw/Crc32.hpp>
#include <pubsub_itc_fw/PubSubItcException.hpp>
#include <pubsub_itc_fw/StringUtils.hpp>
#include <pubsub_itc_fw/WalReader.hpp>

namespace sequencer {

using pubsub_itc_fw::PubSubItcException;

// ---------------------------------------------------------------------------
// Path helpers
// ---------------------------------------------------------------------------

std::string SequencerWal::snapshot_path() const {
    return directory_ + "/snapshot.bin";
}

std::string SequencerWal::segment_path_for_delete(uint64_t seg_num) const {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "/wal_%06" PRIu64 ".log", seg_num);
    return directory_ + buf;
}

// ---------------------------------------------------------------------------
// open()
// ---------------------------------------------------------------------------

int64_t SequencerWal::open(const std::string& directory, size_t segment_size, ReplayCallback replay_cb, WalOpenMode open_mode) {
    directory_ = directory;
    segment_size_ = segment_size;

    // Load snapshot (if present) to get the WAL anchor and pre-populate counters.
    // In IgnoreSnapshot mode the snapshot is intentionally skipped so that every
    // record ever written is visited by the replay callback.
    pubsub_itc_fw::WalPosition anchor{0, 0};
    if (open_mode == WalOpenMode::UseSnapshot) {
        load_snapshot(anchor);
    }

    // Replay post-snapshot entries, unwrapping the sequencer payload format.
    pubsub_itc_fw::WalReader::EntryCallback fw_cb;
    if (replay_cb) {
        fw_cb = [this, &replay_cb](int64_t record_id, const void* payload, size_t size) {
            constexpr size_t header_size = sizeof(int64_t) + sizeof(int16_t);
            if (size < header_size) {
                return; // malformed — skip
            }

            int64_t wall_time_ns{};
            std::memcpy(&wall_time_ns, payload, sizeof(int64_t));

            int16_t pdu_id{};
            std::memcpy(&pdu_id, static_cast<const uint8_t*>(payload) + sizeof(int64_t), sizeof(int16_t));

            const auto* pdu_payload = static_cast<const uint8_t*>(payload) + header_size;
            const size_t pdu_size = size - header_size;

            ++record_count_;
            last_seq_no_ = record_id;

            replay_cb(record_id, pdu_id, pdu_payload, pdu_size, wall_time_ns);
        };
    } else {
        fw_cb = [this](int64_t record_id, const void* /*payload*/, size_t /*size*/) {
            ++record_count_;
            last_seq_no_ = record_id;
        };
    }

    const pubsub_itc_fw::WalPosition end = pubsub_itc_fw::WalReader::replay(directory_, anchor, fw_cb);

    writer_.open(directory_, segment_size_, end);

    return last_seq_no_;
}

// ---------------------------------------------------------------------------
// load_snapshot()
// ---------------------------------------------------------------------------

bool SequencerWal::load_snapshot(pubsub_itc_fw::WalPosition& out_pos) {
    const std::string path = snapshot_path();

    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        if (errno == ENOENT) {
            return false;
        }
        throw PubSubItcException("SequencerWal: load_snapshot open(" + path + "): " + pubsub_itc_fw::StringUtils::get_errno_string());
    }

    SnapshotHeader hdr{};
    const ssize_t n = ::read(fd, &hdr, sizeof(hdr));
    ::close(fd);

    if (n != static_cast<ssize_t>(sizeof(hdr))) {
        return false;
    }
    if (hdr.magic != snapshot_magic || hdr.version != snapshot_version) {
        return false;
    }

    pubsub_itc_fw::Crc32 crc;
    crc.feed(&hdr, snapshot_checksum_offset);
    if (crc.finalize() != hdr.checksum) {
        return false;
    }

    last_seq_no_ = hdr.last_seq_no;
    record_count_ = static_cast<size_t>(hdr.record_count);
    out_pos = {hdr.wal_segment, hdr.wal_offset};
    return true;
}

// ---------------------------------------------------------------------------
// take_snapshot()
// ---------------------------------------------------------------------------

void SequencerWal::take_snapshot() {
    const pubsub_itc_fw::WalPosition pos = writer_.current_position();

    SnapshotHeader hdr{};
    hdr.magic = snapshot_magic;
    hdr.version = snapshot_version;
    hdr.last_seq_no = last_seq_no_;
    hdr.record_count = static_cast<uint64_t>(record_count_);
    hdr.wal_segment = pos.segment;
    hdr.wal_offset = pos.offset;
    hdr.filler = 0;

    pubsub_itc_fw::Crc32 crc;
    crc.feed(&hdr, snapshot_checksum_offset);
    hdr.checksum = crc.finalize();

    const std::string tmp = snapshot_path() + ".tmp";
    const std::string final = snapshot_path();

    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        throw PubSubItcException("SequencerWal: take_snapshot open(" + tmp + "): " + pubsub_itc_fw::StringUtils::get_errno_string());
    }

    const ssize_t written = ::write(fd, &hdr, sizeof(hdr));
    ::close(fd);

    if (written != static_cast<ssize_t>(sizeof(hdr))) {
        throw PubSubItcException("SequencerWal: take_snapshot write to " + tmp + " incomplete");
    }

    if (::rename(tmp.c_str(), final.c_str()) != 0) {
        throw PubSubItcException("SequencerWal: take_snapshot rename(" + tmp + " -> " + final + "): " + pubsub_itc_fw::StringUtils::get_errno_string());
    }

    delete_segments_before(pos.segment);
}

// ---------------------------------------------------------------------------
// append()
// ---------------------------------------------------------------------------

void SequencerWal::append(int64_t seq_no, int16_t pdu_id, const uint8_t* payload, int size, int64_t wall_time_ns) {
    // Sequencer payload = wall_time_ns(8) | pdu_id(2) | PDU bytes.
    // Use a stack buffer for typical sizes to avoid heap allocation on the hot path.
    constexpr int stack_buffer_size = 512;
    uint8_t stack_buffer[stack_buffer_size];
    std::vector<uint8_t> heap_buffer;

    const size_t total = sizeof(int64_t) + sizeof(int16_t) + static_cast<size_t>(size);
    uint8_t* payload_buffer;
    if (total <= stack_buffer_size) {
        payload_buffer = stack_buffer;
    } else {
        heap_buffer.resize(total);
        payload_buffer = heap_buffer.data();
    }

    std::memcpy(payload_buffer, &wall_time_ns, sizeof(int64_t));
    std::memcpy(payload_buffer + sizeof(int64_t), &pdu_id, sizeof(int16_t));
    std::memcpy(payload_buffer + sizeof(int64_t) + sizeof(int16_t), payload, static_cast<size_t>(size));

    writer_.append(seq_no, payload_buffer, total);

    last_seq_no_ = seq_no;
    ++record_count_;
}

// ---------------------------------------------------------------------------
// delete_segments_before()
// ---------------------------------------------------------------------------

void SequencerWal::delete_segments_before(uint64_t seg_num) const {
    for (uint64_t i = 0; i < seg_num; ++i) {
        ::unlink(segment_path_for_delete(i).c_str());
    }
}

} // namespace sequencer
