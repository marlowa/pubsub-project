#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
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
 * @brief An application thread that subscribes to one topic, on each matching engine publisher of a pair, and keeps to
 *        the one that leads, while doing whatever other work its derived class gives it.
 *
 * At its initial event the thread connects to each publisher's service and, as each connection is made, asks it for the
 * topic's records after the last one already applied. Only the leading publisher answers with pages. The other replies
 * TopicNotLeader and keeps both the connection and the request, and answers it as soon as it becomes the leader, so the
 * thread waits on each connection and is served by whichever publisher leads, with no connection opened or closed while
 * the publishers keep their roles (docs/bug_list.md, BUG-0268). A publisher that stops leading closes its subscribers'
 * connections; the reactor connects to it again after its retry interval, and the thread asks again. A record already
 * applied is discarded by its sequence number, so records that arrive from both publishers around a change of leader are
 * delivered once.
 * Each fresh record is delivered through on_pubsub_message(), as a borrowed view valid only for that call; a subscriber
 * that keeps the bytes copies them.
 *
 * The thread claims only what is the topic's: the connections to the publishers' services, recognised by the service
 * name each carries when it is made and remembered from then on, and the messages that arrive on them. Everything else is passed to the virtual
 * functions below, on_started(), on_other_connection_established(), on_other_connection_lost() and
 * on_other_framework_pdu_message(), which a derived thread implements in place of the ApplicationThread functions they
 * stand for. A derived thread that overrides on_initial_event(), on_connection_established(), on_connection_lost() or
 * on_framework_pdu_message() itself calls this class's version from it, or the topic loses its connections and
 * messages. The messages are told apart by connection, not by message number, because the topic's numbers are used by
 * other protocols too: 107 and 108 are also the replicated WAL's LogPositionRequest and LogPositionReply.
 * on_connection_failed() is left to the derived thread: a failed attempt does not say which service it was for.
 *
 * With no publisher services the thread subscribes to nothing, and is an ordinary application thread.
 *
 * Header-only, like the other topic components (TopicSubscriberChannel, TopicPublisher).
 */
