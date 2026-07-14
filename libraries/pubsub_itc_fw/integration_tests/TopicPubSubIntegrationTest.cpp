// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file TopicPubSubIntegrationTest.cpp
 * @brief Integration tests for the reusable topic pub/sub components.
 *
 * Exercises TopicPublisher and TopicSubscriberChannel end-to-end over loopback
 * TCP, with the publisher and subscriber each running on their own reactor in
 * their own thread -- i.e. the pub/sub functionality on its own, not through the
 * MEP/TAP applications.
 *
 * First test (SingleSubscriberReceivesReplayedRecordsInOrder):
 *   The publisher pre-populates a WAL with three records for topic "orders".
 *   A single subscriber subscribes from the oldest cursor, receives all three
 *   records (one TopicPage each) in seq_no order, and acks each page. The test
 *   verifies the records arrive in order with the correct payloads.
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/AllocatorConfiguration.hpp>
#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/ApplicationThreadConfiguration.hpp>
#include <pubsub_itc_fw/BumpAllocator.hpp>
#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/NetworkEndpointConfiguration.hpp>
#include <pubsub_itc_fw/QueueConfiguration.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/Reactor.hpp>
#include <pubsub_itc_fw/ReactorConfiguration.hpp>
#include <pubsub_itc_fw/ReactorControlCommand.hpp>
#include <pubsub_itc_fw/ServiceRegistry.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>
#include <pubsub_itc_fw/ThreadWithJoinTimeout.hpp>
#include <pubsub_itc_fw/TopicControlChannel.hpp>
#include <pubsub_itc_fw/TopicPublisher.hpp>
#include <pubsub_itc_fw/TopicSubscriberChannel.hpp>
#include <pubsub_itc_fw/TopicSubscriberThread.hpp>
#include <pubsub_itc_fw/Wal.hpp>
#include <pubsub_itc_fw/WalPosition.hpp>
#include <pubsub_itc_fw/WalReader.hpp>
#include <pubsub_itc_fw/WalWriter.hpp>

#include <pubsub_itc_fw/tests_common/LoggerWithSink.hpp>
#include <pubsub_itc_fw/tests_common/TestConfigurations.hpp>

#include <fix_equity_orders.hpp>
#include <topics.hpp>

using pubsub_itc_fw::tests::make_allocator_config;
using pubsub_itc_fw::tests::make_queue_config;

namespace pubsub_itc_fw {

// Typed flags for the test publisher/subscriber toggles, so call sites read as a named
// state instead of a bare true/false argument (following the UseHugePagesFlag pattern).
class DisconnectWhenDoneFlag {
  public:
    enum DisconnectWhenDoneFlagTag { DoNotDisconnectWhenDone = 0, DoDisconnectWhenDone = 1 };

    explicit DisconnectWhenDoneFlag(DisconnectWhenDoneFlagTag value) : value_{value} {}

    [[nodiscard]] bool is_equal(const DisconnectWhenDoneFlagTag& rhs) const {
        return value_ == rhs;
    }

  private:
    DisconnectWhenDoneFlagTag value_;
};

inline bool operator==(const DisconnectWhenDoneFlag& lhs, const DisconnectWhenDoneFlag::DisconnectWhenDoneFlagTag& rhs) {
    return lhs.is_equal(rhs);
}

class DisconnectDataWhenDoneFlag {
  public:
    enum DisconnectDataWhenDoneFlagTag { DoNotDisconnectDataWhenDone = 0, DoDisconnectDataWhenDone = 1 };

    explicit DisconnectDataWhenDoneFlag(DisconnectDataWhenDoneFlagTag value) : value_{value} {}

    [[nodiscard]] bool is_equal(const DisconnectDataWhenDoneFlagTag& rhs) const {
        return value_ == rhs;
    }

  private:
    DisconnectDataWhenDoneFlagTag value_;
};

inline bool operator==(const DisconnectDataWhenDoneFlag& lhs, const DisconnectDataWhenDoneFlag::DisconnectDataWhenDoneFlagTag& rhs) {
    return lhs.is_equal(rhs);
}

class SuppressAckFlag {
  public:
    enum SuppressAckFlagTag { DoNotSuppressAck = 0, DoSuppressAck = 1 };

    explicit SuppressAckFlag(SuppressAckFlagTag value) : value_{value} {}

    [[nodiscard]] bool is_equal(const SuppressAckFlagTag& rhs) const {
        return value_ == rhs;
    }

  private:
    SuppressAckFlagTag value_;
};

inline bool operator==(const SuppressAckFlag& lhs, const SuppressAckFlag::SuppressAckFlagTag& rhs) {
    return lhs.is_equal(rhs);
}

class LagOnControlSubscribeFlag {
  public:
    enum LagOnControlSubscribeFlagTag { DoNotLagOnControlSubscribe = 0, DoLagOnControlSubscribe = 1 };

    explicit LagOnControlSubscribeFlag(LagOnControlSubscribeFlagTag value) : value_{value} {}

    [[nodiscard]] bool is_equal(const LagOnControlSubscribeFlagTag& rhs) const {
        return value_ == rhs;
    }

  private:
    LagOnControlSubscribeFlagTag value_;
};

inline bool operator==(const LagOnControlSubscribeFlag& lhs, const LagOnControlSubscribeFlag::LagOnControlSubscribeFlagTag& rhs) {
    return lhs.is_equal(rhs);
}

static constexpr size_t wal_segment_size = 4096;

// Publisher-side ApplicationThread: owns a TopicPublisher for
// the "orders" topic and forwards the topic protocol to it.
class TopicPublisherThread : public ApplicationThread, public TopicPublisherHost {
  public:
    // publish_after_subscribers > 0 arms a one-shot live publish of live_record_count
    // records (seq_no 1..N, pdu_id NOS) once that many subscribers have subscribed --
    // used to exercise the live fanout path. Left at 0, the thread only replays the WAL.
    TopicPublisherThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor, std::string wal_dir, int publish_after_subscribers = 0,
                         int live_record_count = 0,
                         LagOnControlSubscribeFlag lag_on_control_subscribe = LagOnControlSubscribeFlag{LagOnControlSubscribeFlag::DoNotLagOnControlSubscribe},
                         int64_t max_lag_records = 0, int startup_records = 0, int records_after_both_channels = 0, size_t segment_size = wal_segment_size,
                         size_t max_records_per_page = TopicPublisher::default_max_records_per_page)
        : ApplicationThread(token, logger, reactor, "TopicPublisherThread", ThreadID{2}, make_queue_config(), make_allocator_config("TopicPubPool"),
                            ApplicationThreadConfiguration{})
        , publish_after_subscribers_(publish_after_subscribers)
        , live_record_count_(live_record_count)
        , lag_on_control_subscribe_(lag_on_control_subscribe)
        , startup_records_(startup_records)
        , records_after_both_channels_(records_after_both_channels)
        , segment_size_(segment_size)
        , wal_directory_(wal_dir)
        , publisher_(
              *this, "orders",
              [](int16_t pdu_id) {
                  return pdu_id == pubsub_itc_fw_app::NewOrderSingle::message_pdu_id || pdu_id == pubsub_itc_fw_app::OrderCancelRequest::message_pdu_id;
              },
              std::move(wal_dir), max_lag_records, max_records_per_page) {}

    // TopicPublisherHost
    void topic_send_subscribe_ack(ConnectionID connection_id, const pubsub_itc_fw_app::TopicSubscribeAck& ack) override {
        send_pdu(connection_id, pubsub_itc_fw_app::TopicSubscribeAck::message_pdu_id, 0, ack);
    }
    void topic_send_page(ConnectionID connection_id, int64_t seq_no, const pubsub_itc_fw_app::TopicPage& page) override {
        send_pdu(connection_id, pubsub_itc_fw_app::TopicPage::message_pdu_id, seq_no, page);
    }
    void topic_send_not_leader(ConnectionID connection_id, const pubsub_itc_fw_app::TopicNotLeader& not_leader) override {
        send_pdu(connection_id, pubsub_itc_fw_app::TopicNotLeader::message_pdu_id, 0, not_leader);
    }
    void topic_send_lagged(ConnectionID control_connection_id, const pubsub_itc_fw_app::TopicLagged& lagged) override {
        send_pdu(control_connection_id, pubsub_itc_fw_app::TopicLagged::message_pdu_id, 0, lagged);
    }
    void topic_disconnect(ConnectionID connection_id) override {
        ReactorControlCommand cmd(ReactorControlCommand::CommandTag::Disconnect);
        cmd.connection_id_ = connection_id;
        get_reactor().enqueue_control_command(cmd);
    }
    void topic_request_writable_notification(ConnectionID connection_id) override {
        request_writable_notification(connection_id);
    }
    void topic_truncate_wal(int64_t safe_seq_no) override {
        if (wal_.is_open()) {
            wal_.truncate_below(safe_seq_no);
        }
    }

