#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint> // IWYU pragma: keep
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>

#include <leader_follower.hpp>

#include "EngineReportPosition.hpp"
#include "ReportsHeldForUnknownSessions.hpp"
#include "SessionIdentity.hpp"

namespace sequencer {

/**
 * @brief What the sequencer thread supplies so that ReportForwarding can send reports, reach the member sessions, and
 *        tell the test.
 */
class ReportForwardingLinksInterface {
  public:
    virtual ~ReportForwardingLinksInterface() = default;

    /// Sends @p envelope, a report, on @p connection to a gateway; a member is waiting for it.
    virtual void send_report(const pubsub_itc_fw::ConnectionID& connection, int64_t er_seq_no, const pubsub_itc_fw_app::WalRecord& envelope) = 0;
    /// Sends a report already encoded as its envelope, @p size bytes at @p bytes, on @p connection.
    virtual void send_encoded_report(const pubsub_itc_fw::ConnectionID& connection, int64_t er_seq_no, const uint8_t* bytes, size_t size) = 0;
    /// The acknowledgement of a new order has been sent, as @p envelope: the order path's checkpoint for it.
    virtual void acknowledgement_sent(const pubsub_itc_fw_app::WalRecord& envelope) = 0;

    /// Whether this instance leads; only a leader holds reports.
    [[nodiscard]] virtual bool leading() const = 0;
    /// Whether a test sends commands under @p comp_id with no gateway holding a session for it.
    [[nodiscard]] virtual bool expects_no_session_for(std::string_view comp_id) const = 0;

    /// Where the session's reports go now, or nullptr when it is connected nowhere (MemberSessions).
    [[nodiscard]] virtual const fix_common::SessionDestination* destination(const fix_common::SessionIdentity& identity) const = 0;
    /// One report was sent to the session (MemberSessions).
    virtual void report_forwarded(const fix_common::SessionIdentity& identity) = 0;
    /// A report was dropped because its session is connected nowhere (MemberSessions).
    virtual void report_dropped_unbound(const fix_common::SessionIdentity& identity, int64_t er_seq_no) = 0;
    /// A report has ended the order numbered @p seq_no (MemberSessions).
    virtual void order_ended(int64_t seq_no) = 0;

    /// The test event ReportsHeld.
    virtual void reports_held(const fix_common::SessionIdentity& identity, int32_t gateways_not_announced) = 0;
    /// The test event HeldReportsReleased: @p sent reports sent to the session, and @p dropped dropped.
    virtual void held_reports_released(std::string_view comp_id, int16_t protocol, int64_t sent, int64_t dropped) = 0;
    /// The test event ReportDroppedGatewayNotConnected.
    virtual void report_dropped_gateway_not_connected(int16_t protocol, int16_t instance, int64_t er_seq_no) = 0;
    /// The test event ReportsHeldForGateway.
    virtual void reports_held_for_gateway(int16_t protocol, int16_t instance, int64_t er_seq_no) = 0;
    /// The test event ReportsSentToGateway.
    virtual void reports_sent_to_gateway(int16_t protocol, int16_t instance, int64_t sent, int64_t dropped) = 0;
    /// The gauge of reports held for unannounced sessions that were dropped to make room, now @p total.
    virtual void held_reports_dropped(int64_t total) = 0;
};

/**
 * @brief Getting each report from the matching engine to its member's gateway
 *        (docs/venue/dividing_the_sequencer_and_matching_engine.md, section 6.11).
 *
 * The leader gives each report its own record in the WAL; the thread writes that record and then hands the report here.
 * A report goes one of four ways:
 *
 * | Where it goes                         | When |
 * |---------------------------------------|------|
 * | sent at once to its member's gateway  | the record it depends on is committed, or the leader does not wait for its follower |
 * | waits, keyed by that record           | otherwise; sent when the record is committed (deliver_committed_through) |
 * | held for an unannounced session       | its session is connected nowhere, this instance leads, and some configured gateway has not announced its sessions
 * on its present link; sent when the session binds, or dropped once every gateway has announced | | held for its gateway                  | this instance
 * leads, has no connection for reports to that gateway, and the gateway is configured; sent when the connection is made |
 *
 * **How far reports have been forwarded** (forwarded_through), written on every record the leader writes: just before
 * the oldest report waiting or held for an unannounced session, or else the latest report seen. A report held for its
 * gateway does not hold it back, which is a defect (docs/bug_list.md, BUG-0226), kept as it is until it is fixed.
 *
 * Every report held of either kind is discarded when this instance stops leading: the new leader forwards what it kept.
 *
 * One thread, the sequencer's, calls every member function.
 */
class ReportForwarding {
  public:
    /// Gateways are named by protocol and instance, packed into one key.
    using GatewayKey = int32_t;
    [[nodiscard]] static constexpr GatewayKey gateway_key(int16_t protocol, int16_t instance) {
        return (static_cast<GatewayKey>(protocol) << 16) | static_cast<GatewayKey>(static_cast<uint16_t>(instance));
    }

