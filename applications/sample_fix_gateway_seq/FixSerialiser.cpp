// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "FixSerialiser.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>

namespace sample_fix_gateway_seq {

static constexpr char SOH = '\x01';

FixSerialiser::FixSerialiser(std::string sender_comp_id, std::string target_comp_id)
    : sender_comp_id_(std::move(sender_comp_id))
    , target_comp_id_(std::move(target_comp_id))
{
}

std::string FixSerialiser::serialise(const FixMessage& msg, int seq_num) const
{
    // Build the body -- everything after tag 9 and before tag 10.
    // Order follows the FIX standard header field ordering.
    std::string body;
    append_field(body, Tag::MsgType,      msg.msg_type());
    append_field(body, Tag::SenderCompID, sender_comp_id_);
    append_field(body, Tag::TargetCompID, target_comp_id_);
    append_field(body, Tag::MsgSeqNum,    seq_num);
    append_field(body, Tag::SendingTime,  current_utc_timestamp());

    // Append all application fields from msg, skipping session-level tags
    // that we manage ourselves.
    // We iterate by known application tags rather than exposing the map
    // to keep FixMessage's internals private. For this sample the set of
    // application tags is small and fixed.
    constexpr int app_tags[] = {
        Tag::EncryptMethod,
        Tag::HeartBtInt,
        Tag::Text,
        Tag::ClOrdID,
        Tag::OrderID,
        Tag::ExecID,
        Tag::ExecType,
        Tag::OrdStatus,
        Tag::Symbol,
        Tag::Side,
        Tag::OrderQty,
        Tag::Price,
        Tag::OrdType,
        Tag::CumQty,
        Tag::LeavesQty,
    };

    for (const int tag : app_tags) {
        if (msg.has(tag)) {
            append_field(body, tag, msg.get(tag));
        }
    }

    // Build the complete message: BeginString + BodyLength + body + Checksum.
    std::string frame;
    append_field(frame, Tag::BeginString, "FIXT.1.1");
    append_field(frame, Tag::BodyLength,  static_cast<int>(body.size()));
    frame += body;

    const std::string checksum = compute_checksum(frame);
    append_field(frame, Tag::Checksum, checksum);

    return frame;
}

void FixSerialiser::append_field(std::string& buf, int tag, const std::string& value)
{
    buf += std::to_string(tag);
    buf += '=';
    buf += value;
    buf += SOH;
}

void FixSerialiser::append_field(std::string& buf, int tag, int value)
{
    append_field(buf, tag, std::to_string(value));
}

std::string FixSerialiser::compute_checksum(const std::string& buf)
{
    unsigned int sum = 0;
    for (const unsigned char c : buf) {
        sum += c;
    }
    char result[5];
    std::snprintf(result, sizeof(result), "%03u", sum % 256u);
    return {result};
}

std::string FixSerialiser::current_utc_timestamp()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    struct tm utc_tm{};
    gmtime_r(&t, &utc_tm);
    char buf[20];
    std::strftime(buf, sizeof(buf), "%Y%m%d-%H:%M:%S", &utc_tm);
    return {buf};
}

} // namespace sample_fix_gateway
