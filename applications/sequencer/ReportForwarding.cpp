// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "ReportForwarding.hpp"

#include <cstring>
#include <utility>

#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>

#include <fix_orders.hpp>

#include "GatewayIds.hpp"

namespace sequencer {

namespace {

// A report held for an unannounced session, as it is written into the room: this header, then the report's bytes,
// then its ClOrdID.
struct HeldReportHeader {
    int64_t seq_no{0};
    int64_t log_seq_no{0};
    int64_t gateway_ingress_ns{0};
    int64_t position_number{0};
    int32_t position_epoch{0};
    uint32_t payload_size{0};
    uint32_t cl_ord_id_size{0};
    int16_t pdu_id{0};
    uint8_t has_position{0};
    uint8_t has_gateway_ingress_ns{0};
    uint8_t is_new_order_ack{0};
    uint8_t poss_resend{0};
};

} // un-named namespace

ReportForwarding::ReportForwarding(const std::vector<std::pair<int16_t, int16_t>>& gateways, ReportForwardingLinksInterface& links,
                                   pubsub_itc_fw::QuillLogger& logger)
    : links_(links), logger_(logger) {
    for (const auto& [protocol, instance] : gateways) {
        gateway_announcements_[gateway_key(protocol, instance)] = GatewayAnnouncement{};
        reports_held_for_gateways_.try_emplace(gateway_key(protocol, instance), held_report_capacity_bytes, held_report_capacity_reports);
    }
}

size_t ReportForwarding::held_for_gateway(int16_t protocol, int16_t instance) const {
    const auto store = reports_held_for_gateways_.find(gateway_key(protocol, instance));
    return store == reports_held_for_gateways_.end() ? 0 : store->second.count();
}

// ---- Gateways -----------------------------------------------------------------------------------------------------

void ReportForwarding::gateway_connected(int16_t protocol, int16_t instance, const pubsub_itc_fw::ConnectionID& connection) {
    gateway_connections_[gateway_key(protocol, instance)] = connection;
    send_held_for_gateway(protocol, instance, connection);
}

std::optional<ReportForwarding::GatewayKey> ReportForwarding::connection_lost(const pubsub_itc_fw::ConnectionID& connection) {
    // A gateway whose link was lost may hold sessions this instance has not been told of by the time it comes up again.
    for (auto& [gateway, announcement] : gateway_announcements_) {
        if (announcement.announced && announcement.link == connection) {
            announcement.announced = false;
            announcement.link = pubsub_itc_fw::ConnectionID{};
            PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info,
                       "ReportForwarding: gateway id {} lost the link it announced its sessions on -- its sessions are not known until it announces again",
                       gateway);
        }
    }
    for (auto it = gateway_connections_.begin(); it != gateway_connections_.end(); ++it) {
        if (it->second == connection) {
            const GatewayKey gateway = it->first;
            gateway_connections_.erase(it);
            return gateway;
        }
    }
    return std::nullopt;
}

void ReportForwarding::sessions_announced(int16_t protocol, int16_t instance, const pubsub_itc_fw::ConnectionID& link, int64_t sessions) {
    const auto found = gateway_announcements_.find(gateway_key(protocol, instance));
    if (found == gateway_announcements_.end()) {
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Warning,
                   "ReportForwarding: SessionsAnnounced from gateway protocol={} instance={}, which is not in this instance's configuration -- ignored",
                   protocol, instance);
        return;
    }
    found->second.announced = true;
    found->second.link = link;
    const int32_t not_announced = gateways_not_announced();
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info,
               "ReportForwarding: gateway protocol={} instance={} has announced its {} session(s) on connection {}; {} configured gateway instance(s) have "
               "not",
               protocol, instance, sessions, link.get_value(), not_announced);
    if (not_announced == 0) {
        drop_held_reports_for_sessions_connected_nowhere();
    }
}

int32_t ReportForwarding::gateways_not_announced() const {
    int32_t not_announced = 0;
    for (const auto& [gateway, announcement] : gateway_announcements_) {
        if (!announcement.announced) {
            ++not_announced;
        }
    }
    return not_announced;
}

// ---- Sending ------------------------------------------------------------------------------------------------------