    /// The room for reports held for unannounced sessions, and for each gateway's held reports, allocated once.
    static constexpr size_t held_report_capacity_bytes = 16U * 1024U * 1024U;
    static constexpr size_t held_report_capacity_reports = 32768;

    /// A report from the engine waiting to be sent: for its record to be committed, or for its session to be announced.
    struct Pending {
        int16_t pdu_id{};
        /// The number of the command the report answers, sent in the transport header; zero for none.
        int64_t seq_no{};
        std::vector<uint8_t> payload; ///< the encoded report
        /// The ClOrdID of the command the report answers, copied onto the envelope for the gateway.
        std::string cl_ord_id;
        /// The report's own record number in the WAL, carried on the envelope so that a gateway knows how far a member
        /// has been served.
        int64_t log_seq_no{0};
        /// Where the report stands in the engine's reports; while it waits, the leader has not forwarded every report up
        /// to it.
        std::optional<EngineReportPosition> position;
        /// Whose report this is: the identity, not a destination, resolved when the report is sent, because a session
        /// can reconnect elsewhere while the report waits.
        fix_common::SessionIdentity identity;
        /// The gateway's time for the order, carried through the wait for the gateway's measure of its round trip.
        bool has_gateway_ingress_ns{false};
        int64_t gateway_ingress_ns{0};
        /// Whether this acknowledges a new order, the only report the order path's checkpoints time.
        bool is_new_order_ack{false};
        /// Whether this repeats a report the member may already hold (R-0122).
        bool poss_resend{false};
        /// Whether this report ends its order, so that which session placed it is forgotten once it is sent.
        bool erase_routing_entry{false};
    };

    /**
     * @param[in] gateways The configured gateway instances, as protocol and instance; each is given a store for reports
     *                     held for it, and is expected to announce its sessions.
     */
    ReportForwarding(const std::vector<std::pair<int16_t, int16_t>>& gateways, ReportForwardingLinksInterface& links, pubsub_itc_fw::QuillLogger& logger);

    // ---- Gateways ------------------------------------------------------------------------------------------------

    /// The connection for reports to gateway @p protocol, @p instance is established: every report held for it is sent.
    void gateway_connected(int16_t protocol, int16_t instance, const pubsub_itc_fw::ConnectionID& connection);

    /// @p connection is lost; if it was a gateway's connection for reports, which gateway's.
    std::optional<GatewayKey> connection_lost(const pubsub_itc_fw::ConnectionID& connection);

    /// The gateway @p protocol, @p instance has announced its @p sessions sessions on @p link (SessionsAnnounced).
    void sessions_announced(int16_t protocol, int16_t instance, const pubsub_itc_fw::ConnectionID& link, int64_t sessions);

    /// Calls @p each with every gateway connection for reports that is up.
    template <typename Each> void for_each_gateway_connection(Each&& each) const {
        for (const auto& [gateway, connection] : gateway_connections_) {
            if (connection.is_valid()) {
                each(connection);
            }
        }
    }

    // ---- Sending -------------------------------------------------------------------------------------------------

    /**
     * @brief Sends @p envelope, a report numbered @p er_seq_no, to gateway @p protocol, @p instance; holds it if this
     *        instance leads, the gateway is configured, and there is no connection for reports to it; otherwise drops it,
     *        saying so.
     */
    void send_to_gateway(int16_t protocol, int16_t instance, int64_t er_seq_no, const pubsub_itc_fw_app::WalRecord& envelope, bool is_new_order_ack);

