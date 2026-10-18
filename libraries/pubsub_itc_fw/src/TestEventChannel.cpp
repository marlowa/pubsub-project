// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <pubsub_itc_fw/TestEventChannel.hpp>

#include <utility>

#include <pubsub_itc_fw/BumpAllocator.hpp>

namespace pubsub_itc_fw {

TestEventChannel::TestEventChannel(TestEventLinksInterface& links, std::string component, int64_t instance, std::string thread)
    : links_(links), component_(std::move(component)), instance_(instance), thread_(std::move(thread)) {}

bool TestEventChannel::on_connection_established(const ConnectionID& connection) {
    if (connection.service_name() != service_name) {
        return false;
    }
    connection_ = connection;
    connected_ = true;
    TestEventsHello hello{};
    hello.component = component_;
    hello.instance = instance_;
    hello.thread = thread_;
    send(TestEventsHello::message_pdu_id, hello);
    return true;
}

bool TestEventChannel::on_connection_lost(const ConnectionID& connection) {
    if (!connected_ || connection != connection_) {
        return false;
    }
    connected_ = false;
    return true;
}

bool TestEventChannel::on_framework_pdu_message(const EventMessage& message) {
    if (!connected_ || message.connection_id() != connection_) {
        return false;
    }
    if (message.pdu_id() != TestCheckpointRequest::message_pdu_id) {
        return true; // nothing else is sent to a component on this connection
    }
    // The request holds one integer, so decoding it takes nothing from the arena; the decoder still needs one.
    std::array<uint8_t, 16> arena_storage{};
    BumpAllocator arena(arena_storage.data(), arena_storage.size());
    TestCheckpointRequestView request{};
    size_t consumed = 0;
    size_t arena_needed = 0;
    if (!decode(request, message.payload(), static_cast<size_t>(message.payload_size()), consumed, arena, arena_needed)) {
        return true; // the test sent something malformed; it fails for want of an answer
    }
    TestCheckpointReached reached{};
    reached.number = request.number;
    send(TestCheckpointReached::message_pdu_id, reached);
    return true;
}

void TestEventChannel::role_changed(std::string_view owner, int64_t instance, LeadershipRole from, LeadershipRole to, int32_t epoch) {
    if (!connected_) {
        return;
    }
    RoleChanged event{};
    event.owner = owner;
    event.instance = instance;
    event.from_role = from;
    event.to_role = to;
    event.epoch = epoch;
    send(RoleChanged::message_pdu_id, event);
}

template <typename Message> void TestEventChannel::send(int16_t pdu_id, const Message& message) {
    size_t written = 0;
    size_t needed = 0;
    // An event too large for the buffer is not sent, and the test, waiting for it, fails; no event comes close.
    if (!encode(message, buffer_.data(), buffer_.size(), written, needed)) {
        return;
    }
    links_.send_test_event(connection_, pdu_id, buffer_.data(), written);
}

} // namespaces
