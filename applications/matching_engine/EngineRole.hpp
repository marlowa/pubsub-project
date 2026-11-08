#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint> // IWYU pragma: keep
#include <optional>

namespace matching_engine {

/**
 * @brief The role a matching engine instance holds, as its Quill log lines and test events name it.
 *
 * Unknown    -- high availability is off, or the instance neither leads nor follows yet.
 * Follower   -- another instance leads: this one follows the WAL, applying every command and keeping its reports.
 * Reconciling-- catching up from the WAL, to become current, to lead, or to follow again.
 * Leader     -- active: processes orders and sends its reports.
 */
enum class MeRole { Unknown, Follower, Reconciling, Leader };

/**
 * @brief What a matching engine instance is doing: starting, catching up, leading, or following, and why
 *        (docs/venue/dividing_the_sequencer_and_matching_engine.md, section 6.1).
 *
 * **The situations**, each named by what the instance is doing:
 *
 * | Situation                       | Role        | Commands it is sent                     |
 * |---------------------------------|-------------|-----------------------------------------|
 * | starting                        | Unknown     | discarded with high availability on     |
 * | catching_up_to_become_current   | Reconciling | applied, as the catch-up's answer       |
 * | current                         | Unknown     | applied as a follower of the WAL, or, with high availability off, as the one instance |
 * | catching_up_to_lead             | Reconciling | applied, as the catch-up's answer       |
 * | leading                         | Leader      | applied                                 |
 * | catching_up_to_follow_again     | Reconciling | applied, as the catch-up's answer       |
 * | following                       | Follower    | applied as a follower of the WAL, if the latest request asked to follow |
 *
 * **The moves.**
 *
 * - starting, on the first sequencer connection for orders: catching_up_to_become_current.
 * - catching_up_to_become_current, when it completes: current.
 * - starting or current, when the instance agrees that its peer leads: following. One that was starting has not asked
 *   to follow the WAL, and asks then.
 * - any situation but leading, when the lease agent says this instance leads: catching_up_to_lead, replacing a
 *   catch-up under way.
 * - catching_up_to_lead, when it completes: leading (through current, as the role reports it).
 * - leading or catching_up_to_lead, when the lease runs out: following. Either then asks to follow the WAL, because
 *   its latest request, if it made one, asked to lead.
 * - current or following, when a record arrives out of turn, a sequencer connects, or a sequencer says it now leads:
 *   catching_up_to_follow_again, and back to the situation it was in when that completes.
 *
 * **Two facts beside the situation.** Whether the latest catch-up request asked to follow the WAL rather than to lead
 * is kept apart from the situation, because it is acted on in more than one: it decides whether the reports of a
 * catch-up are kept and compared, and whether a follower follows the WAL at all. A follower whose latest request asked
 * to lead, or who never made one, would apply nothing and keep no report, so the engine asks to follow the WAL at the
 * moment an instance becomes a follower in that state (follows_without_following_the_wal). And whether any catch-up has
 * completed is kept for the test's report of where the instance stands.
 *
 * **A peer that leads, agreed while catching up to become current** cannot be followed until the catch-up ends, so it
 * is noted and acted on then (take_peer_leader_note), its epoch checked again then.
 *
 * Whether the instance has refused to lead is not here: FollowerReportComparison holds it.
 *
 * One thread, the engine's, calls every member function.
 */
class EngineRole {
  public:
    /// What the instance is doing.
    enum class Situation : int8_t { starting, catching_up_to_become_current, current, catching_up_to_lead, leading, catching_up_to_follow_again, following };

    /// What a catch-up that has just completed was for.
    enum class Completed : int8_t {
        following_again, ///< the instance is back in the situation it was in
        became_current,  ///< the instance is current
        to_lead          ///< the instance is current, and is to lead now
    };

    /// A peer that this instance agreed leads, while it was catching up.
    struct PeerLeader {
        int64_t instance_id{0};
        int32_t epoch{0};
    };

    /// @param[in] ha_enabled Whether high availability is on. With it off, the one instance applies every command.
    explicit EngineRole(bool ha_enabled) : ha_enabled_(ha_enabled) {}

    [[nodiscard]] Situation situation() const {
        return situation_;
    }