void ReportForwarding::send_to_gateway(int16_t protocol, int16_t instance, int64_t er_seq_no, const pubsub_itc_fw_app::WalRecord& envelope,
                                       bool is_new_order_ack) {
    const auto connection = gateway_connections_.find(gateway_key(protocol, instance));
    if (connection == gateway_connections_.end() || !connection->second.is_valid()) {
        const bool configured = gateway_announcements_.count(gateway_key(protocol, instance)) != 0;
        if (!configured && unknown_gateways_warned_.insert(gateway_key(protocol, instance)).second) {
            PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Error,
                       "ReportForwarding: orders received from gateway protocol={} instance={}, which is not in this sequencer's [[gateway]] configuration -- "
                       "its execution reports cannot be delivered anywhere. Check the gateway's instance_id against the sequencer's endpoints.",
                       protocol, instance);
        }
        if (configured && hold_for_gateway(protocol, instance, er_seq_no, envelope)) {
            return;
        }
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Warning, "ReportForwarding: gateway protocol={} instance={} not connected -- dropping ER seq={}",
                   protocol, instance, er_seq_no);
        links_.report_dropped_gateway_not_connected(protocol, instance, er_seq_no);
        return;
    }
    links_.send_report(connection->second, er_seq_no, envelope);
    if (is_new_order_ack) {
        links_.acknowledgement_sent(envelope);
    }
}

bool ReportForwarding::hold_for_unannounced_session(const Pending& pending) {
    if (!links_.leading() || links_.expects_no_session_for(pending.identity.comp_id_view())) {
        return false;
    }
    const int32_t not_announced = gateways_not_announced();
    if (not_announced == 0) {
        return false;
    }
    HeldReportHeader header{};
    header.seq_no = pending.seq_no;
    header.log_seq_no = pending.log_seq_no;
    header.gateway_ingress_ns = pending.gateway_ingress_ns;
    header.has_gateway_ingress_ns = pending.has_gateway_ingress_ns ? 1 : 0;
    header.has_position = pending.position.has_value() ? 1 : 0;
    header.position_epoch = pending.position.has_value() ? pending.position->epoch : 0;
    header.position_number = pending.position.has_value() ? pending.position->number : 0;
    header.payload_size = static_cast<uint32_t>(pending.payload.size());
    header.cl_ord_id_size = static_cast<uint32_t>(pending.cl_ord_id.size());
    header.pdu_id = pending.pdu_id;
    header.is_new_order_ack = pending.is_new_order_ack ? 1 : 0;
    header.poss_resend = pending.poss_resend ? 1 : 0;
    held_report_encoding_.resize(sizeof(header) + pending.payload.size() + pending.cl_ord_id.size());
    std::memcpy(held_report_encoding_.data(), &header, sizeof(header));
    std::memcpy(held_report_encoding_.data() + sizeof(header), pending.payload.data(), pending.payload.size());
    std::memcpy(held_report_encoding_.data() + sizeof(header) + pending.payload.size(), pending.cl_ord_id.data(), pending.cl_ord_id.size());
    held_reports_.hold(pending.identity, held_report_encoding_.data(), held_report_encoding_.size());
    if (pending.position.has_value() && (!held_reports_oldest_position_.has_value() || held_reports_oldest_position_->is_after(*pending.position))) {
        held_reports_oldest_position_ = pending.position;
    }
    if (!reports_held_since_sessions_known_) {
        reports_held_since_sessions_known_ = true;
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Warning,
                   "ReportForwarding: ER seq_no={} for session comp_id='{}' protocol={} held -- {} gateway instance(s) have not yet announced their sessions, "
                   "so it may be connected after all. Held reports are sent when their session is announced, and dropped once every gateway instance has "
                   "announced",
                   pending.seq_no, pending.identity.comp_id_view(), pending.identity.protocol, not_announced);
        links_.reports_held(pending.identity, not_announced);
    } else {
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Debug, "ReportForwarding: ER seq_no={} for session comp_id='{}' protocol={} held", pending.seq_no,
                   pending.identity.comp_id_view(), pending.identity.protocol);
    }
    if (held_reports_.dropped_for_room() != held_reports_dropped_reported_ && !held_reports_dropping_) {
        held_reports_dropping_ = true;
        // Said once, when the dropping starts; the total is said when reports next leave the room.
        PUBSUB_LOG_STR(logger_, pubsub_itc_fw::FwLogLevel::Warning,
                       "ReportForwarding: the room for reports held until their sessions are announced is full, and the oldest held reports are being "
                       "dropped to make room; their members are not told of them");
    }
    links_.held_reports_dropped(held_reports_.dropped_for_room());
    return true;
}

