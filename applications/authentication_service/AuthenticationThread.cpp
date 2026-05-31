// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "AuthenticationThread.hpp"

#include <cstdint>
#include <string>

#include <pubsub_itc_fw/AllocatorConfiguration.hpp>
#include <pubsub_itc_fw/ApplicationThreadConfiguration.hpp>
#include <pubsub_itc_fw/BumpAllocator.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>
#include <pubsub_itc_fw/QueueConfiguration.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>

#include <authentication.hpp>

namespace authentication_service {

namespace {

// PDU identifiers as declared in authentication.dsl.
static constexpr int16_t pdu_id_authentication_request   = 500;
static constexpr int16_t pdu_id_authentication_challenge = 501;
static constexpr int16_t pdu_id_authentication_proof     = 502;
static constexpr int16_t pdu_id_authentication_result    = 503;

// Stub SCRAM parameters — replace with per-account stored values in production.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays)
static const uint8_t stub_salt[] = {
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10
};
static constexpr int32_t stub_iterations = 4096;

// Fixed suffix appended to the client nonce to form the server nonce.
// In production, replace with a cryptographically random per-exchange value.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays)
static const uint8_t server_nonce_suffix[] = {'s', 'e', 'r', 'v', 'i', 'c', 'e'};

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
        PUBSUB_LOG(logger, pubsub_itc_fw::FwLogLevel::Warning, "AuthenticationPool exhausted: chaining new pool slab ({} objects)", objects_per_pool);
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
    , config_(config) {}

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
                   "AuthenticationThread: unexpected pdu_id={} conn_id={} -- dropping", pdu_id, conn_id.get_value());
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
                   "AuthenticationThread: failed to decode AuthenticationRequest conn_id={} -- dropping", conn_id.get_value());
        return;
    }

    const std::string comp_id(view.comp_id.data(), view.comp_id.size());
    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "AuthenticationThread: AuthenticationRequest request_id={} comp_id={} conn_id={}",
               view.request_id, comp_id, conn_id.get_value());

    // Build server_nonce = client_nonce || fixed suffix.
    // TODO: replace the suffix with a cryptographically random per-exchange value.
    std::vector<uint8_t> server_nonce(view.client_nonce.data, view.client_nonce.data + view.client_nonce.size);
    server_nonce.insert(server_nonce.end(), std::begin(server_nonce_suffix), std::end(server_nonce_suffix));

    ExchangeState& state = exchanges_[conn_id];
    state.request_id = view.request_id;
    state.comp_id = comp_id;
    state.server_nonce = server_nonce;

    pubsub_itc_fw_app::AuthenticationChallenge challenge{};
    challenge.request_id = view.request_id;
    challenge.server_nonce = pubsub_itc_fw_app::BytesView{server_nonce.data(), server_nonce.size()};
    challenge.salt = pubsub_itc_fw_app::BytesView{stub_salt, sizeof(stub_salt)};
    challenge.iterations = stub_iterations;
    send_pdu(conn_id, pdu_id_authentication_challenge, 0, challenge);

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "AuthenticationThread: AuthenticationChallenge sent request_id={} comp_id={}", view.request_id, comp_id);
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
                   "AuthenticationThread: failed to decode AuthenticationProof conn_id={} -- dropping", conn_id.get_value());
        return;
    }

    auto it = exchanges_.find(conn_id);
    if (it == exchanges_.end()) {
        PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Warning,
                   "AuthenticationThread: AuthenticationProof with no pending exchange conn_id={} request_id={} -- dropping",
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

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "AuthenticationThread: AuthenticationProof received request_id={} comp_id={} conn_id={}",
               view.request_id, state.comp_id, conn_id.get_value());

    // TODO: verify view.client_proof against the SCRAM-SHA-256 StoredKey and
    // ServerKey derived from the per-account stored salt, iteration count, and
    // salted password hash. For the skeleton, every proof is accepted.

    pubsub_itc_fw_app::AuthenticationResult result{};
    result.request_id = view.request_id;
    result.outcome = pubsub_itc_fw_app::AuthenticationOutcome::Granted;
    result.server_signature = pubsub_itc_fw_app::BytesView{nullptr, 0}; // TODO: compute SCRAM ServerSignature
    result.force_password_change = false;
    send_pdu(conn_id, pdu_id_authentication_result, 0, result);

    PUBSUB_LOG(get_logger(), pubsub_itc_fw::FwLogLevel::Info,
               "AuthenticationThread: AuthenticationResult Granted request_id={} comp_id={} conn_id={}",
               view.request_id, state.comp_id, conn_id.get_value());

    exchanges_.erase(it);
}

} // namespace authentication_service
