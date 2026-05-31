#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/Reactor.hpp>

#include <AuthenticationServiceConfiguration.hpp>
#include <scram_crypto/ScramCrypto.hpp>

namespace authentication_service {

/**
 * @brief ApplicationThread subclass that handles the SCRAM-SHA-256 authentication protocol.
 *
 * Accepts inbound PDU connections from gateways. Each connection carries one or more
 * four-message SCRAM-SHA-256 exchanges (AuthenticationRequest -> AuthenticationChallenge ->
 * AuthenticationProof -> AuthenticationResult), one per gateway logon attempt.
 *
 * Credentials are loaded from the file named by config.credentials_file at startup.
 * An AuthenticationRequest from an unknown comp_id is answered with UnknownUser.
 *
 * PDU IDs (from authentication.dsl):
 *   500 AuthenticationRequest   -- received from gateway
 *   501 AuthenticationChallenge -- sent to gateway
 *   502 AuthenticationProof     -- received from gateway
 *   503 AuthenticationResult    -- sent to gateway
 *
 * Threading: ThreadID 1.
 */
class AuthenticationThread : public pubsub_itc_fw::ApplicationThread {
  public:
    /**
     * @param[in] token   Constructor token to force use of factory.
     * @param[in] logger  Logger instance. Must outlive this object.
     * @param[in] reactor The owning Reactor. Must outlive this object.
     * @param[in] config  Service configuration.
     */
    AuthenticationThread(pubsub_itc_fw::ApplicationThread::ConstructorToken token,
                         pubsub_itc_fw::QuillLogger& logger,
                         pubsub_itc_fw::Reactor& reactor,
                         const AuthenticationServiceConfiguration& config);

  protected:
    void on_connection_established(pubsub_itc_fw::ConnectionID id) override;
    void on_connection_lost(pubsub_itc_fw::ConnectionID id, const std::string& reason) override;
    void on_framework_pdu_message(const pubsub_itc_fw::EventMessage& msg) override;
    void on_itc_message(const pubsub_itc_fw::EventMessage& msg) override;

  private:
    struct ExchangeState {
        int64_t request_id{0};
        std::string comp_id;
        std::vector<uint8_t> client_nonce;
        std::vector<uint8_t> server_nonce;
        std::vector<uint8_t> salt;
        int32_t iterations{0};
        std::vector<uint8_t> stored_key;
        std::vector<uint8_t> server_key;
    };

    void handle_authentication_request(pubsub_itc_fw::ConnectionID conn_id,
                                        const pubsub_itc_fw::EventMessage& msg);
    void handle_authentication_proof(pubsub_itc_fw::ConnectionID conn_id,
                                      const pubsub_itc_fw::EventMessage& msg);

    const AuthenticationServiceConfiguration& config_;
    std::unordered_map<pubsub_itc_fw::ConnectionID, ExchangeState> exchanges_;
};

} // namespace authentication_service
