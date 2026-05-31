// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <openssl/rand.h>

#include <AuthenticationThread.hpp>

#include <cstdint>
#include <string>

#include <pubsub_itc_fw/AllocatorConfiguration.hpp>
#include <pubsub_itc_fw/ApplicationThreadConfiguration.hpp>
#include <pubsub_itc_fw/BumpAllocator.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>
#include <pubsub_itc_fw/QueueConfiguration.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>

#include <ScramCrypto.hpp>
#include <authentication.hpp>

namespace authentication_service {

namespace {

static constexpr int16_t pdu_id_authentication_request   = 500;
static constexpr int16_t pdu_id_authentication_challenge = 501;
static constexpr int16_t pdu_id_authentication_proof     = 502;
static constexpr int16_t pdu_id_authentication_result    = 503;

// Stub salt and iteration count.  In production these are stored per account.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays)
static const uint8_t stub_salt[] = {
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10
};
static constexpr int32_t stub_iterations = 4096;

// Stub password used to derive the stub credential at startup.
// Replace with a real per-account credential store in production.
static constexpr std::string_view stub_password = "stubpassword";

pubsub_itc_fw::QueueConfiguration make_queue_config() {
    pubsub_itc_fw::QueueConfiguration queue_configuration{};
    queue_configuration.low_watermark = 1;
    queue_configuration.high_watermark = 64;
    return queue_configuration;
}

pubsub_itc_fw::AllocatorConfiguration make_allocator_config(const AuthenticationServiceConfiguration& config,
                                                             pubsub_itc_fw::QuillLogger& logger) {
    pubsub_itc_fw::AllocatorConfiguration allocator_configuration{};
    allocator_configuration.pool_name = "AuthenticationPool";
    allocator_configuration.objects_per_pool = config.event_queue_pool_objects_per_slab;
    allocator_configuration.initial_pools = config.event_queue_pool_initial_slabs;
    allocator_configuration.handler_for_pool_exhausted = [&logger](void* /*context*/, int objects_per_pool) {
        PUBSUB_LOG(logger, pubsub_itc_fw::FwLogLevel::Warning,
                   "AuthenticationPool exhausted: chaining new pool slab ({} objects)", objects_per_pool);
    };
    return allocator_configuration;
}

} // namespace

AuthenticationThread::AuthenticationThread(pubsub_itc_fw::ApplicationThread::ConstructorToken token,
                                           pubsub_itc_fw::QuillLogger& logger,
                                           pubsub_itc_fw::Reactor& reactor,
                                           const AuthenticationServiceConfiguration& config)
    : ApplicationThread(token, logger, reactor, "AuthenticationThread", pubsub_itc_fw::ThreadID{1},
                        make_queue_config(), make_allocator_config(config, logger),
                        pubsub_itc_fw::ApplicationThreadConfiguration{})
    , config_(config)
    , stub_credential_(scram_crypto::make_scram_credential(stub_password, stub_salt, sizeof(stub_salt), stub_iterations)) {}

void AuthenticationThread::on_connection_established(pubsub_itc_fw::ConnectionID id) {
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "AuthenticationThread: connection established conn_id={}", id.get_value());
}

void AuthenticationThread::on_connection_lost(pubsub_itc_fw::ConnectionID id, const std::string& reason) {
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "AuthenticationThread: connection lost conn_id={} reason={}", id.get_value(), reason);
    exchanges_.erase(id);
}

void AuthenticationThread::on_framework_pdu_message(const pubsub_itc_fw::EventMessage& msg) {
    const auto pdu_id = static_cast<int16_t>(msg.pdu_id());
    const pubsub_itc_fw::ConnectionID conn_id = msg.connection_id();

    if (pdu_id == pdu_id_authentication_request) {
        handle_authentication_request(conn_id, msg);
    } else if (pdu_id == pdu_id_authentication_proof) {
        handle_authentication_proof(conn_id, msg);
    } else {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "AuthenticationThread: unexpected pdu_id={} conn_id={} -- dropping",
                   pdu_id, conn_id.get_value());
    }

    release_pdu_payload(msg);
}

void AuthenticationThread::on_itc_message([[maybe_unused]] const pubsub_itc_fw::EventMessage& msg) {}

