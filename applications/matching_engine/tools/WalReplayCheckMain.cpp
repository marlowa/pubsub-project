// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file WalReplayCheckMain.cpp
 * @brief wal_replay_check: whether a fresh matching engine, given a sequencer's WAL, produces the reports the WAL holds.
 *
 *   wal_replay_check --wal <directory> --region-capacity <records> --work-dir <directory>
 *
 * The sequencer's WAL holds every command, and every report the engine that led sent it, each written when the
 * sequencer received it, with the sequence number of the command it answers. This program reads the WAL once,
 * applies each new order and cancel, in order, to a fresh state machine (MatchingEngineStateMachine) with an empty
 * book, and compares every report the state machine produces with the first copy of the report with the same ExecID
 * in the WAL, field by field. It ignores only the mark that says a report is a possible repeat, and the order in
 * which the sequencer received the reports, which is not the order the engine produced them in when two arrive on
 * different connections.
 *
 * Any difference is a failure, and is printed with the command it belongs to: a report the state machine produced
 * that the WAL does not hold, a report the WAL holds that the state machine did not produce, or a field that differs.
 * It exits 0 when there is none, 1 when there is any, and 2 when it could not do the comparison.
 *
 * The state machine numbers its reports from 1, as an engine started with an empty book does, so the comparison is
 * meaningful for a WAL in which one engine led from the start and was neither restarted nor replaced: an engine that
 * restarted numbers its reports again from 1. That is what phase 7 of the recovery plan puts right next
 * (docs/availability/deterministic_matching_engine.md, sections 3 and 7.2).
 *
 * The book's region is created in the work directory, which must be on a filesystem with room for it (about 250 bytes
 * for each record), never in /tmp, and is removed afterwards. --region-capacity must be the capacity the engine was
 * configured with, because an order arriving when the region is full is refused.
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include <pubsub_itc_fw/BumpAllocator.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/PubSubItcException.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/WalEntryContents.hpp>
#include <pubsub_itc_fw/WalPosition.hpp>
#include <pubsub_itc_fw/WalReader.hpp>

#include <fix_orders.hpp>
#include <leader_follower.hpp>

#include "GatewayIds.hpp"
#include "MatchingEngineStateMachine.hpp"
#include "OrderBook.hpp"

