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
    entry->cursor.open(directory_, resume_at);
    followers_[follower.get_value()] = std::move(entry);
}

void WalHandedOverFollowers::remove(const ConnectionID& follower) {
    followers_.erase(follower.get_value());
}

void WalHandedOverFollowers::remove_all() {
    followers_.clear();
}

bool WalHandedOverFollowers::contains(const ConnectionID& follower) const {
    return followers_.find(follower.get_value()) != followers_.end();
}

int64_t WalHandedOverFollowers::last_sent(const ConnectionID& follower) const {
    const auto found = followers_.find(follower.get_value());
    return found == followers_.end() ? -1 : found->second->last_sent_seq_no;
}

bool WalHandedOverFollowers::read_next_record(Follower& follower) {
    // Only before the write position: everything there is completely written.
    const WalPosition written = wal_.write_position();
    while (before(follower.cursor.position(), written)) {
        int64_t record_id = 0;
        const uint8_t* bytes = nullptr;
        size_t size = 0;
        if (!follower.cursor.read_next(record_id, bytes, size)) {
            // The cursor lists the segment files when it is opened, so a segment created since is unknown to it.
            // Opening it again at its own position finds it.
            const WalPosition at = follower.cursor.position();
            follower.cursor.open(directory_, at);
            if (!follower.cursor.read_next(record_id, bytes, size)) {
                return false;
            }
        }
        if (record_id <= follower.last_sent_seq_no) {
            continue; // the follower holds it already
        }
        follower.holding = true;
        follower.held_seq_no = record_id;
        follower.held_bytes = bytes;
        follower.held_size = size;
        return true;
    }
    return false;
}

WalHandedOverFollowers::Outcome WalHandedOverFollowers::send_to(const ConnectionID& follower, int64_t through_seq_no) {
    Outcome outcome;
    const auto found = followers_.find(follower.get_value());
    if (found == followers_.end()) {
        return outcome;
    }
    Follower& entry = *found->second;
    while (entry.last_sent_seq_no < through_seq_no) {
        if (!entry.holding && !read_next_record(entry)) {
            break; // nothing more written yet
        }
        if (entry.held_seq_no > through_seq_no) {
            break; // held until the limit passes it
        }
        // A record that cannot be read may have been one the follower needed; passing over it would leave the
        // follower holding less than it is taken to hold, so nothing more is sent to it.
        const std::optional<WalEntryContents> contents = read_wal_entry_contents(entry.held_bytes, entry.held_size);
        ConvertedRecord converted = ConvertedRecord::damaged();
        if (contents.has_value()) {
            encoded_.clear();
            converted = converter_(entry.held_seq_no, *contents, encoded_);
        }
        if (converted.outcome == ConvertedRecord::Outcome::Damaged) {
            outcome.damaged_seq_no = entry.held_seq_no;
            followers_.erase(found);
            return outcome;
        }
        if (converted.outcome == ConvertedRecord::Outcome::Send && !encoded_.empty()) {
            links_.send_record(entry.connection, converted.pdu_id, entry.held_seq_no, encoded_.data(), encoded_.size());
            ++outcome.records_sent;
        }
        entry.last_sent_seq_no = entry.held_seq_no;
        entry.holding = false;
    }
    return outcome;
}

std::vector<std::pair<ConnectionID, int64_t>> WalHandedOverFollowers::send_to_all(int64_t through_seq_no) {
    std::vector<std::pair<ConnectionID, int64_t>> damaged;
    // Collected first: send_to removes a follower whose record is damaged, which would disturb the iteration.
    // Called at every commit, so the list is kept and reused rather than allocated each time.
    connections_.clear();
    for (const auto& [value, entry] : followers_) {
        connections_.push_back(entry->connection);
    }
    for (const ConnectionID& connection : connections_) {
        const Outcome outcome = send_to(connection, through_seq_no);
        if (outcome.damaged_seq_no != 0) {
            damaged.emplace_back(connection, outcome.damaged_seq_no);
        }
    }
    return damaged;
}

} // namespaces
