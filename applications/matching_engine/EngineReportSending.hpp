#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint> // IWYU pragma: keep
#include <vector>

#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>

#include <leader_follower.hpp>
#include <matching_engine_test_events.hpp>

#include "HeldEngineReport.hpp"
#include "SessionIdentity.hpp"

namespace matching_engine {

/**
 * @brief What the matching engine thread supplies so that EngineReportSending can send reports and tell the test.
 */
class EngineReportSendingLinksInterface {
  public:
    virtual ~EngineReportSendingLinksInterface() = default;

    /**
     * @brief Sends @p envelope, a report from the engine, on @p connection, a connection for reports to a sequencer.
     *
     * A member is waiting for the report, so it is sent at once rather than gathered with others.
     *
     * @param[in] connection The connection to send it on.
     * @param[in] seq_no     The number of the command the report answers, sent in the transport header.
     * @param[in] envelope   The report in its WalRecord envelope, numbered. Valid only during the call.
     */
    virtual void send_report(const pubsub_itc_fw::ConnectionID& connection, int64_t seq_no, const pubsub_itc_fw_app::WalRecord& envelope) = 0;

    /// The test event ReportsHeld: reports have begun to be held because there is no connection for reports, with
    /// @p count held, counting the one that began it.
    virtual void reports_held(int64_t count) = 0;

    /// The test event ReportsReleased: @p count held reports are being sent, for @p reason.
    virtual void reports_released(pubsub_itc_fw_app::ReleaseReason reason, int64_t count) = 0;

    /// The test event ReportsDiscarded: @p count held reports were discarded.
    virtual void reports_discarded(int64_t count) = 0;
};

/**
 * @brief Sends each report the matching engine produces to both sequencers, numbered, or holds it until it can be sent
 *        (docs/venue/dividing_the_sequencer_and_matching_engine.md, section 6.2).
 *
 * Each report is sent on every connection for reports to a sequencer that is up. The leading sequencer forwards it to
 * the member; the other keeps it, in case it takes the lead (docs/availability/change_of_sequencer_leader.md,
 * section 4.4). Each report carries its position in the reports this engine has sent while leading under its epoch,
 * counted from 1 under each epoch, so that both sequencers can say how far they have forwarded the engine's reports.
 *
 * **A report is held instead of sent** in two situations:
 *
 * - **Until the engine knows whether it serves.** An engine catching up for a purpose other than leading may yet learn
 *   that its peer leads, and then it serves nobody. Its reports are held, up to max_held_reports, and sent if it takes
 *   the lead (release_now_serving) or discarded if it follows (discard). Past the limit, reports are sent as they are
 *   produced, and that is said once at Warning: a member that already holds a report can discard a repeat, and one that
 *   never receives it cannot recover.
 * - **For want of a connection.** With neither connection for reports up, reports are held, up to max_held_reports,
 *   and sent when a connection is established (release_for_connection). Past the limit, later reports are lost, and
 *   that is said once at Error.
 *
 * A held report is numbered when it is first sent, keeps that number, and is kept until it has been sent on every
 * connection there is: one with only the primary sequencer connected is sent to the secondary when that connection is
 * established. With high availability off there is only the primary.
 *
 * One thread, the engine's, calls every member function.
 */
class EngineReportSending {
  public:
    /// Which sequencer a connection for reports goes to.
    enum class Sequencer { primary, secondary };

    /// The most reports held at once.
    static constexpr size_t max_held_reports = 50000;

    /**
     * @param[in] secondary_expected Whether there is a secondary sequencer to send to, which is whether high
     *                               availability is on. A held report is dropped once sent on every connection
     *                               expected.
     * @param[in] links              How reports are sent and the test told.
     * @param[in] logger             Where what happens is logged.
     */
    EngineReportSending(bool secondary_expected, EngineReportSendingLinksInterface& links, pubsub_itc_fw::QuillLogger& logger);

    /// The connection for reports to @p sequencer is established, as @p connection.
    void connection_established(Sequencer sequencer, const pubsub_itc_fw::ConnectionID& connection);

    /// The connection for reports to @p sequencer is lost.
    void connection_lost(Sequencer sequencer);

    /// The connection for reports to @p sequencer, or an invalid one when it is not up.
    [[nodiscard]] const pubsub_itc_fw::ConnectionID& connection(Sequencer sequencer) const {
        return sequencer == Sequencer::primary ? primary_ : secondary_;
    }

    /// A report the engine has just produced.
    struct Report {
        const uint8_t* bytes{nullptr}; ///< the encoded ExecutionReport; valid only during the call
        size_t size{0};
        int64_t seq_no{0}; ///< the number of the command it answers
        const fix_common::SessionIdentity* session{nullptr};
        bool poss_resend{false};
        /// The time the member's gateway read the order, for the gateway's measure of its round trip.
        bool has_gateway_ingress_ns{false};
        int64_t gateway_ingress_ns{0};
    };

    /**
     * @brief Sends @p report, numbered under @p epoch, or holds it.
     * @param[in] report                 The report.
     * @param[in] epoch                  The engine's epoch now.
     * @param[in] holding_until_entitled Whether the engine is catching up for a purpose other than leading, and so does
     *                                   not yet know whether it serves.
     * @return Whether it was sent now.
     */
    bool send(const Report& report, int32_t epoch, bool holding_until_entitled);

    /// The engine now serves: every held report is sent on the connections that are up, numbered under @p epoch if it
    /// was not numbered before.
    void release_now_serving(int32_t epoch);

    /// A connection for reports has been established: every held report is sent on the connections that are up, unless
    /// the engine still does not know whether it serves.
    void release_for_connection(int32_t epoch, bool holding_until_entitled);

    /// The engine follows and serves nobody: every held report is discarded, and the number said, with @p reason.
    void discard(const char* reason);

    /// Holds @p report, kept while the engine followed and now owed to its member because the engine takes the lead.
    void hold(HeldEngineReport report);

    /// Sends every held report on the connections that are up, if there are any, saying nothing.
    void send_held_on_the_connections_up(int32_t epoch);

    /// How many reports are held.
    [[nodiscard]] size_t held() const {
        return held_.size();
    }

  private:
    // Sends each held report on each of the named connections it has not yet been sent on, and drops those sent on
    // every connection expected.
    void send_held(bool to_primary, bool to_secondary, int32_t epoch);
    // Gives a report about to be sent its position in the reports this engine sends under @p epoch.
    void number(pubsub_itc_fw_app::WalRecord& envelope, int32_t epoch);
    // A held report as it was produced.
    static HeldEngineReport held_report_of(const Report& report);

    const bool secondary_expected_;
    EngineReportSendingLinksInterface& links_;
    pubsub_itc_fw::QuillLogger& logger_;

    pubsub_itc_fw::ConnectionID primary_{};
    pubsub_itc_fw::ConnectionID secondary_{};

    std::vector<HeldEngineReport> held_;
    // Whether the limit was reached while holding until the engine knows whether it serves, and while holding for want
    // of a connection: each is said once, until the held reports are next all sent or discarded.
    bool overflowed_until_entitled_{false};
    bool overflowed_without_connection_{false};
    // Whether reports are being held for want of a connection, so that the start of it is said once.
    bool waiting_for_connection_{false};

    // How many reports this engine has sent under report_number_epoch_.
    int64_t report_number_{0};
    int32_t report_number_epoch_{0};
};

} // namespaces
