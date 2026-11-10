#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint> // IWYU pragma: keep
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <pubsub_itc_fw/QuillLogger.hpp>

#include "SeqNumRanges.hpp"
#include "SessionIdentity.hpp"

namespace sequencer {

/**
 * @brief What the sequencer thread supplies so that MemberSessions can count and tell the test.
 */
class MemberSessionsLinksInterface {
  public:
    virtual ~MemberSessionsLinksInterface() = default;

    /// Whether a test sends commands under @p comp_id straight to this instance, with no gateway holding a session for
    /// it, so that a report dropped for it is no fault.
    [[nodiscard]] virtual bool expects_no_session_for(std::string_view comp_id) const = 0;

    /// The gauge of reports dropped because their session was connected nowhere, now @p total.
    virtual void reports_dropped_unbound(int64_t total) = 0;

    /// The test event ReportDroppedSessionNotConnected.
    virtual void report_dropped(const fix_common::SessionIdentity& identity, int64_t er_seq_no) = 0;

    /// The test event SessionAnnouncedAgain.
    virtual void session_announced_again(const fix_common::SessionIdentity& identity, const fix_common::SessionDestination& destination) = 0;

    /// The test event SessionUnboundRecorded.
    virtual void session_unbound(const fix_common::SessionIdentity& identity, int16_t instance, int32_t connection) = 0;
};

/**
 * @brief Which gateway connection each member's session is bound to, what the venue remembers about each session between
 *        connections, and which session placed each order still open
 *        (docs/venue/dividing_the_sequencer_and_matching_engine.md, section 6.12).
 *
 * A session, named by its comp id and protocol, is **bound** to a gateway instance and connection, or **connected
 * nowhere**. Every instance keeps the bindings, leading or not, so that one that takes the lead knows where each
 * member's reports go; only the leader answers a logon with the numbers to resume from.
 *
 * **Sequence numbers belong to the session**, not to the connection carrying it, so a gateway that has just taken a
 * session on cannot know where it had reached. The sequencer can, because every instance of every protocol binds
 * through it, and this is what makes a reconnect a continuation rather than a reset. The numbers are reported by the
 * gateway, at unbind and in updates, rather than counted here: the FIX outbound number counts every message sent to
 * the member, including the heartbeats and rejects the sequencer never sees.
 *
 * **Which session placed each order** is kept by its record's number from when the order is written until a report
 * ends it, so that a report is addressed to the member's session where it is bound at the moment of sending, not where
 * it was when the order arrived.
 *
 * One thread, the sequencer's, calls every member function.
 */
class MemberSessions {
  public:
    /// Added to the resume figure when the session's previous gateway died without reporting: covers the admin
    /// messages the sequencer never sees. Erring high is deliberate: resuming above the true position leaves a gap the
    /// member closes with a ResendRequest, whereas resuming below sends it a number lower than it expects, which FIX
    /// requires it to treat as fatal.
    static constexpr int32_t unclean_resume_admin_allowance = 64;

    /// What the venue remembers about a session between connections.
    struct SequenceState {
        /// Highest outbound position the gateway has reported. Never lowered, except by a logon that resets.
        int32_t outbound_seq_num{1};
        /// Execution reports forwarded to this session since that position was reported: how far the reported figure
        /// is known to be behind.
        int32_t ers_since_report{0};
        /// Highest inbound position the gateway has reported, handed back exactly as reported: resuming it too high
        /// would treat a member that has done nothing wrong as having gone backwards.
        int32_t inbound_seq_num{1};
        /// Which of the session's outbound numbers held an execution report, reported by the gateway, because a resend
        /// is served by whichever instance holds the session now (docs/availability/resend_provenance.md).
        std::vector<fix_common::SeqNumRange> report_seq_nums;
        /// The record number of the last execution report the member was delivered, as its gateway last reported it.
        int64_t last_report_delivered{0};
        /// The last record in the WAL when the session bound again; zero until it binds again after an unbind, and zero
        /// again once the reports missed meanwhile have been asked for.
        int64_t bound_at_log_seq_no{0};
        /// Whether the member's previous gateway died without unbinding it, so the delivered figure may be behind.
        bool delivered_figure_may_be_behind{false};
    };

