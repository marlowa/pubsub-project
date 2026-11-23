// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "VenueStateFromTheWal.hpp"

#include <algorithm>
#include <string>
#include <utility>

#include <fmt/format.h>

#include <pubsub_itc_fw/BumpAllocator.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>

#include <fix_orders.hpp>
#include <leader_follower.hpp>

#include "VoidFromWal.hpp"

namespace sequencer {

VenueStateFromTheWal::Noted VenueStateFromTheWal::note_record(int64_t seq_no, const Record& record, How how) {
    Noted noted{};
    if (record.pdu_id == static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::ExecutionReport) && record.has_answers_seq_no) {
        highest_command_answered_ = std::max(highest_command_answered_, record.answers_seq_no);
    }
    if (record.has_report_position && (!latest_engine_report_.has_value() || record.report_position.is_after(*latest_engine_report_))) {
        latest_engine_report_ = record.report_position;
        latest_engine_report_seq_no_ = seq_no;
    }
    noted.void_added = note_void(seq_no, record);
    if (noted.void_added && how == How::written_by_this_leader) {
        // The engines are sent the voids once this record is committed (take_voids_due), because one that is not may yet be
        // lost with the leader that wrote it, and an engine told of it would void commands that every other engine applies.
        void_awaiting_commit_ = seq_no;
    }
    note_technical_event(seq_no, record, noted);
    note_session_cancel(seq_no, record);
    note_trading_schedule(seq_no, record, noted);
    return noted;
}

void VenueStateFromTheWal::note_trading_schedule(int64_t seq_no, const Record& record, Noted& noted) {
    if (record.pdu_id != static_cast<int16_t>(pubsub_itc_fw_app::TradingSchedule::message_pdu_id)) {
        return;
    }
    pubsub_itc_fw::BumpAllocator allocator(decode_arena_.data(), decode_arena_.size());
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw_app::TradingScheduleView schedule{};
    if (!pubsub_itc_fw_app::decode(schedule, record.payload, record.size, bytes_consumed, allocator, arena_bytes_needed)) {
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Error,
                   "VenueStateFromTheWal: the TradingSchedule at seq={} did not decode -- its times are not known, and the schedule before it, if any, is "
                   "kept",
                   seq_no);
        return;
    }
    if (schedule.trading_day != trading_phase_.trading_day()) {
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Error,
                   "VenueStateFromTheWal: the TradingSchedule at seq={} states trading day {}, where the WAL's start of day states {} -- a WAL holds one "
                   "trading day, so this record is from another; it is kept as the latest all the same",
                   seq_no, schedule.trading_day, trading_phase_.trading_day());
    }
    TradingScheduleState::ScheduleRecord kept{};
    kept.seq_no = seq_no;
    kept.trading_day = schedule.trading_day;
    kept.operator_name = std::string(schedule.operator_name);
    for (const pubsub_itc_fw_app::TradingScheduleEntryView& entry : schedule.entries) {
        kept.entries.push_back(ScheduledPhase{entry.phase, entry.earliest_utc_ns});
    }
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Warning,
               "VenueStateFromTheWal: the WAL holds a TradingSchedule at seq={} for trading day {}, supplied by operator {}: {}", seq_no, schedule.trading_day,
               kept.operator_name, schedule_in_words(kept.entries));
    trading_schedule_.note_schedule(std::move(kept));
    noted.schedule_changed = true;
}

void VenueStateFromTheWal::note_session_cancel(int64_t seq_no, const Record& record) {
    pubsub_itc_fw::BumpAllocator allocator(decode_arena_.data(), decode_arena_.size());
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    if (record.pdu_id == static_cast<int16_t>(pubsub_itc_fw_app::SessionEnded::message_pdu_id)) {
        pubsub_itc_fw_app::SessionEndedView ended{};
        if (!pubsub_itc_fw_app::decode(ended, record.payload, record.size, bytes_consumed, allocator, arena_bytes_needed)) {
            PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Error,
                       "VenueStateFromTheWal: the SessionEnded at seq={} did not decode -- the cancel on disconnect it records is not kept", seq_no);
            return;
        }
        session_cancels_.ended(fix_common::SessionIdentity::make(ended.comp_id, ended.gateway_protocol_id), seq_no, record.wall_time_ns,
                               ended.grace_period_seconds);
    } else if (record.pdu_id == static_cast<int16_t>(pubsub_itc_fw_app::SessionEndWithdrawn::message_pdu_id)) {
        pubsub_itc_fw_app::SessionEndWithdrawnView withdrawn{};
        if (!pubsub_itc_fw_app::decode(withdrawn, record.payload, record.size, bytes_consumed, allocator, arena_bytes_needed)) {
            PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Error, "VenueStateFromTheWal: the SessionEndWithdrawn at seq={} did not decode", seq_no);
            return;
        }
        session_cancels_.withdrawn(fix_common::SessionIdentity::make(withdrawn.comp_id, withdrawn.gateway_protocol_id));
    } else if (record.pdu_id == static_cast<int16_t>(pubsub_itc_fw_app::CancelSessionOrders::message_pdu_id)) {
        pubsub_itc_fw_app::CancelSessionOrdersView cancel{};
        if (!pubsub_itc_fw_app::decode(cancel, record.payload, record.size, bytes_consumed, allocator, arena_bytes_needed)) {
            PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Error, "VenueStateFromTheWal: the CancelSessionOrders at seq={} did not decode", seq_no);
            return;
        }
        session_cancels_.carried_out(fix_common::SessionIdentity::make(cancel.comp_id, cancel.gateway_protocol_id), seq_no);
    }
}