void ReportForwarding::wait_for_commit(int64_t gate, Pending pending) {
    pending_.emplace(gate, std::move(pending));
}

void ReportForwarding::deliver_committed_through(int64_t committed_through) {
    const auto covered_end = pending_.upper_bound(committed_through);
    for (auto it = pending_.begin(); it != covered_end; ++it) {
        deliver(it->second);
    }
    pending_.erase(pending_.begin(), covered_end);
}

void ReportForwarding::deliver(const Pending& pending) {
    const fix_common::SessionDestination* destination = pending.identity.empty() ? nullptr : links_.destination(pending.identity);
    if (destination == nullptr && !pending.identity.empty()) {
        if (hold_for_unannounced_session(pending)) {
            if (pending.erase_routing_entry) {
                links_.order_ended(pending.seq_no);
            }
            return;
        }
        links_.report_dropped_unbound(pending.identity, pending.seq_no);
    }
    pubsub_itc_fw_app::WalRecord envelope{};
    envelope.seq_no = pending.log_seq_no;
    envelope.pdu_id = pending.pdu_id;
    envelope.payload.data = pending.payload.data();
    envelope.payload.size = pending.payload.size();
    envelope.has_gateway_session_conn_id = destination != nullptr;
    envelope.gateway_session_conn_id = destination != nullptr ? destination->conn_id : 0;
    envelope.has_origin_gateway_id = destination != nullptr;
    envelope.origin_gateway_id = pending.identity.protocol;
    envelope.has_gateway_instance_id = destination != nullptr;
    envelope.gateway_instance_id = destination != nullptr ? destination->instance : gateway_ids::first_instance;
    envelope.has_gateway_ingress_ns = pending.has_gateway_ingress_ns;
    envelope.gateway_ingress_ns = pending.gateway_ingress_ns;
    envelope.has_sender_comp_id = !pending.identity.empty();
    envelope.sender_comp_id = pending.identity.comp_id_view();
    envelope.has_cl_ord_id = !pending.cl_ord_id.empty();
    envelope.cl_ord_id = pending.cl_ord_id;
    envelope.poss_resend = pending.poss_resend;
    if (destination != nullptr) {
        send_to_gateway(pending.identity.protocol, destination->instance, pending.seq_no, envelope, pending.is_new_order_ack);
        links_.report_forwarded(pending.identity);
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Debug, "ReportForwarding: buffered ER seq={} forwarded to protocol={} instance={}", pending.seq_no,
                   pending.identity.protocol, destination->instance);
    }
    if (pending.erase_routing_entry) {
        links_.order_ended(pending.seq_no);
    }
}

void ReportForwarding::report_seen(const std::optional<EngineReportPosition>& position) {
    if (position.has_value() && (!latest_report_seen_.has_value() || position->is_after(*latest_report_seen_))) {
        latest_report_seen_ = position;
    }
}

std::optional<EngineReportPosition> ReportForwarding::forwarded_through() const {
    if (!latest_report_seen_.has_value()) {
        return std::nullopt;
    }
    std::optional<EngineReportPosition> oldest_waiting;
    for (const auto& [gate, pending] : pending_) {
        if (pending.position.has_value() && (!oldest_waiting.has_value() || oldest_waiting->is_after(*pending.position))) {
            oldest_waiting = pending.position;
        }
    }
    if (held_reports_oldest_position_.has_value() && (!oldest_waiting.has_value() || oldest_waiting->is_after(*held_reports_oldest_position_))) {
        oldest_waiting = held_reports_oldest_position_;
    }
    return oldest_waiting.has_value() ? oldest_waiting->just_before() : latest_report_seen_;
}

// ---- Holding ------------------------------------------------------------------------------------------------------

