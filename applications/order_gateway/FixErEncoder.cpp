// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cassert>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <ctime>

#include <FixErEncoder.hpp>

#include <FixMessage.hpp>

namespace order_gateway {
namespace {

static constexpr char fix_delimiter = '\x01';
static constexpr size_t timestamp_length = 17; // YYYYMMDD-HH:MM:SS

// ── Wire writer ──────────────────────────────────────────────────────────────

struct FixWireWriter {
    char* cursor;
    char* limit;
    bool valid{true};

    void write_sv(std::string_view sv) {
        const size_t n = sv.size();
        if (cursor + n > limit) {
            valid = false;
            return;
        }
        std::memcpy(cursor, sv.data(), n);
        cursor += n;
    }

    void write_char(char c) {
        if (cursor >= limit) {
            valid = false;
            return;
        }
        *cursor++ = c;
    }

    void write_int(int v) {
        auto [end, ec] = std::to_chars(cursor, limit, v);
        if (ec != std::errc{}) {
            valid = false;
            return;
        }
        cursor = end;
    }

    void field(int tag, std::string_view value) {
        write_int(tag);
        write_char('=');
        write_sv(value);
        write_char(fix_delimiter);
    }

    void field_char(int tag, char c) {
        write_int(tag);
        write_char('=');
        write_char(c);
        write_char(fix_delimiter);
    }