namespace {

using matching_engine::CommandSource;
using matching_engine::MatchingEngineOutputsInterface;
using matching_engine::MatchingEngineStateMachine;
using matching_engine::OrderBook;
using matching_engine::OrderEntry;
using matching_engine::ReportIsRepeat;

// What is compared of a report, with its strings copied out. Every field the engine fills in, and the sizes of the
// two groups it echoes.
struct ReportFields {
    std::string order_id;
    std::string exec_id;
    int exec_type{0};
    int ord_status{0};
    std::string symbol;
    int side{0};
    std::string leaves_qty;
    std::string cum_qty;
    std::string avg_px;
    int64_t transact_time{0};
    std::optional<std::string> cl_ord_id;
    std::optional<std::string> orig_cl_ord_id;
    std::optional<int> ord_type;
    std::optional<std::string> price;
    std::optional<std::string> order_qty;
    std::optional<int> time_in_force;
    std::optional<int64_t> expire_time;
    std::optional<int> ord_rej_reason;
    std::optional<std::string> text;
    size_t party_count{0};
    size_t underlying_count{0};
};

// An ExecutionReport and an ExecutionReportView have the same fields, so one function reads either.
template <typename Report> ReportFields fields_of(const Report& report) {
    ReportFields fields;
    fields.order_id = std::string(report.order_id);
    fields.exec_id = std::string(report.exec_id);
    fields.exec_type = static_cast<int>(report.exec_type);
    fields.ord_status = static_cast<int>(report.ord_status);
    fields.symbol = std::string(report.symbol);
    fields.side = static_cast<int>(report.side);
    fields.leaves_qty = std::string(report.leaves_qty);
    fields.cum_qty = std::string(report.cum_qty);
    fields.avg_px = std::string(report.avg_px);
    fields.transact_time = report.transact_time;
    if (report.has_cl_ord_id) {
        fields.cl_ord_id = std::string(report.cl_ord_id);
    }
    if (report.has_orig_cl_ord_id) {
        fields.orig_cl_ord_id = std::string(report.orig_cl_ord_id);
    }
    if (report.has_ord_type) {
        fields.ord_type = static_cast<int>(report.ord_type);
    }
    if (report.has_price) {
        fields.price = std::string(report.price);
    }
    if (report.has_order_qty) {
        fields.order_qty = std::string(report.order_qty);
    }
    if (report.has_time_in_force) {
        fields.time_in_force = static_cast<int>(report.time_in_force);
    }
    if (report.has_expire_time) {
        fields.expire_time = report.expire_time;
    }
    if (report.has_ord_rej_reason) {
        fields.ord_rej_reason = static_cast<int>(report.ord_rej_reason);
    }
    if (report.has_text) {
        fields.text = std::string(report.text);
    }
    fields.party_count = report.no_party_i_ds.size;
    fields.underlying_count = report.no_underlyings.size;
    return fields;
}

template <typename T> std::string shown(const std::optional<T>& value) {
    return value.has_value() ? fmt::format("{}", *value) : std::string("(absent)");
}

// Each field that differs, as "name: replayed X, WAL Y".
std::vector<std::string> differences(const ReportFields& replayed, const ReportFields& logged) {
    std::vector<std::string> found;
    const auto compare = [&found](const char* name, const auto& left, const auto& right) {
        if (!(left == right)) {
            found.push_back(fmt::format("{}: replayed {}, WAL {}", name, left, right));
        }
    };
    const auto compare_optional = [&found](const char* name, const auto& left, const auto& right) {
        if (!(left == right)) {
            found.push_back(fmt::format("{}: replayed {}, WAL {}", name, shown(left), shown(right)));
        }
    };
    compare("OrderID", replayed.order_id, logged.order_id);
    compare("ExecType", replayed.exec_type, logged.exec_type);
    compare("OrdStatus", replayed.ord_status, logged.ord_status);
    compare("Symbol", replayed.symbol, logged.symbol);
    compare("Side", replayed.side, logged.side);
    compare("LeavesQty", replayed.leaves_qty, logged.leaves_qty);
    compare("CumQty", replayed.cum_qty, logged.cum_qty);
    compare("AvgPx", replayed.avg_px, logged.avg_px);
    compare("TransactTime", replayed.transact_time, logged.transact_time);
    compare_optional("ClOrdID", replayed.cl_ord_id, logged.cl_ord_id);
    compare_optional("OrigClOrdID", replayed.orig_cl_ord_id, logged.orig_cl_ord_id);
    compare_optional("OrdType", replayed.ord_type, logged.ord_type);
    compare_optional("Price", replayed.price, logged.price);
    compare_optional("OrderQty", replayed.order_qty, logged.order_qty);
    compare_optional("TimeInForce", replayed.time_in_force, logged.time_in_force);
    compare_optional("ExpireTime", replayed.expire_time, logged.expire_time);
    compare_optional("OrdRejReason", replayed.ord_rej_reason, logged.ord_rej_reason);
    compare_optional("Text", replayed.text, logged.text);
    compare("NoPartyIDs entries", replayed.party_count, logged.party_count);
    compare("NoUnderlyings entries", replayed.underlying_count, logged.underlying_count);
    return found;
}

// A report the state machine produced, and the command it answers.
struct ProducedReport {
    int64_t sequence_number{0};
    ReportFields fields;
};

// A report the WAL holds: the first copy with its ExecID, the command it answers, and how many copies there are.
struct LoggedReport {
    std::optional<int64_t> answers_seq_no;
    int64_t wal_seq_no{0};
    ReportFields fields;
    int copies{0};
    bool matched{false};
};

// Keeps every report the state machine produces. The changes to the book are not needed: the book is compared through
// the reports, which describe every order the engine accepts and every one it cancels.
class RecordingOutputs : public MatchingEngineOutputsInterface {
  public:
    void send_report(const pubsub_itc_fw_app::ExecutionReport& report, int64_t sequence_number, const fix_common::SessionIdentity& /*session*/,
                     ReportIsRepeat /*repeat*/) override {
        produced.push_back(ProducedReport{sequence_number, fields_of(report)});
    }
    void order_added_to_book(int64_t /*sequence_number*/, const fix_common::SessionIdentity& /*session*/, std::string_view /*cl_ord_id*/,
                             const OrderEntry& /*entry*/) override {}
    void order_removed_from_book(int64_t /*sequence_number*/, const fix_common::SessionIdentity& /*session*/, std::string_view /*cl_ord_id*/) override {}
    void order_outcome(const fix_common::SessionIdentity& /*session*/, std::string_view /*cl_ord_id*/, pubsub_itc_fw_app::OrderOutcomeKind /*outcome*/,
                       ReportIsRepeat /*repeat*/, int64_t /*sequence_number*/, std::string_view /*orig_cl_ord_id*/) override {}
    void order_accepted() override {}