void AuthenticationThread::handle_authentication_request(pubsub_itc_fw::ConnectionID conn_id,
                                                          const pubsub_itc_fw::EventMessage& msg) {
    auto& arena_buffer = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buffer.data(), arena_buffer.size());
    arena.reset();

    pubsub_itc_fw_app::AuthenticationRequestView view{};
    size_t bytes_consumed = 0;
    size_t arena_bytes_needed = 0;

    if (!pubsub_itc_fw_app::decode(view, msg.payload(), static_cast<size_t>(msg.payload_size()),
                                    bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Error,
                   "AuthenticationThread: failed to decode AuthenticationRequest conn_id={} -- dropping",
                   conn_id.get_value());
        return;
    }

    const std::string comp_id(view.comp_id.data(), view.comp_id.size());
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "AuthenticationThread: AuthenticationRequest request_id={} comp_id={} conn_id={}",
               view.request_id, comp_id, conn_id.get_value());

    // Build server_nonce = client_nonce || 16 cryptographically random bytes.
    std::vector<uint8_t> client_nonce(view.client_nonce.data,
                                      view.client_nonce.data + view.client_nonce.size);
    std::vector<uint8_t> server_nonce = client_nonce;
    uint8_t random_suffix[16];
    if (RAND_bytes(random_suffix, static_cast<int>(sizeof(random_suffix))) != 1) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Error,
                   "AuthenticationThread: RAND_bytes failed conn_id={} -- dropping", conn_id.get_value());
        return;
    }
    server_nonce.insert(server_nonce.end(), random_suffix, random_suffix + sizeof(random_suffix));

    // Stub credential lookup: all comp_ids map to the same pre-derived credential.
    // Replace with a real per-account lookup in production.
    ExchangeState& state = exchanges_[conn_id];
    state.request_id  = view.request_id;
    state.comp_id     = comp_id;
    state.client_nonce = client_nonce;
    state.server_nonce = server_nonce;
    state.salt        = stub_credential_.salt;
    state.iterations  = stub_credential_.iterations;
    state.stored_key  = stub_credential_.stored_key;
    state.server_key  = stub_credential_.server_key;

    pubsub_itc_fw_app::AuthenticationChallenge challenge{};
    challenge.request_id  = view.request_id;
    challenge.server_nonce = pubsub_itc_fw_app::BytesView{server_nonce.data(), server_nonce.size()};
    challenge.salt        = pubsub_itc_fw_app::BytesView{state.salt.data(), state.salt.size()};
    challenge.iterations  = state.iterations;
    send_pdu(conn_id, pdu_id_authentication_challenge, 0, challenge);

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "AuthenticationThread: AuthenticationChallenge sent request_id={} comp_id={}",
               view.request_id, comp_id);
}

void AuthenticationThread::handle_authentication_proof(pubsub_itc_fw::ConnectionID conn_id,
                                                        const pubsub_itc_fw::EventMessage& msg) {
    auto& arena_buffer = decode_arena_buffer();
    pubsub_itc_fw::BumpAllocator arena(arena_buffer.data(), arena_buffer.size());
    arena.reset();

    pubsub_itc_fw_app::AuthenticationProofView view{};
    size_t bytes_consumed = 0;
    size_t arena_bytes_needed = 0;

    if (!pubsub_itc_fw_app::decode(view, msg.payload(), static_cast<size_t>(msg.payload_size()),
                                    bytes_consumed, arena, arena_bytes_needed)) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Error,
                   "AuthenticationThread: failed to decode AuthenticationProof conn_id={} -- dropping",
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
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "AuthenticationThread: request_id mismatch expected={} got={} conn_id={} -- dropping",
                   state.request_id, view.request_id, conn_id.get_value());
        return;
    }

    auto send_result = [&](pubsub_itc_fw_app::AuthenticationOutcome outcome,
                           const std::vector<uint8_t>& server_signature) {
        pubsub_itc_fw_app::AuthenticationResult result{};
        result.request_id = view.request_id;
        result.outcome = outcome;
        result.server_signature = pubsub_itc_fw_app::BytesView{
            server_signature.empty() ? nullptr : server_signature.data(),
            server_signature.size()
        };
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
                   view.client_proof.size, sha256_size,
                   view.request_id, state.comp_id, conn_id.get_value());
        exchanges_.erase(it);
        send_result(pubsub_itc_fw_app::AuthenticationOutcome::BadPassword, {});
        return;
    }

    const std::vector<uint8_t> auth_message = scram_crypto::compute_auth_message(
        state.comp_id, state.client_nonce, state.server_nonce,
        state.salt.data(), state.salt.size(), state.iterations);

    const std::vector<uint8_t> client_signature = scram_crypto::hmac_sha256(
        state.stored_key.data(), state.stored_key.size(),
        auth_message.data(), auth_message.size());

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
    const std::vector<uint8_t> server_signature = scram_crypto::hmac_sha256(
        state.server_key.data(), state.server_key.size(),
        auth_message.data(), auth_message.size());

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "AuthenticationThread: AuthenticationResult Granted request_id={} comp_id={} conn_id={}",
               view.request_id, state.comp_id, conn_id.get_value());

    exchanges_.erase(it);
    send_result(pubsub_itc_fw_app::AuthenticationOutcome::Granted, server_signature);
}

} // namespace authentication_service
