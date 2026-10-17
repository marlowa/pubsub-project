// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <pubsub_itc_fw/WalHandedOverFollowers.hpp>

#include <optional>
#include <utility>

#include <pubsub_itc_fw/WalEntryContents.hpp>

namespace pubsub_itc_fw {

namespace {

// Whether position a comes before position b in the WAL.
bool before(const WalPosition& a, const WalPosition& b) {
    return a.segment < b.segment || (a.segment == b.segment && a.offset < b.offset);
}

} // un-named namespace

WalHandedOverFollowers::WalHandedOverFollowers(const Wal& wal, std::string directory, WalRecordConverter converter, WalHandedOverFollowersLinksInterface& links)
    : wal_(wal), directory_(std::move(directory)), converter_(std::move(converter)), links_(links) {}

void WalHandedOverFollowers::add(const ConnectionID& follower, int64_t through, WalPosition resume_at) {
    auto entry = std::make_unique<Follower>();
    entry->connection = follower;
    entry->last_sent_seq_no = through;
    entry->own_reading = std::make_unique<Reading>();
    entry->own_reading->cursor.open(directory_, resume_at);
    entry->own_reading->last_seq_no = through;
    followers_[follower.get_value()] = std::move(entry);
    close_shared_reading_if_unused();
}

void WalHandedOverFollowers::remove(const ConnectionID& follower) {
    followers_.erase(follower.get_value());
    close_shared_reading_if_unused();
}

void WalHandedOverFollowers::remove_all() {
    followers_.clear();
    shared_reading_.reset();
}

bool WalHandedOverFollowers::contains(const ConnectionID& follower) const {
    return followers_.find(follower.get_value()) != followers_.end();
}

int64_t WalHandedOverFollowers::last_sent(const ConnectionID& follower) const {
    const auto found = followers_.find(follower.get_value());
    return found == followers_.end() ? -1 : found->second->last_sent_seq_no;
}

void WalHandedOverFollowers::close_shared_reading_if_unused() {
    for (const auto& [value, entry] : followers_) {
        if (entry->own_reading == nullptr) {
            return;
        }
    }
    shared_reading_.reset();
}

bool WalHandedOverFollowers::read_next_record(Reading& reading) {
    // Only before the write position: everything there is completely written.
    const WalPosition written = wal_.write_position();
    while (before(reading.cursor.position(), written)) {
        int64_t record_id = 0;
        const uint8_t* bytes = nullptr;
        size_t size = 0;
        if (!reading.cursor.read_next(record_id, bytes, size)) {
            // The cursor lists the segment files when it is opened, so a segment created since is unknown to it.
            // Opening it again at its own position finds it.
            const WalPosition at = reading.cursor.position();
            reading.cursor.open(directory_, at);
            if (!reading.cursor.read_next(record_id, bytes, size)) {
                return false;
            }
        }
        if (record_id <= reading.last_seq_no) {
            continue; // dealt with already
        }
        reading.holding = true;
        reading.held_seq_no = record_id;
        reading.held_bytes = bytes;
        reading.held_size = size;
        return true;
    }
    return false;
}

ConvertedRecord WalHandedOverFollowers::convert_held(const Reading& reading) {
    // A record that cannot be read may have been one a follower needed; passing over it would leave the follower
    // holding less than it is taken to hold, so it is treated as damaged.
    const std::optional<WalEntryContents> contents = read_wal_entry_contents(reading.held_bytes, reading.held_size);
    encoded_.clear();
    if (!contents.has_value()) {
        return ConvertedRecord::damaged();
    }
    return converter_(reading.held_seq_no, *contents, encoded_);
}

WalHandedOverFollowers::Outcome WalHandedOverFollowers::send_on_own_reading(Follower& follower, int64_t through_seq_no) {
    Outcome outcome;
    Reading& reading = *follower.own_reading;
    while (reading.last_seq_no < through_seq_no) {
        if (!reading.holding && !read_next_record(reading)) {
            break; // nothing more written yet
        }
        if (reading.held_seq_no > through_seq_no) {
            break; // held until the limit passes it
        }
        const ConvertedRecord converted = convert_held(reading);
        if (converted.outcome == ConvertedRecord::Outcome::Damaged) {
            outcome.damaged_seq_no = reading.held_seq_no;
            return outcome;
        }
        if (converted.outcome == ConvertedRecord::Outcome::Send && !encoded_.empty()) {
            links_.send_record(follower.connection, converted.pdu_id, reading.held_seq_no, encoded_.data(), encoded_.size());
            ++outcome.records_sent;
        }
        reading.last_seq_no = reading.held_seq_no;
        follower.last_sent_seq_no = reading.held_seq_no;
        reading.holding = false;
    }
    return outcome;
}

WalHandedOverFollowers::Outcome WalHandedOverFollowers::send_to(const ConnectionID& follower, int64_t through_seq_no) {
    const auto found = followers_.find(follower.get_value());
    if (found == followers_.end() || found->second->own_reading == nullptr) {
        return Outcome{};
    }
    const Outcome outcome = send_on_own_reading(*found->second, through_seq_no);
    if (outcome.damaged_seq_no != 0) {
        followers_.erase(found);
        close_shared_reading_if_unused();
    }
    return outcome;
}

void WalHandedOverFollowers::send_on_shared_reading(int64_t through_seq_no, std::vector<std::pair<ConnectionID, int64_t>>& damaged) {
    Reading& reading = *shared_reading_;
    while (reading.last_seq_no < through_seq_no) {
        if (!reading.holding && !read_next_record(reading)) {
            break; // nothing more written yet
        }
        if (reading.held_seq_no > through_seq_no) {
            break; // held until the limit passes it
        }
        const int64_t seq_no = reading.held_seq_no;
        const ConvertedRecord converted = convert_held(reading);
        // Every follower in step that lacks this record is dealt with; one already past it, because it joined
        // ahead of the shared reading, is left alone.
        connections_.clear();
        for (const auto& [value, entry] : followers_) {
            if (entry->own_reading == nullptr && entry->last_sent_seq_no < seq_no) {
                connections_.push_back(entry->connection);
            }
        }
        for (const ConnectionID& connection : connections_) {
            if (converted.outcome == ConvertedRecord::Outcome::Damaged) {
                damaged.emplace_back(connection, seq_no);
                followers_.erase(connection.get_value());
                continue;
            }
            if (converted.outcome == ConvertedRecord::Outcome::Send && !encoded_.empty()) {
                links_.send_record(connection, converted.pdu_id, seq_no, encoded_.data(), encoded_.size());
            }
            followers_[connection.get_value()]->last_sent_seq_no = seq_no;
        }
        reading.last_seq_no = seq_no;
        reading.holding = false;
    }
}

std::vector<std::pair<ConnectionID, int64_t>> WalHandedOverFollowers::send_to_all(int64_t through_seq_no) {
    std::vector<std::pair<ConnectionID, int64_t>> damaged;

    // Followers still on their own cursors first. Collected before sending: a follower whose record is damaged is
    // removed, which would disturb the iteration. Called at every commit, so the list is kept and reused rather
    // than allocated each time.
    connections_.clear();
    for (const auto& [value, entry] : followers_) {
        if (entry->own_reading != nullptr) {
            connections_.push_back(entry->connection);
        }
    }
    for (const ConnectionID& connection : connections_) {
        Follower& follower = *followers_[connection.get_value()];
        const Outcome outcome = send_on_own_reading(follower, through_seq_no);
        if (outcome.damaged_seq_no != 0) {
            damaged.emplace_back(connection, outcome.damaged_seq_no);
            followers_.erase(connection.get_value());
            continue;
        }
        // A follower that holds every record the shared reading has read joins it. With no shared reading, the
        // follower's own reading becomes it.
        if (shared_reading_ == nullptr) {
            shared_reading_ = std::move(follower.own_reading);
        } else if (follower.last_sent_seq_no >= shared_reading_->last_seq_no) {
            follower.own_reading.reset();
        }
    }

    if (shared_reading_ != nullptr) {
        send_on_shared_reading(through_seq_no, damaged);
    }
    close_shared_reading_if_unused();
    return damaged;
}

} // namespaces
