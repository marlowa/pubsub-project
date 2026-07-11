#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <unordered_set>

#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/ExternalWalSubscriberRegistry.hpp>
#include <pubsub_itc_fw/WalReader.hpp>

#include <topics.hpp>

namespace pubsub_itc_fw {

/**
 * @brief What TopicPublisher needs its owning ApplicationThread to do.
 *
 * TopicPublisher is transport-agnostic: it decides *what* to send and *to whom*,
 * and defers the actual send/disconnect to its host. The owning ApplicationThread
 * implements these by forwarding to its (protected) send_pdu and the reactor. This
 * keeps TopicPublisher independent of ApplicationThread internals and lets it be
 * exercised from a plain test thread without going through the MEP application.
 */
class TopicPublisherHost {
  public:
    virtual ~TopicPublisherHost() = default;

    virtual void topic_send_subscribe_ack(ConnectionID connection_id, const pubsub_itc_fw_app::TopicSubscribeAck& ack) = 0;
    virtual void topic_send_page(ConnectionID connection_id, int64_t seq_no, const pubsub_itc_fw_app::TopicPage& page) = 0;
    virtual void topic_send_not_leader(ConnectionID connection_id, const pubsub_itc_fw_app::TopicNotLeader& not_leader) = 0;
    virtual void topic_disconnect(ConnectionID connection_id) = 0;
};

/**
 * @brief Reusable publisher side of the topic pub/sub protocol, for ONE topic.
 *
 * Lifted from MatchingEnginePublisherThread so the publish/subscribe machinery is
 * a component the MEP, TAP-side tooling, and integration tests all use, rather than
 * logic trapped inside one application. A publisher that serves several topics owns
 * one TopicPublisher per topic.
 *
 * Responsibilities: accept a TopicSubscribeRequest for its topic, register the
 * subscriber and reply with TopicSubscribeAck, replay matching WAL records from the
 * requested cursor, fan out live records (publish()), track per-subscriber cursors
 * (TopicAck), and drop subscribers on connection loss. Non-leaders reply
 * TopicNotLeader and disconnect.
 *
 * WAL record framing (on disk and as replayed): [wall_time_ns : int64][pdu_id : int16][payload...].
 *
 * Not thread-safe: all calls must come from the owning ApplicationThread.
 */
class TopicPublisher {
  public:
    // Returns true if a pdu id belongs to this publisher's topic.
    using MembershipPredicate = std::function<bool(int16_t pdu_id)>;

    static constexpr size_t wal_record_header_size = sizeof(int64_t) + sizeof(int16_t);

    TopicPublisher(TopicPublisherHost& host, std::string topic_name, MembershipPredicate is_member, std::string wal_directory)
        : host_(host), topic_name_(std::move(topic_name)), is_member_(std::move(is_member)), wal_directory_(std::move(wal_directory)) {}

    void set_leader(bool is_leader) {
        is_leader_ = is_leader;
    }

    [[nodiscard]] size_t subscriber_count() const {
        return live_connection_ids_.size();
    }

    /**
     * @brief Handle a TopicSubscribeRequest: register, ack, and replay.
     *
     * A non-leader replies TopicNotLeader and disconnects. A request for a
     * different topic is rejected by disconnect.
     */
    void on_subscribe_request(ConnectionID connection_id, const pubsub_itc_fw_app::TopicSubscribeRequestView& view) {
        if (!is_leader_) {
            pubsub_itc_fw_app::TopicNotLeader not_leader{};
            host_.topic_send_not_leader(connection_id, not_leader);
            host_.topic_disconnect(connection_id);
            return;
        }

        const std::string requested_topic(view.topic_name);
        if (requested_topic != topic_name_) {
            host_.topic_disconnect(connection_id);
            return;
        }

        const std::string subscriber_id(view.subscriber_id);
        const int64_t from_seq_no = view.from_seq_no;

        const ConnectionID orphan = registry_.register_subscriber(connection_id, subscriber_id, from_seq_no);
        if (orphan.is_valid()) {
            live_connection_ids_.erase(orphan);
            host_.topic_disconnect(orphan);
        }
        live_connection_ids_.insert(connection_id);

        // NB: the "-1 == from head" convention is deferred; the first test uses a
        // concrete cursor (0 == oldest). accepted_from_seq_no echoes the request.
        pubsub_itc_fw_app::TopicSubscribeAck ack{};
        ack.accepted_from_seq_no = from_seq_no;
        host_.topic_send_subscribe_ack(connection_id, ack);

        replay(connection_id, from_seq_no);
    }

    /// Advance a subscriber's cursor on TopicAck.
    void on_ack(ConnectionID connection_id, const pubsub_itc_fw_app::TopicAckView& view) {
        registry_.update_cursor(connection_id, view.last_seq_no);
    }

    /// Drop a subscriber when its connection is lost.
    void on_connection_lost(ConnectionID connection_id) {
        live_connection_ids_.erase(connection_id);
        registry_.remove_subscriber(connection_id);
    }

    /// Fan a freshly-sequenced WAL record out to live subscribers of this topic.
    void publish(int64_t seq_no, int16_t pdu_id, int64_t wall_time_ns, const uint8_t* payload, size_t payload_size) {
        if (!is_leader_ || !is_member_(pdu_id) || live_connection_ids_.empty()) {
            return;
        }
        for (const ConnectionID& connection_id : live_connection_ids_) {
            send_page(connection_id, seq_no, pdu_id, wall_time_ns, payload, payload_size);
        }
    }

  private:
    void send_page(ConnectionID connection_id, int64_t seq_no, int16_t pdu_id, int64_t wall_time_ns, const uint8_t* payload, size_t payload_size) {
        pubsub_itc_fw_app::TopicRecord record{};
        record.seq_no = seq_no;
        record.pdu_id = pdu_id;
        record.wall_time_ns = wall_time_ns;
        record.payload.data = payload;
        record.payload.size = payload_size;

        pubsub_itc_fw_app::TopicPage page{};
        page.record_count = 1;
        page.page_number = 1;
        page.total_pages = 1;
        page.records.data = &record;
        page.records.size = 1;

        host_.topic_send_page(connection_id, seq_no, page);
    }

    void replay(ConnectionID connection_id, int64_t from_seq_no) {
        [[maybe_unused]] auto end_position =
            WalReader::replay(wal_directory_, {0, 0}, [this, connection_id, from_seq_no](int64_t record_id, const void* data, size_t size) {
                if (record_id <= from_seq_no) {
                    return;
                }
                if (size < wal_record_header_size) {
                    return;
                }
                int64_t wall_time_ns{};
                int16_t pdu_id{};
                std::memcpy(&wall_time_ns, data, sizeof(int64_t));
                std::memcpy(&pdu_id, static_cast<const uint8_t*>(data) + sizeof(int64_t), sizeof(int16_t));
                if (!is_member_(pdu_id)) {
                    return;
                }
                const uint8_t* pdu_payload = static_cast<const uint8_t*>(data) + wal_record_header_size;
                send_page(connection_id, record_id, pdu_id, wall_time_ns, pdu_payload, size - wal_record_header_size);
            });
    }

    TopicPublisherHost& host_;
    std::string topic_name_;
    MembershipPredicate is_member_;
    std::string wal_directory_;
    bool is_leader_ = true;
    ExternalWalSubscriberRegistry registry_;
    std::unordered_set<ConnectionID> live_connection_ids_;
};

} // namespaces
