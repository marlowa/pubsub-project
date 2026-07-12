#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

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
 * request_writable_notification, and the reactor.
 */
class TopicPublisherHost {
  public:
    virtual ~TopicPublisherHost() = default;

    virtual void topic_send_subscribe_ack(ConnectionID connection_id, const pubsub_itc_fw_app::TopicSubscribeAck& ack) = 0;
    virtual void topic_send_page(ConnectionID connection_id, int64_t seq_no, const pubsub_itc_fw_app::TopicPage& page) = 0;
    virtual void topic_send_not_leader(ConnectionID connection_id, const pubsub_itc_fw_app::TopicNotLeader& not_leader) = 0;
    virtual void topic_send_lagged(ConnectionID control_connection_id, const pubsub_itc_fw_app::TopicLagged& lagged) = 0;
    virtual void topic_disconnect(ConnectionID connection_id) = 0;

    /// Arm a one-shot on_connection_writable() for this connection (send pacing).
    virtual void topic_request_writable_notification(ConnectionID connection_id) = 0;

    /// Reclaim WAL disk below safe_seq_no (records every subscriber has consumed).
    virtual void topic_truncate_wal(int64_t safe_seq_no) = 0;
};

/**
 * @brief Reusable publisher side of the topic pub/sub protocol, for ONE topic.
 *
 * Each subscriber opens two connections to the publisher's single port: a **data**
 * channel (bulk record delivery) and a **control** channel (rare out-of-band
 * signals). Each connection is classified by the role field on its
 * TopicSubscribeRequest and correlated to the other by subscriber_id. The two
 * connections are one logical subscription: if either drops, the pair is torn down.
 * (The control channel is optional here -- a data-only subscriber still streams; a
 * stricter "control required" policy could be added later.)
 *
 * Data delivery is streamed straight from the WAL and paced by the TCP socket, not
 * by acks (see docs/design/pubsub_flow_control.md): each subscriber has its own
 * WalCursor and receives one TopicPage per on_connection_writable(). A page batches
 * up to max_records_per_page records (bounded also by an encoded-payload budget that
 * stays well under the outbound slab / decode arena), which amortises the epoll
 * writable round-trip over many records -- the dominant throughput cost. When only
 * one record is available (the live 1-at-a-time case) a page simply carries one, so
 * batching never adds latency. Catch-up and live records flow through the same cursor
 * path, so a live record must be in the WAL before notify_record_appended(). The
 * backlog for a slow subscriber lives in the WAL (a cursor position), not in memory.
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

    // A page batches at most this many records (also bounded by max_batch_payload_bytes).
    static constexpr size_t default_max_records_per_page = 256;
    // ...and at most this many payload bytes, kept well under the 64 KiB outbound slab /
    // decode arena so a batched page can never overflow either.
    static constexpr size_t max_batch_payload_bytes = 32UL * 1024UL;

    // max_lag_records is the per-topic retention window (F3/R3 policy): the publisher
    // serves at most this many records behind the WAL head, and a subscriber whose ack
    // cursor falls out of the window is dropped with a TopicLagged. 0 disables the
    // policy (unbounded retention; no subscriber is ever dropped for lag).
    // max_records_per_page caps how many records a single TopicPage carries (1 restores
    // the old one-record-per-writable behaviour).
    TopicPublisher(TopicPublisherHost& host, std::string topic_name, MembershipPredicate is_member, std::string wal_directory, int64_t max_lag_records = 0,
                   size_t max_records_per_page = default_max_records_per_page)
        : host_(host)
        , topic_name_(std::move(topic_name))
        , is_member_(std::move(is_member))
        , wal_directory_(std::move(wal_directory))
        , max_lag_records_(max_lag_records)
        , max_records_per_page_(max_records_per_page == 0 ? 1 : max_records_per_page) {}

    void set_leader(bool is_leader) {
        is_leader_ = is_leader;
    }

    /// Number of logical subscribers (correlated data+control pairs, keyed by id).
    [[nodiscard]] size_t subscriber_count() const {
        return subscribers_.size();
    }

    /// Tear down every logical subscriber (disconnecting both connections of each) and
    /// forget all subscriber state. Used on loss of leadership so subscribers reconnect
    /// and rediscover the new leader. The WAL head and topic identity are unaffected.
    void drop_all_subscribers() {
        for (auto& entry : subscribers_) {
            const ConnectionID data_connection_id = entry.second.data_connection_id;
            const ConnectionID control_connection_id = entry.second.control_connection_id;
            if (data_connection_id.is_valid()) {
                host_.topic_disconnect(data_connection_id);
            }
            if (control_connection_id.is_valid()) {
                host_.topic_disconnect(control_connection_id);
            }
        }
        subscribers_.clear();
        connection_to_subscriber_id_.clear();
        registry_ = ExternalWalSubscriberRegistry{};
        last_truncation_floor_ = 0;
    }

    /**
     * @brief Handle a TopicSubscribeRequest on the data or control channel.
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
        if (std::string(view.topic_name) != topic_name_) {
            host_.topic_disconnect(connection_id);
            return;
        }

        const std::string subscriber_id(view.subscriber_id);
        if (view.role == pubsub_itc_fw_app::TopicChannelRole::Control) {
            handle_control_subscribe(connection_id, subscriber_id);
        } else {
            handle_data_subscribe(connection_id, subscriber_id, view.from_seq_no);
        }
    }

    /// Advance a subscriber's ack cursor (for WAL truncation + lag) on TopicAck (data channel).
    /// A no-op for a connection this publisher does not own, so a host serving several
    /// publishers (e.g. the MEP, one publisher per topic over a shared WAL) may fan an ack
    /// to all of them and let the owning one act.
    void on_ack(ConnectionID connection_id, const pubsub_itc_fw_app::TopicAckView& view) {
        auto conn_it = connection_to_subscriber_id_.find(connection_id);
        if (conn_it == connection_to_subscriber_id_.end()) {
            return;
        }
        registry_.update_cursor(connection_id, view.last_seq_no);
        auto sub_it = subscribers_.find(conn_it->second);
        if (sub_it != subscribers_.end()) {
            sub_it->second.acked_cursor = view.last_seq_no;
        }

        // The ack is a truncation cursor (F2): once every subscriber has consumed past a
        // seq_no, the publisher can reclaim the WAL below it. Truncate only when the
        // safe floor actually advances.
        const int64_t safe_floor = registry_.min_cursor();
        if (safe_floor != ExternalWalSubscriberRegistry::no_constraint && safe_floor > last_truncation_floor_) {
            last_truncation_floor_ = safe_floor;
            host_.topic_truncate_wal(safe_floor);
        }
    }

    /// Connection lost: tear down the whole logical subscriber (disconnect the pair).
    void on_connection_lost(ConnectionID connection_id) {
        auto conn_it = connection_to_subscriber_id_.find(connection_id);
        if (conn_it == connection_to_subscriber_id_.end()) {
            return;
        }
        const std::string subscriber_id = conn_it->second;
        auto sub_it = subscribers_.find(subscriber_id);
        if (sub_it == subscribers_.end()) {
            connection_to_subscriber_id_.erase(conn_it);
            return;
        }

        const ConnectionID data_connection_id = sub_it->second.data_connection_id;
        const ConnectionID control_connection_id = sub_it->second.control_connection_id;
        ConnectionID paired;
        if (connection_id == data_connection_id) {
            paired = control_connection_id;
        } else if (connection_id == control_connection_id) {
            paired = data_connection_id;
        }

        subscribers_.erase(sub_it);
        connection_to_subscriber_id_.erase(data_connection_id);
        connection_to_subscriber_id_.erase(control_connection_id);
        registry_.remove_subscriber(data_connection_id);

        if (paired.is_valid() && paired != connection_id) {
            host_.topic_disconnect(paired);
        }
    }

    /// The socket can accept another frame: stream the subscriber's next record.
    void on_connection_writable(ConnectionID connection_id) {
        auto conn_it = connection_to_subscriber_id_.find(connection_id);
        if (conn_it == connection_to_subscriber_id_.end()) {
            return;
        }
        auto sub_it = subscribers_.find(conn_it->second);
        if (sub_it != subscribers_.end() && connection_id == sub_it->second.data_connection_id) {
            pump_data(conn_it->second);
        }
    }

    /**
     * @brief A record was appended to the WAL: advance the head, wake idle subscribers,
     *        and drop any subscriber that has fallen out of the retention window.
     *
     * The record must already be in the WAL. seq_no is the record's (authoritative)
     * sequence number, tracked as the WAL head for the lag policy.
     */
    void notify_record_appended(int64_t seq_no, int16_t pdu_id) {
        if (seq_no > head_seq_no_) {
            head_seq_no_ = seq_no;
        }
        if (!is_leader_ || !is_member_(pdu_id)) {
            return;
        }

        // Wake idle subscribers so their cursor streams the new record.
        for (auto& entry : subscribers_) {
            const SubscriberStream* stream = entry.second.stream.get();
            if (stream != nullptr && stream->idle && entry.second.data_connection_id.is_valid()) {
                pump_data(entry.first);
            }
        }

        // Drop subscribers whose ack cursor has fallen out of the retention window.
        if (max_lag_records_ <= 0) {
            return;
        }
        const int64_t oldest_retained = oldest_retained_seq_no();
        std::vector<std::string> lagged_out;
        for (const auto& entry : subscribers_) {
            if (entry.second.stream && entry.second.acked_cursor < oldest_retained) {
                lagged_out.push_back(entry.first);
            }
        }
        for (const std::string& subscriber_id : lagged_out) {
            notify_lagged(subscriber_id, "subscriber too far behind the retention window", oldest_retained);
            tear_down(subscriber_id);
        }
    }

    /**
     * @brief Best-effort: send a TopicLagged on a subscriber's control channel.
     *
     * No-op if the subscriber is unknown or has no control channel. Used by the
     * slow-consumer policy (step 4) to notify a lagging subscriber out-of-band.
     */
    void notify_lagged(const std::string& subscriber_id, const std::string& reason, int64_t oldest_retained_seq_no) {
        auto sub_it = subscribers_.find(subscriber_id);
        if (sub_it == subscribers_.end() || !sub_it->second.control_connection_id.is_valid()) {
            return;
        }
        pubsub_itc_fw_app::TopicLagged lagged{};
        lagged.reason = reason;
        lagged.oldest_retained_seq_no = oldest_retained_seq_no;
        host_.topic_send_lagged(sub_it->second.control_connection_id, lagged);
    }

  private:
    struct SubscriberStream {
        WalCursor cursor;
        int64_t from_seq_no = 0; // records with record_id <= this are skipped
        bool idle = true;        // true == caught up to the WAL head, waiting for a wakeup
    };

    struct LogicalSubscriber {
        ConnectionID data_connection_id;
        ConnectionID control_connection_id;
        std::unique_ptr<SubscriberStream> stream; // present once the data channel has subscribed
        int64_t acked_cursor = 0;                 // last seq_no the subscriber acked; drives the lag policy
    };

    void handle_data_subscribe(ConnectionID connection_id, const std::string& subscriber_id, int64_t from_seq_no) {
        LogicalSubscriber& subscriber = subscribers_[subscriber_id];

        // Pre-empt a stale data connection for this subscriber id (reconnect).
        if (subscriber.data_connection_id.is_valid() && subscriber.data_connection_id != connection_id) {
            const ConnectionID stale = subscriber.data_connection_id;
            connection_to_subscriber_id_.erase(stale);
            registry_.remove_subscriber(stale);
            host_.topic_disconnect(stale);
        }

        // Gap-on-resubscribe: a request for a cursor older than the oldest retained
        // record is clamped forward. accepted_from_seq_no > requested tells the
        // subscriber it has a gap and where the stream now starts (F3, the reliable signal).
        const int64_t effective_from = std::max(from_seq_no, oldest_retained_seq_no());

        subscriber.data_connection_id = connection_id;
        subscriber.acked_cursor = effective_from;
        connection_to_subscriber_id_[connection_id] = subscriber_id;
        registry_.register_subscriber(connection_id, subscriber_id, effective_from);

        subscriber.stream = std::make_unique<SubscriberStream>();
        subscriber.stream->from_seq_no = effective_from;
        subscriber.stream->cursor.open(wal_directory_, WalPosition{0, 0});

        // NB: the "-1 == from head" convention is deferred; the tests use a concrete cursor.
        pubsub_itc_fw_app::TopicSubscribeAck ack{};
        ack.accepted_from_seq_no = effective_from;
        host_.topic_send_subscribe_ack(connection_id, ack);

        pump_data(subscriber_id); // send the first record (if any) and arm
    }

    void handle_control_subscribe(ConnectionID connection_id, const std::string& subscriber_id) {
        LogicalSubscriber& subscriber = subscribers_[subscriber_id];

        if (subscriber.control_connection_id.is_valid() && subscriber.control_connection_id != connection_id) {
            const ConnectionID stale = subscriber.control_connection_id;
            connection_to_subscriber_id_.erase(stale);
            host_.topic_disconnect(stale);
        }

        subscriber.control_connection_id = connection_id;
        connection_to_subscriber_id_[connection_id] = subscriber_id;
        // The control channel neither acks nor streams; it just carries out-of-band signals.
    }

    // The oldest seq_no the publisher will still serve: head - retention window.
    // 0 (serve everything) when the policy is disabled or the head is within the window.
    [[nodiscard]] int64_t oldest_retained_seq_no() const {
        return (max_lag_records_ > 0 && head_seq_no_ > max_lag_records_) ? (head_seq_no_ - max_lag_records_) : 0;
    }

    // Tear down a whole logical subscriber, disconnecting both of its connections.
    void tear_down(const std::string& subscriber_id) {
        auto it = subscribers_.find(subscriber_id);
        if (it == subscribers_.end()) {
            return;
        }
        const ConnectionID data_connection_id = it->second.data_connection_id;
        const ConnectionID control_connection_id = it->second.control_connection_id;
        subscribers_.erase(it);
        connection_to_subscriber_id_.erase(data_connection_id);
        connection_to_subscriber_id_.erase(control_connection_id);
        registry_.remove_subscriber(data_connection_id);
        if (data_connection_id.is_valid()) {
            host_.topic_disconnect(data_connection_id);
        }
        if (control_connection_id.is_valid()) {
            host_.topic_disconnect(control_connection_id);
        }
    }

    // Stream one TopicPage -- a batch of up to max_records_per_page_ records (also
    // bounded by max_batch_payload_bytes) -- on a subscriber's data channel and re-arm
    // its writable notification; or mark it idle if it has caught up to the WAL head.
    // One page per writable keeps delivery socket-paced while amortising the epoll
    // round-trip over the whole batch. Payloads are copied into a reused scratch buffer
    // so they stay valid regardless of the cursor's WAL segment mapping.
    void pump_data(const std::string& subscriber_id) {
        auto it = subscribers_.find(subscriber_id);
        if (it == subscribers_.end() || !it->second.stream || !it->second.data_connection_id.is_valid()) {
            return;
        }
        SubscriberStream& stream = *it->second.stream;
        const ConnectionID data_connection_id = it->second.data_connection_id;

        batch_scratch_.clear();
        batch_records_.clear();
        batch_offsets_.clear();

        bool reopened = false;
        int64_t record_id = 0;
        const uint8_t* data = nullptr;
        size_t size = 0;
        int64_t last_batched_seq_no = 0;
        for (;;) {
            if (!stream.cursor.read_next(record_id, data, size)) {
                if (!reopened) {
                    // A record may have been appended since the cursor's snapshot;
                    // re-discover from the current position before declaring idle.
                    stream.cursor.open(wal_directory_, stream.cursor.position());
                    reopened = true;
                    continue;
                }
                break; // genuinely caught up to the WAL head
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
            const size_t pdu_payload_size = size - wal_record_header_size;

            const size_t offset = batch_scratch_.size();
            batch_scratch_.insert(batch_scratch_.end(), pdu_payload, pdu_payload + pdu_payload_size);
            batch_offsets_.push_back(offset);

            pubsub_itc_fw_app::TopicRecord record{};
            record.seq_no = record_id;
            record.pdu_id = pdu_id;
            record.wall_time_ns = wall_time_ns;
            record.payload.data = nullptr; // fixed up below, once the scratch buffer is final
            record.payload.size = pdu_payload_size;
            batch_records_.push_back(record);
            last_batched_seq_no = record_id;

            // The cursor is now positioned at the next unread record, so stopping here
            // resumes cleanly on the next writable.
            if (batch_records_.size() >= max_records_per_page_ || batch_scratch_.size() >= max_batch_payload_bytes) {
                break;
            }
        }

        if (batch_records_.empty()) {
            stream.idle = true;
            return;
        }

        // The scratch buffer will not move again: point each record at its bytes.
        for (size_t i = 0; i < batch_records_.size(); ++i) {
            batch_records_[i].payload.data = batch_scratch_.data() + batch_offsets_[i];
        }

        pubsub_itc_fw_app::TopicPage page{};
        page.record_count = static_cast<int16_t>(batch_records_.size());
        page.page_number = 1;
        page.total_pages = 1;
        page.records.data = batch_records_.data();
        page.records.size = batch_records_.size();

        host_.topic_send_page(data_connection_id, last_batched_seq_no, page);
        host_.topic_request_writable_notification(data_connection_id);
        stream.idle = false;
    }

    TopicPublisherHost& host_;
    std::string topic_name_;
    MembershipPredicate is_member_;
    std::string wal_directory_;
    int64_t max_lag_records_ = 0;                                // retention window; 0 disables the lag policy
    size_t max_records_per_page_ = default_max_records_per_page; // batch cap per TopicPage
    int64_t head_seq_no_ = 0;                                    // highest seq_no appended to the WAL
    int64_t last_truncation_floor_ = 0;                          // highest safe floor already truncated to
    bool is_leader_ = true;
    std::vector<uint8_t> batch_scratch_;                        // reused per-page payload copy buffer
    std::vector<pubsub_itc_fw_app::TopicRecord> batch_records_; // reused per-page record list
    std::vector<size_t> batch_offsets_;                         // payload offsets into batch_scratch_
    ExternalWalSubscriberRegistry registry_;
    std::unordered_map<std::string, LogicalSubscriber> subscribers_;            // subscriber_id -> pair + stream
    std::unordered_map<ConnectionID, std::string> connection_to_subscriber_id_; // either connection -> subscriber_id
};

} // namespaces
