// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "FixSerialiser.hpp"

#include <cstddef>
#include <ctime>
#include <string>
#include <string_view>

#include <fix_codec/FixMessageWriter.hpp>

namespace order_gateway {

namespace {

// Session-level tags the serialiser writes itself (MsgType, SenderCompID,
// TargetCompID, MsgSeqNum, SendingTime) or that FixMessageWriter frames
// (BeginString, BodyLength, Checksum). Any of these set on the FixMessage by a
// caller is skipped when its application fields are emitted, so they are never
// written twice.
bool is_session_managed_tag(int tag) {
    return tag == Tag::BeginString || tag == Tag::BodyLength || tag == Tag::Checksum || tag == Tag::MsgType || tag == Tag::SenderCompID ||
           tag == Tag::TargetCompID || tag == Tag::MsgSeqNum || tag == Tag::SendingTime;
}

// Comfortably larger than any session or admin message this gateway emits.
constexpr size_t serialise_buffer_size = 512;

} // namespaces

FixSerialiser::FixSerialiser(std::string sender_comp_id, std::string target_comp_id, const pubsub_itc_fw::WallClock& wall_clock)
    : sender_comp_id_(std::move(sender_comp_id)), target_comp_id_(std::move(target_comp_id)), wall_clock_(wall_clock) {}

std::string FixSerialiser::serialise(const FixMessage& msg, int seq_num) const {
    return serialise(msg, seq_num, target_comp_id_);
}

std::string FixSerialiser::serialise(const FixMessage& msg, int seq_num, const std::string& target_comp_id) const {
    // FixMessageWriter frames the message (BeginString, BodyLength, Checksum) and
    // draws tag numbers from the generated FIX dictionary, so there is no
    // hand-maintained field list or checksum arithmetic here.
    char buffer[serialise_buffer_size];
    fix_codec::FixMessageWriter writer(buffer, sizeof(buffer));

    writer.push_back_field(Tag::MsgType, msg.msg_type());
    writer.push_back_field(Tag::SenderCompID, sender_comp_id_);
    writer.push_back_field(Tag::TargetCompID, target_comp_id);
    writer.push_back_field(Tag::MsgSeqNum, seq_num);
    writer.push_back_field(Tag::SendingTime, current_utc_timestamp());

    // Every application field the caller set, in the order it was set.
    for (const FixMessage::Field& field : msg.fields()) {
        if (!is_session_managed_tag(field.first)) {
            writer.push_back_field(field.first, field.second);
        }
    }

    return std::string(writer.finish());
}

std::string FixSerialiser::current_utc_timestamp() const {
    const auto t = static_cast<std::time_t>(wall_clock_.now_ns() / 1'000'000'000LL);
    struct tm utc_tm {};
    gmtime_r(&t, &utc_tm);
    char timestamp_buffer[20];
    std::strftime(timestamp_buffer, sizeof(timestamp_buffer), "%Y%m%d-%H:%M:%S", &utc_tm);
    return {timestamp_buffer};
}

} // namespaces
