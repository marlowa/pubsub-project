// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <openssl/rand.h>

#include <AuthenticationThread.hpp>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>

#include <pubsub_itc_fw/AllocatorConfiguration.hpp>
#include <pubsub_itc_fw/ApplicationThreadConfiguration.hpp>
#include <pubsub_itc_fw/BumpAllocator.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>
#include <pubsub_itc_fw/QueueConfiguration.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>

#include <authentication.hpp>
#include <scram_crypto/ScramCrypto.hpp>

namespace authentication_service {

namespace {

constexpr int16_t pdu_id_authentication_request = 500;
constexpr int16_t pdu_id_authentication_challenge = 501;
constexpr int16_t pdu_id_authentication_proof = 502;
constexpr int16_t pdu_id_authentication_result = 503;

constexpr uint16_t pdu_id_set_credential_request = 510;
constexpr uint16_t pdu_id_set_credential_result = 511;
constexpr uint16_t pdu_id_remove_credential_request = 512;
constexpr uint16_t pdu_id_remove_credential_result = 513;
constexpr uint16_t pdu_id_restore_credential_request = 514;
constexpr uint16_t pdu_id_restore_credential_result = 515;

// PDU header layout (24 bytes, all fields big-endian):
//   byte_count(4) pdu_id(2) version(1) filler(1) seq_no(8) canary(4) filler(4)
constexpr size_t admin_pdu_header_size = 24;
constexpr uint32_t admin_pdu_canary = 0xC0FFEE00;
constexpr uint8_t admin_pdu_version = 1;

pubsub_itc_fw::QueueConfiguration make_queue_config() {
    pubsub_itc_fw::QueueConfiguration queue_configuration{};
    queue_configuration.low_watermark = 1;
    queue_configuration.high_watermark = 64;
    return queue_configuration;
}

pubsub_itc_fw::AllocatorConfiguration make_allocator_config(const AuthenticationServiceConfiguration& config, pubsub_itc_fw::QuillLogger& logger) {
    pubsub_itc_fw::AllocatorConfiguration allocator_configuration{};
    allocator_configuration.pool_name = "AuthenticationPool";
    allocator_configuration.objects_per_pool = config.event_queue_pool_objects_per_slab;
    allocator_configuration.initial_pools = config.event_queue_pool_initial_slabs;
    allocator_configuration.handler_for_pool_exhausted = [&logger](void* /*context*/, int objects_per_pool) {
        PUBSUB_LOG(logger, pubsub_itc_fw::FwLogLevel::Warning, "AuthenticationPool exhausted: chaining new pool slab ({} objects)", objects_per_pool);
    };
    return allocator_configuration;
}

} // namespace

AuthenticationThread::AuthenticationThread(pubsub_itc_fw::ApplicationThread::ConstructorToken token, pubsub_itc_fw::QuillLogger& logger,
                                           pubsub_itc_fw::Reactor& reactor, const AuthenticationServiceConfiguration& config)
    : ApplicationThread(token, logger, reactor, "AuthenticationThread", pubsub_itc_fw::ThreadID{1}, make_queue_config(), make_allocator_config(config, logger),
                        pubsub_itc_fw::ApplicationThreadConfiguration{})
    , config_(config)
    , credentials_(config.credentials) {}

void AuthenticationThread::on_connection_established(pubsub_itc_fw::ConnectionID id) {
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "AuthenticationThread: connection established conn_id={}", id.get_value());
}

void AuthenticationThread::on_connection_lost(pubsub_itc_fw::ConnectionID id, const std::string& reason) {
    if (admin_connections_.erase(id) > 0) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "AuthenticationThread: admin connection lost conn_id={} reason={}", id.get_value(), reason);
    } else {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "AuthenticationThread: connection lost conn_id={} reason={}", id.get_value(), reason);
        exchanges_.erase(id);
    }
}

