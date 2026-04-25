#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <string>

#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/Reactor.hpp>

#include "ArbiterConfiguration.hpp"

namespace arbiter {

/**
 * @brief ApplicationThread subclass implementing the main-site arbiter.
 *
 * The arbiter has no involvement in the order flow. Its sole responsibility
 * is to implement the arbiter side of the leader-follower protocol:
 *
 *   - Accepts inbound PDU connections from sequencer instances.
 *   - Receives ArbitrationReport PDUs.
 *   - Replies with ArbitrationDecision PDUs.
 *
 * The leader-follower state machine is TODO. The stub logs inbound PDUs.
 *
 * Threading: ThreadID 1.
 */
class ArbiterThread : public pubsub_itc_fw::ApplicationThread {
  public:
    /**
     * @param[in] token    Constructor token to force use of factory.
     * @param[in] logger   Logger. Must outlive this object.
     * @param[in] reactor  Owning Reactor. Must outlive this object.
     * @param[in] config   Arbiter configuration.
     */
    ArbiterThread(pubsub_itc_fw::ApplicationThread::ConstructorToken token,
                  pubsub_itc_fw::QuillLogger& logger,
                  pubsub_itc_fw::Reactor& reactor,
                  const ArbiterConfiguration& config);

  protected:
    void on_connection_established(pubsub_itc_fw::ConnectionID id) override;
    void on_connection_lost(pubsub_itc_fw::ConnectionID id, const std::string& reason) override;
    void on_framework_pdu_message(const pubsub_itc_fw::EventMessage& message) override;
    void on_timer_event(const std::string& name) override;
    void on_itc_message(const pubsub_itc_fw::EventMessage& message) override;

  private:
    const ArbiterConfiguration& config_;
};

} // namespace arbiter
