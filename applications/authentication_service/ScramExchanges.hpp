#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <pubsub_itc_fw/ConnectionID.hpp>

#include <authentication.hpp>
#include <scram_crypto/ScramCrypto.hpp>

#include "AuthenticationServiceConfiguration.hpp"

namespace authentication_service {

/**
 * @brief The authentication service's side of the SCRAM-SHA-256 exchanges (RFC 5802) that the order gateways run
 *        when a member logs on: what to answer to each request and each proof.
 *
 * An exchange is four messages. The gateway sends AuthenticationRequest with the comp id and a random client nonce.
 * The service answers with AuthenticationChallenge, carrying the server nonce (the client nonce followed by random
 * bytes of the service's own), and the salt and iteration count stored for that comp id. The gateway derives a proof
 * from the member's password and sends AuthenticationProof. The service checks the proof against the stored key and
 * answers with AuthenticationResult: Granted with the server signature, which lets the gateway check that it is talking
 * to the real service, or BadPassword. An unknown comp id is answered at once with an AuthenticationResult of
 * UnknownUser, and no challenge.
 *
 * This class holds the exchanges in progress, between a challenge and its proof, keyed by the gateway's request id. It
 * decides; it does not send, decode or log. AuthenticationThread does those, so that the decisions can be tested
 * without a reactor.
 *
 * **Random bytes.** The caller supplies the random part of the server nonce, so that a test can say what it is. In
 * the service it comes from the operating system's cryptographic random number generator.
 *
 * **Credentials.** The class reads the credential map and the session policies through references to maps the caller
 * owns, because the administration channel changes the credentials while the service runs. Both maps must outlive
 * this object. An exchange keeps its own copy of the credential it started with, so a credential changed between
 * challenge and proof does not affect an exchange already in progress.
 *
 * **Threading.** Not thread-safe. The service uses it from its one application thread.
 */
class ScramExchanges {
  public:
    using Credentials = std::unordered_map<std::string, scram_crypto::ScramCredential>;
    using SessionPolicy = AuthenticationServiceConfiguration::SessionPolicy;
    using SessionPolicies = std::unordered_map<std::string, SessionPolicy>;

    /// The size of a SHA-256 digest, and so of a client proof, a stored key and a server signature.
    static constexpr size_t sha256_size = 32;

    /// What to send in answer to an AuthenticationRequest.
    struct RequestAnswer {
        /// true to send an AuthenticationChallenge; false to send an AuthenticationResult of UnknownUser.
        bool challenge{false};
        std::vector<uint8_t> server_nonce; ///< For a challenge: the client nonce followed by the random suffix.
        std::vector<uint8_t> salt;         ///< For a challenge: the salt stored for the comp id.
        int32_t iterations{0};             ///< For a challenge: the iteration count stored for the comp id.
    };

    /// What to send in answer to an AuthenticationProof.
    struct ProofAnswer {
        /// false when no exchange is in progress under the request id; then nothing is sent.
        bool exchange_found{false};
        std::string comp_id;                                                                                     ///< The comp id the exchange is for.
        pubsub_itc_fw_app::AuthenticationOutcome outcome{pubsub_itc_fw_app::AuthenticationOutcome::BadPassword}; ///< Granted or BadPassword.
        std::vector<uint8_t> server_signature; ///< For Granted: the SCRAM server signature. Empty otherwise.
        /// For Granted: the comp id's session policy, or nullptr if it has none. Points into the caller's map.
        const SessionPolicy* session_policy{nullptr};
    };

    ~ScramExchanges() = default;

    /**
     * @param[in] credentials      The SCRAM credential of each comp id. Must outlive this object.
     * @param[in] session_policies The session policy of each comp id that has one. Must outlive this object.
     */
    ScramExchanges(const Credentials& credentials, const SessionPolicies& session_policies);

    ScramExchanges(const ScramExchanges& other) = delete;
    ScramExchanges& operator=(const ScramExchanges& other) = delete;

    /**
     * @brief Decides the answer to an AuthenticationRequest, and for a known comp id starts an exchange.
     *
     * A second request under a request id already in progress replaces the first exchange.
     *
     * @param[in] connection_id       The gateway connection the request arrived on.
     * @param[in] request_id          The gateway's identifier for this exchange.
     * @param[in] comp_id             The comp id logging on.
     * @param[in] client_nonce        The gateway's random nonce.
     * @param[in] server_nonce_suffix The service's random bytes, appended to the client nonce to make the server nonce.
     * @return The challenge to send, or that the comp id is unknown.
     */
    [[nodiscard]] RequestAnswer on_request(const pubsub_itc_fw::ConnectionID& connection_id, int64_t request_id, const std::string& comp_id,
                                           const std::vector<uint8_t>& client_nonce, const std::vector<uint8_t>& server_nonce_suffix);

    /**
     * @brief Checks an AuthenticationProof and ends its exchange.
     *
     * @param[in] request_id        The gateway's identifier for the exchange.
     * @param[in] client_proof      The proof's bytes. May be nullptr when client_proof_size is zero.
     * @param[in] client_proof_size The number of bytes. Anything but sha256_size is a bad proof.
     * @return The outcome, or that no exchange is in progress under @p request_id.
     */
    [[nodiscard]] ProofAnswer on_proof(int64_t request_id, const uint8_t* client_proof, size_t client_proof_size);

    /**
     * @brief Ends every exchange begun on a connection, because the connection has gone.
     *
     * @param[in] connection_id The connection lost.
     */
    void forget_connection(const pubsub_itc_fw::ConnectionID& connection_id);

    /// How many exchanges are between their challenge and their proof.
    [[nodiscard]] size_t exchanges_in_progress() const {
        return exchanges_.size();
    }

    /**
     * @brief The AuthenticationResult message for an answer to a proof.
     *
     * The session policy is sent only with Granted: sending it on a refusal would hand session policy to something
     * that has just failed to prove who it is. A comp id with no policy sends none of the optional fields, leaving the
     * gateway on its own defaults, and zero for each throttle limit, meaning no limit.
     *
     * @param[in] request_id The gateway's identifier for the exchange.
     * @param[in] answer     The answer from on_proof(). The message refers to its server signature, so it must outlive
     *                       the message.
     */
    [[nodiscard]] static pubsub_itc_fw_app::AuthenticationResult result_message(int64_t request_id, const ProofAnswer& answer);

    /**
     * @brief The AuthenticationResult message refusing a logon for an unknown comp id.
     *
     * @param[in] request_id The gateway's identifier for the exchange.
     */
    [[nodiscard]] static pubsub_itc_fw_app::AuthenticationResult unknown_user_message(int64_t request_id);

  private:
    struct Exchange {
        pubsub_itc_fw::ConnectionID connection_id;
        std::string comp_id;
        std::vector<uint8_t> client_nonce;
        std::vector<uint8_t> server_nonce;
        scram_crypto::ScramCredential credential;
    };

    [[nodiscard]] ProofAnswer check_proof(const Exchange& exchange, const uint8_t* client_proof, size_t client_proof_size) const;

    const Credentials& credentials_;
    const SessionPolicies& session_policies_;
    std::unordered_map<int64_t, Exchange> exchanges_;
};

} // namespaces