ReportForwarding::Pending ReportForwarding::decode_held_report(const uint8_t* bytes, size_t size) {
    HeldReportHeader header{};
    if (size < sizeof(header)) {
        throw pubsub_itc_fw::PreconditionAssertion("ReportForwarding: a held report is shorter than its header", __FILE__, __LINE__);
    }
    std::memcpy(&header, bytes, sizeof(header));
    if (sizeof(header) + header.payload_size + header.cl_ord_id_size != size) {
        throw pubsub_itc_fw::PreconditionAssertion("ReportForwarding: a held report's sizes do not add up", __FILE__, __LINE__);
    }
    Pending pending{};
    pending.pdu_id = header.pdu_id;
    pending.seq_no = header.seq_no;
    pending.log_seq_no = header.log_seq_no;
    pending.has_gateway_ingress_ns = header.has_gateway_ingress_ns != 0;
    pending.gateway_ingress_ns = header.gateway_ingress_ns;
    pending.is_new_order_ack = header.is_new_order_ack != 0;
    pending.poss_resend = header.poss_resend != 0;
    if (header.has_position != 0) {
        pending.position = EngineReportPosition{header.position_epoch, header.position_number};
    }
    const uint8_t* payload = bytes + sizeof(header);
    pending.payload.assign(payload, payload + header.payload_size);
    pending.cl_ord_id.assign(reinterpret_cast<const char*>(payload + header.payload_size), header.cl_ord_id_size);
    pending.erase_routing_entry = false;
    return pending;
}

void ReportForwarding::held_reports_left_the_room() {
    held_reports_oldest_position_.reset();
    held_reports_.for_each_held([this](const fix_common::SessionIdentity&, const uint8_t* bytes, size_t size) {
        const Pending held = decode_held_report(bytes, size);
        if (held.position.has_value() && (!held_reports_oldest_position_.has_value() || held_reports_oldest_position_->is_after(*held.position))) {
            held_reports_oldest_position_ = held.position;
        }
    });
    if (held_reports_dropping_) {
        held_reports_dropping_ = false;
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Warning,
                   "ReportForwarding: {} held report(s) were dropped to make room while the room for reports held until their sessions are announced was "
                   "full; their members are not told of them",
                   held_reports_.dropped_for_room() - held_reports_dropped_reported_);
        held_reports_dropped_reported_ = held_reports_.dropped_for_room();
    }
}

void ReportForwarding::drop_held_reports_for_sessions_connected_nowhere() {
    reports_held_since_sessions_known_ = false;
    if (held_reports_.count() == 0) {
        return;
    }
    std::vector<std::pair<fix_common::SessionIdentity, int64_t>> dropped;
    held_reports_.take_all([&dropped](const fix_common::SessionIdentity& identity, const uint8_t* bytes, size_t size) {
        dropped.emplace_back(identity, decode_held_report(bytes, size).seq_no);
    });
    held_reports_left_the_room();
    for (const auto& [identity, seq_no] : dropped) {
        links_.report_dropped_unbound(identity, seq_no);
    }
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info,
               "ReportForwarding: every gateway instance has announced its sessions -- {} held report(s) were for sessions connected nowhere, and are "
               "dropped",
               dropped.size());
    links_.held_reports_released(std::string_view{}, 0, 0, static_cast<int64_t>(dropped.size()));
}

void ReportForwarding::send_held_for_session(const fix_common::SessionIdentity& identity) {
    if (held_reports_.count() == 0) {
        return;
    }
    std::vector<Pending> to_send;
    held_reports_.release(identity, [&to_send](const uint8_t* bytes, size_t size) { to_send.push_back(decode_held_report(bytes, size)); });
    if (to_send.empty()) {
        return;
    }
    held_reports_left_the_room();
    for (Pending& pending : to_send) {
        pending.identity = identity;
        deliver(pending);
    }
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info,
               "ReportForwarding: session comp_id='{}' protocol={} announced -- sent it {} report(s) held until it was", identity.comp_id_view(),
               identity.protocol, to_send.size());
    links_.held_reports_released(identity.comp_id_view(), identity.protocol, static_cast<int64_t>(to_send.size()), 0);
}

size_t ReportForwarding::release_held_for_session(const fix_common::SessionIdentity& identity) {
    const size_t discarded = held_reports_.release(identity, [](const uint8_t*, size_t) {});
    if (discarded > 0) {
        held_reports_left_the_room();
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info,
                   "ReportForwarding: session comp_id='{}' protocol={} logged on -- {} report(s) held for it are left to the reports it asks for once "
                   "established",
                   identity.comp_id_view(), identity.protocol, discarded);
        links_.held_reports_released(identity.comp_id_view(), identity.protocol, 0, static_cast<int64_t>(discarded));
    }
    return discarded;
}