    /// The role, as the Quill log and the test name it.
    [[nodiscard]] MeRole role() const;

    /// Whether the instance is catching up, for any purpose.
    [[nodiscard]] bool catching_up() const {
        return situation_ == Situation::catching_up_to_become_current || situation_ == Situation::catching_up_to_lead ||
               situation_ == Situation::catching_up_to_follow_again;
    }

    /// Whether the instance is catching up in order to lead.
    [[nodiscard]] bool catching_up_to_lead() const {
        return situation_ == Situation::catching_up_to_lead;
    }

    /// Whether the instance follows the WAL now: high availability is on, the latest request asked to follow, and it
    /// neither leads nor is catching up.
    [[nodiscard]] bool following_the_wal() const {
        return ha_enabled_ && latest_request_asked_to_follow_ && !catching_up() && situation_ != Situation::leading;
    }

    /// Whether the instance keeps the reports of the commands it applies rather than sending them: it follows the WAL,
    /// or is catching up after a request to follow it.
    [[nodiscard]] bool keeps_its_reports() const {
        return ha_enabled_ && latest_request_asked_to_follow_ && situation_ != Situation::leading;
    }

    /// Whether reports are held until the instance knows whether it serves: it is catching up, with high availability
    /// on, for a purpose other than leading.
    [[nodiscard]] bool holding_reports_until_entitled() const {
        return ha_enabled_ && catching_up() && !catching_up_to_lead();
    }

    /// Whether the latest catch-up request asked to follow the WAL rather than to lead.
    [[nodiscard]] bool latest_request_asked_to_follow() const {
        return latest_request_asked_to_follow_;
    }

    /// Whether any catch-up has completed. Not set again by a command discarded without a lease, which happens only
    /// while starting, when none has.
    [[nodiscard]] bool catch_up_completed() const {
        return catch_up_completed_;
    }

    /// Whether the instance follows, but has not asked to follow the WAL: its latest request asked to lead, or it made
    /// none. Until it asks, it applies nothing and keeps no report, so the engine asks as soon as this is true.
    [[nodiscard]] bool follows_without_following_the_wal() const {
        return ha_enabled_ && situation_ == Situation::following && !latest_request_asked_to_follow_;
    }

    // ---- The moves -------------------------------------------------------------------------------------------------

    void start_catching_up_to_become_current() {
        situation_ = Situation::catching_up_to_become_current;
    }
    void start_catching_up_to_lead() {
        situation_ = Situation::catching_up_to_lead;
    }
    /// The instance, current or following, catches up again and goes on following; it goes back to the situation it is
    /// in now when the catch-up completes.
    void start_catching_up_to_follow_again();

    /// A catch-up request has been sent; @p asked_to_lead says whether it asked to lead rather than to follow.
    void request_sent(bool asked_to_lead) {
        latest_request_asked_to_follow_ = !asked_to_lead;
    }

    /// The catch-up has completed: the situation moves on, and what the catch-up was for is returned. To lead, the
    /// instance is current until became_leader().
    Completed catch_up_completed_now();

    void became_leader() {
        situation_ = Situation::leading;
    }
    void became_follower() {
        situation_ = Situation::following;
    }

    /// A command was discarded because the instance holds no lease: it is not current, if it was.
    void command_discarded_without_a_lease() {
        catch_up_completed_ = false;
    }

    /// The instance agreed that its peer @p instance_id leads, at @p epoch, while catching up to become current.
    void note_peer_leader(int64_t instance_id, int32_t epoch) {
        peer_leader_ = PeerLeader{instance_id, epoch};
    }
    /// The peer noted as leading, if there is one, which is then no longer noted, so that it is acted on once.
    std::optional<PeerLeader> take_peer_leader_note() {
        std::optional<PeerLeader> noted = peer_leader_;
        peer_leader_.reset();
        return noted;
    }

  private:
    const bool ha_enabled_;
    Situation situation_{Situation::starting};
    // For a catch-up to follow again: the situation to go back to when it completes.
    Situation follow_again_returns_to_{Situation::current};
    bool latest_request_asked_to_follow_{false};
    bool catch_up_completed_{false};
    std::optional<PeerLeader> peer_leader_;
};

} // namespaces