    /// Holds @p pending if this instance leads, some configured gateway has not announced its sessions, and its session
    /// is not one a test expects to have none. @return Whether it was held.
    bool hold_for_unannounced_session(const Pending& pending);

    /// @p pending waits for record @p gate to be committed.
    void wait_for_commit(int64_t gate, Pending pending);

    /// Sends every waiting report whose record is committed through @p committed_through.
    void deliver_committed_through(int64_t committed_through);

    /// Sends @p pending to where its session is now, holding it or dropping it if it is connected nowhere.
    void deliver(const Pending& pending);

    /// A report at @p position has been handled while leading.
    void report_seen(const std::optional<EngineReportPosition>& position);

    /// How far every report from the engine has been forwarded, for the record the leader writes next.
    [[nodiscard]] std::optional<EngineReportPosition> forwarded_through() const;

    // ---- Sessions ------------------------------------------------------------------------------------------------

    /// The session @p identity is bound: every report held for it is sent.
    void send_held_for_session(const fix_common::SessionIdentity& identity);

    /// The session @p identity logged on through the FIX gateway, which asks for every report since the last it was
    /// delivered: the reports held for it are discarded, since they are among those. @return How many.
    size_t release_held_for_session(const fix_common::SessionIdentity& identity);

    /// This instance has stopped leading: every report held, of either kind, is discarded.
    void stopped_leading();

    [[nodiscard]] size_t waiting() const {
        return pending_.size();
    }
    [[nodiscard]] size_t held_for_sessions() const {
        return held_reports_.count();
    }
    [[nodiscard]] size_t held_for_gateway(int16_t protocol, int16_t instance) const;

  private:
    // For each configured gateway, whether it has announced its sessions on its present link, and that link.
    struct GatewayAnnouncement {
        bool announced{false};
        pubsub_itc_fw::ConnectionID link{};
    };

    [[nodiscard]] int32_t gateways_not_announced() const;
    // Every gateway has announced: every report still held is for a session connected nowhere, and is dropped.
    void drop_held_reports_for_sessions_connected_nowhere();
    // Works out the oldest held position afresh, and says how many were dropped while the room was full, if any were.
    void held_reports_left_the_room();
    bool hold_for_gateway(int16_t protocol, int16_t instance, int64_t er_seq_no, const pubsub_itc_fw_app::WalRecord& envelope);
    void send_held_for_gateway(int16_t protocol, int16_t instance, const pubsub_itc_fw::ConnectionID& connection);
    static Pending decode_held_report(const uint8_t* bytes, size_t size);

    ReportForwardingLinksInterface& links_;
    pubsub_itc_fw::QuillLogger& logger_;

    std::unordered_map<GatewayKey, pubsub_itc_fw::ConnectionID> gateway_connections_;
    std::unordered_map<GatewayKey, GatewayAnnouncement> gateway_announcements_;
    std::unordered_set<GatewayKey> unknown_gateways_warned_;

    // Reports waiting for the record they depend on to be committed, ordered by that record, so that a commit releases
    // every report it covers from the front.
    std::multimap<int64_t, Pending> pending_;
    std::optional<EngineReportPosition> latest_report_seen_;

    ReportsHeldForUnknownSessions held_reports_{held_report_capacity_bytes, held_report_capacity_reports};
    bool reports_held_since_sessions_known_{false};
    int64_t held_reports_dropped_reported_{0};
    bool held_reports_dropping_{false};
    // At or before the position of every held report that has one; lowered as each report is held and worked out afresh
    // when reports leave the room, so that working out how far reports have been forwarded reads no held report.
    std::optional<EngineReportPosition> held_reports_oldest_position_;
    std::vector<uint8_t> held_report_encoding_;

    std::unordered_map<GatewayKey, ReportsHeldForUnknownSessions> reports_held_for_gateways_;
    std::unordered_set<GatewayKey> gateways_holding_reports_;
    std::unordered_map<GatewayKey, int64_t> gateway_held_reports_dropped_reported_;
    std::vector<uint8_t> held_envelope_encoding_;
};

} // namespaces