  protected:
    void on_initial_event() override {
        // Any test that writes records needs an open WAL to append to.
        if (publish_after_subscribers_ > 0 || startup_records_ > 0 || records_after_both_channels_ > 0) {
            wal_.open(wal_directory_, segment_size_);
        }
        // Records appended before any subscriber connects (drives gap-on-resubscribe).
        for (int i = 1; i <= startup_records_; ++i) {
            append_and_notify(i);
        }
    }

    void on_connection_writable(ConnectionID id) override {
        publisher_.on_connection_writable(id);
    }

    void on_connection_lost(const ConnectionID& id, const std::string&) override {
        publisher_.on_connection_lost(id);
    }

    void on_framework_pdu_message(const EventMessage& message) override {
        const ConnectionID connection_id = message.connection_id();
        const uint8_t* payload = message.payload();
        const size_t size = static_cast<size_t>(message.payload_size());
        BumpAllocator arena(decode_arena_buffer().data(), decode_arena_buffer().capacity());
        size_t consumed = 0;
        size_t arena_needed = 0;

        if (message.pdu_id() == pubsub_itc_fw_app::TopicSubscribeRequest::message_pdu_id) {
            pubsub_itc_fw_app::TopicSubscribeRequestView view{};
            if (!pubsub_itc_fw_app::decode(view, payload, size, consumed, arena, arena_needed)) {
                return;
            }
            publisher_.on_subscribe_request(connection_id, view);
            maybe_publish_live_batch();
            // Demonstrate the control channel: once the control channel is up, send a
            // TopicLagged on it (step 4 decides *when* from the lag policy; this is a demo).
            if (lag_on_control_subscribe_ == LagOnControlSubscribeFlag::DoLagOnControlSubscribe && view.role == pubsub_itc_fw_app::TopicChannelRole::Control) {
                publisher_.notify_lagged(std::string(view.subscriber_id), "too far behind", 42);
            }
            // Once BOTH channels are up, append a batch (drives the lag-out policy while
            // the control channel is available to carry the TopicLagged).
            if (view.role == pubsub_itc_fw_app::TopicChannelRole::Control) {
                control_subscribed_ = true;
            } else {
                data_subscribed_ = true;
            }
            if (data_subscribed_ && control_subscribed_ && !appended_after_both_ && records_after_both_channels_ > 0) {
                appended_after_both_ = true;
                for (int i = 1; i <= records_after_both_channels_; ++i) {
                    append_and_notify(i);
                }
            }
        } else if (message.pdu_id() == pubsub_itc_fw_app::TopicAck::message_pdu_id) {
            pubsub_itc_fw_app::TopicAckView view{};
            if (!pubsub_itc_fw_app::decode(view, payload, size, consumed, arena, arena_needed)) {
                return;
            }
            publisher_.on_ack(connection_id, view);
        }
    }

    void on_itc_message(const EventMessage&) override {}

  private:
    // Once publish_after_subscribers_ subscribers are connected, append the live
    // batch to the WAL (exactly once) and wake subscribers to stream it. Records must
    // be in the WAL before notify_record_appended() -- delivery is cursor-based.
    void maybe_publish_live_batch() {
        if (publish_after_subscribers_ <= 0 || published_live_) {
            return;
        }
        if (static_cast<int>(publisher_.subscriber_count()) < publish_after_subscribers_) {
            return;
        }
        published_live_ = true;
        for (int i = 1; i <= live_record_count_; ++i) {
            append_and_notify(i);
        }
    }

    // Append a record (seq_no i, pdu_id NOS) to the WAL and tell the publisher.
    void append_and_notify(int i) {
        const uint32_t value = static_cast<uint32_t>(i * 100);
        uint8_t payload[sizeof(uint32_t)];
        std::memcpy(payload, &value, sizeof(value));
        wal_.append(static_cast<int64_t>(i), pubsub_itc_fw_app::NewOrderSingle::message_pdu_id, payload, static_cast<int>(sizeof(payload)),
                    static_cast<int64_t>(i) * 1000);
        publisher_.notify_record_appended(static_cast<int64_t>(i), pubsub_itc_fw_app::NewOrderSingle::message_pdu_id);
    }

    int publish_after_subscribers_;
    int live_record_count_;
    LagOnControlSubscribeFlag lag_on_control_subscribe_;
    int startup_records_;
    int records_after_both_channels_;
    size_t segment_size_;
    bool published_live_ = false;
    bool data_subscribed_ = false;
    bool control_subscribed_ = false;
    bool appended_after_both_ = false;
    std::string wal_directory_;
    Wal wal_;
    TopicPublisher publisher_;
};

// Subscriber-side ApplicationThread: owns a TopicSubscriberChannel
// and delivers received records into a vector for the test to check.
class ChannelSubscriberThread : public ApplicationThread, public TopicSubscriberChannelHost {
  public:
    struct ReceivedRecord {
        int64_t seq_no;
        int16_t pdu_id;
        std::vector<uint8_t> payload;
    };

    std::atomic<bool> all_records_received{false};
    std::atomic<int> records_received{0};
    std::atomic<int> pages_received{0}; // number of TopicPage PDUs seen (batching observable)
    std::vector<ReceivedRecord> received_records;

    // disconnect_when_done: tear the data connection down as soon as the expected count
    // arrives. Left true for tests that just want a clean end. A test that needs the
    // subscriber's final TopicAck to reach the publisher (e.g. F2 WAL truncation) must
    // pass false: with page batching the last records and the completion can land in one
    // reactor turn, and a self-disconnect there would race ahead of the ack send.
    ChannelSubscriberThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor, int expected_record_count,
                            std::string subscriber_id = "test_subscriber", int64_t from_seq_no = 0,
                            DisconnectWhenDoneFlag disconnect_when_done = DisconnectWhenDoneFlag{DisconnectWhenDoneFlag::DoDisconnectWhenDone})
        : ApplicationThread(token, logger, reactor, "ChannelSubscriberThread", ThreadID{1}, make_queue_config(), make_allocator_config("TopicSubPool"),
                            ApplicationThreadConfiguration{})
        , expected_record_count_(expected_record_count)
        , disconnect_when_done_(disconnect_when_done)
        , channel_(*this, std::move(subscriber_id), "orders", from_seq_no, [this](int64_t seq_no, int16_t pdu_id, const uint8_t* payload, size_t payload_size) {
            received_records.push_back({seq_no, pdu_id, {payload, payload + payload_size}});
            const int count = records_received.fetch_add(1, std::memory_order_acq_rel) + 1;
            if (count == expected_record_count_) {
                all_records_received.store(true, std::memory_order_release);
                if (disconnect_when_done_ == DisconnectWhenDoneFlag::DoDisconnectWhenDone) {
                    ReactorControlCommand cmd(ReactorControlCommand::CommandTag::Disconnect);
                    cmd.connection_id_ = connection_id_;
                    get_reactor().enqueue_control_command(cmd);
                }
            }
        }) {}

    // TopicSubscriberChannelHost
    void topic_send_subscribe_request(ConnectionID connection_id, const pubsub_itc_fw_app::TopicSubscribeRequest& request) override {
        send_pdu(connection_id, pubsub_itc_fw_app::TopicSubscribeRequest::message_pdu_id, 0, request);
    }
    void topic_send_ack(ConnectionID connection_id, const pubsub_itc_fw_app::TopicAck& ack) override {
        send_pdu(connection_id, pubsub_itc_fw_app::TopicAck::message_pdu_id, 0, ack);
    }

  protected:
    void on_initial_event() override {
        connect_to_service("publisher");
    }

    void on_connection_established(ConnectionID id) override {
        connection_id_ = id;
        channel_.on_connected(id);
    }

    void on_connection_failed(const std::string& reason) override {
        shutdown("connection failed: " + reason);
    }

    void on_connection_lost(const ConnectionID&, const std::string&) override {
        shutdown("connection lost");
    }

    void on_framework_pdu_message(const EventMessage& message) override {
        const uint8_t* payload = message.payload();
        const size_t size = static_cast<size_t>(message.payload_size());
        BumpAllocator arena(decode_arena_buffer().data(), decode_arena_buffer().capacity());
        size_t consumed = 0;
        size_t arena_needed = 0;

        if (message.pdu_id() == pubsub_itc_fw_app::TopicSubscribeAck::message_pdu_id) {
            pubsub_itc_fw_app::TopicSubscribeAckView view{};
            if (!pubsub_itc_fw_app::decode(view, payload, size, consumed, arena, arena_needed)) {
                return;
            }
            channel_.on_subscribe_ack(view);
        } else if (message.pdu_id() == pubsub_itc_fw_app::TopicPage::message_pdu_id) {
            pages_received.fetch_add(1, std::memory_order_acq_rel);
            channel_.on_page(payload, size, arena);
        }
    }

    void on_itc_message(const EventMessage&) override {}

  private:
    int expected_record_count_;
    DisconnectWhenDoneFlag disconnect_when_done_;
    ConnectionID connection_id_;
    TopicSubscriberChannel channel_;
};