bool ReportForwarding::hold_for_gateway(int16_t protocol, int16_t instance, int64_t er_seq_no, const pubsub_itc_fw_app::WalRecord& envelope) {
    if (!links_.leading()) {
        return false;
    }
    const GatewayKey gateway = gateway_key(protocol, instance);
    const auto store = reports_held_for_gateways_.find(gateway);
    if (store == reports_held_for_gateways_.end()) {
        return false;
    }
    size_t bytes_written = 0;
    size_t bytes_needed = 0;
    [[maybe_unused]] const bool measured = pubsub_itc_fw_app::encode(envelope, nullptr, 0, bytes_written, bytes_needed);
    if (held_envelope_encoding_.size() < sizeof(er_seq_no) + bytes_needed) {
        held_envelope_encoding_.resize(sizeof(er_seq_no) + bytes_needed);
    }
    std::memcpy(held_envelope_encoding_.data(), &er_seq_no, sizeof(er_seq_no));
    if (!pubsub_itc_fw_app::encode(envelope, held_envelope_encoding_.data() + sizeof(er_seq_no), held_envelope_encoding_.size() - sizeof(er_seq_no),
                                   bytes_written, bytes_needed)) {
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Error,
                   "ReportForwarding: failed to encode ER seq={} for gateway protocol={} instance={} to hold it ({} bytes needed) -- dropping it", er_seq_no,
                   protocol, instance, bytes_needed);
        return false;
    }
    const fix_common::SessionIdentity identity =
        fix_common::SessionIdentity::make(envelope.has_sender_comp_id ? envelope.sender_comp_id : std::string_view{}, protocol);
    store->second.hold(identity, held_envelope_encoding_.data(), sizeof(er_seq_no) + bytes_written);
    if (gateways_holding_reports_.insert(gateway).second) {
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Warning,
                   "ReportForwarding: gateway protocol={} instance={} has no connection for reports from this instance -- ER seq={} held, and every report "
                   "for it after this, until the connection is made",
                   protocol, instance, er_seq_no);
        links_.reports_held_for_gateway(protocol, instance, er_seq_no);
    } else {
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Debug, "ReportForwarding: ER seq={} for gateway protocol={} instance={} held", er_seq_no, protocol,
                   instance);
    }
    return true;
}

void ReportForwarding::send_held_for_gateway(int16_t protocol, int16_t instance, const pubsub_itc_fw::ConnectionID& connection) {
    const GatewayKey gateway = gateway_key(protocol, instance);
    gateways_holding_reports_.erase(gateway);
    const auto store = reports_held_for_gateways_.find(gateway);
    if (store == reports_held_for_gateways_.end() || store->second.count() == 0) {
        return;
    }
    const size_t sent = store->second.take_all([this, &connection](const fix_common::SessionIdentity&, const uint8_t* bytes, size_t size) {
        int64_t er_seq_no = 0;
        std::memcpy(&er_seq_no, bytes, sizeof(er_seq_no));
        links_.send_encoded_report(connection, er_seq_no, bytes + sizeof(er_seq_no), size - sizeof(er_seq_no));
    });
    int64_t& dropped_reported = gateway_held_reports_dropped_reported_[gateway];
    const int64_t dropped = store->second.dropped_for_room() - dropped_reported;
    dropped_reported = store->second.dropped_for_room();
    if (dropped > 0) {
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Error,
                   "ReportForwarding: {} report(s) held for gateway protocol={} instance={} were dropped to make room while it had no connection for reports; "
                   "their members are not told of them",
                   dropped, protocol, instance);
    }
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info,
               "ReportForwarding: gateway protocol={} instance={} connection for reports made -- sent it {} report(s) held until it was", protocol, instance,
               sent);
    links_.reports_sent_to_gateway(protocol, instance, static_cast<int64_t>(sent), dropped);
}

void ReportForwarding::stopped_leading() {
    // Reports held for sessions not yet announced are the new leader's to send: it forwards what it kept while following.
    held_reports_.discard_all();
    held_reports_left_the_room();
    reports_held_since_sessions_known_ = false;
    // Likewise the reports held for a gateway with no connection: they are in the WAL, and the new leader sends them.
    for (auto& [gateway, held] : reports_held_for_gateways_) {
        held.discard_all();
    }
    gateways_holding_reports_.clear();
}

} // namespaces
