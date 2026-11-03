// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "EngineReportSending.hpp"

#include <algorithm>
#include <utility>

#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>

#include <fix_orders.hpp>

namespace matching_engine {

EngineReportSending::EngineReportSending(bool secondary_expected, EngineReportSendingLinksInterface& links, pubsub_itc_fw::QuillLogger& logger)
    : secondary_expected_(secondary_expected), links_(links), logger_(logger) {}

void EngineReportSending::connection_established(Sequencer sequencer, const pubsub_itc_fw::ConnectionID& connection) {
    (sequencer == Sequencer::primary ? primary_ : secondary_) = connection;
}

void EngineReportSending::connection_lost(Sequencer sequencer) {
    (sequencer == Sequencer::primary ? primary_ : secondary_) = pubsub_itc_fw::ConnectionID{};
}

HeldEngineReport EngineReportSending::held_report_of(const Report& report) {
    HeldEngineReport held;
    held.payload.assign(report.bytes, report.bytes + report.size);
    held.seq_no = report.seq_no;
    held.session = *report.session;
    held.poss_resend = report.poss_resend;
    return held;
}

bool EngineReportSending::send(const Report& report, int32_t epoch, bool holding_until_entitled) {
    if (holding_until_entitled) {
        if (held_.size() < max_held_reports) {
            held_.push_back(held_report_of(report));
            return false;
        }
        if (!overflowed_until_entitled_) {
            overflowed_until_entitled_ = true;
            PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Warning,
                       "EngineReportSending: {} reports held while waiting to be told what this instance may do, which is as many as are kept -- the rest "
                       "are sent as they are produced. A member may receive a report for an order this instance never serves; it is marked as a possible "
                       "repeat and can be discarded, which is the lesser harm",
                       held_.size());
        }
    }

    if (!primary_.is_valid() && !secondary_.is_valid()) {
        if (!waiting_for_connection_) {
            waiting_for_connection_ = true;
            PUBSUB_LOG_STR(logger_, pubsub_itc_fw::FwLogLevel::Warning,
                           "EngineReportSending: no connection to a sequencer for reports -- acting on orders and holding their reports until one is "
                           "established");
            links_.reports_held(static_cast<int64_t>(held_.size()) + 1);
        }
        if (held_.size() < max_held_reports) {
            held_.push_back(held_report_of(report));
        } else if (!overflowed_without_connection_) {
            overflowed_without_connection_ = true;
            PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Error,
                       "EngineReportSending: {} reports held with no connection to a sequencer, which is as many as are kept -- later reports are lost "
                       "until a connection is established",
                       held_.size());
        }
        return false;
    }

    pubsub_itc_fw_app::WalRecord envelope{};
    envelope.seq_no = report.seq_no;
    envelope.pdu_id = static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::ExecutionReport);
    envelope.payload.data = report.bytes;
    envelope.payload.size = report.size;
    envelope.has_sender_comp_id = !report.session->empty();
    envelope.sender_comp_id = report.session->comp_id_view();
    envelope.has_origin_gateway_id = !report.session->empty();
    envelope.origin_gateway_id = report.session->protocol;
    envelope.poss_resend = report.poss_resend;
    envelope.has_gateway_ingress_ns = report.has_gateway_ingress_ns;
    envelope.gateway_ingress_ns = report.gateway_ingress_ns;
    number(envelope, epoch);
    if (primary_.is_valid()) {
        links_.send_report(primary_, report.seq_no, envelope);
    }
    if (secondary_.is_valid()) {
        links_.send_report(secondary_, report.seq_no, envelope);
    }
    return true;
}

void EngineReportSending::release_now_serving(int32_t epoch) {
    if (held_.empty()) {
        overflowed_until_entitled_ = false;
        overflowed_without_connection_ = false;
        waiting_for_connection_ = false;
        return;
    }
    // With no connection to a sequencer the reports would go nowhere, so they stay held until one is established, when
    // release_for_connection sends them.
    if (!primary_.is_valid() && !secondary_.is_valid()) {
        return;
    }
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info,
               "EngineReportSending: releasing {} report(s) held while this instance was becoming current -- it now serves, so the members who placed "
               "those orders are told",
               held_.size());
    links_.reports_released(pubsub_itc_fw_app::ReleaseReason::NowServes, static_cast<int64_t>(held_.size()));
    send_held(primary_.is_valid(), secondary_.is_valid(), epoch);
}