// A subscriber that opens BOTH the data and control channels --
// two connections to the publisher's single port, distinguished
// by role -- and records data records + control (TopicLagged) signals.
class DualChannelSubscriberThread : public ApplicationThread, public TopicSubscriberChannelHost, public TopicControlChannelHost {
  public:
    struct ReceivedRecord {
        int64_t seq_no;
        int16_t pdu_id;
    };
    struct ReceivedLagged {
        std::string reason;
        int64_t oldest_retained_seq_no;
    };

    std::atomic<bool> all_records_received{false};
    std::atomic<bool> lagged_received{false};
    std::atomic<bool> control_channel_lost{false};
    std::atomic<bool> data_channel_lost{false};
    std::vector<ReceivedRecord> received_records;
    std::vector<ReceivedLagged> received_lagged;

    // subscriber_from_seq_no is the cursor requested on the data channel. suppress_ack
    // makes the subscriber receive records but never ack (a slow/stalled consumer, for
    // the lag policy).
    DualChannelSubscriberThread(
        ConstructorToken token, QuillLogger& logger, Reactor& reactor, std::string subscriber_id, int expected_record_count,
        DisconnectDataWhenDoneFlag disconnect_data_when_done = DisconnectDataWhenDoneFlag{DisconnectDataWhenDoneFlag::DoNotDisconnectDataWhenDone},
        SuppressAckFlag suppress_ack = SuppressAckFlag{SuppressAckFlag::DoNotSuppressAck}, int64_t subscriber_from_seq_no = 0)
        : ApplicationThread(token, logger, reactor, "DualChannelSubscriberThread", ThreadID{1}, make_queue_config(), make_allocator_config("DualSubPool"),
                            ApplicationThreadConfiguration{})
        , expected_record_count_(expected_record_count)
        , disconnect_data_when_done_(disconnect_data_when_done)
        , suppress_ack_(suppress_ack)
        , data_channel_(*this, subscriber_id, "orders", subscriber_from_seq_no,
                        [this](int64_t seq_no, int16_t pdu_id, const uint8_t*, size_t) {
                            received_records.push_back({seq_no, pdu_id});
                            if (static_cast<int>(received_records.size()) == expected_record_count_) {
                                all_records_received.store(true, std::memory_order_release);
                                if (disconnect_data_when_done_ == DisconnectDataWhenDoneFlag::DoDisconnectDataWhenDone) {
                                    ReactorControlCommand cmd(ReactorControlCommand::CommandTag::Disconnect);
                                    cmd.connection_id_ = data_connection_id_;
                                    get_reactor().enqueue_control_command(cmd);
                                }
                            }
                        })
        , control_channel_(*this, subscriber_id, "orders", [this](const std::string& reason, int64_t oldest_retained_seq_no) {
            received_lagged.push_back({reason, oldest_retained_seq_no});
            lagged_received.store(true, std::memory_order_release);
        }) {}

    // TopicSubscriberChannelHost (data)
    void topic_send_subscribe_request(ConnectionID connection_id, const pubsub_itc_fw_app::TopicSubscribeRequest& request) override {
        send_pdu(connection_id, pubsub_itc_fw_app::TopicSubscribeRequest::message_pdu_id, 0, request);
    }
    void topic_send_ack(ConnectionID connection_id, const pubsub_itc_fw_app::TopicAck& ack) override {
        if (suppress_ack_ == SuppressAckFlag::DoSuppressAck) {
            return; // simulate a stalled consumer that never advances its ack cursor
        }
        send_pdu(connection_id, pubsub_itc_fw_app::TopicAck::message_pdu_id, 0, ack);
    }
    // TopicControlChannelHost (control)
    void topic_control_send_subscribe_request(ConnectionID connection_id, const pubsub_itc_fw_app::TopicSubscribeRequest& request) override {
        send_pdu(connection_id, pubsub_itc_fw_app::TopicSubscribeRequest::message_pdu_id, 0, request);
    }

    /// The accepted_from_seq_no the data channel received (for gap-on-resubscribe assertions).
    [[nodiscard]] int64_t data_accepted_from_seq_no() const {
        return data_channel_.accepted_from_seq_no();
    }

  protected:
    void on_initial_event() override {
        connect_to_service("publisher_data");
        connect_to_service("publisher_control");
    }

    void on_connection_established(ConnectionID id) override {
        if (id.service_name() == "publisher_control") {
            control_connection_id_ = id;
            control_channel_.on_connected(id);
        } else {
            data_connection_id_ = id;
            data_channel_.on_connected(id);
        }
    }

    void on_connection_failed(const std::string& reason) override {
        shutdown("connection failed: " + reason);
    }

    void on_connection_lost(const ConnectionID& id, const std::string&) override {
        if (id == control_connection_id_) {
            control_channel_lost.store(true, std::memory_order_release);
        } else if (id == data_connection_id_) {
            data_channel_lost.store(true, std::memory_order_release);
        }
    }

    void on_framework_pdu_message(const EventMessage& message) override {
        const ConnectionID connection_id = message.connection_id();
        const uint8_t* payload = message.payload();
        const size_t size = static_cast<size_t>(message.payload_size());
        BumpAllocator arena(decode_arena_buffer().data(), decode_arena_buffer().capacity());
        size_t consumed = 0;
        size_t arena_needed = 0;

        if (connection_id == control_connection_id_) {
            if (message.pdu_id() == pubsub_itc_fw_app::TopicLagged::message_pdu_id) {
                pubsub_itc_fw_app::TopicLaggedView view{};
                if (pubsub_itc_fw_app::decode(view, payload, size, consumed, arena, arena_needed)) {
                    control_channel_.on_lagged(view);
                }
            }
            return;
        }

        if (message.pdu_id() == pubsub_itc_fw_app::TopicSubscribeAck::message_pdu_id) {
            pubsub_itc_fw_app::TopicSubscribeAckView view{};
            if (pubsub_itc_fw_app::decode(view, payload, size, consumed, arena, arena_needed)) {
                data_channel_.on_subscribe_ack(view);
            }
        } else if (message.pdu_id() == pubsub_itc_fw_app::TopicPage::message_pdu_id) {
            data_channel_.on_page(payload, size, arena);
        }
    }

    void on_itc_message(const EventMessage&) override {}

  private:
    int expected_record_count_;
    DisconnectDataWhenDoneFlag disconnect_data_when_done_;
    SuppressAckFlag suppress_ack_;
    ConnectionID data_connection_id_;
    ConnectionID control_connection_id_;
    TopicSubscriberChannel data_channel_;
    TopicControlChannel control_channel_;
};

// Subscriber built on the framework's TopicSubscriberThread base. It overrides only
// on_pubsub_message and captures each delivered record, proving records reach the
// application through on_pubsub_message rather than a hand-written PDU handler.
class CapturingSubscriberThread : public TopicSubscriberThread {
  public:
    struct ReceivedRecord {
        int64_t seq_no;
        int16_t pdu_id;
        std::vector<uint8_t> payload;
    };

    std::atomic<bool> all_records_received{false};
    std::atomic<int> records_received{0};
    std::vector<ReceivedRecord> received_records;

    CapturingSubscriberThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor, int expected_record_count, std::string subscriber_id = "capturing",
                              int64_t from_seq_no = 0)
        : TopicSubscriberThread(token, logger, reactor, "CapturingSubscriberThread", ThreadID{1}, make_queue_config(), make_allocator_config("CapSubPool"),
                                ApplicationThreadConfiguration{}, "publisher", std::move(subscriber_id), "orders", from_seq_no)
        , expected_record_count_(expected_record_count) {}

  protected:
    void on_pubsub_message(const EventMessage& message) override {
        received_records.push_back({message.seq_no(), message.pdu_id(), {message.payload(), message.payload() + message.payload_size()}});
        const int count = records_received.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (count == expected_record_count_) {
            all_records_received.store(true, std::memory_order_release);
        }
    }

  private:
    int expected_record_count_;
};

// Test fixture
class TopicPubSubTest : public ::testing::Test {
  protected:
    void SetUp() override {
        logger_ = std::make_unique<LoggerWithSink>();
        std::string tmpl = "/dev/shm/topic_pubsub_test_XXXXXX";
        ASSERT_NE(::mkdtemp(tmpl.data()), nullptr);
        wal_dir_ = tmpl;
    }

    void TearDown() override {
        std::filesystem::remove_all(wal_dir_);
        logger_.reset();
    }

