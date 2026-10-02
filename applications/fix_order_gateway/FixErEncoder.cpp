// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <ctime>
#include <string_view>

#include <FixErEncoder.hpp>

#include <fix_codec/FixMessageWriter.hpp>
#include <fix_codec/fix_dictionary.hpp>

#include <CancelRejection.hpp>
#include <FixMessage.hpp>

namespace fix_order_gateway {
namespace {

constexpr size_t timestamp_length = 17; // YYYYMMDD-HH:MM:SS

// Formats one wall-clock instant. Split from fill_utc_timestamp so a *stored* time can be
// rendered too: a resent report has to say when the event originally happened, which is not
// the same as when this copy is going out, and only the caller knows which is which.
void fill_utc_timestamp_from(char* out, int64_t wall_time_ns) {
    const auto t = static_cast<std::time_t>(wall_time_ns / 1'000'000'000LL);
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

void fill_utc_timestamp(char* out, const pubsub_itc_fw::WallClock& clock) {
    fill_utc_timestamp_from(out, clock.now_ns());
}

} // namespaces

// -- Public encoder ------------------------------------------------------------

std::string_view encode_execution_report(const pubsub_itc_fw_app::ExecutionReportView& view, std::string_view sender_comp_id, std::string_view target_comp_id,
                                         int seq_num, const pubsub_itc_fw::WallClock& wall_clock, char* output_buffer, size_t output_buffer_size, bool poss_dup,
                                         int64_t orig_sending_time_ns, bool poss_resend) {
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

    // Resent, not new. PossDupFlag is what stops a member treating a replayed report as a
    // fresh event, and OrigSendingTime is what lets it tell when the event actually happened
    // -- SendingTime above is only when this copy left. Both are standard header fields, so
    // no dictionary change is involved. Written immediately after SendingTime because that
    // is where the FIX standard header puts them.
    if (poss_dup) {
        writer.push_back_field(Tag::PossDupFlag, 'Y');
        char orig_timestamp_buffer[timestamp_length + 1];
        fill_utc_timestamp_from(orig_timestamp_buffer, orig_sending_time_ns);
        writer.push_back_field(Tag::OrigSendingTime, std::string_view{orig_timestamp_buffer, timestamp_length});
    }

    // A new message carrying an event the member may already have been told about, which is
    // what a matching engine catching up on the venue's record produces: it cannot tell a
    // record some earlier engine reported from one no engine ever saw, so it reports every
    // one and marks them all. PossResend is the standard field for exactly that, and it asks
    // the member to check the identifiers rather than to assume a second event. Also a
    // standard header field, and written here for the same reason PossDupFlag is.
    if (poss_resend) {
        writer.push_back_field(Tag::PossResend, 'Y');
    }

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
    // The presence flag alone is not enough. A request to cancel need not carry a quantity
    // (R-0144), and the matching engine copies the quantity into its report with the flag
    // set whether or not there was one, so a view reaches here holding an empty value.
    // "38=" with nothing after it is not a valid field, so check the value as well.
    if (view.has_order_qty && !view.order_qty.empty()) {
        writer.push_back_field(Tag::OrderQty, view.order_qty);
    }
    if (view.has_price) {
        writer.push_back_field(Tag::Price, view.price);
    }
    if (view.has_ord_type) {
        writer.push_back_field(Tag::OrdType, static_cast<char>(view.ord_type));
    }
    // The order's time-in-force terms, when the matching engine echoed them.
    //
    // Neither was written before, so a member never saw them on a report -- which is how a
    // GoodTillDate order came to be acknowledged with no expiry on it. The report is the
    // venue's statement of the order it accepted, and for a GTD order the expiry is one of
    // the terms; a member that cannot see it back has no way to confirm the venue holds what
    // it sent.
    if (view.has_time_in_force) {
        writer.push_back_field(Tag::TimeInForce, static_cast<char>(view.time_in_force));
    }
    if (view.has_expire_time) {
        char expire_buffer[timestamp_length + 1];
        fill_utc_timestamp_from(expire_buffer, view.expire_time);
        writer.push_back_field(Tag::ExpireTime, std::string_view{expire_buffer, timestamp_length});
    }
    writer.push_back_field(Tag::CumQty, view.cum_qty);
    writer.push_back_field(Tag::LeavesQty, view.leaves_qty);

    // Repeating groups the matching engine echoed onto the ER (NoUnderlyings, NoPartyIDs
    // with nested NoPartySubIDs). Emitted after the flat fields; each instance leads with
    // its delimiter tag (UnderlyingSymbol / PartyID / PartySubID). Tag numbers come from
    // the generated FIX dictionary. The gateway Tag enum only covers the flat ER fields.
    if (view.no_underlyings.size > 0) {
        writer.push_back_field(fix_codec::tag::NoUnderlyings, static_cast<int>(view.no_underlyings.size));
        for (size_t i = 0; i < view.no_underlyings.size; ++i) {
            const pubsub_itc_fw_app::UnderlyingsView& underlying = view.no_underlyings.data[i];
            if (underlying.has_underlying_symbol) {
                writer.push_back_field(fix_codec::tag::UnderlyingSymbol, underlying.underlying_symbol);
            }
            if (underlying.has_underlying_security_id) {
                writer.push_back_field(fix_codec::tag::UnderlyingSecurityID, underlying.underlying_security_id);
            }
            if (underlying.has_underlying_qty) {
                writer.push_back_field(fix_codec::tag::UnderlyingQty, underlying.underlying_qty);
            }
        }
    }
    if (view.no_party_i_ds.size > 0) {
        writer.push_back_field(fix_codec::tag::NoPartyIDs, static_cast<int>(view.no_party_i_ds.size));
        for (size_t i = 0; i < view.no_party_i_ds.size; ++i) {
            const pubsub_itc_fw_app::PartyIDsView& party = view.no_party_i_ds.data[i];
            if (party.has_party_id) {
                writer.push_back_field(fix_codec::tag::PartyID, party.party_id);
            }
            if (party.has_party_id_source) {
                writer.push_back_field(fix_codec::tag::PartyIDSource, static_cast<char>(party.party_id_source));
            }
            if (party.has_party_role) {
                writer.push_back_field(fix_codec::tag::PartyRole, static_cast<int>(party.party_role));
            }
            if (party.no_party_sub_i_ds.size > 0) {
                writer.push_back_field(fix_codec::tag::NoPartySubIDs, static_cast<int>(party.no_party_sub_i_ds.size));
                for (size_t j = 0; j < party.no_party_sub_i_ds.size; ++j) {
                    const pubsub_itc_fw_app::PartySubIDsView& sub = party.no_party_sub_i_ds.data[j];
                    if (sub.has_party_sub_id) {
                        writer.push_back_field(fix_codec::tag::PartySubID, sub.party_sub_id);
                    }
                    if (sub.has_party_sub_id_type) {
                        writer.push_back_field(fix_codec::tag::PartySubIDType, static_cast<int>(sub.party_sub_id_type));
                    }
                }
            }
        }
    }

    return writer.finish(); // empty view if the buffer overflowed
}

std::string_view encode_order_cancel_reject(const pubsub_itc_fw_app::ExecutionReportView& view, std::string_view sender_comp_id,
                                            std::string_view target_comp_id, int seq_num, const pubsub_itc_fw::WallClock& wall_clock, char* output_buffer,
                                            size_t output_buffer_size, bool poss_dup, int64_t orig_sending_time_ns, bool poss_resend) {
    char timestamp_buffer[timestamp_length + 1];
    fill_utc_timestamp(timestamp_buffer, wall_clock);
    const std::string_view timestamp{timestamp_buffer, timestamp_length};

    fix_codec::FixMessageWriter writer(output_buffer, output_buffer_size);

    writer.push_back_field(Tag::MsgType, fix_codec::msg_type::OrderCancelReject);
    writer.push_back_field(Tag::SenderCompID, sender_comp_id);
    writer.push_back_field(Tag::TargetCompID, target_comp_id);
    writer.push_back_field(Tag::MsgSeqNum, seq_num);
    writer.push_back_field(Tag::SendingTime, timestamp);

    // The same header flags, for the same reasons, as encode_execution_report: a refused cancel
    // can be resent to a member or replayed by a matching engine catching up, like any report.
    if (poss_dup) {
        writer.push_back_field(Tag::PossDupFlag, 'Y');
        char orig_timestamp_buffer[timestamp_length + 1];
        fill_utc_timestamp_from(orig_timestamp_buffer, orig_sending_time_ns);
        writer.push_back_field(Tag::OrigSendingTime, std::string_view{orig_timestamp_buffer, timestamp_length});
    }
    if (poss_resend) {
        writer.push_back_field(Tag::PossResend, 'Y');
    }

    writer.push_back_field(Tag::OrderID, view.order_id);
    if (view.has_cl_ord_id) {
        writer.push_back_field(Tag::ClOrdID, view.cl_ord_id);
    }
    writer.push_back_field(Tag::OrigClOrdID, view.orig_cl_ord_id);
    writer.push_back_field(Tag::OrdStatus, static_cast<char>(view.ord_status));
    // 1 = Order cancel request. The venue cannot yet amend an order, so the engine refuses only
    // cancels; an engine that refused an amend would report 2.
    writer.push_back_field(Tag::CxlRejResponseTo, '1');
    writer.push_back_field(Tag::CxlRejReason, static_cast<int>(fix_common::cancel_reject_reason_for(view)));
    if (view.has_text && !view.text.empty()) {
        writer.push_back_field(Tag::Text, view.text);
    }

    return writer.finish(); // empty view if the buffer overflowed
}

} // namespaces
