// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <ctime>
#include <string_view>

#include <FixErEncoder.hpp>

#include <fix_codec/FixMessageWriter.hpp>
#include <fix_codec/fix_dictionary.hpp>

#include <FixMessage.hpp>

namespace order_gateway {
namespace {

constexpr size_t timestamp_length = 17; // YYYYMMDD-HH:MM:SS

void fill_utc_timestamp(char* out, const pubsub_itc_fw::WallClock& clock) {
    const auto t = static_cast<std::time_t>(clock.now_ns() / 1'000'000'000LL);
    struct tm utc {};
    gmtime_r(&t, &utc);

    // Manual digit formatting -- no strftime, no printf machinery, no locale.
    // Format: YYYYMMDD-HH:MM:SS (exactly timestamp_length = 17 bytes).
    const int year = utc.tm_year + 1900;
    const int month = utc.tm_mon + 1;
    const int day = utc.tm_mday;
    const int hour = utc.tm_hour;
    const int min = utc.tm_min;
    const int sec = utc.tm_sec;

    out[0] = static_cast<char>('0' + year / 1000);
    out[1] = static_cast<char>('0' + (year / 100) % 10);
    out[2] = static_cast<char>('0' + (year / 10) % 10);
    out[3] = static_cast<char>('0' + year % 10);
    out[4] = static_cast<char>('0' + month / 10);
    out[5] = static_cast<char>('0' + month % 10);
    out[6] = static_cast<char>('0' + day / 10);
    out[7] = static_cast<char>('0' + day % 10);
    out[8] = '-';
    out[9] = static_cast<char>('0' + hour / 10);
    out[10] = static_cast<char>('0' + hour % 10);
    out[11] = ':';
    out[12] = static_cast<char>('0' + min / 10);
    out[13] = static_cast<char>('0' + min % 10);
    out[14] = ':';
    out[15] = static_cast<char>('0' + sec / 10);
    out[16] = static_cast<char>('0' + sec % 10);
    // No NUL terminator -- callers use timestamp_length directly.
}

} // namespaces

// -- Public encoder ------------------------------------------------------------

std::string_view encode_execution_report(const pubsub_itc_fw_app::ExecutionReportView& view, std::string_view sender_comp_id, std::string_view target_comp_id,
                                         int seq_num, const pubsub_itc_fw::WallClock& wall_clock, char* output_buffer, size_t output_buffer_size) {
    // Stack-allocated timestamp -- no heap allocation.
    char timestamp_buffer[timestamp_length + 1];
    fill_utc_timestamp(timestamp_buffer, wall_clock);
    const std::string_view timestamp{timestamp_buffer, timestamp_length};

    // FixMessageWriter frames the message (BeginString, BodyLength, Checksum) into
    // output_buffer and draws tag numbers from the generated FIX dictionary. The
    // returned view is the wire message; it does not start at output_buffer[0]
    // because the header is written backward into a reserved prefix.
    fix_codec::FixMessageWriter writer(output_buffer, output_buffer_size);

    writer.push_back_field(Tag::MsgType, fix_codec::msg_type::ExecutionReport);
    writer.push_back_field(Tag::SenderCompID, sender_comp_id);
    writer.push_back_field(Tag::TargetCompID, target_comp_id);
    writer.push_back_field(Tag::MsgSeqNum, seq_num);
    writer.push_back_field(Tag::SendingTime, timestamp);

    if (view.has_cl_ord_id) {
        writer.push_back_field(Tag::ClOrdID, view.cl_ord_id);
    }
    if (view.has_orig_cl_ord_id) {
        writer.push_back_field(Tag::OrigClOrdID, view.orig_cl_ord_id);
    }
    writer.push_back_field(Tag::OrderID, view.order_id);
    writer.push_back_field(Tag::ExecID, view.exec_id);
    writer.push_back_field(Tag::ExecType, static_cast<char>(view.exec_type));
    writer.push_back_field(Tag::OrdStatus, static_cast<char>(view.ord_status));
    if (view.has_ord_rej_reason) {
        writer.push_back_field(Tag::OrdRejReason, static_cast<int>(view.ord_rej_reason));
    }
    writer.push_back_field(Tag::Symbol, view.symbol);
    writer.push_back_field(Tag::Side, static_cast<char>(view.side));
    if (view.has_order_qty) {
        writer.push_back_field(Tag::OrderQty, view.order_qty);
    }
    if (view.has_price) {
        writer.push_back_field(Tag::Price, view.price);
    }
    if (view.has_ord_type) {
        writer.push_back_field(Tag::OrdType, static_cast<char>(view.ord_type));
    }
    writer.push_back_field(Tag::CumQty, view.cum_qty);
    writer.push_back_field(Tag::LeavesQty, view.leaves_qty);

    return writer.finish(); // empty view if the buffer overflowed
}

} // namespaces