void EngineReportSending::release_for_connection(int32_t epoch, bool holding_until_entitled) {
    // Reports held while the engine waits to be told whether it may serve stay held until it is told. Any others go
    // now: those held for want of a connection, and those of an engine told it may serve while it had no connection.
    if (held_.empty() || holding_until_entitled) {
        return;
    }
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info,
               "EngineReportSending: a connection to a sequencer for reports is established -- sending {} report(s) held without one", held_.size());
    links_.reports_released(pubsub_itc_fw_app::ReleaseReason::ConnectionMade, static_cast<int64_t>(held_.size()));
    send_held(primary_.is_valid(), secondary_.is_valid(), epoch);
}

void EngineReportSending::discard(const char* reason) {
    if (held_.empty()) {
        overflowed_until_entitled_ = false;
        overflowed_without_connection_ = false;
        return;
    }
    // Said plainly rather than dropped quietly. These are reports a member would have received had this instance gone
    // the other way, and an operator reconciling a member's day against this log should be able to see that they were
    // not sent, and why.
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info, "EngineReportSending: discarding {} held report(s) -- {}", held_.size(), reason);
    links_.reports_discarded(static_cast<int64_t>(held_.size()));
    held_.clear();
    held_.shrink_to_fit();
    overflowed_until_entitled_ = false;
    overflowed_without_connection_ = false;
}

void EngineReportSending::hold(HeldEngineReport report) {
    held_.push_back(std::move(report));
}

void EngineReportSending::send_held_on_the_connections_up(int32_t epoch) {
    if (!held_.empty() && (primary_.is_valid() || secondary_.is_valid())) {
        send_held(primary_.is_valid(), secondary_.is_valid(), epoch);
    }
}

void EngineReportSending::send_held(bool to_primary, bool to_secondary, int32_t epoch) {
    // Each report goes to each sequencer, as a report sent at once does, but a connection may come up after the others:
    // so each held report records which connections it has been sent on, is sent on each as it becomes available, and
    // is kept until it has been sent on every connection there is. It keeps the number it was given when first sent, so
    // that both sequencers see the same number for it (docs/bug_list.md, BUG-0120).
    for (HeldEngineReport& held : held_) {
        pubsub_itc_fw_app::WalRecord envelope{};
        envelope.seq_no = held.seq_no;
        envelope.pdu_id = static_cast<int16_t>(pubsub_itc_fw_app::PduId::PduIdTag::ExecutionReport);
        envelope.payload.data = held.payload.data();
        envelope.payload.size = held.payload.size();
        envelope.has_sender_comp_id = !held.session.empty();
        envelope.sender_comp_id = held.session.comp_id_view();
        envelope.has_origin_gateway_id = !held.session.empty();
        envelope.origin_gateway_id = held.session.protocol;
        envelope.poss_resend = held.poss_resend;
        if (held.numbered) {
            envelope.has_report_engine_epoch = true;
            envelope.report_engine_epoch = held.report_engine_epoch;
            envelope.has_report_number = true;
            envelope.report_number = held.report_number;
        } else {
            number(envelope, epoch);
            held.numbered = true;
            held.report_engine_epoch = envelope.report_engine_epoch;
            held.report_number = envelope.report_number;
        }
        if (to_primary && !held.sent_to_primary) {
            links_.send_report(primary_, held.seq_no, envelope);
            held.sent_to_primary = true;
        }
        if (to_secondary && !held.sent_to_secondary) {
            links_.send_report(secondary_, held.seq_no, envelope);
            held.sent_to_secondary = true;
        }
    }
    const bool secondary_expected = secondary_expected_;
    held_.erase(
        std::remove_if(held_.begin(), held_.end(),
                       [secondary_expected](const HeldEngineReport& held) { return held.sent_to_primary && (held.sent_to_secondary || !secondary_expected); }),
        held_.end());
    if (held_.empty()) {
        overflowed_until_entitled_ = false;
        overflowed_without_connection_ = false;
        waiting_for_connection_ = false;
    }
}

void EngineReportSending::number(pubsub_itc_fw_app::WalRecord& envelope, int32_t epoch) {
    // Where this report stands in the reports this engine sends, so that the leading sequencer can say how far it has
    // forwarded them and a follower can discard the copies it keeps (docs/bug_list.md, BUG-0116). Numbered as it is
    // sent, so the numbers follow the order the sequencers receive the reports in. A new leadership numbers from 1 again
    // under its higher epoch.
    if (report_number_epoch_ != epoch) {
        report_number_epoch_ = epoch;
        report_number_ = 0;
    }
    envelope.has_report_engine_epoch = true;
    envelope.report_engine_epoch = epoch;
    envelope.has_report_number = true;
    envelope.report_number = ++report_number_;
}

} // namespaces