void AuthenticationThread::on_framework_pdu_message(const pubsub_itc_fw::EventMessage& msg) {
    const auto pdu_id = msg.pdu_id();
    const pubsub_itc_fw::ConnectionID& conn_id = msg.connection_id();

    if (pdu_id == pdu_id_authentication_request) {
        handle_authentication_request(conn_id, msg);
    } else if (pdu_id == pdu_id_authentication_proof) {
        handle_authentication_proof(conn_id, msg);
    } else {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "AuthenticationThread: unexpected pdu_id={} conn_id={} -- dropping", pdu_id,
                   conn_id.get_value());
    }

    release_pdu_payload(msg);
}

void AuthenticationThread::on_itc_message([[maybe_unused]] const pubsub_itc_fw::EventMessage& msg) {}

void AuthenticationThread::handle_authentication_request(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& msg) {
    auto& arena_buffer = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buffer.data(), arena_buffer.size());
    arena.reset();

    pubsub_itc_fw_app::AuthenticationRequestView view{};
    size_t bytes_consumed = 0;
    size_t arena_bytes_needed = 0;

    if (!pubsub_itc_fw_app::decode(view, msg.payload(), static_cast<size_t>(msg.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Error, "AuthenticationThread: failed to decode AuthenticationRequest conn_id={} -- dropping",
                   conn_id.get_value());
        return;
    }

    const std::string comp_id(view.comp_id.data(), view.comp_id.size());
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "AuthenticationThread: AuthenticationRequest request_id={} comp_id={} conn_id={}",
               view.request_id, comp_id, conn_id.get_value());

    // Build server_nonce = client_nonce || 16 cryptographically random bytes.
    const std::vector<uint8_t> client_nonce(view.client_nonce.data, view.client_nonce.data + view.client_nonce.size);
    std::vector<uint8_t> server_nonce = client_nonce;
    uint8_t random_suffix[16];
    if (RAND_bytes(random_suffix, static_cast<int>(sizeof(random_suffix))) != 1) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Error, "AuthenticationThread: RAND_bytes failed conn_id={} -- dropping", conn_id.get_value());
        return;
    }
    server_nonce.insert(server_nonce.end(), random_suffix, random_suffix + sizeof(random_suffix));

    auto cred_it = credentials_.find(comp_id);
    if (cred_it == config_.credentials.end()) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "AuthenticationThread: unknown comp_id={} conn_id={} request_id={} -- UnknownUser",
                   comp_id, conn_id.get_value(), view.request_id);
        pubsub_itc_fw_app::AuthenticationResult result{};
        result.request_id = view.request_id;
        result.outcome = pubsub_itc_fw_app::AuthenticationOutcome::UnknownUser;
        result.server_signature = pubsub_itc_fw_app::BytesView{nullptr, 0};
        result.force_password_change = false;
        send_pdu(conn_id, pdu_id_authentication_result, 0, result);
        return;
    }

    const scram_crypto::ScramCredential& cred = cred_it->second;
    ExchangeState& state = exchanges_[conn_id];
    state.request_id = view.request_id;
    state.comp_id = comp_id;
    state.client_nonce = client_nonce;
    state.server_nonce = server_nonce;
    state.salt = cred.salt;
    state.iterations = cred.iterations;
    state.stored_key = cred.stored_key;
    state.server_key = cred.server_key;

    pubsub_itc_fw_app::AuthenticationChallenge challenge{};
    challenge.request_id = view.request_id;
    challenge.server_nonce = pubsub_itc_fw_app::BytesView{server_nonce.data(), server_nonce.size()};
    challenge.salt = pubsub_itc_fw_app::BytesView{state.salt.data(), state.salt.size()};
    challenge.iterations = state.iterations;
    send_pdu(conn_id, pdu_id_authentication_challenge, 0, challenge);

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "AuthenticationThread: AuthenticationChallenge sent request_id={} comp_id={}", view.request_id,
               comp_id);
}