    std::vector<ProducedReport> produced;
};

// The session a command belongs to, as the matching engine works it out from the command's envelope.
fix_common::SessionIdentity session_of(const pubsub_itc_fw_app::WalRecordView& envelope) {
    if (!envelope.has_sender_comp_id || envelope.sender_comp_id.empty()) {
        return fix_common::SessionIdentity{};
    }
    return fix_common::SessionIdentity::make(envelope.sender_comp_id,
                                             envelope.has_origin_gateway_id ? envelope.origin_gateway_id : gateway_ids::default_when_absent);
}

int usage() {
    fmt::print(stderr, "usage: wal_replay_check --wal <directory> --region-capacity <records> --work-dir <directory>\n");
    return 2;
}

// The differences printed in full; past this many, only counted.
constexpr size_t differences_shown = 20;

} // un-named namespace

int main(int argc, char** argv) {
    std::string wal_directory;
    std::string work_directory;
    long region_capacity = 0;
    for (int index = 1; index + 1 < argc; index += 2) {
        const std::string_view option = argv[index];
        if (option == "--wal") {
            wal_directory = argv[index + 1];
        } else if (option == "--work-dir") {
            work_directory = argv[index + 1];
        } else if (option == "--region-capacity") {
            region_capacity = std::atol(argv[index + 1]);
        } else {
            return usage();
        }
    }
    if (argc % 2 == 0 || wal_directory.empty() || work_directory.empty() || region_capacity <= 0) {
        return usage();
    }

    const std::filesystem::path region_path = std::filesystem::path(work_directory) / "wal_replay_check.region";
    std::error_code ignored;
    std::filesystem::remove(region_path, ignored);

    // Critical only, which the state machine never logs: what it logs about each command, such as a refusal because the
    // region is full, would bury the comparison, which reports every difference itself.
    pubsub_itc_fw::QuillLogger logger(pubsub_itc_fw::FwLogLevel::Critical);
    RecordingOutputs outputs;
    std::map<std::string, LoggedReport> logged;
    int64_t commands = 0;
    int64_t report_records = 0;
    int64_t undecodable = 0;
    int exit_status = 0;
    {
        OrderBook book;
        book.open(region_path.string(), static_cast<OrderBook::SlotIndex>(region_capacity), static_cast<size_t>(region_capacity));
        MatchingEngineStateMachine machine{book, outputs, logger};
        std::vector<uint8_t> arena_buffer(1U << 20);

        try {
            static_cast<void>(
                pubsub_itc_fw::WalReader::replay(wal_directory, pubsub_itc_fw::WalPosition{}, [&](int64_t record_id, const void* payload, size_t size) {
                    const std::optional<pubsub_itc_fw::WalEntryContents> contents = pubsub_itc_fw::read_wal_entry_contents(payload, size);
                    if (!contents.has_value() || contents->pdu_id != pubsub_itc_fw_app::WalRecord::message_pdu_id) {
                        return;
                    }
                    pubsub_itc_fw::BumpAllocator arena(arena_buffer.data(), arena_buffer.size());
                    size_t arena_bytes_needed = 0;
                    size_t bytes_consumed = 0;
                    pubsub_itc_fw_app::WalRecordView envelope{};
                    if (!pubsub_itc_fw_app::decode(envelope, contents->bytes, contents->size, bytes_consumed, arena, arena_bytes_needed)) {
                        ++undecodable;
                        return;
                    }
                    const auto inner = static_cast<pubsub_itc_fw_app::PduId::PduIdTag>(envelope.pdu_id);
                    if (inner == pubsub_itc_fw_app::PduId::PduIdTag::NewOrderSingle) {
                        pubsub_itc_fw_app::NewOrderSingleView order{};
                        if (!pubsub_itc_fw_app::decode(order, envelope.payload.data, envelope.payload.size, bytes_consumed, arena, arena_bytes_needed)) {
                            ++undecodable;
                            return;
                        }
                        ++commands;
                        machine.apply_new_order(order, record_id, envelope.wall_time_ns, session_of(envelope), CommandSource::live);
                    } else if (inner == pubsub_itc_fw_app::PduId::PduIdTag::OrderCancelRequest) {
                        pubsub_itc_fw_app::OrderCancelRequestView cancel{};
                        if (!pubsub_itc_fw_app::decode(cancel, envelope.payload.data, envelope.payload.size, bytes_consumed, arena, arena_bytes_needed)) {
                            ++undecodable;
                            return;
                        }
                        ++commands;
                        machine.apply_cancel(cancel, record_id, envelope.wall_time_ns, session_of(envelope), CommandSource::live);
                    } else if (inner == pubsub_itc_fw_app::PduId::PduIdTag::ExecutionReport) {
                        pubsub_itc_fw_app::ExecutionReportView report{};
                        if (!pubsub_itc_fw_app::decode(report, envelope.payload.data, envelope.payload.size, bytes_consumed, arena, arena_bytes_needed)) {
                            ++undecodable;
                            return;
                        }
                        ++report_records;
                        LoggedReport& entry = logged[std::string(report.exec_id)];
                        if (entry.copies++ == 0) {
                            entry.fields = fields_of(report);
                            entry.wal_seq_no = record_id;
                            if (envelope.has_answers_seq_no) {
                                entry.answers_seq_no = envelope.answers_seq_no;
                            }
                        }
                    }
                }));
        } catch (const pubsub_itc_fw::PubSubItcException& error) {
            fmt::print(stderr, "wal_replay_check: could not read the WAL in {}: {}\n", wal_directory, error.what());
            exit_status = 2;
        }
    }
    std::filesystem::remove(region_path, ignored);
    if (exit_status != 0) {
        return exit_status;
    }

    std::vector<std::string> failures;
    for (const ProducedReport& produced : outputs.produced) {
        const auto found = logged.find(produced.fields.exec_id);
        if (found == logged.end()) {
            failures.push_back(fmt::format("command {}: the replay produced report ExecID={} ({} for ClOrdID={}) and the WAL holds no report with that ExecID",
                                           produced.sequence_number, produced.fields.exec_id, produced.fields.order_id, shown(produced.fields.cl_ord_id)));
            continue;
        }
        LoggedReport& entry = found->second;
        entry.matched = true;
        if (entry.answers_seq_no.has_value() && *entry.answers_seq_no != produced.sequence_number) {
            failures.push_back(fmt::format("command {}: report ExecID={} answers command {} in the WAL", produced.sequence_number, produced.fields.exec_id,
                                           *entry.answers_seq_no));
        }
        for (const std::string& difference : differences(produced.fields, entry.fields)) {
            failures.push_back(fmt::format("command {}: report ExecID={} differs -- {}", produced.sequence_number, produced.fields.exec_id, difference));
        }
    }
    for (const auto& [exec_id, entry] : logged) {
        if (!entry.matched) {
            failures.push_back(fmt::format("command {}: the WAL holds report ExecID={} (record {}) and the replay produced no report with that ExecID",
                                           entry.answers_seq_no.has_value() ? std::to_string(*entry.answers_seq_no) : std::string("(none)"), exec_id,
                                           entry.wal_seq_no));
        }
    }

    int64_t repeated_copies = 0;
    for (const auto& [exec_id, entry] : logged) {
        repeated_copies += entry.copies - 1;
    }
    fmt::print("wal_replay_check: {} command(s) applied; the replay produced {} report(s); the WAL holds {} report record(s), {} distinct, {} repeated "
               "copies; {} record(s) could not be decoded\n",
               commands, outputs.produced.size(), report_records, logged.size(), repeated_copies, undecodable);
    for (size_t index = 0; index < failures.size() && index < differences_shown; ++index) {
        fmt::print("  {}\n", failures[index]);
    }
    if (failures.size() > differences_shown) {
        fmt::print("  ... and {} more\n", failures.size() - differences_shown);
    }
    if (!failures.empty() || undecodable > 0) {
        fmt::print("wal_replay_check: FAIL -- {} difference(s)\n", failures.size());
        return 1;
    }
    fmt::print("wal_replay_check: PASS -- every report the replay produced is in the WAL as the engine sent it, and every report in the WAL was "
               "produced\n");
    return 0;
}