    /// The session that placed an order, and when its gateway read the order, for the gateway's measure of its round
    /// trip. has_ingress_ns false means the order arrived without one.
    struct OriginSession {
        fix_common::SessionIdentity identity;
        bool has_ingress_ns{false};
        int64_t gateway_ingress_ns{0};
    };

    /// A member's instruction for its resting orders when its connection ends: whether they are to be cancelled, and how
    /// many seconds to wait first. Each part is empty when the member has not said, which is not the same as saying no:
    /// the venue's default then applies.
    struct CancelOnDisconnect {
        std::optional<bool> enabled;
        std::optional<int32_t> grace_period_seconds;
    };

    /// What a member's instruction comes to when its session ends: whether its resting orders are cancelled, and after
    /// how many seconds.
    struct CancelOnDisconnectApplied {
        bool cancel{false};
        int32_t grace_period_seconds{0};
    };

    /// What bind() found.
    struct Bound {
        /// Whether the session was announced again by a gateway still holding it, rather than logging on.
        bool still_connected{false};
        /// For a logon: whether a binding was still present, which means the previous gateway died without unbinding.
        bool previous_session_died{false};
    };

    /// The numbers a logon resumes from, which the leader sends back to the gateway (SessionBoundAck).
    struct Resume {
        bool known{false};
        int32_t outbound_seq_num{1};
        int32_t inbound_seq_num{1};
        std::vector<fix_common::SeqNumRange> report_seq_nums;
    };

    /// The reports a member that has bound again missed, which its gateway asks for once.
    struct Undelivered {
        int64_t after{0};   ///< the last report the member was delivered
        int64_t through{0}; ///< the last record in the WAL when it bound again
        bool possible_repeat{false};
        fix_common::SessionDestination destination{};
    };

    MemberSessions(MemberSessionsLinksInterface& links, pubsub_itc_fw::QuillLogger& logger) : links_(links), logger_(logger) {}

    /// Where the session's reports go now, or nullptr when it is connected nowhere.
    [[nodiscard]] const fix_common::SessionDestination* destination(const fix_common::SessionIdentity& identity) const;

    // ---- Binding -------------------------------------------------------------------------------------------------

    /**
     * @brief The session @p identity is bound to @p destination (SessionBound). Records where it is, and says so.
     * @param[in] still_connected Whether a gateway still holding the session is announcing it again, after its link to
     *                            this instance came up, rather than the member logging on.
     */
    Bound bind(const fix_common::SessionIdentity& identity, const fix_common::SessionDestination& destination, bool still_connected);

    /**
     * @brief Records what a binding of the session says of the member's instruction for its resting orders when its
     *        connection ends.
     *
     * A part the binding states replaces what is held; a part it does not state leaves what an earlier binding said. A
     * gateway that has just taken the session on may not have been told the instruction, and a member that said
     * something at one logon has not withdrawn it by saying nothing at the next. Kept apart from the session's sequence
     * numbers, which a logon that resets them discards: the instruction is not part of the numbering.
     */
    void instruction_stated(const fix_common::SessionIdentity& identity, const CancelOnDisconnect& stated);

    /// The member's instruction for its resting orders when its connection ends, as held; both parts empty if never stated.
    [[nodiscard]] CancelOnDisconnect instruction(const fix_common::SessionIdentity& identity) const;

    /**
     * @brief Records what the gateway binding the session applies to a member that has not said: its own
     *        [cancel_on_disconnect] settings, which are the venue's default for the sessions it holds.
     */
    void gateway_default_stated(const fix_common::SessionIdentity& identity, const CancelOnDisconnectApplied& gateway_default);

    /**
     * @brief What the member's instruction comes to when its session ends: each part the member stated, and otherwise the
     *        default of the gateway that last bound the session. A session no gateway has bound since this process
     *        started, and whose member has not said, is not cancelled: nothing says it should be.
     */
    [[nodiscard]] CancelOnDisconnectApplied applied_instruction(const fix_common::SessionIdentity& identity) const;

    /// Calls @p each with the identity of every session bound to instance @p instance of protocol @p protocol.
    template <typename Each> void for_each_bound_to(int16_t protocol, int16_t instance, Each&& each) const {
        for (const auto& [identity, destination] : destinations_) {
            if (identity.protocol == protocol && destination.instance == instance) {
                each(identity);
            }
        }
    }