    static bool wait_for(std::function<bool()> pred, int timeout_ms = 5000) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (!pred()) {
            if (std::chrono::steady_clock::now() > deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }

    static ReactorConfiguration make_reactor_config() {
        ReactorConfiguration cfg{};
        cfg.inactivity_check_interval_ = std::chrono::milliseconds(100);
        cfg.init_phase_timeout_ = std::chrono::milliseconds(5000);
        cfg.shutdown_timeout_ = std::chrono::milliseconds(1000);
        cfg.connect_timeout = std::chrono::milliseconds(2000);
        return cfg;
    }

    // One WAL record for the "orders" topic, in the on-disk framing the publisher
    // expects: [wall_time_ns : int64][pdu_id : int16][payload : uint32].
    static std::vector<uint8_t> encode_wal_record(int i) {
        std::vector<uint8_t> buffer(TopicPublisher::wal_record_header_size + sizeof(uint32_t));
        const int64_t wall_time_ns = static_cast<int64_t>(i) * 1000;
        const int16_t pdu_id = pubsub_itc_fw_app::NewOrderSingle::message_pdu_id;
        const uint32_t payload = static_cast<uint32_t>(i * 100);
        std::memcpy(buffer.data(), &wall_time_ns, sizeof(wall_time_ns));
        std::memcpy(buffer.data() + sizeof(int64_t), &pdu_id, sizeof(pdu_id));
        std::memcpy(buffer.data() + TopicPublisher::wal_record_header_size, &payload, sizeof(payload));
        return buffer;
    }

    // Create/overwrite the WAL with records seq_no 1..count.
    void write_topic_wal_records(int count) {
        WalWriter writer;
        writer.open(wal_dir_, wal_segment_size, {0, 0});
        for (int i = 1; i <= count; ++i) {
            const std::vector<uint8_t> buffer = encode_wal_record(i);
            writer.append(static_cast<int64_t>(i), buffer.data(), buffer.size());
        }
    }

    // Append records seq_no first..last to the existing WAL, continuing from its end.
    void append_topic_wal_records(int first, int last) {
        const WalPosition end = WalReader::replay(wal_dir_, {0, 0}, [](int64_t, const void*, size_t) {});
        WalWriter writer;
        writer.open(wal_dir_, wal_segment_size, end);
        for (int i = first; i <= last; ++i) {
            const std::vector<uint8_t> buffer = encode_wal_record(i);
            writer.append(static_cast<int64_t>(i), buffer.data(), buffer.size());
        }
    }

    std::unique_ptr<LoggerWithSink> logger_;
    std::string wal_dir_;
};

// Test 1: one publisher, one subscriber, three replayed records
// received in seq_no order with correct payloads.
TEST_F(TopicPubSubTest, SingleSubscriberReceivesReplayedRecordsInOrder) {
    static constexpr int record_count = 3;
    write_topic_wal_records(record_count);

    // --- Publisher side ---
    const ServiceRegistry publisher_registry;
    auto publisher_reactor = std::make_unique<Reactor>(make_reactor_config(), publisher_registry, logger_->logger);
    publisher_reactor->register_inbound_listener(NetworkEndpointConfiguration{"127.0.0.1", 0}, ThreadID{2});

    auto publisher_thread = ApplicationThread::create<TopicPublisherThread>(logger_->logger, *publisher_reactor, wal_dir_);
    publisher_reactor->register_thread(publisher_thread);

    ThreadWithJoinTimeout publisher_reactor_thread([&]() { publisher_reactor->run(); });

    ASSERT_TRUE(wait_for([&]() { return publisher_reactor->is_initialized(); })) << "Publisher reactor did not initialize";

    const uint16_t listen_port = publisher_reactor->get_inbound_listener_port(0);
    ASSERT_NE(listen_port, 0U);

    // --- Subscriber side ---
    ServiceRegistry subscriber_registry;
    subscriber_registry.add("publisher", NetworkEndpointConfiguration{"127.0.0.1", listen_port}, NetworkEndpointConfiguration{});

    auto subscriber_reactor = std::make_unique<Reactor>(make_reactor_config(), subscriber_registry, logger_->logger);
    auto subscriber_thread = ApplicationThread::create<ChannelSubscriberThread>(logger_->logger, *subscriber_reactor, record_count);
    subscriber_reactor->register_thread(subscriber_thread);

    ThreadWithJoinTimeout subscriber_reactor_thread([&]() { subscriber_reactor->run(); });

    // --- Wait for all records to arrive ---
    EXPECT_TRUE(wait_for([&]() { return subscriber_thread->all_records_received.load(std::memory_order_acquire); }))
        << "Subscriber did not receive all TopicPage records";

    // --- Verify records received in order with correct field values ---
    ASSERT_EQ(static_cast<int>(subscriber_thread->received_records.size()), record_count);
    for (int i = 0; i < record_count; ++i) {
        const auto& rec = subscriber_thread->received_records[static_cast<size_t>(i)];
        EXPECT_EQ(rec.seq_no, i + 1) << "Wrong seq_no at index " << i;
        EXPECT_EQ(rec.pdu_id, pubsub_itc_fw_app::NewOrderSingle::message_pdu_id) << "Wrong pdu_id at index " << i;
        ASSERT_EQ(rec.payload.size(), sizeof(uint32_t));
        uint32_t value{};
        std::memcpy(&value, rec.payload.data(), sizeof(value));
        EXPECT_EQ(value, static_cast<uint32_t>((i + 1) * 100)) << "Wrong payload at index " << i;
    }

    // --- Shutdown ---
    subscriber_reactor->shutdown("test complete");
    publisher_reactor->shutdown("test complete");

    if (subscriber_reactor_thread.joinable()) {
        subscriber_reactor_thread.join();
    }
    if (publisher_reactor_thread.joinable()) {
        publisher_reactor_thread.join();
    }
}

// Test 2: two subscribers both receive every live-published
// record (fanout via TopicPublisher::publish()).
TEST_F(TopicPubSubTest, TwoSubscribersReceiveLiveFanout) {
    static constexpr int record_count = 3;
    // No pre-populated WAL: the publisher opens its own Wal and appends the records
    // live once both subscribers are connected.

    // --- Publisher: publish the batch once both subscribers have subscribed ---
    const ServiceRegistry publisher_registry;
    auto publisher_reactor = std::make_unique<Reactor>(make_reactor_config(), publisher_registry, logger_->logger);
    publisher_reactor->register_inbound_listener(NetworkEndpointConfiguration{"127.0.0.1", 0}, ThreadID{2});
    auto publisher_thread = ApplicationThread::create<TopicPublisherThread>(logger_->logger, *publisher_reactor, wal_dir_, 2, record_count);
    publisher_reactor->register_thread(publisher_thread);
    ThreadWithJoinTimeout publisher_reactor_thread([&]() { publisher_reactor->run(); });
    ASSERT_TRUE(wait_for([&]() { return publisher_reactor->is_initialized(); })) << "Publisher reactor did not initialize";
    const uint16_t listen_port = publisher_reactor->get_inbound_listener_port(0);
    ASSERT_NE(listen_port, 0U);

    // --- Two subscribers with distinct ids. Distinct ids matter: the registry's
    //     orphan pre-emption would otherwise make one displace the other. ---
    ServiceRegistry registry_a;
    registry_a.add("publisher", NetworkEndpointConfiguration{"127.0.0.1", listen_port}, NetworkEndpointConfiguration{});
    auto reactor_a = std::make_unique<Reactor>(make_reactor_config(), registry_a, logger_->logger);
    auto sub_a = ApplicationThread::create<ChannelSubscriberThread>(logger_->logger, *reactor_a, record_count, "sub_a", 0);
    reactor_a->register_thread(sub_a);
    ThreadWithJoinTimeout reactor_a_thread([&]() { reactor_a->run(); });

    ServiceRegistry registry_b;
    registry_b.add("publisher", NetworkEndpointConfiguration{"127.0.0.1", listen_port}, NetworkEndpointConfiguration{});
    auto reactor_b = std::make_unique<Reactor>(make_reactor_config(), registry_b, logger_->logger);
    auto sub_b = ApplicationThread::create<ChannelSubscriberThread>(logger_->logger, *reactor_b, record_count, "sub_b", 0);
    reactor_b->register_thread(sub_b);
    ThreadWithJoinTimeout reactor_b_thread([&]() { reactor_b->run(); });

    EXPECT_TRUE(wait_for([&]() { return sub_a->all_records_received.load(std::memory_order_acquire); })) << "sub_a incomplete";
    EXPECT_TRUE(wait_for([&]() { return sub_b->all_records_received.load(std::memory_order_acquire); })) << "sub_b incomplete";

    // --- Both subscribers saw every live record, in order ---
    for (auto* subscriber : {sub_a.get(), sub_b.get()}) {
        ASSERT_EQ(static_cast<int>(subscriber->received_records.size()), record_count);
        for (int i = 0; i < record_count; ++i) {
            const auto& rec = subscriber->received_records[static_cast<size_t>(i)];
            EXPECT_EQ(rec.seq_no, i + 1);
            EXPECT_EQ(rec.pdu_id, pubsub_itc_fw_app::NewOrderSingle::message_pdu_id);
            ASSERT_EQ(rec.payload.size(), sizeof(uint32_t));
            uint32_t value{};
            std::memcpy(&value, rec.payload.data(), sizeof(value));
            EXPECT_EQ(value, static_cast<uint32_t>((i + 1) * 100));
        }
    }

    reactor_a->shutdown("test complete");
    reactor_b->shutdown("test complete");
    publisher_reactor->shutdown("test complete");
    if (reactor_a_thread.joinable()) {
        reactor_a_thread.join();
    }
    if (reactor_b_thread.joinable()) {
        reactor_b_thread.join();
    }
    if (publisher_reactor_thread.joinable()) {
        publisher_reactor_thread.join();
    }
}

// Test 3: two subscribers subscribing from different cursors get
// different replay sets -- each subscriber's cursor is independent.
TEST_F(TopicPubSubTest, TwoSubscribersHaveIndependentCursors) {
    write_topic_wal_records(3); // records with seq_no 1, 2, 3

    // --- Publisher (replay only; no live publish) ---
    const ServiceRegistry publisher_registry;
    auto publisher_reactor = std::make_unique<Reactor>(make_reactor_config(), publisher_registry, logger_->logger);
    publisher_reactor->register_inbound_listener(NetworkEndpointConfiguration{"127.0.0.1", 0}, ThreadID{2});
    auto publisher_thread = ApplicationThread::create<TopicPublisherThread>(logger_->logger, *publisher_reactor, wal_dir_);
    publisher_reactor->register_thread(publisher_thread);
    ThreadWithJoinTimeout publisher_reactor_thread([&]() { publisher_reactor->run(); });
    ASSERT_TRUE(wait_for([&]() { return publisher_reactor->is_initialized(); })) << "Publisher reactor did not initialize";
    const uint16_t listen_port = publisher_reactor->get_inbound_listener_port(0);
    ASSERT_NE(listen_port, 0U);

    // --- sub_a subscribes from 0 (expects 1,2,3); sub_b from 1 (expects 2,3) ---
    ServiceRegistry registry_a;
    registry_a.add("publisher", NetworkEndpointConfiguration{"127.0.0.1", listen_port}, NetworkEndpointConfiguration{});
    auto reactor_a = std::make_unique<Reactor>(make_reactor_config(), registry_a, logger_->logger);
    auto sub_a = ApplicationThread::create<ChannelSubscriberThread>(logger_->logger, *reactor_a, 3, "sub_a", 0);
    reactor_a->register_thread(sub_a);
    ThreadWithJoinTimeout reactor_a_thread([&]() { reactor_a->run(); });

    ServiceRegistry registry_b;
    registry_b.add("publisher", NetworkEndpointConfiguration{"127.0.0.1", listen_port}, NetworkEndpointConfiguration{});
    auto reactor_b = std::make_unique<Reactor>(make_reactor_config(), registry_b, logger_->logger);
    auto sub_b = ApplicationThread::create<ChannelSubscriberThread>(logger_->logger, *reactor_b, 2, "sub_b", 1);
    reactor_b->register_thread(sub_b);
    ThreadWithJoinTimeout reactor_b_thread([&]() { reactor_b->run(); });

    EXPECT_TRUE(wait_for([&]() { return sub_a->all_records_received.load(std::memory_order_acquire); })) << "sub_a incomplete";
    EXPECT_TRUE(wait_for([&]() { return sub_b->all_records_received.load(std::memory_order_acquire); })) << "sub_b incomplete";

    // sub_a from cursor 0 -> seq_nos 1, 2, 3
    ASSERT_EQ(static_cast<int>(sub_a->received_records.size()), 3);
    EXPECT_EQ(sub_a->received_records[0].seq_no, 1);
    EXPECT_EQ(sub_a->received_records[1].seq_no, 2);
    EXPECT_EQ(sub_a->received_records[2].seq_no, 3);

    // sub_b from cursor 1 -> seq_nos 2, 3 only
    ASSERT_EQ(static_cast<int>(sub_b->received_records.size()), 2);
    EXPECT_EQ(sub_b->received_records[0].seq_no, 2);
    EXPECT_EQ(sub_b->received_records[1].seq_no, 3);

    reactor_a->shutdown("test complete");
    reactor_b->shutdown("test complete");
    publisher_reactor->shutdown("test complete");
    if (reactor_a_thread.joinable()) {
        reactor_a_thread.join();
    }
    if (reactor_b_thread.joinable()) {
        reactor_b_thread.join();
    }
    if (publisher_reactor_thread.joinable()) {
        publisher_reactor_thread.join();
    }
}

// Test 4: a subscriber that disconnects mid-stream and reconnects
// resumes exactly at its cursor -- no gap and no duplicate at the seam.
// Same subscriber_id across both sessions (one logical subscriber).
TEST_F(TopicPubSubTest, SubscriberReconnectResumesFromCursorWithoutGap) {
    // Two records exist when session 1 runs; three more are appended before the
    // reconnect. (Replay currently bursts all matching records, so each session
    // must consume exactly what its cursor makes available -- hence appending the
    // new records only after session 1 has finished.)
    write_topic_wal_records(2); // records with seq_no 1, 2

    // --- Publisher stays up across both subscriber sessions ---
    const ServiceRegistry publisher_registry;
    auto publisher_reactor = std::make_unique<Reactor>(make_reactor_config(), publisher_registry, logger_->logger);
    publisher_reactor->register_inbound_listener(NetworkEndpointConfiguration{"127.0.0.1", 0}, ThreadID{2});
    auto publisher_thread = ApplicationThread::create<TopicPublisherThread>(logger_->logger, *publisher_reactor, wal_dir_);
    publisher_reactor->register_thread(publisher_thread);
    ThreadWithJoinTimeout publisher_reactor_thread([&]() { publisher_reactor->run(); });
    ASSERT_TRUE(wait_for([&]() { return publisher_reactor->is_initialized(); })) << "Publisher reactor did not initialize";
    const uint16_t listen_port = publisher_reactor->get_inbound_listener_port(0);
    ASSERT_NE(listen_port, 0U);

    ServiceRegistry subscriber_registry;
    subscriber_registry.add("publisher", NetworkEndpointConfiguration{"127.0.0.1", listen_port}, NetworkEndpointConfiguration{});

    // --- Session 1: subscribe from 0, take 2 records, then disconnect ---
    auto reactor_1 = std::make_unique<Reactor>(make_reactor_config(), subscriber_registry, logger_->logger);
    auto session_1 = ApplicationThread::create<ChannelSubscriberThread>(logger_->logger, *reactor_1, 2, "sub", 0);
    reactor_1->register_thread(session_1);
    ThreadWithJoinTimeout reactor_1_thread([&]() { reactor_1->run(); });
    EXPECT_TRUE(wait_for([&]() { return session_1->all_records_received.load(std::memory_order_acquire); })) << "session 1 incomplete";
    reactor_1->shutdown("session 1 complete");
    if (reactor_1_thread.joinable()) {
        reactor_1_thread.join();
    }

    ASSERT_EQ(static_cast<int>(session_1->received_records.size()), 2);
    const int64_t resume_cursor = session_1->received_records.back().seq_no; // the cursor a real client would persist (== 2)

    // --- New records arrive while the subscriber is away ---
    append_topic_wal_records(3, 5); // WAL now has seq_no 1..5

    // --- Session 2: same subscriber id, resume from the cursor, take the rest ---
    auto reactor_2 = std::make_unique<Reactor>(make_reactor_config(), subscriber_registry, logger_->logger);
    auto session_2 = ApplicationThread::create<ChannelSubscriberThread>(logger_->logger, *reactor_2, 3, "sub", resume_cursor);
    reactor_2->register_thread(session_2);
    ThreadWithJoinTimeout reactor_2_thread([&]() { reactor_2->run(); });
    EXPECT_TRUE(wait_for([&]() { return session_2->all_records_received.load(std::memory_order_acquire); })) << "session 2 incomplete";
    reactor_2->shutdown("session 2 complete");
    if (reactor_2_thread.joinable()) {
        reactor_2_thread.join();
    }

    // Session 1 saw 1,2; session 2 resumes at 3 -- no gap, and record 2 is not re-delivered.
    EXPECT_EQ(session_1->received_records[0].seq_no, 1);
    EXPECT_EQ(session_1->received_records[1].seq_no, 2);
    ASSERT_EQ(static_cast<int>(session_2->received_records.size()), 3);
    EXPECT_EQ(session_2->received_records[0].seq_no, 3);
    EXPECT_EQ(session_2->received_records[1].seq_no, 4);
    EXPECT_EQ(session_2->received_records[2].seq_no, 5);

    publisher_reactor->shutdown("test complete");
    if (publisher_reactor_thread.joinable()) {
        publisher_reactor_thread.join();
    }
}

// Test 5: a subscriber's data channel streams records while its
// control channel receives an out-of-band TopicLagged signal.
TEST_F(TopicPubSubTest, DualChannelStreamsDataAndDeliversControlSignal) {
    static constexpr int record_count = 3;
    write_topic_wal_records(record_count);

    // --- Publisher: send a TopicLagged on the control channel once it is up ---
    const ServiceRegistry publisher_registry;
    auto publisher_reactor = std::make_unique<Reactor>(make_reactor_config(), publisher_registry, logger_->logger);
    publisher_reactor->register_inbound_listener(NetworkEndpointConfiguration{"127.0.0.1", 0}, ThreadID{2});
    auto publisher_thread = ApplicationThread::create<TopicPublisherThread>(logger_->logger, *publisher_reactor, wal_dir_, 0, 0,
                                                                            LagOnControlSubscribeFlag{LagOnControlSubscribeFlag::DoLagOnControlSubscribe});
    publisher_reactor->register_thread(publisher_thread);
    ThreadWithJoinTimeout publisher_reactor_thread([&]() { publisher_reactor->run(); });
    ASSERT_TRUE(wait_for([&]() { return publisher_reactor->is_initialized(); })) << "Publisher reactor did not initialize";
    const uint16_t listen_port = publisher_reactor->get_inbound_listener_port(0);
    ASSERT_NE(listen_port, 0U);

    // --- Subscriber: both channels point at the publisher's single port ---
    ServiceRegistry subscriber_registry;
    subscriber_registry.add("publisher_data", NetworkEndpointConfiguration{"127.0.0.1", listen_port}, NetworkEndpointConfiguration{});
    subscriber_registry.add("publisher_control", NetworkEndpointConfiguration{"127.0.0.1", listen_port}, NetworkEndpointConfiguration{});
    auto subscriber_reactor = std::make_unique<Reactor>(make_reactor_config(), subscriber_registry, logger_->logger);
    auto subscriber_thread = ApplicationThread::create<DualChannelSubscriberThread>(logger_->logger, *subscriber_reactor, "dual", record_count);
    subscriber_reactor->register_thread(subscriber_thread);
    ThreadWithJoinTimeout subscriber_reactor_thread([&]() { subscriber_reactor->run(); });

    EXPECT_TRUE(wait_for([&]() { return subscriber_thread->all_records_received.load(std::memory_order_acquire); })) << "data records not received";
    EXPECT_TRUE(wait_for([&]() { return subscriber_thread->lagged_received.load(std::memory_order_acquire); }))
        << "TopicLagged not received on the control channel";

    // Data arrived on the data channel, in order.
    ASSERT_EQ(static_cast<int>(subscriber_thread->received_records.size()), record_count);
    for (int i = 0; i < record_count; ++i) {
        EXPECT_EQ(subscriber_thread->received_records[static_cast<size_t>(i)].seq_no, i + 1);
    }
    // The control signal arrived on the control channel with the right fields.
    ASSERT_EQ(subscriber_thread->received_lagged.size(), 1U);
    EXPECT_EQ(subscriber_thread->received_lagged[0].reason, "too far behind");
    EXPECT_EQ(subscriber_thread->received_lagged[0].oldest_retained_seq_no, 42);

    subscriber_reactor->shutdown("test complete");
    publisher_reactor->shutdown("test complete");
    if (subscriber_reactor_thread.joinable()) {
        subscriber_reactor_thread.join();
    }
    if (publisher_reactor_thread.joinable()) {
        publisher_reactor_thread.join();
    }
}

// Test 6: the two connections are one logical subscription --
// dropping the data channel tears down the control channel.
TEST_F(TopicPubSubTest, DroppingDataChannelTearsDownControlChannel) {
    static constexpr int record_count = 3;
    write_topic_wal_records(record_count);

    const ServiceRegistry publisher_registry;
    auto publisher_reactor = std::make_unique<Reactor>(make_reactor_config(), publisher_registry, logger_->logger);
    publisher_reactor->register_inbound_listener(NetworkEndpointConfiguration{"127.0.0.1", 0}, ThreadID{2});
    auto publisher_thread = ApplicationThread::create<TopicPublisherThread>(logger_->logger, *publisher_reactor, wal_dir_);
    publisher_reactor->register_thread(publisher_thread);
    ThreadWithJoinTimeout publisher_reactor_thread([&]() { publisher_reactor->run(); });
    ASSERT_TRUE(wait_for([&]() { return publisher_reactor->is_initialized(); })) << "Publisher reactor did not initialize";
    const uint16_t listen_port = publisher_reactor->get_inbound_listener_port(0);
    ASSERT_NE(listen_port, 0U);

    ServiceRegistry subscriber_registry;
    subscriber_registry.add("publisher_data", NetworkEndpointConfiguration{"127.0.0.1", listen_port}, NetworkEndpointConfiguration{});
    subscriber_registry.add("publisher_control", NetworkEndpointConfiguration{"127.0.0.1", listen_port}, NetworkEndpointConfiguration{});
    auto subscriber_reactor = std::make_unique<Reactor>(make_reactor_config(), subscriber_registry, logger_->logger);
    // Disconnect the data channel once all records are in.
    auto subscriber_thread = ApplicationThread::create<DualChannelSubscriberThread>(
        logger_->logger, *subscriber_reactor, "dual", record_count, DisconnectDataWhenDoneFlag{DisconnectDataWhenDoneFlag::DoDisconnectDataWhenDone});
    subscriber_reactor->register_thread(subscriber_thread);
    ThreadWithJoinTimeout subscriber_reactor_thread([&]() { subscriber_reactor->run(); });

    // The publisher tears down the pair, so the subscriber's control connection is closed.
    EXPECT_TRUE(wait_for([&]() { return subscriber_thread->control_channel_lost.load(std::memory_order_acquire); }))
        << "control channel was not torn down when the data channel dropped";

    subscriber_reactor->shutdown("test complete");
    publisher_reactor->shutdown("test complete");
    if (subscriber_reactor_thread.joinable()) {
        subscriber_reactor_thread.join();
    }
    if (publisher_reactor_thread.joinable()) {
        publisher_reactor_thread.join();
    }
}

// Test 7: gap-on-resubscribe -- a request for a cursor older than
// the retention window is clamped forward, and accepted_from_seq_no
// tells the subscriber where the stream now starts.
TEST_F(TopicPubSubTest, GapOnResubscribeClampsTooOldCursor) {
    static constexpr int64_t max_lag = 3;

    // --- Publisher appends 10 records at startup; retention window keeps the last 3 ---
    const ServiceRegistry publisher_registry;
    auto publisher_reactor = std::make_unique<Reactor>(make_reactor_config(), publisher_registry, logger_->logger);
    publisher_reactor->register_inbound_listener(NetworkEndpointConfiguration{"127.0.0.1", 0}, ThreadID{2});
    auto publisher_thread = ApplicationThread::create<TopicPublisherThread>(
        logger_->logger, *publisher_reactor, wal_dir_, 0, 0, LagOnControlSubscribeFlag{LagOnControlSubscribeFlag::DoNotLagOnControlSubscribe}, max_lag,
        /*startup_records=*/10);
    publisher_reactor->register_thread(publisher_thread);
    ThreadWithJoinTimeout publisher_reactor_thread([&]() { publisher_reactor->run(); });
    ASSERT_TRUE(wait_for([&]() { return publisher_reactor->is_initialized(); })) << "Publisher reactor did not initialize";
    const uint16_t listen_port = publisher_reactor->get_inbound_listener_port(0);
    ASSERT_NE(listen_port, 0U);

    // --- Subscriber asks for cursor 0 (everything) but the oldest retained is 7 ---
    ServiceRegistry subscriber_registry;
    subscriber_registry.add("publisher_data", NetworkEndpointConfiguration{"127.0.0.1", listen_port}, NetworkEndpointConfiguration{});
    subscriber_registry.add("publisher_control", NetworkEndpointConfiguration{"127.0.0.1", listen_port}, NetworkEndpointConfiguration{});
    auto subscriber_reactor = std::make_unique<Reactor>(make_reactor_config(), subscriber_registry, logger_->logger);
    auto subscriber_thread = ApplicationThread::create<DualChannelSubscriberThread>(logger_->logger, *subscriber_reactor, "gapper", /*expected=*/3);
    subscriber_reactor->register_thread(subscriber_thread);
    ThreadWithJoinTimeout subscriber_reactor_thread([&]() { subscriber_reactor->run(); });

    EXPECT_TRUE(wait_for([&]() { return subscriber_thread->all_records_received.load(std::memory_order_acquire); })) << "records not received";

    // The subscriber was told it has a gap: accepted = 7 (not the requested 0), and it
    // received records 8, 9, 10 -- the window, not the full history.
    EXPECT_EQ(subscriber_thread->data_accepted_from_seq_no(), 7);
    ASSERT_EQ(static_cast<int>(subscriber_thread->received_records.size()), 3);
    EXPECT_EQ(subscriber_thread->received_records[0].seq_no, 8);
    EXPECT_EQ(subscriber_thread->received_records[1].seq_no, 9);
    EXPECT_EQ(subscriber_thread->received_records[2].seq_no, 10);

    subscriber_reactor->shutdown("test complete");
    publisher_reactor->shutdown("test complete");
    if (subscriber_reactor_thread.joinable()) {
        subscriber_reactor_thread.join();
    }
    if (publisher_reactor_thread.joinable()) {
        publisher_reactor_thread.join();
    }
}

// Test 8: a subscriber whose ack cursor falls out of the retention
// window is dropped with a best-effort TopicLagged, and its pair
// (data + control) is torn down.
TEST_F(TopicPubSubTest, SlowSubscriberIsLaggedOutAndDropped) {
    static constexpr int64_t max_lag = 3;

    // --- Publisher: once both channels are up, append 6 records without the subscriber
    //     ever acking -- so it falls out of the 3-record window and is dropped. ---
    const ServiceRegistry publisher_registry;
    auto publisher_reactor = std::make_unique<Reactor>(make_reactor_config(), publisher_registry, logger_->logger);
    publisher_reactor->register_inbound_listener(NetworkEndpointConfiguration{"127.0.0.1", 0}, ThreadID{2});
    auto publisher_thread = ApplicationThread::create<TopicPublisherThread>(
        logger_->logger, *publisher_reactor, wal_dir_, 0, 0, LagOnControlSubscribeFlag{LagOnControlSubscribeFlag::DoNotLagOnControlSubscribe}, max_lag,
        /*startup_records=*/0, /*records_after_both_channels=*/6);
    publisher_reactor->register_thread(publisher_thread);
    ThreadWithJoinTimeout publisher_reactor_thread([&]() { publisher_reactor->run(); });
    ASSERT_TRUE(wait_for([&]() { return publisher_reactor->is_initialized(); })) << "Publisher reactor did not initialize";
    const uint16_t listen_port = publisher_reactor->get_inbound_listener_port(0);
    ASSERT_NE(listen_port, 0U);

    ServiceRegistry subscriber_registry;
    subscriber_registry.add("publisher_data", NetworkEndpointConfiguration{"127.0.0.1", listen_port}, NetworkEndpointConfiguration{});
    subscriber_registry.add("publisher_control", NetworkEndpointConfiguration{"127.0.0.1", listen_port}, NetworkEndpointConfiguration{});
    auto subscriber_reactor = std::make_unique<Reactor>(make_reactor_config(), subscriber_registry, logger_->logger);
    // expected count is high (never reached); suppress_ack = true -> a stalled consumer.
    auto subscriber_thread = ApplicationThread::create<DualChannelSubscriberThread>(
        logger_->logger, *subscriber_reactor, "slowpoke", /*expected=*/1000,
        DisconnectDataWhenDoneFlag{DisconnectDataWhenDoneFlag::DoNotDisconnectDataWhenDone}, SuppressAckFlag{SuppressAckFlag::DoSuppressAck});
    subscriber_reactor->register_thread(subscriber_thread);
    ThreadWithJoinTimeout subscriber_reactor_thread([&]() { subscriber_reactor->run(); });

    // The lagging subscriber receives a TopicLagged on its control channel and is dropped.
    EXPECT_TRUE(wait_for([&]() { return subscriber_thread->lagged_received.load(std::memory_order_acquire); })) << "TopicLagged not received";
    EXPECT_TRUE(wait_for([&]() {
        return subscriber_thread->control_channel_lost.load(std::memory_order_acquire) && subscriber_thread->data_channel_lost.load(std::memory_order_acquire);
    })) << "the lagging subscriber's connections were not torn down";

    ASSERT_FALSE(subscriber_thread->received_lagged.empty());
    EXPECT_EQ(subscriber_thread->received_lagged[0].reason, "subscriber too far behind the retention window");
    EXPECT_EQ(subscriber_thread->received_lagged[0].oldest_retained_seq_no, 1); // head 4 - window 3

    subscriber_reactor->shutdown("test complete");
    publisher_reactor->shutdown("test complete");
    if (subscriber_reactor_thread.joinable()) {
        subscriber_reactor_thread.join();
    }
    if (publisher_reactor_thread.joinable()) {
        publisher_reactor_thread.join();
    }
}

// Test 9 (F2): a subscriber's periodic TopicAck is a truncation
// cursor -- once it has consumed past a segment, the publisher
// reclaims that segment from disk.
TEST_F(TopicPubSubTest, SubscriberAcksReclaimConsumedWalSegments) {
    static constexpr int record_count = 12;
    static constexpr size_t small_segment = 128; // ~3 records/segment -> several segments

    // --- Publisher appends 12 records to its own (small-segment) WAL at startup ---
    const ServiceRegistry publisher_registry;
    auto publisher_reactor = std::make_unique<Reactor>(make_reactor_config(), publisher_registry, logger_->logger);
    publisher_reactor->register_inbound_listener(NetworkEndpointConfiguration{"127.0.0.1", 0}, ThreadID{2});
    auto publisher_thread = ApplicationThread::create<TopicPublisherThread>(logger_->logger, *publisher_reactor, wal_dir_, 0, 0,
                                                                            LagOnControlSubscribeFlag{LagOnControlSubscribeFlag::DoNotLagOnControlSubscribe}, 0,
                                                                            /*startup_records=*/record_count, /*records_after_both_channels=*/0,
                                                                            /*segment_size=*/small_segment);
    publisher_reactor->register_thread(publisher_thread);
    ThreadWithJoinTimeout publisher_reactor_thread([&]() { publisher_reactor->run(); });
    ASSERT_TRUE(wait_for([&]() { return publisher_reactor->is_initialized(); })) << "Publisher reactor did not initialize";
    const uint16_t listen_port = publisher_reactor->get_inbound_listener_port(0);
    ASSERT_NE(listen_port, 0U);
    // The oldest segment exists before the subscriber consumes.
    ASSERT_TRUE(std::filesystem::exists(wal_dir_ + "/wal_000000.log"));

    ServiceRegistry subscriber_registry;
    subscriber_registry.add("publisher", NetworkEndpointConfiguration{"127.0.0.1", listen_port}, NetworkEndpointConfiguration{});
    auto subscriber_reactor = std::make_unique<Reactor>(make_reactor_config(), subscriber_registry, logger_->logger);
    // disconnect_when_done = false: keep the connection open so the final TopicAck reaches
    // the publisher and drives truncation (with batching, all records + completion can land
    // in one reactor turn, so a self-disconnect would race ahead of the ack).
    auto subscriber_thread = ApplicationThread::create<ChannelSubscriberThread>(logger_->logger, *subscriber_reactor, record_count, "test_subscriber", 0,
                                                                                DisconnectWhenDoneFlag{DisconnectWhenDoneFlag::DoNotDisconnectWhenDone});
    subscriber_reactor->register_thread(subscriber_thread);
    ThreadWithJoinTimeout subscriber_reactor_thread([&]() { subscriber_reactor->run(); });

    // The subscriber receives everything...
    EXPECT_TRUE(wait_for([&]() { return subscriber_thread->all_records_received.load(std::memory_order_acquire); })) << "records not received";
    // ...and its ack lets the publisher reclaim the oldest (consumed) segment from disk.
    EXPECT_TRUE(wait_for([&]() { return !std::filesystem::exists(wal_dir_ + "/wal_000000.log"); })) << "consumed WAL segment was not reclaimed";

    // The small backlog (default page cap 256 > record_count) arrives as ONE batched page;
    // its single mid/end-of-stream ack is what drives the truncation checked above. The
    // multi-page counterpart is MultiplePagesDeliverInOrderAndTruncateProgressively.
    EXPECT_EQ(subscriber_thread->pages_received.load(), 1) << "small backlog should arrive as one batched page";
    ASSERT_EQ(static_cast<int>(subscriber_thread->received_records.size()), record_count);
    for (int i = 0; i < record_count; ++i) {
        EXPECT_EQ(subscriber_thread->received_records[static_cast<size_t>(i)].seq_no, i + 1) << "wrong record at " << i;
    }

    subscriber_reactor->shutdown("test complete");
    publisher_reactor->shutdown("test complete");
    if (subscriber_reactor_thread.joinable()) {
        subscriber_reactor_thread.join();
    }
    if (publisher_reactor_thread.joinable()) {
        publisher_reactor_thread.join();
    }
}

// Test 10 (batching): when many records are already in the WAL, the
// publisher packs them into a SINGLE TopicPage (one page per writable),
// so the subscriber sees exactly one page carrying every record.
TEST_F(TopicPubSubTest, AllAvailableRecordsArriveInOneBatchedPage) {
    static constexpr int record_count = 20;
    write_topic_wal_records(record_count);

    const ServiceRegistry publisher_registry;
    auto publisher_reactor = std::make_unique<Reactor>(make_reactor_config(), publisher_registry, logger_->logger);
    publisher_reactor->register_inbound_listener(NetworkEndpointConfiguration{"127.0.0.1", 0}, ThreadID{2});
    auto publisher_thread = ApplicationThread::create<TopicPublisherThread>(logger_->logger, *publisher_reactor, wal_dir_);
    publisher_reactor->register_thread(publisher_thread);
    ThreadWithJoinTimeout publisher_reactor_thread([&]() { publisher_reactor->run(); });
    ASSERT_TRUE(wait_for([&]() { return publisher_reactor->is_initialized(); })) << "Publisher reactor did not initialize";
    const uint16_t listen_port = publisher_reactor->get_inbound_listener_port(0);
    ASSERT_NE(listen_port, 0U);

    ServiceRegistry subscriber_registry;
    subscriber_registry.add("publisher", NetworkEndpointConfiguration{"127.0.0.1", listen_port}, NetworkEndpointConfiguration{});
    auto subscriber_reactor = std::make_unique<Reactor>(make_reactor_config(), subscriber_registry, logger_->logger);
    auto subscriber_thread = ApplicationThread::create<ChannelSubscriberThread>(logger_->logger, *subscriber_reactor, record_count);
    subscriber_reactor->register_thread(subscriber_thread);
    ThreadWithJoinTimeout subscriber_reactor_thread([&]() { subscriber_reactor->run(); });

    EXPECT_TRUE(wait_for([&]() { return subscriber_thread->all_records_received.load(std::memory_order_acquire); })) << "records not received";

    // Default page cap (256) exceeds record_count, so all 20 records arrive in ONE page.
    EXPECT_EQ(subscriber_thread->pages_received.load(), 1) << "records were not batched into a single page";
    ASSERT_EQ(static_cast<int>(subscriber_thread->received_records.size()), record_count);
    for (int i = 0; i < record_count; ++i) {
        EXPECT_EQ(subscriber_thread->received_records[static_cast<size_t>(i)].seq_no, i + 1) << "wrong/out-of-order record at " << i;
    }

    subscriber_reactor->shutdown("test complete");
    publisher_reactor->shutdown("test complete");
    if (subscriber_reactor_thread.joinable()) {
        subscriber_reactor_thread.join();
    }
    if (publisher_reactor_thread.joinable()) {
        publisher_reactor_thread.join();
    }
}

// Test 11 (batching + F2): with a small per-page cap the records span
// MANY pages. The subscriber acks part-way through the stream (every
// ack_interval records -- well before the last page, so the ack does
// not race any self-disconnect), which reclaims already-consumed WAL
// segments while later pages are still arriving. Exercises both cursor
// resume across page boundaries and progressive mid-stream truncation.
TEST_F(TopicPubSubTest, MultiplePagesDeliverInOrderAndTruncateProgressively) {
    static constexpr int record_count = 12;
    static constexpr size_t small_segment = 128;      // ~3 records/segment -> several segments
    static constexpr size_t max_records_per_page = 3; // force the 12 records across several pages

    const ServiceRegistry publisher_registry;
    auto publisher_reactor = std::make_unique<Reactor>(make_reactor_config(), publisher_registry, logger_->logger);
    publisher_reactor->register_inbound_listener(NetworkEndpointConfiguration{"127.0.0.1", 0}, ThreadID{2});
    auto publisher_thread = ApplicationThread::create<TopicPublisherThread>(logger_->logger, *publisher_reactor, wal_dir_, 0, 0,
                                                                            LagOnControlSubscribeFlag{LagOnControlSubscribeFlag::DoNotLagOnControlSubscribe}, 0,
                                                                            /*startup_records=*/record_count, /*records_after_both_channels=*/0,
                                                                            /*segment_size=*/small_segment, /*max_records_per_page=*/max_records_per_page);
    publisher_reactor->register_thread(publisher_thread);
    ThreadWithJoinTimeout publisher_reactor_thread([&]() { publisher_reactor->run(); });
    ASSERT_TRUE(wait_for([&]() { return publisher_reactor->is_initialized(); })) << "Publisher reactor did not initialize";
    const uint16_t listen_port = publisher_reactor->get_inbound_listener_port(0);
    ASSERT_NE(listen_port, 0U);
    ASSERT_TRUE(std::filesystem::exists(wal_dir_ + "/wal_000000.log"));

    ServiceRegistry subscriber_registry;
    subscriber_registry.add("publisher", NetworkEndpointConfiguration{"127.0.0.1", listen_port}, NetworkEndpointConfiguration{});
    auto subscriber_reactor = std::make_unique<Reactor>(make_reactor_config(), subscriber_registry, logger_->logger);
    auto subscriber_thread = ApplicationThread::create<ChannelSubscriberThread>(logger_->logger, *subscriber_reactor, record_count);
    subscriber_reactor->register_thread(subscriber_thread);
    ThreadWithJoinTimeout subscriber_reactor_thread([&]() { subscriber_reactor->run(); });

    EXPECT_TRUE(wait_for([&]() { return subscriber_thread->all_records_received.load(std::memory_order_acquire); })) << "records not received";
    // A mid-stream ack reclaims the oldest consumed segment even though delivery continues.
    EXPECT_TRUE(wait_for([&]() { return !std::filesystem::exists(wal_dir_ + "/wal_000000.log"); })) << "consumed WAL segment was not reclaimed";

    // The records genuinely spanned multiple pages (ceil(record_count / max_records_per_page)).
    EXPECT_GT(subscriber_thread->pages_received.load(), 1) << "records did not span multiple pages";
    ASSERT_EQ(static_cast<int>(subscriber_thread->received_records.size()), record_count);
    for (int i = 0; i < record_count; ++i) {
        EXPECT_EQ(subscriber_thread->received_records[static_cast<size_t>(i)].seq_no, i + 1) << "wrong/out-of-order record at " << i;
    }

    subscriber_reactor->shutdown("test complete");
    publisher_reactor->shutdown("test complete");
    if (subscriber_reactor_thread.joinable()) {
        subscriber_reactor_thread.join();
    }
    if (publisher_reactor_thread.joinable()) {
        publisher_reactor_thread.join();
    }
}

// Test 12: a subscriber built on the framework TopicSubscriberThread base receives
// replayed records through on_pubsub_message() -- the intended delivery path, with the
// application overriding on_pubsub_message only and handling no PDUs or connections.
TEST_F(TopicPubSubTest, FrameworkSubscriberThreadDeliversRecordsViaOnPubsubMessage) {
    static constexpr int record_count = 3;
    write_topic_wal_records(record_count);

    const ServiceRegistry publisher_registry;
    auto publisher_reactor = std::make_unique<Reactor>(make_reactor_config(), publisher_registry, logger_->logger);
    publisher_reactor->register_inbound_listener(NetworkEndpointConfiguration{"127.0.0.1", 0}, ThreadID{2});
    auto publisher_thread = ApplicationThread::create<TopicPublisherThread>(logger_->logger, *publisher_reactor, wal_dir_);
    publisher_reactor->register_thread(publisher_thread);
    ThreadWithJoinTimeout publisher_reactor_thread([&]() { publisher_reactor->run(); });
    ASSERT_TRUE(wait_for([&]() { return publisher_reactor->is_initialized(); })) << "Publisher reactor did not initialize";
    const uint16_t listen_port = publisher_reactor->get_inbound_listener_port(0);
    ASSERT_NE(listen_port, 0U);

    ServiceRegistry subscriber_registry;
    subscriber_registry.add("publisher", NetworkEndpointConfiguration{"127.0.0.1", listen_port}, NetworkEndpointConfiguration{});
    auto subscriber_reactor = std::make_unique<Reactor>(make_reactor_config(), subscriber_registry, logger_->logger);
    auto subscriber_thread = ApplicationThread::create<CapturingSubscriberThread>(logger_->logger, *subscriber_reactor, record_count);
    subscriber_reactor->register_thread(subscriber_thread);
    ThreadWithJoinTimeout subscriber_reactor_thread([&]() { subscriber_reactor->run(); });

    EXPECT_TRUE(wait_for([&]() { return subscriber_thread->all_records_received.load(std::memory_order_acquire); }))
        << "records were not delivered via on_pubsub_message";

    ASSERT_EQ(static_cast<int>(subscriber_thread->received_records.size()), record_count);
    for (int i = 0; i < record_count; ++i) {
        const auto& rec = subscriber_thread->received_records[static_cast<size_t>(i)];
        EXPECT_EQ(rec.seq_no, i + 1) << "wrong seq_no at " << i;
        EXPECT_EQ(rec.pdu_id, pubsub_itc_fw_app::NewOrderSingle::message_pdu_id) << "wrong pdu_id at " << i;
        ASSERT_EQ(rec.payload.size(), sizeof(uint32_t));
        uint32_t value{};
        std::memcpy(&value, rec.payload.data(), sizeof(value));
        EXPECT_EQ(value, static_cast<uint32_t>((i + 1) * 100)) << "wrong payload at " << i;
    }

    subscriber_reactor->shutdown("test complete");
    publisher_reactor->shutdown("test complete");
    if (subscriber_reactor_thread.joinable()) {
        subscriber_reactor_thread.join();
    }
    if (publisher_reactor_thread.joinable()) {
        publisher_reactor_thread.join();
    }
}

} // namespaces
