// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <pubsub_itc_fw/WalFollowerClient.hpp>

#include <cstdint>
#include <string>
#include <utility>

namespace pubsub_itc_fw {

WalFollowerClient::WalFollowerClient(std::string follower_name, bool wants_new_records, int64_t last_seq_no, WalFollowerClientLinksInterface& links)
    : follower_name_(std::move(follower_name)), wants_new_records_(wants_new_records), last_seq_no_(last_seq_no), links_(links) {}

void WalFollowerClient::ask(const ConnectionID& connection) {
    received_since_request_[connection.get_value()] = 0;
    caught_up_ = false;
    WalFollowRequest request{};
    request.follower_name = follower_name_;
    request.last_seq_no = last_seq_no_;
    request.wants_new_records = wants_new_records_;
    links_.send_follow_request(connection, request);
}

void WalFollowerClient::on_connected(const ConnectionID& connection) {
    ask(connection);
}

void WalFollowerClient::on_disconnected(const ConnectionID& connection) {
    received_since_request_.erase(connection.get_value());
}

void WalFollowerClient::on_declined(const ConnectionID& /*connection*/) {}

void WalFollowerClient::on_leader_changed(const ConnectionID& connection) {
    ask(connection);
}

WalFollowerClient::RecordVerdict WalFollowerClient::on_record(const ConnectionID& connection, int64_t seq_no) {
    // Every record sent is counted, held or not, because the instance counts every record it sent.
    ++received_since_request_[connection.get_value()];
    if (last_seq_no_ >= 0 && seq_no <= last_seq_no_) {
        return RecordVerdict::AlreadyHeld;
    }
    if (last_seq_no_ >= 0 && seq_no != last_seq_no_ + 1) {
        ask(connection);
        return RecordVerdict::Gap;
    }
    last_seq_no_ = seq_no;
    return RecordVerdict::Accept;
}

WalFollowerClient::CaughtUpVerdict WalFollowerClient::on_caught_up(const ConnectionID& connection, const WalFollowCaughtUpView& caught_up) {
    const auto counted = received_since_request_.find(connection.get_value());
    const int64_t received = counted == received_since_request_.end() ? 0 : counted->second;
    if (received != caught_up.records_sent) {
        ask(connection);
        return CaughtUpVerdict::CountMismatch;
    }
    // A follower that held nothing is placed at the newest record, and holds everything up to it from now.
    if (last_seq_no_ < 0) {
        last_seq_no_ = caught_up.last_seq_no;
    }
    caught_up_ = true;
    first_seq_no_held_ = caught_up.first_seq_no_held;
    return CaughtUpVerdict::CaughtUp;
}

} // namespaces
