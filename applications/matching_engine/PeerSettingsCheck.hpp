#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint> // IWYU pragma: keep
#include <string>

#include <pubsub_itc_fw/QuillLogger.hpp>

namespace matching_engine {

/**
 * @brief What the matching engine thread supplies so that PeerSettingsCheck can act through the lease agent.
 */
class PeerSettingsCheckLinksInterface {
  public:
    virtual ~PeerSettingsCheckLinksInterface() = default;

    /// Whether this instance leads, as the lease agent says.
    [[nodiscard]] virtual bool leading() const = 0;
    /// Whether this instance says, through the lease agent, that its peer may lead.
    [[nodiscard]] virtual bool says_peer_may_lead() const = 0;
    /// Says, through the lease agent, that the peer may not lead, or that it may lead again.
    virtual void say_peer_may_not_lead() = 0;
    virtual void say_peer_may_lead() = 0;

    /**
     * @brief The test event SettingsDifferFromPeer.
     * @param[in] own_capacity    This instance's open-order region capacity.
     * @param[in] peer_capacity   The peer's.
     * @param[in] barred_peer     Whether this instance, leading, has said its peer may not lead.
     * @param[in] secrets_differ  Whether the identifier secrets differ.
     */
    virtual void settings_differ(int64_t own_capacity, int64_t peer_capacity, bool barred_peer, bool secrets_differ) = 0;
};

/**
 * @brief The settings the two matching engines of a pair must share, compared, and a peer whose settings differ barred
 *        from leading (docs/availability/deterministic_matching_engine.md, section 5;
 *        docs/venue/dividing_the_sequencer_and_matching_engine.md, section 6.6).
 *
 * Two settings change what an engine does with a command: the open-order region's capacity, which decides when an
 * order is refused because the venue holds as many as it can, and the identifier secret, from which the keys of the
 * OrderID and ExecID values are derived. Two engines that differ in either would apply the same commands differently.
 *
 * The situations are: the peer's settings not yet known; known and the same; known and different. Known settings are
 * kept when the connection to the peer is lost: a peer that restarts says again what it is configured with.
 *
 * While this instance leads, it says through the lease agent that its peer may not lead whenever the settings differ,
 * and that it may lead again when they no longer differ (bar_peer_if_settings_differ). This is checked when the peer's
 * settings arrive and when this instance takes the lead.
 *
 * One thread, the engine's, calls every member function.
 */
class PeerSettingsCheck {
  public:
    /**
     * @param[in] own_capacity    This instance's open-order region capacity.
     * @param[in] own_fingerprint The fingerprint of this instance's identifier secret.
     * @param[in] peer_instance   The peer's instance number, for the Quill log.
     * @param[in] secret_file     Where the secret is read from, for the Quill log.
     * @param[in] links           How the lease agent is told and the test is told.
     * @param[in] logger          Where what happens is logged.
     */
    PeerSettingsCheck(int64_t own_capacity, int64_t own_fingerprint, int64_t peer_instance, std::string secret_file, PeerSettingsCheckLinksInterface& links,
                      pubsub_itc_fw::QuillLogger& logger);

    /// The peer, instance @p instance_id, has said what it is configured with.
    void peer_settings_arrived(int64_t instance_id, int64_t region_capacity, int64_t secret_fingerprint);

    /**
     * @brief While this instance leads, says that its peer may not lead if the settings differ, and that it may lead
     *        again if they no longer differ.
     * @return Whether it has just said its peer may not lead.
     */
    bool bar_peer_if_settings_differ();

    /// Tells the test, as the instance's present state, that the peer's settings differ, if they are known to.
    void report_present_state();

    [[nodiscard]] bool known() const {
        return known_;
    }
    [[nodiscard]] bool differ() const {
        return differ_;
    }

  private:
    void report_differ();

    const int64_t own_capacity_;
    const int64_t own_fingerprint_;
    const int64_t peer_instance_;
    const std::string secret_file_;
    PeerSettingsCheckLinksInterface& links_;
    pubsub_itc_fw::QuillLogger& logger_;

    bool known_{false};
    int64_t peer_capacity_{0};
    int64_t peer_fingerprint_{0};
    bool differ_{false};
};

} // namespaces
