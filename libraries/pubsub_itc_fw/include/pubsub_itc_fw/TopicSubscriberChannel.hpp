#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include <pubsub_itc_fw/BumpAllocator.hpp>
#include <pubsub_itc_fw/ConnectionID.hpp>

#include <topics.hpp>

namespace pubsub_itc_fw {

/**
 * @brief What TopicSubscriberChannel needs its owning ApplicationThread to do.
 *
 * As with TopicPublisherHost, the channel decides *what* to send and defers the
 * actual send to its host, so it can be driven from a plain test thread as well as
 * from a real subscriber application (TAP, later the market-data programs).
 */
class TopicSubscriberChannelHost {
  public:
    virtual ~TopicSubscriberChannelHost() = default;

    virtual void topic_send_subscribe_request(ConnectionID connection_id, const pubsub_itc_fw_app::TopicSubscribeRequest& request) = 0;
    virtual void topic_send_ack(ConnectionID connection_id, const pubsub_itc_fw_app::TopicAck& ack) = 0;
};

/**
 * @brief Reusable subscriber side of the topic pub/sub protocol.
 *
 * The client-side component named in topics.dsl. Owns the subscribe handshake,
 * receipt of TopicPage records, dedup by seq_no, delivery of fresh records to the
 * application, and acking. Since seq_no is a globally authoritative total order
 * (records are keyed by the sequencer's seq_no), dedup is trivial: any record whose
 * seq_no is at or below the last one already applied is discarded. This is what
 * makes at-least-once delivery behave as exactly-once for the application across a
 * reconnect or failover seam, with no coordination on the publisher side.
 *
 * Not thread-safe: all calls must come from the owning ApplicationThread.
 */
class TopicSubscriberChannel {
  public:
    // Delivered for each fresh (non-duplicate) record: (seq_no, pdu_id, payload, size).
    using RecordSink = std::function<void(int64_t seq_no, int16_t pdu_id, const uint8_t* payload, size_t payload_size)>;

    // Records applied between TopicAcks. The ack is a coarse truncation cursor (F2),
    // not a per-page or flow-control signal, so it is sent every N records, not per page.
    static constexpr int default_ack_interval = 8;

    TopicSubscriberChannel(TopicSubscriberChannelHost& host, std::string subscriber_id, std::string topic_name, int64_t from_seq_no, RecordSink sink,
                           int ack_interval = default_ack_interval)
        : host_(host)
        , subscriber_id_(std::move(subscriber_id))
        , topic_name_(std::move(topic_name))
        , from_seq_no_(from_seq_no)
        , last_applied_seq_no_(from_seq_no)
        , sink_(std::move(sink))
        , ack_interval_(ack_interval) {}

    [[nodiscard]] bool subscribe_ack_received() const {
        return subscribe_ack_received_;
    }
    [[nodiscard]] int64_t accepted_from_seq_no() const {
        return accepted_from_seq_no_;
    }
    [[nodiscard]] int64_t last_applied_seq_no() const {
        return last_applied_seq_no_;
    }

    /// Send the TopicSubscribeRequest once the connection is up.
    void on_connected(ConnectionID connection_id) {
        connection_id_ = connection_id;
        pubsub_itc_fw_app::TopicSubscribeRequest request{};
        request.subscriber_id = subscriber_id_;
        request.topic_name = topic_name_;
        request.from_seq_no = from_seq_no_;
        request.role = pubsub_itc_fw_app::TopicChannelRole::Data; // control channel arrives in a later step
        host_.topic_send_subscribe_request(connection_id, request);
    }

    void on_subscribe_ack(const pubsub_itc_fw_app::TopicSubscribeAckView& view) {
        accepted_from_seq_no_ = view.accepted_from_seq_no;
        subscribe_ack_received_ = true;
    }

    /**
     * @brief Decode a TopicPage, deliver fresh records, and periodically ack.
     *
     * The TopicAck is a coarse truncation cursor (F2): it is sent once every
     * ack_interval fresh records, not per page, and does not pace delivery.
     *
     * @param payload  the TopicPage PDU payload bytes.
     * @param size     length of @p payload.
     * @param arena    decode arena for the page's record list.
     */
    void on_page(const uint8_t* payload, size_t size, BumpAllocator& arena) {
        pubsub_itc_fw_app::TopicPageView page{};
        size_t consumed = 0;
        size_t arena_needed = 0;
        if (!pubsub_itc_fw_app::decode(page, payload, size, consumed, arena, arena_needed)) {
            return;
        }

        for (size_t index = 0; index < page.records.size; ++index) {
            const auto& record = page.records.data[index];
            if (record.seq_no <= last_applied_seq_no_) {
                continue; // dedup by seq_no: already applied
            }
            sink_(record.seq_no, record.pdu_id, record.payload.data, record.payload.size);
            last_applied_seq_no_ = record.seq_no;
            ++records_since_ack_;
        }

        if (records_since_ack_ >= ack_interval_) {
            pubsub_itc_fw_app::TopicAck ack{};
            ack.last_seq_no = last_applied_seq_no_;
            host_.topic_send_ack(connection_id_, ack);
            records_since_ack_ = 0;
        }
    }

  private:
    TopicSubscriberChannelHost& host_;
    std::string subscriber_id_;
    std::string topic_name_;
    int64_t from_seq_no_;
    int64_t last_applied_seq_no_;
    int64_t accepted_from_seq_no_ = -1;
    bool subscribe_ack_received_ = false;
    RecordSink sink_;
    int ack_interval_;
    int records_since_ack_ = 0;
    ConnectionID connection_id_;
};

} // namespaces
