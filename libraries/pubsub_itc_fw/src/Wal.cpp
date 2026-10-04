// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <pubsub_itc_fw/Wal.hpp>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <fmt/format.h>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <pubsub_itc_fw/Crc32.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>
#include <pubsub_itc_fw/PubSubItcException.hpp>
#include <pubsub_itc_fw/StringUtils.hpp>
#include <pubsub_itc_fw/WalCursor.hpp>
#include <pubsub_itc_fw/WalReader.hpp>

namespace pubsub_itc_fw {

std::string Wal::snapshot_path() const {
    return directory_ + "/snapshot.bin";
}

std::string Wal::segment_path_for_delete(uint64_t seg_num) const {
    return directory_ + fmt::format("/wal_{:06}.log", seg_num);
}

int64_t Wal::open(const std::string& directory, size_t segment_size, ReplayCallback replay_cb, WalOpenMode open_mode) {
    directory_ = directory;
    segment_size_ = segment_size;

    WalPosition anchor{0, 0};
    if (open_mode == WalOpenMode::UseSnapshot) {
        load_snapshot(anchor);
    }

    WalReader::EntryCallback fw_cb;
    if (replay_cb) {
        fw_cb = [this, &replay_cb](int64_t record_id, const void* payload, size_t size) {
            constexpr size_t header_size = sizeof(int64_t) + sizeof(int16_t);
            if (size < header_size) {
                return;
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

    const WalPosition end = WalReader::replay(directory_, anchor, fw_cb);

    writer_.open(directory_, segment_size_, end);

    return last_seq_no_;
}

bool Wal::load_snapshot(WalPosition& out_pos) {
    const std::string path = snapshot_path();

    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        if (errno == ENOENT) {
            return false;
        }
        throw PubSubItcException("Wal: load_snapshot open(" + path + "): " + StringUtils::get_errno_string());
    }

    WalSnapshotHeader hdr{};
    const ssize_t n = ::read(fd, &hdr, sizeof(hdr));
    ::close(fd);

    if (n != static_cast<ssize_t>(sizeof(hdr))) {
        return false;
    }
    if (hdr.magic != snapshot_magic || hdr.version != snapshot_version) {
        return false;
    }

    Crc32 crc;
    crc.feed(&hdr, snapshot_checksum_offset);
    if (crc.finalize() != hdr.checksum) {
        return false;
    }

    last_seq_no_ = hdr.last_seq_no;
    record_count_ = static_cast<size_t>(hdr.record_count);
    out_pos = {hdr.wal_segment, hdr.wal_offset};
    return true;
}

void Wal::take_snapshot() {
    const WalPosition pos = writer_.current_position();

    WalSnapshotHeader hdr{};
    hdr.magic = snapshot_magic;
    hdr.version = snapshot_version;
    hdr.last_seq_no = last_seq_no_;
    hdr.record_count = static_cast<uint64_t>(record_count_);
    hdr.wal_segment = pos.segment;
    hdr.wal_offset = pos.offset;
    hdr.filler = 0;

    Crc32 crc;
    crc.feed(&hdr, snapshot_checksum_offset);
    hdr.checksum = crc.finalize();

    const std::string tmp = snapshot_path() + ".tmp";
    const std::string final_path = snapshot_path();

    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        throw PubSubItcException("Wal: take_snapshot open(" + tmp + "): " + StringUtils::get_errno_string());
    }

    const ssize_t written = ::write(fd, &hdr, sizeof(hdr));
    ::close(fd);

    if (written != static_cast<ssize_t>(sizeof(hdr))) {
        throw PubSubItcException("Wal: take_snapshot write to " + tmp + " incomplete");
    }

    if (::rename(tmp.c_str(), final_path.c_str()) != 0) {
        throw PubSubItcException("Wal: take_snapshot rename(" + tmp + " -> " + final_path + "): " + StringUtils::get_errno_string());
    }
}

void Wal::append(int64_t seq_no, int16_t pdu_id, const uint8_t* payload, int size, int64_t wall_time_ns) {
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

void Wal::truncate_below(int64_t safe_seq_no) {
    // Find the segment holding the first record at/after safe_seq_no; every segment
    // before it contains only already-consumed records and can be deleted.
    WalCursor cursor;
    cursor.open(directory_, WalPosition{0, 0});
    int64_t record_id = 0;
    const uint8_t* payload = nullptr;
    size_t size = 0;
    while (cursor.read_next(record_id, payload, size)) {
        if (record_id >= safe_seq_no) {
            // position().segment is the segment this record lives in (the offset has
            // advanced past the record but not yet rolled to the next segment).
            delete_segments_before(cursor.position().segment);
            return;
        }
    }
    // No record at/after safe_seq_no: everything is consumed but nothing is safe to
    // reclaim yet (the current segment is still being written), so leave it.
}

void Wal::truncate_after(int64_t seq_no) {
    if (seq_no < 0 || seq_no > last_seq_no_) {
        throw PreconditionAssertion(fmt::format("Wal::truncate_after: record {} is not in a log whose last record is {}", seq_no, last_seq_no_), __FILE__,
                                    __LINE__);
    }
    if (seq_no == last_seq_no_) {
        return;
    }

    // Where the record to keep ends, and how many records follow it. Reading starts at the segment
    // that holds the record, not at the start of the log.
    WalPosition end{0, 0};
    size_t discarded = record_count_;
    if (seq_no > 0) {
        WalCursor cursor;
        cursor.open(directory_, scan_start_for(seq_no));
        int64_t record_id = 0;
        const uint8_t* payload = nullptr;
        size_t size = 0;
        bool found = false;
        while (cursor.read_next(record_id, payload, size)) {
            if (record_id == seq_no) {
                end = cursor.position();
                found = true;
                break;
            }
        }
        if (!found) {
            throw PubSubItcException(fmt::format("Wal::truncate_after: record {} was not found in {}", seq_no, directory_));
        }
        discarded = 0;
        while (cursor.read_next(record_id, payload, size)) {
            ++discarded;
        }
    }

    writer_.close();

    // Zero the rest of the segment the record ends in. Zeroing only the next entry's header would not
    // do: a record appended later could end part way through an older entry, and an older entry
    // after that could then be read as if it followed.
    const std::string segment = segment_path_for_delete(end.segment);
    const int fd = ::open(segment.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        throw PubSubItcException("Wal::truncate_after: open(" + segment + "): " + StringUtils::get_errno_string());
    }
    std::vector<uint8_t> zeros(64 * 1024, 0);
    for (size_t offset = end.offset; offset < segment_size_;) {
        const size_t chunk = std::min(zeros.size(), segment_size_ - offset);
        const ssize_t written = ::pwrite(fd, zeros.data(), chunk, static_cast<off_t>(offset));
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            const std::string reason = StringUtils::get_errno_string();
            ::close(fd);
            throw PubSubItcException("Wal::truncate_after: pwrite(" + segment + "): " + reason);
        }
        offset += static_cast<size_t>(written);
    }
    ::fsync(fd);
    ::close(fd);

    delete_segments_after(end.segment);
    ::unlink(snapshot_path().c_str());

    last_seq_no_ = seq_no;
    record_count_ = discarded > record_count_ ? 0 : record_count_ - discarded;
    writer_.open(directory_, segment_size_, end);
}

WalPosition Wal::scan_start_for(int64_t seq_no) const {
    WalPosition start{0, 0};
    for (const uint64_t segment : segment_numbers()) {
        const int fd = ::open(segment_path_for_delete(segment).c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            break;
        }
        // The first entry's header: magic (4 bytes), payload size (4), record id (8), reserved (8).
        uint8_t header[24] = {};
        const ssize_t got = ::pread(fd, header, sizeof(header), 0);
        ::close(fd);
        uint32_t magic = 0;
        int64_t record_id = 0;
        std::memcpy(&magic, header, sizeof(magic));
        std::memcpy(&record_id, header + 8, sizeof(record_id));
        if (got != static_cast<ssize_t>(sizeof(header)) || magic != WalWriter::entry_magic || record_id > seq_no) {
            break;
        }
        start = WalPosition{segment, 0};
    }
    return start;
}

std::vector<uint64_t> Wal::segment_numbers() const {
    std::vector<uint64_t> numbers;
    DIR* dir = ::opendir(directory_.c_str());
    if (dir == nullptr) {
        return numbers;
    }
    while (const dirent* entry = ::readdir(dir)) {
        const std::string name(entry->d_name);
        if (name.size() != 14 || name.compare(0, 4, "wal_") != 0 || name.compare(10, 4, ".log") != 0) {
            continue;
        }
        char* parsed_end = nullptr;
        const unsigned long long number = std::strtoull(name.c_str() + 4, &parsed_end, 10);
        if (parsed_end == name.c_str() + 10) {
            numbers.push_back(number);
        }
    }
    ::closedir(dir);
    std::sort(numbers.begin(), numbers.end());
    return numbers;
}

void Wal::delete_segments_after(uint64_t seg_num) const {
    for (const uint64_t segment : segment_numbers()) {
        if (segment > seg_num) {
            ::unlink(segment_path_for_delete(segment).c_str());
        }
    }
}

void Wal::delete_segments_before(uint64_t seg_num) const {
    for (uint64_t i = 0; i < seg_num; ++i) {
        ::unlink(segment_path_for_delete(i).c_str());
    }
}

} // namespaces