void AuthenticationThread::handle_authentication_proof(const pubsub_itc_fw::ConnectionID& conn_id, const pubsub_itc_fw::EventMessage& msg) {
    auto& arena_buffer = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buffer.data(), arena_buffer.size());
    arena.reset();

    pubsub_itc_fw_app::AuthenticationProofView view{};
    size_t bytes_consumed = 0;
    size_t arena_bytes_needed = 0;

    if (!pubsub_itc_fw_app::decode(view, msg.payload(), static_cast<size_t>(msg.payload_size()), bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Error, "AuthenticationThread: failed to decode AuthenticationProof conn_id={} -- dropping",
                   conn_id.get_value());
        return;
    }

    auto it = exchanges_.find(conn_id);
    if (it == exchanges_.end()) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "AuthenticationThread: AuthenticationProof with no pending exchange "
                   "conn_id={} request_id={} -- dropping",
                   conn_id.get_value(), view.request_id);
        return;
    }

    const ExchangeState& state = it->second;
    if (state.request_id != view.request_id) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "AuthenticationThread: request_id mismatch expected={} got={} conn_id={} -- dropping",
                   state.request_id, view.request_id, conn_id.get_value());
        return;
    }

    auto send_result = [&](pubsub_itc_fw_app::AuthenticationOutcome outcome, const std::vector<uint8_t>& server_signature) {
        pubsub_itc_fw_app::AuthenticationResult result{};
        result.request_id = view.request_id;
        result.outcome = outcome;
        result.server_signature = pubsub_itc_fw_app::BytesView{server_signature.empty() ? nullptr : server_signature.data(), server_signature.size()};
        result.force_password_change = false;
        send_pdu(conn_id, pdu_id_authentication_result, 0, result);
    };

    // Verify ClientProof:
    //   ClientSignature = HMAC-SHA-256(stored_key, AuthMessage)
    //   ClientKey       = ClientProof XOR ClientSignature
    //   Check: SHA-256(ClientKey) == stored_key
    static constexpr size_t sha256_size = 32;
    if (view.client_proof.size != sha256_size) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "AuthenticationThread: client_proof has wrong size {} (expected {}) "
                   "request_id={} comp_id={} conn_id={} -- BadPassword",
                   view.client_proof.size, sha256_size, view.request_id, state.comp_id, conn_id.get_value());
        exchanges_.erase(it);
        send_result(pubsub_itc_fw_app::AuthenticationOutcome::BadPassword, {});
        return;
    }

    const std::vector<uint8_t> auth_message =
        scram_crypto::compute_auth_message(state.comp_id, state.client_nonce, state.server_nonce, state.salt.data(), state.salt.size(), state.iterations);

    const std::vector<uint8_t> client_signature =
        scram_crypto::hmac_sha256(state.stored_key.data(), state.stored_key.size(), auth_message.data(), auth_message.size());

    std::vector<uint8_t> client_key(sha256_size);
    for (size_t i = 0; i < sha256_size; ++i) {
        client_key[i] = view.client_proof.data[i] ^ client_signature[i];
    }

    const std::vector<uint8_t> recovered_stored_key = scram_crypto::sha256(client_key.data(), client_key.size());
    if (recovered_stored_key != state.stored_key) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "AuthenticationThread: ClientProof verification failed "
                   "request_id={} comp_id={} conn_id={} -- BadPassword",
                   view.request_id, state.comp_id, conn_id.get_value());
        exchanges_.erase(it);
        send_result(pubsub_itc_fw_app::AuthenticationOutcome::BadPassword, {});
        return;
    }

    // Proof is valid. Compute ServerSignature and send Granted.
    const std::vector<uint8_t> server_signature =
        scram_crypto::hmac_sha256(state.server_key.data(), state.server_key.size(), auth_message.data(), auth_message.size());

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "AuthenticationThread: AuthenticationResult Granted request_id={} comp_id={} conn_id={}",
               view.request_id, state.comp_id, conn_id.get_value());

    exchanges_.erase(it);
    send_result(pubsub_itc_fw_app::AuthenticationOutcome::Granted, server_signature);
}

