// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "FixSerialiser.hpp"

#include <cstdio>
#include <ctime>
#include <string>

namespace order_gateway {

namespace {
constexpr char fix_delimiter = '\x01';
} // namespace

FixSerialiser::FixSerialiser(std::string sender_comp_id, std::string target_comp_id, const pubsub_itc_fw::WallClock& wall_clock)
    : sender_comp_id_(std::move(sender_comp_id)), target_comp_id_(std::move(target_comp_id)), wall_clock_(wall_clock) {}

std::string FixSerialiser::serialise(const FixMessage& msg, int seq_num) const {
    return serialise(msg, seq_num, target_comp_id_);
}

std::string FixSerialiser::serialise(const FixMessage& msg, int seq_num,
                                     const std::string& target_comp_id) const {
    std::string body;
    append_field(body, Tag::MsgType, msg.msg_type());
    append_field(body, Tag::SenderCompID, sender_comp_id_);
    append_field(body, Tag::TargetCompID, target_comp_id);
    append_field(body, Tag::MsgSeqNum, seq_num);
    append_field(body, Tag::SendingTime, current_utc_timestamp());

    // Append all application fields from msg, skipping session-level tags
    // that we manage ourselves.
    // We iterate by known application tags rather than exposing the map
    // to keep FixMessage's internals private. For this sample the set of
    // application tags is small and fixed.
    constexpr int app_tags[] = {
        Tag::EncryptMethod, Tag::HeartBtInt, Tag::DefaultApplVerID, Tag::Text,  Tag::ClOrdID, Tag::OrderID, Tag::ExecID,    Tag::ExecType, Tag::OrdStatus,
        Tag::Symbol,        Tag::Side,       Tag::OrderQty,         Tag::Price, Tag::OrdType, Tag::CumQty,  Tag::LeavesQty,
    };

    for (const int tag : app_tags) {
        if (msg.has(tag)) {
            append_field(body, tag, msg.get(tag));
        }
    }

    // Build the complete message: BeginString + BodyLength + body + Checksum.
    std::string frame;
    append_field(frame, Tag::BeginString, "FIXT.1.1");
    append_field(frame, Tag::BodyLength, static_cast<int>(body.size()));
    frame += body;

    const std::string checksum = compute_checksum(frame);
    append_field(frame, Tag::Checksum, checksum);

    return frame;
}

void FixSerialiser::append_field(std::string& output, int tag, const std::string& value) {
    output += std::to_string(tag);
    output += '=';
    output += value;
    output += fix_delimiter;
}

void FixSerialiser::append_field(std::string& output, int tag, int value) {
    append_field(output, tag, std::to_string(value));
}

std::string FixSerialiser::compute_checksum(const std::string& input) {
    unsigned int sum = 0;
    for (const unsigned char c : input) {
        sum += c;
    }
    char result[5];
    std::snprintf(result, sizeof(result), "%03u", sum % 256U);
    return {result};
}

std::string FixSerialiser::current_utc_timestamp() const {
    const auto t = static_cast<std::time_t>(wall_clock_.now_ns() / 1'000'000'000LL);
    struct tm utc_tm {};
    gmtime_r(&t, &utc_tm);
    char timestamp_buffer[20];
    std::strftime(timestamp_buffer, sizeof(timestamp_buffer), "%Y%m%d-%H:%M:%S", &utc_tm);
    return {timestamp_buffer};
}

} // namespace order_gateway