void VenueStateFromTheWal::record_did_not_decode(int64_t seq_no, How how) {
    if (how == How::read_at_open) {
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Error, "VenueStateFromTheWal: the record at seq={} did not decode -- if it holds a void, it is not kept",
                   seq_no);
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

void VenueStateFromTheWal::note_technical_event(int64_t seq_no, const Record& record, Noted& noted) {
    if (record.pdu_id == static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::NewOrderSingle) ||
        record.pdu_id == static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::OrderCancelRequest)) {
        trading_halt_.note_members_command(seq_no);
        return;
    }
    if (record.pdu_id != static_cast<int16_t>(pubsub_itc_fw_app::TechnicalEvent::message_pdu_id)) {
        return;
    }
    pubsub_itc_fw::BumpAllocator allocator(decode_arena_.data(), decode_arena_.size());
    size_t arena_bytes_needed = 0;
    size_t bytes_consumed = 0;
    pubsub_itc_fw_app::TechnicalEventView event{};
    if (!pubsub_itc_fw_app::decode(event, record.payload, record.size, bytes_consumed, allocator, arena_bytes_needed)) {
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Error,
                   "VenueStateFromTheWal: the TechnicalEvent at seq={} did not decode -- the phase it enters and whether it halts trading are not known, and "
                   "it is not kept",
                   seq_no);
        return;
    }
    if (trading_phase_.started() && event.trading_day != trading_phase_.trading_day()) {
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Error,
                   "VenueStateFromTheWal: the TechnicalEvent at seq={} states trading day {}, where the WAL's start of day states {} -- a WAL holds one "
                   "trading day, so this record is from another; it is kept as the latest all the same",
                   seq_no, event.trading_day, trading_phase_.trading_day());
    }
    noted.phase_changed = trading_phase_.note_technical_event(seq_no, event.phase, event.trading_day);
    noted.halt_changed = trading_halt_.note_technical_event(seq_no, event.halted, event.reason);
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Warning,
               "VenueStateFromTheWal: the WAL holds a TechnicalEvent at seq={} ({}) -- trading day {}, phase {}, trading is {}{}; the last order or cancel "
               "before it is seq={}",
               seq_no, pubsub_itc_fw_app::to_string(event.reason), event.trading_day, pubsub_itc_fw_app::to_string(event.phase),
               event.halted ? "halted" : "not halted", noted.halt_changed || noted.phase_changed ? "" : ", as it was already",
               trading_halt_.latest()->last_members_command_before);
}

bool VenueStateFromTheWal::discard_after(int64_t seq_no) {
    // A void whose record is discarded is no longer in the WAL. Its record was never committed, so no engine was told of
    // it.
    voids_.discard_after(seq_no);
    session_cancels_.discard_after(seq_no);
    if (trading_schedule_.discard_after(seq_no)) {
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Warning, "VenueStateFromTheWal: the WAL is cut back to seq={}, removing a TradingSchedule -- {}", seq_no,
                   trading_schedule_.recorded() ? fmt::format("the schedule is the one at seq={}: {}", trading_schedule_.latest()->seq_no,
                                                              schedule_in_words(trading_schedule_.latest()->entries))
                                                : std::string("the WAL now holds no schedule"));
    }
    // The reports before the latest one cannot be told apart without reading the WAL again, so the latest is forgotten
    // until the next report is written. Until then nothing waits on it is ready, which is the safe way to be wrong.
    if (latest_engine_report_seq_no_ > seq_no) {
        latest_engine_report_.reset();
        latest_engine_report_seq_no_ = 0;
    }
    // As for a void: a TechnicalEvent whose record is discarded is no longer in the WAL, and the phase and whether
    // trading is halted are what the latest one left says.
    if (trading_phase_.discard_after(seq_no)) {
        const std::optional<TradingPhaseState::PhaseRecord> latest = trading_phase_.latest();
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Warning, "VenueStateFromTheWal: the WAL is cut back to seq={}, removing a TechnicalEvent -- {}", seq_no,
                   latest.has_value() ? fmt::format("the venue is in phase {} of trading day {} as the WAL now stands",
                                                    pubsub_itc_fw_app::to_string(latest->phase), latest->trading_day)
                                      : std::string("the WAL now holds no start of day"));
    }
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

} // namespaces