void AuthenticationThread::on_raw_socket_message(const pubsub_itc_fw::EventMessage& msg) {
    const pubsub_itc_fw::ConnectionID& conn_id = msg.connection_id();
    admin_connections_.insert(conn_id);

    const auto* data = msg.payload();
    const auto available = static_cast<int64_t>(msg.payload_size());

    if (available < static_cast<int64_t>(admin_pdu_header_size)) {
        return; // Partial header — wait for more bytes, do not commit.
    }

    // Parse big-endian header fields.
    const uint32_t byte_count = (uint32_t{data[0]} << 24) | (uint32_t{data[1]} << 16) | (uint32_t{data[2]} << 8) | uint32_t{data[3]};
    const uint16_t pdu_id = (uint16_t{data[4]} << 8) | uint16_t{data[5]};
    const uint32_t canary = (uint32_t{data[16]} << 24) | (uint32_t{data[17]} << 16) | (uint32_t{data[18]} << 8) | uint32_t{data[19]};

    const int64_t total_size = static_cast<int64_t>(admin_pdu_header_size) + static_cast<int64_t>(byte_count);
    if (available < total_size) {
        return; // Partial payload — wait for more bytes, do not commit.
    }

    if (canary != admin_pdu_canary) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "AuthenticationThread: admin conn_id={} bad canary 0x{:08x} -- closing",
                   conn_id.get_value(), canary);
        commit_raw_bytes(conn_id, total_size);
        return;
    }

    const uint8_t* payload = data + admin_pdu_header_size;

    if (pdu_id == pdu_id_set_credential_request) {
        handle_set_credential_request(conn_id, payload, static_cast<uint32_t>(byte_count));
    } else if (pdu_id == pdu_id_remove_credential_request) {
        handle_remove_credential_request(conn_id, payload, static_cast<uint32_t>(byte_count));
    } else if (pdu_id == pdu_id_restore_credential_request) {
        handle_restore_credential_request(conn_id, payload, static_cast<uint32_t>(byte_count));
    } else {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "AuthenticationThread: admin conn_id={} unknown pdu_id={} -- dropping",
                   conn_id.get_value(), pdu_id);
    }

    commit_raw_bytes(conn_id, total_size);
}

void AuthenticationThread::handle_set_credential_request(const pubsub_itc_fw::ConnectionID& conn_id, const uint8_t* payload, uint32_t payload_size) {
    auto& arena_buffer = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buffer.data(), arena_buffer.size());
    arena.reset();

    pubsub_itc_fw_app::SetCredentialRequestView view{};
    size_t bytes_consumed = 0;
    size_t arena_bytes_needed = 0;

    if (!pubsub_itc_fw_app::decode(view, payload, payload_size, bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Error, "AuthenticationThread: failed to decode SetCredentialRequest conn_id={} -- dropping",
                   conn_id.get_value());
        return;
    }

    const std::string comp_id(view.comp_id.data(), view.comp_id.size());
    const std::string password(view.password.data(), view.password.size());
    int32_t iterations = view.iterations;
    if (iterations == 0) {
        iterations = 4096;
    }

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "AuthenticationThread: SetCredentialRequest request_id={} comp_id={} iterations={} conn_id={}",
               view.request_id, comp_id, iterations, conn_id.get_value());

    auto send_result = [&](pubsub_itc_fw_app::SetCredentialOutcome outcome) {
        pubsub_itc_fw_app::SetCredentialResult result{};
        result.request_id = view.request_id;
        result.comp_id = comp_id;
        result.outcome = outcome;

        static constexpr size_t result_buffer_size = 512;
        uint8_t result_buffer[result_buffer_size];
        size_t bytes_written = 0;
        size_t bytes_needed = 0;
        if (!pubsub_itc_fw_app::encode(result, result_buffer, result_buffer_size, bytes_written, bytes_needed)) {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Error, "AuthenticationThread: failed to encode SetCredentialResult conn_id={}",
                       conn_id.get_value());
            return;
        }
        send_admin_pdu(conn_id, pdu_id_set_credential_result, result_buffer, static_cast<uint32_t>(bytes_written));
    };

    if (comp_id.empty()) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "AuthenticationThread: SetCredentialRequest empty comp_id -- InvalidInput");
        send_result(pubsub_itc_fw_app::SetCredentialOutcome::InvalidInput);
        return;
    }
    if (password.empty()) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "AuthenticationThread: SetCredentialRequest empty password comp_id={} -- InvalidInput",
                   comp_id);
        send_result(pubsub_itc_fw_app::SetCredentialOutcome::InvalidInput);
        return;
    }
    if (iterations < 1000 || iterations > 1000000) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "AuthenticationThread: SetCredentialRequest iterations={} out of range "
                   "[1000,1000000] comp_id={} -- InvalidInput",
                   iterations, comp_id);
        send_result(pubsub_itc_fw_app::SetCredentialOutcome::InvalidInput);
        return;
    }

    try {
        uint8_t salt_bytes[16];
        if (RAND_bytes(salt_bytes, static_cast<int>(sizeof(salt_bytes))) != 1) {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Error, "AuthenticationThread: RAND_bytes failed for SetCredentialRequest comp_id={}", comp_id);
            send_result(pubsub_itc_fw_app::SetCredentialOutcome::InternalError);
            return;
        }

        credentials_[comp_id] = scram_crypto::make_scram_credential(password, salt_bytes, sizeof(salt_bytes), iterations);

        persist_credentials();

        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "AuthenticationThread: SetCredentialRequest request_id={} comp_id={} -- Success",
                   view.request_id, comp_id);
        send_result(pubsub_itc_fw_app::SetCredentialOutcome::Success);

    } catch (const std::exception& ex) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Error, "AuthenticationThread: SetCredentialRequest comp_id={} exception: {}", comp_id, ex.what());
        send_result(pubsub_itc_fw_app::SetCredentialOutcome::InternalError);
    }
}