    /**
     * @brief For a logon, after bind(): the numbers the session resumes from.
     * @param[in] reset_seq_nums        Whether the member asked to restart its numbering, so that what is remembered is
     *                                  discarded.
     * @param[in] previous_session_died From bind().
     * @param[in] leading               Whether this instance leads; only the leader answers, and only it resumes.
     * @param[in] wal_last_seq_no       The last record in the WAL now.
     * @return The numbers to send back, when this instance leads.
     */
    std::optional<Resume> resume(const fix_common::SessionIdentity& identity, bool reset_seq_nums, bool previous_session_died, bool leading,
                                 int64_t wal_last_seq_no);

    /// What a gateway reports of a session, at unbind or in an update.
    struct Reported {
        int32_t outbound_seq_num{0};
        int32_t inbound_seq_num{0};
        std::vector<fix_common::SeqNumRange> report_seq_nums;
        bool has_last_report_delivered{false};
        int64_t last_report_delivered{0};
    };

    /// The session is unbound from @p instance and @p connection (SessionUnbound), unless it is bound elsewhere now;
    /// the numbers the gateway reports are kept. @return true when the session was unbound; false when the unbind was
    /// stale or the session was not bound.
    bool unbind(const fix_common::SessionIdentity& identity, int16_t instance, int32_t connection, const Reported& reported);

    /// The gateway reports where the session has got to (SessionSequenceUpdate). Never lowered.
    void sequence_update(const fix_common::SessionIdentity& identity, const Reported& reported);

    /// The reports the member missed while its session was connected nowhere, if it has bound again and they have not
    /// been asked for since; asked for once for each binding.
    std::optional<Undelivered> take_undelivered(const fix_common::SessionIdentity& identity);

    // ---- Reports -------------------------------------------------------------------------------------------------

    /// One execution report was sent to the session; counted against the position last reported.
    void report_forwarded(const fix_common::SessionIdentity& identity);

    /// A report for @p identity was dropped, because the session is connected nowhere: counted, and said at Warning for
    /// the first since the session was last connected and at Debug for the rest.
    void report_dropped_unbound(const fix_common::SessionIdentity& identity, int64_t er_seq_no);

    // ---- Orders --------------------------------------------------------------------------------------------------

    /// The order numbered @p seq_no was placed by @p origin.
    void order_placed(int64_t seq_no, const OriginSession& origin) {
        origins_[seq_no] = origin;
    }
    /// The session that placed the order numbered @p seq_no, or nullptr when none is kept.
    [[nodiscard]] const OriginSession* origin(int64_t seq_no) const {
        const auto it = origins_.find(seq_no);
        return it == origins_.end() ? nullptr : &it->second;
    }
    /// A report has ended the order numbered @p seq_no.
    void order_ended(int64_t seq_no) {
        origins_.erase(seq_no);
    }

    /// The sequence state of @p identity, if any is remembered, for tests and the Quill log.
    [[nodiscard]] const SequenceState* sequence_state(const fix_common::SessionIdentity& identity) const;

  private:
    MemberSessionsLinksInterface& links_;
    pubsub_itc_fw::QuillLogger& logger_;

    std::unordered_map<fix_common::SessionIdentity, fix_common::SessionDestination, fix_common::SessionIdentityHash> destinations_;
    std::unordered_map<fix_common::SessionIdentity, SequenceState, fix_common::SessionIdentityHash> sequence_states_;
    // Each member's instruction for its resting orders when its connection ends, by session (instruction_stated()).
    std::unordered_map<fix_common::SessionIdentity, CancelOnDisconnect, fix_common::SessionIdentityHash> instructions_;
    // The default of the gateway that last bound each session (gateway_default_stated()).
    std::unordered_map<fix_common::SessionIdentity, CancelOnDisconnectApplied, fix_common::SessionIdentityHash> gateway_defaults_;
    std::unordered_map<int64_t, OriginSession> origins_;
    // Every session that has had a report dropped since it was last connected, so that the first is said as a Warning.
    std::unordered_set<fix_common::SessionIdentity, fix_common::SessionIdentityHash> sessions_with_reports_dropped_;
    int64_t reports_dropped_unbound_{0};
};

} // namespaces