class TopicSubscriberThread : public ApplicationThread, public TopicSubscriberChannelHost {
  public:
    TopicSubscriberThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor, std::string thread_name, ThreadID thread_id,
                          const QueueConfiguration& queue_config, const AllocatorConfiguration& allocator_config,
                          const ApplicationThreadConfiguration& thread_config, std::vector<std::string> publisher_services, std::string subscriber_id,
                          std::string topic_name, int64_t from_seq_no, int ack_interval = TopicSubscriberChannel::default_ack_interval)
        : ApplicationThread(token, logger, reactor, std::move(thread_name), thread_id, queue_config, allocator_config, thread_config)
        , publisher_services_(std::move(publisher_services))
        , topic_name_(std::move(topic_name))
        , channel_(
              *this, std::move(subscriber_id), topic_name_, from_seq_no,
              [this](int64_t seq_no, int16_t pdu_id, const uint8_t* payload, size_t payload_size) { deliver_record(seq_no, pdu_id, payload, payload_size); },
              ack_interval) {}

    void topic_send_subscribe_request(ConnectionID connection_id, const pubsub_itc_fw::TopicSubscribeRequest& request) override {
        send_pdu(connection_id, pubsub_itc_fw::TopicSubscribeRequest::message_pdu_id, 0, request);
    }

    void topic_send_ack(ConnectionID connection_id, const pubsub_itc_fw::TopicAck& ack) override {
        send_pdu(connection_id, pubsub_itc_fw::TopicAck::message_pdu_id, 0, ack);
    }

  protected:
    /// The thread's first event, after it has asked to connect to the publishers.
    virtual void on_started() {}
    /// A connection made that is not to a publisher of the topic, including one another process opened to this one.
    virtual void on_other_connection_established([[maybe_unused]] ConnectionID id) {}
    /// A connection lost that is not to a publisher of the topic.
    virtual void on_other_connection_lost([[maybe_unused]] const ConnectionID& id, [[maybe_unused]] const std::string& reason) {}
    /// A framework message that arrived on a connection that is not to a publisher of the topic. The derived thread
    /// releases its payload, as for any framework message; the default does.
    virtual void on_other_framework_pdu_message(const EventMessage& message) {
        release_pdu_payload(message);
    }

    void on_initial_event() override {
        for (const std::string& service : publisher_services_) {
            connect_to_service(service);
        }
        on_started();
    }

    void on_connection_established(ConnectionID id) override {
        if (!is_publisher_service(id.service_name())) {
            on_other_connection_established(id);
            return;
        }
        publisher_connections_.push_back(id);
        PUBSUB_LOG(get_logger(), FwLogLevel::Debug, "TopicSubscriberThread: connected to '{}', asking for topic '{}' after seq={}", id.service_name(),
                   topic_name_, channel_.last_applied_seq_no());
        channel_.on_connected(id);
    }

    void on_connection_lost(const ConnectionID& id, const std::string& reason) override {
        if (!is_publisher_connection(id)) {
            on_other_connection_lost(id, reason);
            return;
        }
        // The connection a loss is reported on carries its number but not its service's name, so the name is taken from the
        // connection as it was made, before it is forgotten.
        const std::string service_name = service_name_of(id);
        publisher_connections_.erase(std::remove(publisher_connections_.begin(), publisher_connections_.end(), id), publisher_connections_.end());
        // Losing a publisher that was not sending pages costs this thread nothing; only the loss of the one that was is
        // worth a line at Info.
        if (id == serving_connection_) {
            serving_connection_ = ConnectionID{};
            PUBSUB_LOG(get_logger(), FwLogLevel::Info,
                       "TopicSubscriberThread: lost the connection to '{}', which was sending topic '{}' ({}), after seq={} -- following whichever publisher "
                       "leads",
                       service_name, topic_name_, reason, channel_.last_applied_seq_no());
        } else {
            PUBSUB_LOG(get_logger(), FwLogLevel::Debug, "TopicSubscriberThread: connection to '{}' for topic '{}' closed: {}", service_name, topic_name_,
                       reason);
        }
    }

    void on_framework_pdu_message(const EventMessage& message) override {
        if (!is_publisher_connection(message.connection_id())) {
            on_other_framework_pdu_message(message);
            return;
        }
        const uint8_t* payload = message.payload();
        const size_t size = static_cast<size_t>(message.payload_size());
        std::vector<uint8_t>& arena_buffer = decode_arena_buffer();
        BumpAllocator arena(arena_buffer.data(), arena_buffer.capacity());
        size_t consumed = 0;
        size_t arena_needed = 0;

        if (message.pdu_id() == pubsub_itc_fw::TopicSubscribeAck::message_pdu_id) {
            pubsub_itc_fw::TopicSubscribeAckView view{};
            if (pubsub_itc_fw::decode(view, payload, size, consumed, arena, arena_needed)) {
                channel_.on_subscribe_ack(view);
                serving_connection_ = message.connection_id();
                PUBSUB_LOG(get_logger(), FwLogLevel::Info, "TopicSubscriberThread: subscribed to topic '{}' on '{}', which leads, from seq={}", topic_name_,
                           service_name_of(message.connection_id()), view.accepted_from_seq_no);
            }
        } else if (message.pdu_id() == pubsub_itc_fw::TopicPage::message_pdu_id) {
            channel_.on_page_from(message.connection_id(), payload, size, arena);
        } else if (message.pdu_id() == pubsub_itc_fw::TopicNotLeader::message_pdu_id) {
            PUBSUB_LOG(get_logger(), FwLogLevel::Debug, "TopicSubscriberThread: '{}' does not lead, for topic '{}'", service_name_of(message.connection_id()),
                       topic_name_);
        }

        release_pdu_payload(message);
    }

    /// Messages from other threads of the process; a derived thread that has any overrides this.
    void on_itc_message([[maybe_unused]] const EventMessage& message) override {}

    [[nodiscard]] const std::string& topic_name() const {
        return topic_name_;
    }

    /// The sequence number of the last record delivered.
    [[nodiscard]] int64_t last_applied_seq_no() const {
        return channel_.last_applied_seq_no();
    }

  private:
    // The name of the publisher's service that @p id connects to, from the connection as it was made.
    [[nodiscard]] std::string service_name_of(const ConnectionID& id) const {
        const auto found = std::find(publisher_connections_.begin(), publisher_connections_.end(), id);
        return found == publisher_connections_.end() ? std::string{} : std::string(found->service_name());
    }

    [[nodiscard]] bool is_publisher_service(std::string_view service_name) const {
        return std::find(publisher_services_.begin(), publisher_services_.end(), service_name) != publisher_services_.end();
    }

    // Whether @p id is an open connection to a publisher. Recognised by the connection itself, recorded as it was made,
    // because the connection a framework message carries is identified by its number, not always with its service's name.
    [[nodiscard]] bool is_publisher_connection(const ConnectionID& id) const {
        return std::find(publisher_connections_.begin(), publisher_connections_.end(), id) != publisher_connections_.end();
    }

    void deliver_record(int64_t seq_no, int16_t pdu_id, const uint8_t* payload, size_t payload_size) {
        EventMessage event = EventMessage::create_pubsub_message(payload, static_cast<int>(payload_size), pdu_id, seq_no);
        on_pubsub_message(event);
    }

    std::vector<std::string> publisher_services_;
    // The open connections to the publishers, at most one to each.
    std::vector<ConnectionID> publisher_connections_;
    std::string topic_name_;
    TopicSubscriberChannel channel_;
    // The connection whose publisher accepted the subscription and is sending pages; none while no publisher has.
    ConnectionID serving_connection_;
};

} // namespaces