    void field_int(int tag, int value) {
        write_int(tag);
        write_char('=');
        write_int(value);
        write_char(fix_delimiter);
    }
};

// ── Field-size helpers (arithmetic only, no writing) ─────────────────────────

size_t count_digits(int v) {
    if (v < 10)
        return 1;
    if (v < 100)
        return 2;
    if (v < 1000)
        return 3;
    if (v < 10000)
        return 4;
    if (v < 100000)
        return 5;
    if (v < 1000000)
        return 6;
    if (v < 10000000)
        return 7;
    return 8;
}

// Wire size of "tag=value\x01"
size_t field_size(int tag, std::string_view value) {
    return count_digits(tag) + 1 + value.size() + 1;
}

// Wire size of "tag=X\x01" (single-char value)
size_t field_size_char(int tag) {
    return count_digits(tag) + 3;
}

// Wire size of "tag=N\x01" (integer value)
size_t field_size_int(int tag, int value) {
    return count_digits(tag) + 1 + count_digits(value) + 1;
}

// ── Checksum ─────────────────────────────────────────────────────────────────

uint8_t compute_checksum(const char* buf, size_t length) {
    unsigned sum = 0;
    for (size_t i = 0; i < length; ++i) {
        sum += static_cast<unsigned char>(buf[i]);
    }
    return static_cast<uint8_t>(sum & 0xFFu);
}

// ── Timestamp ────────────────────────────────────────────────────────────────

void fill_utc_timestamp(char* out, const pubsub_itc_fw::WallClock& clock) {
    const std::time_t t = static_cast<std::time_t>(clock.now_ns() / 1'000'000'000LL);
    struct tm utc {};
    gmtime_r(&t, &utc);
    std::strftime(out, timestamp_length + 1, "%Y%m%d-%H:%M:%S", &utc);
}

} // namespace

// ── Public encoder ────────────────────────────────────────────────────────────

size_t encode_execution_report(const pubsub_itc_fw_app::ExecutionReportView& view, std::string_view sender_comp_id, std::string_view target_comp_id,
                               int seq_num, const pubsub_itc_fw::WallClock& wall_clock,
                               char* output_buffer, size_t output_buffer_size) {
    // Single-char wire representations of enum fields.
    const char exec_type_char = static_cast<char>(view.exec_type);
    const char ord_status_char = static_cast<char>(view.ord_status);
    const char side_char = static_cast<char>(view.side);

    // Stack-allocated timestamp — no heap allocation.
    char timestamp_buffer[timestamp_length + 1];
    fill_utc_timestamp(timestamp_buffer, wall_clock);
    const std::string_view timestamp{timestamp_buffer, timestamp_length};

    static constexpr std::string_view er_msg_type = "8";

    // Pre-compute body length so it can be written before the body itself.
    // Body = everything from MsgType (tag 35) up to but not including Checksum (tag 10).
    size_t body_length = field_size(Tag::MsgType, er_msg_type) + field_size(Tag::SenderCompID, sender_comp_id) + field_size(Tag::TargetCompID, target_comp_id) +
                         field_size_int(Tag::MsgSeqNum, seq_num) + field_size(Tag::SendingTime, timestamp) + field_size(Tag::OrderID, view.order_id) +
                         field_size(Tag::ExecID, view.exec_id) + field_size_char(Tag::ExecType) + field_size_char(Tag::OrdStatus) +
                         field_size(Tag::Symbol, view.symbol) + field_size_char(Tag::Side) + field_size(Tag::CumQty, view.cum_qty) +
                         field_size(Tag::LeavesQty, view.leaves_qty);

    if (view.has_cl_ord_id) {
        body_length += field_size(Tag::ClOrdID, view.cl_ord_id);
    }
    if (view.has_orig_cl_ord_id) {
        body_length += field_size(Tag::OrigClOrdID, view.orig_cl_ord_id);
    }
    if (view.has_order_qty) {
        body_length += field_size(Tag::OrderQty, view.order_qty);
    }
    if (view.has_price) {
        body_length += field_size(Tag::Price, view.price);
    }
    if (view.has_ord_type) {
        body_length += field_size_char(Tag::OrdType);
    }
    if (view.has_ord_rej_reason) {
        body_length += field_size_int(Tag::OrdRejReason, static_cast<int>(view.ord_rej_reason));
    }

    // Write the complete FIX message in a single pass.
    FixWireWriter writer{output_buffer, output_buffer + output_buffer_size};

    writer.field(Tag::BeginString, "FIXT.1.1");
    writer.field_int(Tag::BodyLength, static_cast<int>(body_length));

    [[maybe_unused]] const char* body_start = writer.cursor;

    // Header fields (body begins here).
    writer.field(Tag::MsgType, er_msg_type);
    writer.field(Tag::SenderCompID, sender_comp_id);
    writer.field(Tag::TargetCompID, target_comp_id);
    writer.field_int(Tag::MsgSeqNum, seq_num);
    writer.field(Tag::SendingTime, timestamp);

    // App fields in the same order as FixSerialiser.
    if (view.has_cl_ord_id) {
        writer.field(Tag::ClOrdID, view.cl_ord_id);
    }
    if (view.has_orig_cl_ord_id) {
        writer.field(Tag::OrigClOrdID, view.orig_cl_ord_id);
    }
    writer.field(Tag::OrderID, view.order_id);
    writer.field(Tag::ExecID, view.exec_id);
    writer.field_char(Tag::ExecType, exec_type_char);
    writer.field_char(Tag::OrdStatus, ord_status_char);
    if (view.has_ord_rej_reason) {
        writer.field_int(Tag::OrdRejReason, static_cast<int>(view.ord_rej_reason));
    }
    writer.field(Tag::Symbol, view.symbol);
    writer.field_char(Tag::Side, side_char);
    if (view.has_order_qty) {
        writer.field(Tag::OrderQty, view.order_qty);
    }
    if (view.has_price) {
        writer.field(Tag::Price, view.price);
    }
    if (view.has_ord_type) {
        writer.field_char(Tag::OrdType, static_cast<char>(view.ord_type));
    }
    writer.field(Tag::CumQty, view.cum_qty);
    writer.field(Tag::LeavesQty, view.leaves_qty);

    assert(static_cast<size_t>(writer.cursor - body_start) == body_length);

    // Checksum over everything written so far.
    const uint8_t checksum = compute_checksum(output_buffer, static_cast<size_t>(writer.cursor - output_buffer));
    char checksum_str[4];
    std::snprintf(checksum_str, sizeof(checksum_str), "%03u", static_cast<unsigned>(checksum));
    writer.field(Tag::Checksum, std::string_view{checksum_str, 3});

    return writer.valid ? static_cast<size_t>(writer.cursor - output_buffer) : 0;
}

} // namespace order_gateway
