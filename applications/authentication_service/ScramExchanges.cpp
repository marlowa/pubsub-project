// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "ScramExchanges.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include <authentication.hpp>
#include <scram_crypto/ScramCrypto.hpp>

namespace authentication_service {

ScramExchanges::ScramExchanges(const Credentials& credentials, const SessionPolicies& session_policies)
    : credentials_(credentials), session_policies_(session_policies) {}

ScramExchanges::RequestAnswer ScramExchanges::on_request(const pubsub_itc_fw::ConnectionID& connection_id, int64_t request_id, const std::string& comp_id,
                                                         const std::vector<uint8_t>& client_nonce, const std::vector<uint8_t>& server_nonce_suffix) {
    RequestAnswer answer;
    const auto credential = credentials_.find(comp_id);
    if (credential == credentials_.end()) {
        exchanges_.erase(request_id);
        return answer;
    }

    Exchange exchange;
    exchange.connection_id = connection_id;
    exchange.comp_id = comp_id;
    exchange.client_nonce = client_nonce;
    exchange.server_nonce = client_nonce;
    exchange.server_nonce.insert(exchange.server_nonce.end(), server_nonce_suffix.begin(), server_nonce_suffix.end());
    exchange.credential = credential->second;

    answer.challenge = true;
    answer.server_nonce = exchange.server_nonce;
    answer.salt = exchange.credential.salt;
    answer.iterations = exchange.credential.iterations;

    exchanges_[request_id] = std::move(exchange);
    return answer;
}

ScramExchanges::ProofAnswer ScramExchanges::on_proof(int64_t request_id, const uint8_t* client_proof, size_t client_proof_size) {
    const auto exchange = exchanges_.find(request_id);
    if (exchange == exchanges_.end()) {
        return ProofAnswer{};
    }
    ProofAnswer answer = check_proof(exchange->second, client_proof, client_proof_size);
    exchanges_.erase(exchange);
    return answer;
}

ScramExchanges::ProofAnswer ScramExchanges::check_proof(const Exchange& exchange, const uint8_t* client_proof, size_t client_proof_size) const {
    ProofAnswer answer;
    answer.exchange_found = true;
    answer.comp_id = exchange.comp_id;
    answer.outcome = pubsub_itc_fw_app::AuthenticationOutcome::BadPassword;

    if (client_proof == nullptr || client_proof_size != sha256_size) {
        return answer;
    }

    // RFC 5802, section 3: ClientSignature = HMAC(StoredKey, AuthMessage); ClientKey = ClientProof XOR ClientSignature.
    // The proof is good when H(ClientKey) is the stored key, which only a party holding the password can bring about.
    const scram_crypto::ScramCredential& credential = exchange.credential;
    const std::vector<uint8_t> auth_message = scram_crypto::compute_auth_message(exchange.comp_id, exchange.client_nonce, exchange.server_nonce,
                                                                                 credential.salt.data(), credential.salt.size(), credential.iterations);
    const std::vector<uint8_t> client_signature =
        scram_crypto::hmac_sha256(credential.stored_key.data(), credential.stored_key.size(), auth_message.data(), auth_message.size());

    std::vector<uint8_t> client_key(sha256_size);
    for (size_t index = 0; index < sha256_size; ++index) {
        client_key[index] = client_proof[index] ^ client_signature[index];
    }
    if (scram_crypto::sha256(client_key.data(), client_key.size()) != credential.stored_key) {
        return answer;
    }

    answer.outcome = pubsub_itc_fw_app::AuthenticationOutcome::Granted;
    answer.server_signature = scram_crypto::hmac_sha256(credential.server_key.data(), credential.server_key.size(), auth_message.data(), auth_message.size());
    const auto policy = session_policies_.find(exchange.comp_id);
    if (policy != session_policies_.end()) {
        answer.session_policy = &policy->second;
    }
    return answer;
}

void ScramExchanges::forget_connection(const pubsub_itc_fw::ConnectionID& connection_id) {
    for (auto it = exchanges_.begin(); it != exchanges_.end();) {
        if (it->second.connection_id == connection_id) {
            it = exchanges_.erase(it);
        } else {
            ++it;
        }
    }
}

pubsub_itc_fw_app::AuthenticationResult ScramExchanges::result_message(int64_t request_id, const ProofAnswer& answer) {
    pubsub_itc_fw_app::AuthenticationResult result{};
    result.request_id = request_id;
    result.outcome = answer.outcome;
    result.server_signature =
        pubsub_itc_fw_app::BytesView{answer.server_signature.empty() ? nullptr : answer.server_signature.data(), answer.server_signature.size()};
    result.force_password_change = false;

    if (answer.outcome != pubsub_itc_fw_app::AuthenticationOutcome::Granted || answer.session_policy == nullptr) {
        return result;
    }
    const SessionPolicy& policy = *answer.session_policy;
    // Each optional field is sent only when the comp id has a value for it: absent tells the gateway the member said
    // nothing, which is not the same as the value that happens to be the default (see the note on the message in
    // authentication.dsl).
    if (policy.cancel_on_disconnect_enabled.has_value()) {
        result.has_cancel_on_disconnect_enabled = true;
        result.cancel_on_disconnect_enabled = *policy.cancel_on_disconnect_enabled;
    }
    if (policy.cancel_on_disconnect_grace_period_seconds.has_value()) {
        result.has_cancel_on_disconnect_grace_period_seconds = true;
        result.cancel_on_disconnect_grace_period_seconds = *policy.cancel_on_disconnect_grace_period_seconds;
    }
    // The gateway compares these with its own instance id and refuses a logon it is not provisioned for, so this
    // service never needs to know which instance it is talking to.
    if (policy.primary_gateway_instance.has_value()) {
        result.has_primary_gateway_instance = true;
        result.primary_gateway_instance = *policy.primary_gateway_instance;
    }
    if (policy.backup_gateway_instance.has_value()) {
        result.has_backup_gateway_instance = true;
        result.backup_gateway_instance = *policy.backup_gateway_instance;
    }
    result.max_place_per_second = policy.max_place_per_second;
    result.max_amend_per_second = policy.max_amend_per_second;
    result.max_cancel_per_second = policy.max_cancel_per_second;
    return result;
}

pubsub_itc_fw_app::AuthenticationResult ScramExchanges::unknown_user_message(int64_t request_id) {
    pubsub_itc_fw_app::AuthenticationResult result{};
    result.request_id = request_id;
    result.outcome = pubsub_itc_fw_app::AuthenticationOutcome::UnknownUser;
    result.server_signature = pubsub_itc_fw_app::BytesView{nullptr, 0};
    result.force_password_change = false;
    return result;
}

} // namespaces