void AuthenticationThread::handle_remove_credential_request(const pubsub_itc_fw::ConnectionID& conn_id, const uint8_t* payload, uint32_t payload_size) {
    auto& arena_buffer = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buffer.data(), arena_buffer.size());
    arena.reset();

    pubsub_itc_fw_app::RemoveCredentialRequestView view{};
    size_t bytes_consumed = 0;
    size_t arena_bytes_needed = 0;

    if (!pubsub_itc_fw_app::decode(view, payload, payload_size, bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Error, "AuthenticationThread: failed to decode RemoveCredentialRequest conn_id={} -- dropping",
                   conn_id.get_value());
        return;
    }

    const std::string comp_id(view.comp_id.data(), view.comp_id.size());

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "AuthenticationThread: RemoveCredentialRequest request_id={} comp_id={} conn_id={}",
               view.request_id, comp_id, conn_id.get_value());

    auto send_result = [&](pubsub_itc_fw_app::RemoveCredentialOutcome outcome) {
        pubsub_itc_fw_app::RemoveCredentialResult result{};
        result.request_id = view.request_id;
        result.comp_id = comp_id;
        result.outcome = outcome;

        static constexpr size_t result_buffer_size = 512;
        uint8_t result_buffer[result_buffer_size];
        size_t bytes_written = 0;
        size_t bytes_needed = 0;
        if (!pubsub_itc_fw_app::encode(result, result_buffer, result_buffer_size, bytes_written, bytes_needed)) {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Error, "AuthenticationThread: failed to encode RemoveCredentialResult conn_id={}",
                       conn_id.get_value());
            return;
        }
        send_admin_pdu(conn_id, pdu_id_remove_credential_result, result_buffer, static_cast<uint32_t>(bytes_written));
    };

    if (comp_id.empty()) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "AuthenticationThread: RemoveCredentialRequest empty comp_id -- InvalidInput");
        send_result(pubsub_itc_fw_app::RemoveCredentialOutcome::InvalidInput);
        return;
    }

    try {
        if (credentials_.erase(comp_id) > 0) {
            persist_credentials();
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "AuthenticationThread: RemoveCredentialRequest request_id={} comp_id={} -- Success",
                       view.request_id, comp_id);
            send_result(pubsub_itc_fw_app::RemoveCredentialOutcome::Success);
        } else {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "AuthenticationThread: RemoveCredentialRequest request_id={} comp_id={} -- NotFound",
                       view.request_id, comp_id);
            send_result(pubsub_itc_fw_app::RemoveCredentialOutcome::NotFound);
        }
    } catch (const std::exception& ex) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Error, "AuthenticationThread: RemoveCredentialRequest comp_id={} exception: {}", comp_id,
                   ex.what());
        send_result(pubsub_itc_fw_app::RemoveCredentialOutcome::InternalError);
    }
}

