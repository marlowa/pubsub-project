// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "EngineReportPacing.hpp"

#include <algorithm>
#include <cstdint>
#include <string>

#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>

namespace matching_engine {

void EngineReportPacing::Reports::add(int64_t seq_no) {
    if (count == 0) {
        lowest_seq_no = seq_no;
        highest_seq_no = seq_no;
    } else {
        lowest_seq_no = std::min(lowest_seq_no, seq_no);
        highest_seq_no = std::max(highest_seq_no, seq_no);
    }
    ++count;
}

void EngineReportPacing::Reports::add(const Reports& other) {
    if (other.count == 0) {
        return;
    }
    if (count == 0) {
        *this = other;
        return;
    }
    lowest_seq_no = std::min(lowest_seq_no, other.lowest_seq_no);
    highest_seq_no = std::max(highest_seq_no, other.highest_seq_no);
    count += other.count;
}

EngineReportPacing::EngineReportPacing(EngineReportPacingLinksInterface& links, pubsub_itc_fw::QuillLogger& logger) : links_(links), logger_(logger) {}

EngineReportPacing::Connection* EngineReportPacing::find(const pubsub_itc_fw::ConnectionID& connection) {
    for (Connection& entry : connections_) {
        if (entry.id.is_valid() && entry.id == connection) {
            return &entry;
        }
    }
    return nullptr;
}

const EngineReportPacing::Connection* EngineReportPacing::find(const pubsub_itc_fw::ConnectionID& connection) const {
    for (const Connection& entry : connections_) {
        if (entry.id.is_valid() && entry.id == connection) {
            return &entry;
        }
    }
    return nullptr;
}

EngineReportPacing::Connection& EngineReportPacing::find_or_add(const pubsub_itc_fw::ConnectionID& connection) {
    Connection* found = find(connection);
    if (found != nullptr) {
        return *found;
    }
    // A connection for reports goes to one of the two sequencers, so a free entry exists unless a connection was replaced
    // without being reported lost; then the entry with fewer reports outstanding is reused.
    Connection* free_entry = &connections_[0];
    for (Connection& entry : connections_) {
        if (!entry.id.is_valid()) {
            free_entry = &entry;
            break;
        }
        if (entry.before_request.count + entry.since_request.count < free_entry->before_request.count + free_entry->since_request.count) {
            free_entry = &entry;
        }
    }
    *free_entry = Connection{};
    free_entry->id = connection;
    return *free_entry;
}

void EngineReportPacing::pace_on(const pubsub_itc_fw::ConnectionID& connection) {
    if (paced_ == connection) {
        return;
    }
    paced_ = connection;
    if (paused_) {
        resume();
    }
    Connection* found = find(connection);
    if (found == nullptr) {
        return;
    }
    if (found->notification_asked) {
        // A limit's worth was already waiting on it when it was counted as the other sequencer's connection.
        pause();
        return;
    }
    ask_if_needed(*found);
}

void EngineReportPacing::report_sent(const pubsub_itc_fw::ConnectionID& connection, int64_t seq_no) {
    if (!connection.is_valid()) {
        return;
    }
    Connection& entry = find_or_add(connection);
    entry.since_request.add(seq_no);
    summary_.most_not_known_written = std::max(summary_.most_not_known_written, entry.before_request.count + entry.since_request.count);
    ask_if_needed(entry);
}

void EngineReportPacing::ask_if_needed(Connection& connection) {
    if (connection.notification_asked || connection.since_request.count < pause_at_reports) {
        return;
    }
    connection.before_request.add(connection.since_request);
    connection.since_request = Reports{};
    connection.notification_asked = true;
    links_.request_written_notification(connection.id);
    if (connection.id == paced_ && !paused_) {
        pause();
    }
}

void EngineReportPacing::connection_written(const pubsub_itc_fw::ConnectionID& connection) {
    Connection* found = find(connection);
    if (found == nullptr || !found->notification_asked) {
        return;
    }
    // Everything sent before the request is written. What was sent after it is not known to be: it waits for the next.
    found->before_request = Reports{};
    found->notification_asked = false;
    ask_if_needed(*found);
    if (paused_ && !found->notification_asked && found->id == paced_) {
        resume();
    }
}

void EngineReportPacing::connection_lost(const pubsub_itc_fw::ConnectionID& connection, const std::string& sequencer_name) {
    Connection* found = find(connection);
    if (found == nullptr) {
        return;
    }
    Reports unknown = found->before_request;
    unknown.add(found->since_request);
    if (unknown.count > 0) {
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Error,
                   "EngineReportPacing: the connection for reports to the {} sequencer is lost with {} report(s) sent on it not known to have been written, "
                   "answering commands {} to {} -- the {} sequencer may not hold them, and their members may not be told",
                   sequencer_name, unknown.count, unknown.lowest_seq_no, unknown.highest_seq_no, sequencer_name);
    }
    *found = Connection{};
    if (connection == paced_) {
        paced_ = pubsub_itc_fw::ConnectionID{};
        if (paused_) {
            // No notification will come on a connection that is gone, so waiting for one would stop the engine for good.
            resume();
        }
    }
}

int64_t EngineReportPacing::not_known_written(const pubsub_itc_fw::ConnectionID& connection) const {
    const Connection* found = find(connection);
    return found == nullptr ? 0 : found->before_request.count + found->since_request.count;
}

void EngineReportPacing::pause() {
    if (!pausing_) {
        return;
    }
    ++summary_.pauses;
    paused_ = true;
    links_.pause_reading_commands();
}

void EngineReportPacing::resume() {
    paused_ = false;
    links_.resume_reading_commands();
}

} // namespaces
