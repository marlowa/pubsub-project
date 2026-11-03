// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "VenueStateFromTheWal.hpp"

#include <algorithm>

#include <pubsub_itc_fw/BumpAllocator.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>

#include <fix_orders.hpp>
#include <leader_follower.hpp>

#include "TradingDate.hpp"
#include "VoidFromWal.hpp"

namespace sequencer {

VenueStateFromTheWal::Noted VenueStateFromTheWal::note_record(int64_t seq_no, const Record& record, How how) {
    Noted noted{};
    // A follower learns the trading date from each command its leader wrote, so that on taking the lead it writes the
    // date its leader wrote. A leader reads it from the first record in its WAL instead, and the WAL read at opening is
    // not consulted for it: the first record says it.
    if (how == How::written_as_follower) {
        note_trading_date(seq_no, record);
    }
    if (record.pdu_id == static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::ExecutionReport) && record.has_answers_seq_no) {
        highest_command_answered_ = std::max(highest_command_answered_, record.answers_seq_no);
    }
    noted.void_added = note_void(seq_no, record);
    if (noted.void_added && how == How::written_by_this_leader) {
        // The engines are sent the voids once this record is committed (take_voids_due), because one that is not may yet be
        // lost with the leader that wrote it, and an engine told of it would void commands that every other engine applies.
        void_awaiting_commit_ = seq_no;
    }
    noted.halt_changed = note_trading_halt(seq_no, record);
    return noted;
}

void VenueStateFromTheWal::record_did_not_decode(int64_t seq_no, How how) {
    if (how == How::read_at_open) {
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Error, "VenueStateFromTheWal: the record at seq={} did not decode -- if it holds a void, it is not kept",
                   seq_no);
    }
}

void VenueStateFromTheWal::note_trading_date(int64_t seq_no, const Record& record) {
    if (record.has_trading_date && record.trading_date != trading_date_) {
        if (trading_date_ != 0) {
            PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Error,
                       "VenueStateFromTheWal: the leader wrote trading date {} on record {}, where this instance had {} -- following the leader",
                       record.trading_date, seq_no, trading_date_);
        }
        trading_date_ = record.trading_date;
    }
}

bool VenueStateFromTheWal::note_void(int64_t seq_no, const Record& record) {
    fix_common::VoidedRange voided{};
    const VoidFromWal found = void_from_command(seq_no, record.pdu_id, record.payload, record.size, decode_arena_, voided);
    if (found == VoidFromWal::NotDecodable) {
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Error, "VenueStateFromTheWal: the record at seq={} did not decode -- if it holds a void, it is not kept",
                   seq_no);
        return false;
    }
    if (found != VoidFromWal::Void || !voids_.add(voided)) {
        return false;
    }
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Warning,
               "VenueStateFromTheWal: the WAL holds a void at seq={} of the {}commands from seq={} to seq={} ({}); {} void(s) in all", seq_no,
               voided.new_orders_only ? "new orders among the " : "", voided.first_seq_no, voided.last_seq_no, pubsub_itc_fw_app::to_string(voided.reason),
               voids_.size());
    return true;
}

bool VenueStateFromTheWal::note_trading_halt(int64_t seq_no, const Record& record) {
    if (record.pdu_id == static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::NewOrderSingle) ||
        record.pdu_id == static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::OrderCancelRequest)) {
        trading_halt_.note_members_command(seq_no);
        return false;
    }
    if (record.pdu_id != static_cast<int16_t>(pubsub_itc_fw_app::TechnicalEvent::message_pdu_id)) {
        return false;
    }
    pubsub_itc_fw::BumpAllocator allocator(decode_arena_.data(), decode_arena_.size());
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw_app::TechnicalEventView event{};
    if (!pubsub_itc_fw_app::decode(event, record.payload, record.size, bytes_consumed, allocator, arena_bytes_needed)) {
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Error,
                   "VenueStateFromTheWal: the TechnicalEvent at seq={} did not decode -- whether it halts trading is not known, and it is not kept", seq_no);
        return false;
    }
    const bool changed = trading_halt_.note_technical_event(seq_no, event.halted, event.reason);
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Warning,
               "VenueStateFromTheWal: the WAL holds a TechnicalEvent at seq={} ({}) -- trading is {}{}; the last order or cancel before it is seq={}", seq_no,
               pubsub_itc_fw_app::to_string(event.reason), event.halted ? "halted" : "not halted", changed ? "" : ", as it was already",
               trading_halt_.latest()->last_members_command_before);
    return changed;
}

bool VenueStateFromTheWal::discard_after(int64_t seq_no) {
    // A void whose record is discarded is no longer in the WAL. Its record was never committed, so no engine was told of
    // it.
    voids_.discard_after(seq_no);
    // As for a void: a TechnicalEvent whose record is discarded is no longer in the WAL, and whether trading is halted is
    // what the latest one left says.
    if (!trading_halt_.discard_after(seq_no)) {
        return false;
    }
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Warning,
               "VenueStateFromTheWal: the WAL is cut back to seq={}, removing a TechnicalEvent -- trading is {} as the WAL now stands", seq_no,
               trading_halt_.halted() ? "halted" : "not halted");
    return true;
}

bool VenueStateFromTheWal::take_voids_due(int64_t committed_through) {
    if (void_awaiting_commit_ == 0 || committed_through < void_awaiting_commit_) {
        return false;
    }
    void_awaiting_commit_ = 0;
    return true;
}

int32_t VenueStateFromTheWal::trading_date(std::optional<int64_t> first_record_wall_time_ns, int64_t first_seq_no, int64_t wall_time_ns) {
    if (trading_date_ != 0) {
        return trading_date_;
    }
    if (first_record_wall_time_ns.has_value()) {
        trading_date_ = fix_common::trading_date_of(*first_record_wall_time_ns);
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info, "VenueStateFromTheWal: the trading date is {}, the date of record {}, the first in the WAL",
                   fix_common::trading_date_text(trading_date_), first_seq_no);
    } else {
        trading_date_ = fix_common::trading_date_of(wall_time_ns);
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info,
                   "VenueStateFromTheWal: the trading date is {}, the date of the command being written, which is the first record in the WAL",
                   fix_common::trading_date_text(trading_date_));
    }
    return trading_date_;
}

} // namespaces