void AuthenticationThread::handle_restore_credential_request(const pubsub_itc_fw::ConnectionID& conn_id, const uint8_t* payload, uint32_t payload_size) {
    auto& arena_buffer = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buffer.data(), arena_buffer.size());
    arena.reset();

    pubsub_itc_fw_app::RestoreCredentialRequestView view{};
    size_t bytes_consumed = 0;
    size_t arena_bytes_needed = 0;

    if (!pubsub_itc_fw_app::decode(view, payload, payload_size, bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Error, "AuthenticationThread: failed to decode RestoreCredentialRequest conn_id={} -- dropping",
                   conn_id.get_value());
        return;
    }

    const std::string comp_id(view.comp_id.data(), view.comp_id.size());

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "AuthenticationThread: RestoreCredentialRequest request_id={} comp_id={} conn_id={}",
               view.request_id, comp_id, conn_id.get_value());

    auto send_result = [&](pubsub_itc_fw_app::RestoreCredentialOutcome outcome) {
        pubsub_itc_fw_app::RestoreCredentialResult result{};
        result.request_id = view.request_id;
        result.comp_id = comp_id;
        result.outcome = outcome;

        static constexpr size_t result_buffer_size = 512;
        uint8_t result_buffer[result_buffer_size];
        size_t bytes_written = 0;
        size_t bytes_needed = 0;
        if (!pubsub_itc_fw_app::encode(result, result_buffer, result_buffer_size, bytes_written, bytes_needed)) {
            PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Error, "AuthenticationThread: failed to encode RestoreCredentialResult conn_id={}",
                       conn_id.get_value());
            return;
        }
        send_admin_pdu(conn_id, pdu_id_restore_credential_result, result_buffer, static_cast<uint32_t>(bytes_written));
    };

    if (comp_id.empty()) {
        PUBSUB_LOG_STR(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "AuthenticationThread: RestoreCredentialRequest empty comp_id -- InvalidInput");
        send_result(pubsub_itc_fw_app::RestoreCredentialOutcome::InvalidInput);
        return;
    }
    if (view.stored_key.size != 32) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "AuthenticationThread: RestoreCredentialRequest stored_key size={} (expected 32) comp_id={} -- InvalidInput",
                   view.stored_key.size, comp_id);
        send_result(pubsub_itc_fw_app::RestoreCredentialOutcome::InvalidInput);
        return;
    }
    if (view.server_key.size != 32) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "AuthenticationThread: RestoreCredentialRequest server_key size={} (expected 32) comp_id={} -- InvalidInput",
                   view.server_key.size, comp_id);
        send_result(pubsub_itc_fw_app::RestoreCredentialOutcome::InvalidInput);
        return;
    }
    if (view.salt.size == 0) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning, "AuthenticationThread: RestoreCredentialRequest empty salt comp_id={} -- InvalidInput", comp_id);
        send_result(pubsub_itc_fw_app::RestoreCredentialOutcome::InvalidInput);
        return;
    }
    if (view.iterations < 1000 || view.iterations > 1000000) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "AuthenticationThread: RestoreCredentialRequest iterations={} out of range [1000,1000000] comp_id={} -- InvalidInput",
                   view.iterations, comp_id);
        send_result(pubsub_itc_fw_app::RestoreCredentialOutcome::InvalidInput);
        return;
    }

    try {
        scram_crypto::ScramCredential cred;
        cred.salt.assign(view.salt.data, view.salt.data + view.salt.size);
        cred.iterations = view.iterations;
        cred.stored_key.assign(view.stored_key.data, view.stored_key.data + view.stored_key.size);
        cred.server_key.assign(view.server_key.data, view.server_key.data + view.server_key.size);

        credentials_[comp_id] = std::move(cred);
        persist_credentials();

        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info, "AuthenticationThread: RestoreCredentialRequest request_id={} comp_id={} -- Success",
                   view.request_id, comp_id);
        send_result(pubsub_itc_fw_app::RestoreCredentialOutcome::Success);

    } catch (const std::exception& ex) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Error, "AuthenticationThread: RestoreCredentialRequest comp_id={} exception: {}", comp_id, ex.what());
        send_result(pubsub_itc_fw_app::RestoreCredentialOutcome::InternalError);
    }
}

