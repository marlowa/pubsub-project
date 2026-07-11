#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/ExternalWalSubscriberRegistry.hpp>
#include <pubsub_itc_fw/WalCursor.hpp>
#include <pubsub_itc_fw/WalPosition.hpp>

#include <topics.hpp>

namespace pubsub_itc_fw {

/**
 * @brief What TopicPublisher needs its owning ApplicationThread to do.
 *
 * TopicPublisher is transport-agnostic: it decides *what* to send and *to whom*,
 * and defers the actual send/disconnect/pace to its host. The owning
 * ApplicationThread implements these by forwarding to its (protected) send_pdu,
 * request_writable_notification, and the reactor. This keeps TopicPublisher
 * independent of ApplicationThread internals and lets it be exercised from a plain
 * test thread without going through the MEP application.
 */
class TopicPublisherHost {
  public:
    virtual ~TopicPublisherHost() = default;

    virtual void topic_send_subscribe_ack(ConnectionID connection_id, const pubsub_itc_fw_app::TopicSubscribeAck& ack) = 0;
    virtual void topic_send_page(ConnectionID connection_id, int64_t seq_no, const pubsub_itc_fw_app::TopicPage& page) = 0;
    virtual void topic_send_not_leader(ConnectionID connection_id, const pubsub_itc_fw_app::TopicNotLeader& not_leader) = 0;
    virtual void topic_disconnect(ConnectionID connection_id) = 0;

    /// Arm a one-shot on_connection_writable() for this connection (send pacing).
    virtual void topic_request_writable_notification(ConnectionID connection_id) = 0;
};

/**
 * @brief Reusable publisher side of the topic pub/sub protocol, for ONE topic.
 *
 * Lifted from MatchingEnginePublisherThread so the publish/subscribe machinery is
 * a component the MEP, TAP-side tooling, and integration tests all use, rather than
 * logic trapped inside one application. A publisher that serves several topics owns
 * one TopicPublisher per topic.
 *
 * Delivery is streamed straight from the WAL and paced by the TCP socket, not by
 * acks (see docs/design/pubsub_flow_control.md). Each subscriber has its own
 * WalCursor. On subscribe -- and again each time the socket becomes writable
 * (on_connection_writable) -- the publisher reads the next matching record after
 * the subscriber's position and sends it as a one-record TopicPage, then re-arms
 * the writable notification. When the subscriber catches up to the WAL head it goes
 * idle; a later notify_record_appended() wakes it to stream the new records. The
 * backlog for a slow subscriber therefore lives in the WAL (a cursor position), not
 * in memory.
 *
 * Both catch-up (history) and live records flow through the same cursor path, so a
 * live record must be in the WAL *before* notify_record_appended() is called.
 *
 * WAL record framing (on disk): [wall_time_ns : int64][pdu_id : int16][payload...].
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
        return subscribers_.size();
    }

    /**
     * @brief Handle a TopicSubscribeRequest: register, ack, and start streaming.
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
            subscribers_.erase(orphan);
            host_.topic_disconnect(orphan);
        }

        auto stream = std::make_unique<SubscriberStream>();
        stream->from_seq_no = from_seq_no;
        stream->cursor.open(wal_directory_, WalPosition{0, 0});
        subscribers_[connection_id] = std::move(stream);

        // NB: the "-1 == from head" convention is deferred; the tests use a concrete
        // cursor (0 == oldest). accepted_from_seq_no echoes the request.
        pubsub_itc_fw_app::TopicSubscribeAck ack{};
        ack.accepted_from_seq_no = from_seq_no;
        host_.topic_send_subscribe_ack(connection_id, ack);

        pump(connection_id); // send the first record (if any) and arm
    }

    /// Advance a subscriber's ack cursor (for WAL truncation) on TopicAck.
    void on_ack(ConnectionID connection_id, const pubsub_itc_fw_app::TopicAckView& view) {
        registry_.update_cursor(connection_id, view.last_seq_no);
    }

    /// Drop a subscriber when its connection is lost.
    void on_connection_lost(ConnectionID connection_id) {
        subscribers_.erase(connection_id);
        registry_.remove_subscriber(connection_id);
    }

    /// The socket can accept another frame: stream the subscriber's next record.
    void on_connection_writable(ConnectionID connection_id) {
        pump(connection_id);
    }

    /**
     * @brief A record of pdu_id was appended to the WAL: wake idle subscribers.
     *
     * The record must already be in the WAL. Idle (caught-up) subscribers are
     * pumped so their cursor reads and streams it; subscribers still mid-stream
     * pick it up when they reach the head and re-scan.
     */
    void notify_record_appended(int16_t pdu_id) {
        if (!is_leader_ || !is_member_(pdu_id)) {
            return;
        }
        for (auto& entry : subscribers_) {
            if (entry.second->idle) {
                pump(entry.first);
            }
        }
    }

  private:
    struct SubscriberStream {
        WalCursor cursor;
        int64_t from_seq_no = 0; // records with record_id <= this are skipped
        bool idle = true;        // true == caught up to the WAL head, waiting for a wakeup
    };

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

    // Send at most one record to a subscriber and re-arm its writable notification;
    // or mark it idle if it has caught up to the WAL head.
    void pump(ConnectionID connection_id) {
        auto it = subscribers_.find(connection_id);
        if (it == subscribers_.end()) {
            return;
        }
        SubscriberStream& stream = *it->second;

        bool reopened = false;
        int64_t record_id = 0;
        const uint8_t* data = nullptr;
        size_t size = 0;
        for (;;) {
            if (!stream.cursor.read_next(record_id, data, size)) {
                if (!reopened) {
                    // A record may have been appended since the cursor's snapshot;
                    // re-discover from the current position before declaring idle.
                    stream.cursor.open(wal_directory_, stream.cursor.position());
                    reopened = true;
                    continue;
                }
                stream.idle = true;
                return;
            }
            reopened = false;

            if (size < wal_record_header_size) {
                continue; // malformed record; skip
            }
            int64_t wall_time_ns = 0;
            int16_t pdu_id = 0;
            std::memcpy(&wall_time_ns, data, sizeof(int64_t));
            std::memcpy(&pdu_id, data + sizeof(int64_t), sizeof(int16_t));
            if (record_id <= stream.from_seq_no || !is_member_(pdu_id)) {
                continue; // already past, or not this topic; skip
            }

            const uint8_t* pdu_payload = data + wal_record_header_size;
            send_page(connection_id, record_id, pdu_id, wall_time_ns, pdu_payload, size - wal_record_header_size);
            host_.topic_request_writable_notification(connection_id);
            stream.idle = false;
            return;
        }
    }

    TopicPublisherHost& host_;
    std::string topic_name_;
    MembershipPredicate is_member_;
    std::string wal_directory_;
    bool is_leader_ = true;
    ExternalWalSubscriberRegistry registry_;
    std::unordered_map<ConnectionID, std::unique_ptr<SubscriberStream>> subscribers_;
};

} // namespaces
