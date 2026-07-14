#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <pubsub_itc_fw/AllocatorConfiguration.hpp>
#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/ApplicationThreadConfiguration.hpp>
#include <pubsub_itc_fw/BumpAllocator.hpp>
#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>
#include <pubsub_itc_fw/QueueConfiguration.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>
#include <pubsub_itc_fw/TopicSubscriberChannel.hpp>

#include <topics.hpp>

namespace pubsub_itc_fw {

class Reactor;

/**
 * @brief Pre-built subscriber base for the topic pub/sub protocol.
 *
 * Owns the whole subscriber boilerplate: connecting to the publisher, the subscribe
 * handshake, routing inbound topic PDUs, and dedup/acking via TopicSubscriberChannel.
 * Each fresh record is delivered to the application through on_pubsub_message(). A
 * concrete subscriber subclasses this and overrides on_pubsub_message() only; it never
 * handles PDUs or connections.
 *
 * The record delivered to on_pubsub_message() is a borrowed zero-copy view valid only
 * for the duration of that call (see EventMessage payload ownership); a subscriber that
 * needs to retain the bytes must copy them.
 *
 * Header-only, like the other topic components (TopicSubscriberChannel, TopicPublisher):
 * only the components that include the generated topics.hpp compile against it, so the
 * core framework library never depends on the generated header.
 */
class TopicSubscriberThread : public ApplicationThread, public TopicSubscriberChannelHost {
  public:
    TopicSubscriberThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor, std::string thread_name, ThreadID thread_id,
                          const QueueConfiguration& queue_config, const AllocatorConfiguration& allocator_config,
                          const ApplicationThreadConfiguration& thread_config, std::string service_name, std::string subscriber_id, std::string topic_name,
                          int64_t from_seq_no, int ack_interval = TopicSubscriberChannel::default_ack_interval)
        : ApplicationThread(token, logger, reactor, std::move(thread_name), thread_id, queue_config, allocator_config, thread_config)
        , service_name_(std::move(service_name))
        , topic_name_(std::move(topic_name))
        , channel_(
              *this, std::move(subscriber_id), topic_name_, from_seq_no,
              [this](int64_t seq_no, int16_t pdu_id, const uint8_t* payload, size_t payload_size) { deliver_record(seq_no, pdu_id, payload, payload_size); },
              ack_interval) {}

    void topic_send_subscribe_request(ConnectionID connection_id, const pubsub_itc_fw_app::TopicSubscribeRequest& request) override {
        send_pdu(connection_id, pubsub_itc_fw_app::TopicSubscribeRequest::message_pdu_id, 0, request);
    }

    void topic_send_ack(ConnectionID connection_id, const pubsub_itc_fw_app::TopicAck& ack) override {
        send_pdu(connection_id, pubsub_itc_fw_app::TopicAck::message_pdu_id, 0, ack);
    }

  protected:
    void on_initial_event() override {
        connect_to_service(service_name_);
    }

    void on_connection_established(ConnectionID id) override {
        channel_.on_connected(id);
    }

    void on_connection_failed(const std::string& reason) override {
        PUBSUB_LOG(get_logger(), FwLogLevel::Warning, "TopicSubscriberThread: connect to '{}' failed: {}", service_name_, reason);
    }

    void on_connection_lost(const ConnectionID&, const std::string& reason) override {
        PUBSUB_LOG(get_logger(), FwLogLevel::Warning, "TopicSubscriberThread: connection to '{}' lost: {}", service_name_, reason);
    }

    void on_framework_pdu_message(const EventMessage& message) override {
        const uint8_t* payload = message.payload();
        const size_t size = static_cast<size_t>(message.payload_size());
        std::vector<uint8_t>& arena_buffer = decode_arena_buffer();
        BumpAllocator arena(arena_buffer.data(), arena_buffer.capacity());
        size_t consumed = 0;
        size_t arena_needed = 0;

        if (message.pdu_id() == pubsub_itc_fw_app::TopicSubscribeAck::message_pdu_id) {
            pubsub_itc_fw_app::TopicSubscribeAckView view{};
            if (pubsub_itc_fw_app::decode(view, payload, size, consumed, arena, arena_needed)) {
                channel_.on_subscribe_ack(view);
            }
        } else if (message.pdu_id() == pubsub_itc_fw_app::TopicPage::message_pdu_id) {
            channel_.on_page(payload, size, arena);
        } else if (message.pdu_id() == pubsub_itc_fw_app::TopicNotLeader::message_pdu_id) {
            PUBSUB_LOG(get_logger(), FwLogLevel::Notice, "TopicSubscriberThread: '{}' endpoint is not the leader; awaiting failover", service_name_);
        }

        release_pdu_payload(message);
    }

    void on_itc_message(const EventMessage&) override {}

    [[nodiscard]] const std::string& topic_name() const {
        return topic_name_;
    }

  private:
    void deliver_record(int64_t seq_no, int16_t pdu_id, const uint8_t* payload, size_t payload_size) {
        EventMessage event = EventMessage::create_pubsub_message(payload, static_cast<int>(payload_size), pdu_id, seq_no);
        on_pubsub_message(event);
    }

    std::string service_name_;
    std::string topic_name_;
    TopicSubscriberChannel channel_;
};

} // namespaces