void AuthenticationThread::send_admin_pdu(const pubsub_itc_fw::ConnectionID& conn_id, uint16_t pdu_id, const uint8_t* payload, uint32_t payload_size) {
    const size_t total_size = admin_pdu_header_size + payload_size;
    std::vector<uint8_t> buffer(total_size);
    uint8_t* h = buffer.data();

    h[0] = static_cast<uint8_t>((payload_size >> 24) & 0xFF);
    h[1] = static_cast<uint8_t>((payload_size >> 16) & 0xFF);
    h[2] = static_cast<uint8_t>((payload_size >> 8) & 0xFF);
    h[3] = static_cast<uint8_t>(payload_size & 0xFF);
    h[4] = static_cast<uint8_t>((pdu_id >> 8) & 0xFF);
    h[5] = static_cast<uint8_t>(pdu_id & 0xFF);
    h[6] = admin_pdu_version;
    h[7] = 0; // filler_a
    // seq_no (bytes 8-15): zero for admin PDUs
    h[8] = h[9] = h[10] = h[11] = h[12] = h[13] = h[14] = h[15] = 0;
    h[16] = static_cast<uint8_t>((admin_pdu_canary >> 24) & 0xFF);
    h[17] = static_cast<uint8_t>((admin_pdu_canary >> 16) & 0xFF);
    h[18] = static_cast<uint8_t>((admin_pdu_canary >> 8) & 0xFF);
    h[19] = static_cast<uint8_t>(admin_pdu_canary & 0xFF);
    h[20] = h[21] = h[22] = h[23] = 0; // filler_b

    if (payload_size > 0) {
        std::memcpy(h + admin_pdu_header_size, payload, payload_size);
    }

    send_raw(conn_id, buffer.data(), static_cast<uint32_t>(total_size));
}

void AuthenticationThread::persist_credentials() {
    const std::string tmp_path = config_.credentials_file + ".tmp";

    auto hex_encode = [](const std::vector<uint8_t>& bytes) -> std::string {
        static constexpr char hex_chars[] = "0123456789abcdef";
        std::string result;
        result.reserve(bytes.size() * 2);
        for (const uint8_t byte : bytes) {
            result += hex_chars[(byte >> 4) & 0xF];
            result += hex_chars[byte & 0xF];
        }
        return result;
    };

    {
        std::ofstream out(tmp_path);
        if (!out) {
            throw std::runtime_error("persist_credentials: failed to open " + tmp_path);
        }
        out << "# SCRAM-SHA-256 credentials managed by the authentication service.\n"
            << "# Do not edit manually while the service is running.\n\n";
        for (const auto& [comp_id, cred] : credentials_) {
            out << "[[credential]]\n"
                << "comp_id    = \"" << comp_id << "\"\n"
                << "stored_key = \"" << hex_encode(cred.stored_key) << "\"\n"
                << "server_key = \"" << hex_encode(cred.server_key) << "\"\n"
                << "salt       = \"" << hex_encode(cred.salt) << "\"\n"
                << "iterations = " << cred.iterations << "\n\n";
        }
    }

    if (std::rename(tmp_path.c_str(), config_.credentials_file.c_str()) != 0) {
        throw std::runtime_error("persist_credentials: rename failed for " + config_.credentials_file);
    }
}

} // namespace authentication_service
