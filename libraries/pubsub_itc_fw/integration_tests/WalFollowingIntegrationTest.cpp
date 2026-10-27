// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file WalFollowingIntegrationTest.cpp
 * @brief Followers catch up on a WAL that is being written fast, through the reading thread, and are
 *        handed over to new records without a gap or a repeat.
 *
 * One reactor holds the thread that owns the WAL, which writes records on a recurring timer and sends
 * each handed-over follower its new records itself, and the reading thread. A second reactor holds the
 * followers, which connect once many records are already written, so that they catch up while writing
 * goes on. Each record is a WalAck carrying its own number. Each follower checks, independently of the
 * client, that it accepts every record exactly once and in order. The design is
 * docs/availability/following_the_wal.md.
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/BumpAllocator.hpp>
#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/NetworkEndpointConfiguration.hpp>
#include <pubsub_itc_fw/Reactor.hpp>
#include <pubsub_itc_fw/ReactorConfiguration.hpp>
#include <pubsub_itc_fw/ServiceRegistry.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>
#include <pubsub_itc_fw/ThreadWithJoinTimeout.hpp>
#include <pubsub_itc_fw/TimerType.hpp>
#include <pubsub_itc_fw/Wal.hpp>
#include <pubsub_itc_fw/WalFollowerClient.hpp>
#include <pubsub_itc_fw/WalFollowerService.hpp>
#include <pubsub_itc_fw/WalFollowing.hpp>
#include <pubsub_itc_fw/WalReadingThread.hpp>

#include <pubsub_itc_fw/tests_common/LoggerWithSink.hpp>
#include <pubsub_itc_fw/tests_common/ScratchDirectory.hpp>
#include <pubsub_itc_fw/tests_common/TestConfigurations.hpp>

#include <replication_messages.hpp>

using pubsub_itc_fw::tests_common::LoggerWithSink;
using pubsub_itc_fw::tests_common::make_allocator_config;
using pubsub_itc_fw::tests_common::make_queue_config;

namespace pubsub_itc_fw {

namespace {

constexpr ThreadID owner_thread_id{1};
constexpr ThreadID reading_thread_id{2};
constexpr size_t segment_size = 1U << 20U; // small, so that reading crosses segments as they are made

// The record a follower is sent: a WalAck carrying the record's own number.
void encode_record(int64_t seq_no, std::vector<uint8_t>& out) {
    WalAck record{};
    record.seq_no = seq_no;
    out.resize(fixed_encoded_size(record));
    size_t written = 0;
    size_t needed = 0;
    if (!encode(record, out.data(), out.size(), written, needed)) {
        out.clear();
    }
}

WalRecordConverter record_converter() {
    return [](int64_t seq_no, const WalEntryContents& /*entry*/, std::vector<uint8_t>& out) -> ConvertedRecord {
        encode_record(seq_no, out);
        return ConvertedRecord::send(WalAck::message_pdu_id);
    };
}

// Owns the WAL: writes records_per_tick records on each tick, publishes the positions, and sends each
// new record once, as it passes a limit, to every follower handed over at that moment -- as the thread
// that owns a real WAL does, sending what it releases from memory. A follower that was handed over too
// early, or too late, therefore misses records or is sent them twice. A follower named "written" is
// served up to the last written record, any other up to the last committed, which lags commit_lag
// records behind until writing stops.
class OwnerThread : public ApplicationThread, public WalFollowerLinksInterface, public WalFollowerEventsInterface {
  public:
    std::atomic<int64_t> written{0};
    std::atomic<bool> writing{true};
    std::atomic<int> falling_behind_count{0};
    std::atomic<int64_t> committed{0};
    std::atomic<bool> stopped_writing{false}; // set once writing has stopped, after which written does not change

    OwnerThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor, std::string directory, WalFollowingPositions& positions, int64_t initial_records,
                int records_per_tick, int64_t commit_lag)
        : ApplicationThread(token, logger, reactor, "OwnerThread", owner_thread_id, make_queue_config(), make_allocator_config("OwnerPool"),
                            ApplicationThreadConfiguration{})
        , directory_(std::move(directory))
        , positions_(positions)
        , initial_records_(initial_records)
        , records_per_tick_(records_per_tick)
        , commit_lag_(commit_lag) {}

    // WalFollowerLinksInterface
    void send_to_reading_thread(std::unique_ptr<WalReadingCommand> command) override {
        post_message(reading_thread_id, carry(get_thread_id(), std::move(command)));
    }
    void send_declined(const ConnectionID& to, const WalFollowDeclined& declined) override {
        send_pdu(to, WalFollowDeclined::message_pdu_id, 0, declined);
    }
    void send_caught_up(const ConnectionID& to, const WalFollowCaughtUp& caught_up) override {
        send_pdu(to, WalFollowCaughtUp::message_pdu_id, 0, caught_up);
    }
    void send_leader_changed(const ConnectionID& to, const WalLeaderChanged& changed) override {
        send_pdu(to, WalLeaderChanged::message_pdu_id, 0, changed);
    }

    // WalFollowerEventsInterface
    void follower_handed_over(const ConnectionID& follower, int64_t /*through*/, int64_t /*records_sent*/, WalPosition /*resume_at*/) override {
        // From the next record released after now: the follower holds everything up to the limit now.
        handed_over_[follower.get_value()] = limits_[follower.get_value()];
    }
    void hand_over_falling_behind(const ConnectionID& /*follower*/) override {
        // As the sequencer pauses reading new orders: write nothing for one tick.
        falling_behind_count.fetch_add(1, std::memory_order_acq_rel);
        skip_next_tick_ = true;
    }
    void follower_cannot_be_served(const ConnectionID& /*follower*/, int64_t /*damaged_seq_no*/) override {}
    void declined_while_not_leading(const ConnectionID& /*follower*/) override {}
    void reading_stopped() override {}

  protected:
    void on_initial_event() override {
        wal_.open(directory_, segment_size);
        write(initial_records_);
        service_ = std::make_unique<WalFollowerService>(get_logger(), wal_, positions_, *this, *this);
        static_cast<void>(schedule_timer(std::chrono::microseconds{100}, TimerType(TimerType::Recurring)));
    }

    void on_timer_event(TimerID /*id*/) override {
        if (writing.load(std::memory_order_acquire)) {
            if (skip_next_tick_) {
                skip_next_tick_ = false;
            } else {
                write(records_per_tick_);
            }
        } else {
            publish_committed(last_seq_no_); // nothing more lags once writing stops
            stopped_writing.store(true, std::memory_order_release);
        }
        send_new_records();
    }

    void on_framework_pdu_message(const EventMessage& message) override {
        if (message.pdu_id() != WalFollowRequest::message_pdu_id) {
            return;
        }
        BumpAllocator arena(decode_arena_buffer().data(), decode_arena_buffer().capacity());
        size_t consumed = 0;
        size_t arena_needed = 0;
        WalFollowRequestView request{};
        if (!decode(request, message.payload(), static_cast<size_t>(message.payload_size()), consumed, arena, arena_needed)) {
            return;
        }
        const FollowerLimit limit = request.follower_name == "written" ? FollowerLimit::LastWritten : FollowerLimit::LastCommitted;
        limits_[message.connection_id().get_value()] = limit;
        handed_over_.erase(message.connection_id().get_value());
        service_->on_follow_request(message.connection_id(), request, Leading::Leads, limit, 0, 0);
    }

    void on_itc_message(const EventMessage& message) override {
        // The race the hand-over's comparison exists for, made certain rather than left to timing:
        // before each of the first reports reaches the service, more records are released, as orders
        // keep arriving while the reading thread reports, so that the report falls short.
        if (carries_report(message) && reports_made_short_ < reports_to_make_short && writing.load(std::memory_order_acquire)) {
            ++reports_made_short_;
            write(records_per_tick_);
            send_new_records();
        }
        static_cast<void>(service_->on_inter_thread_message(message));
    }

    void on_connection_lost(const ConnectionID& id, const std::string& /*reason*/) override {
        service_->on_connection_lost(id);
        handed_over_.erase(id.get_value());
    }

  private:
    // Enough that every follower of the test has its first report fall short at least once.
    static constexpr int reports_to_make_short = 4;

    static bool carries_report(const EventMessage& message) {
        if (message.payload() == nullptr || message.payload_size() != static_cast<int>(sizeof(WalReadingReport))) {
            return false;
        }
        uint32_t identity = 0;
        std::memcpy(&identity, message.payload(), sizeof(identity));
        return identity == WalReadingReport::marker;
    }

    void write(int64_t count) {
        const uint8_t byte = 0;
        for (int64_t i = 0; i < count; ++i) {
            ++last_seq_no_;
            wal_.append(last_seq_no_, 1, &byte, 1, 0, 1);
            positions_.publish_written(last_seq_no_, wal_.write_position());
        }
        publish_committed(last_seq_no_ > commit_lag_ ? last_seq_no_ - commit_lag_ : 0);
        written.store(last_seq_no_, std::memory_order_release);
    }

    void publish_committed(int64_t seq_no) {
        committed.store(seq_no, std::memory_order_release);
        positions_.publish_committed(seq_no);
    }

    // Each record that has passed a limit since the last tick is sent, once, to every follower of that
    // limit handed over now.
    void send_new_records() {
        for (const FollowerLimit kind : {FollowerLimit::LastWritten, FollowerLimit::LastCommitted}) {
            int64_t& sent_through = released_through_[static_cast<int>(kind)];
            const int64_t limit = positions_.limit(kind);
            while (sent_through < limit) {
                ++sent_through;
                encode_record(sent_through, encoded_);
                for (const auto& [value, follower_kind] : handed_over_) {
                    if (follower_kind == kind) {
                        send_pdu_payload(ConnectionID{value}, WalAck::message_pdu_id, sent_through, encoded_.data(), encoded_.size());
                    }
                }
            }
        }
    }

    std::string directory_;
    WalFollowingPositions& positions_;
    int64_t initial_records_;
    int records_per_tick_;
    int64_t commit_lag_;
    Wal wal_;
    int64_t last_seq_no_{0};
    bool skip_next_tick_{false};
    int reports_made_short_{0};
    std::unique_ptr<WalFollowerService> service_;
    std::unordered_map<int, FollowerLimit> limits_;
    std::unordered_map<int, FollowerLimit> handed_over_;
    int64_t released_through_[3]{0, 0, 0}; // for each FollowerLimit, the last record sent to its handed-over followers
    std::vector<uint8_t> encoded_;
};

// Follows the WAL through WalFollowerClient, and checks for itself that every record it accepts is the
// one after the last.
class FollowerThread : public ApplicationThread, public WalFollowerClientLinksInterface {
  public:
    std::atomic<int64_t> last_accepted{0};
    std::atomic<int64_t> accepted{0};
    std::atomic<int64_t> out_of_order{0};
    std::atomic<int64_t> gaps{0};
    std::atomic<int64_t> count_mismatches{0};
    std::atomic<int64_t> times_caught_up{0};
    std::atomic<int64_t> first_catch_up_records_sent{-1};
    std::atomic<int64_t> above_limit{0};

    // @p limit is the owner's committed position, which this follower must never be sent past, or null
    // for a follower served up to the last written record. @p start is the last record it holds already.
    FollowerThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor, ThreadID id, const std::string& name, const std::atomic<int64_t>* limit,
                   int64_t start = 0)
        : ApplicationThread(token, logger, reactor, "Follower-" + name, id, make_queue_config(), make_allocator_config("FollowerPool-" + name),
                            ApplicationThreadConfiguration{})
        , last_accepted(start)
        , client_(name, true, start, *this)
        , limit_(limit) {}

    void send_follow_request(const ConnectionID& to, const WalFollowRequest& request) override {
        send_pdu(to, WalFollowRequest::message_pdu_id, 0, request);
    }

  protected:
    void on_initial_event() override {
        connect_to_service("owner");
    }

    void on_connection_established(ConnectionID id) override {
        client_.on_connected(id);
    }

    void on_framework_pdu_message(const EventMessage& message) override {
        BumpAllocator arena(decode_arena_buffer().data(), decode_arena_buffer().capacity());
        size_t consumed = 0;
        size_t arena_needed = 0;
        const size_t size = static_cast<size_t>(message.payload_size());
        if (message.pdu_id() == WalAck::message_pdu_id) {
            WalAckView record{};
            if (!decode(record, message.payload(), size, consumed, arena, arena_needed) || record.seq_no != message.seq_no()) {
                out_of_order.fetch_add(1, std::memory_order_acq_rel);
                return;
            }
            // The committed position only rises, so a record above it as read now was above it when sent.
            if (limit_ != nullptr && record.seq_no > limit_->load(std::memory_order_acquire)) {
                above_limit.fetch_add(1, std::memory_order_acq_rel);
            }
            switch (client_.on_record(message.connection_id(), record.seq_no)) {
                case WalFollowerClient::RecordVerdict::Accept:
                    if (record.seq_no != last_accepted.load(std::memory_order_relaxed) + 1) {
                        out_of_order.fetch_add(1, std::memory_order_acq_rel);
                    }
                    last_accepted.store(record.seq_no, std::memory_order_release);
                    accepted.fetch_add(1, std::memory_order_acq_rel);
                    return;
                case WalFollowerClient::RecordVerdict::AlreadyHeld:
                    return;
                case WalFollowerClient::RecordVerdict::Gap:
                    gaps.fetch_add(1, std::memory_order_acq_rel);
                    return;
                case WalFollowerClient::RecordVerdict::ForAnEarlierRequest:
                    return;
            }
        }
        if (message.pdu_id() == WalFollowStarted::message_pdu_id) {
            WalFollowStartedView started{};
            if (decode(started, message.payload(), size, consumed, arena, arena_needed)) {
                client_.on_started(message.connection_id(), started);
            }
            return;
        }
        if (message.pdu_id() == WalFollowCaughtUp::message_pdu_id) {
            WalFollowCaughtUpView caught_up{};
            if (!decode(caught_up, message.payload(), size, consumed, arena, arena_needed)) {
                return;
            }
            if (client_.on_caught_up(message.connection_id(), caught_up) == WalFollowerClient::CaughtUpVerdict::CaughtUp) {
                if (times_caught_up.fetch_add(1, std::memory_order_acq_rel) == 0) {
                    first_catch_up_records_sent.store(caught_up.records_sent, std::memory_order_release);
                }
            } else {
                count_mismatches.fetch_add(1, std::memory_order_acq_rel);
            }
        }
    }

    void on_itc_message(const EventMessage& /*message*/) override {}

  private:
    WalFollowerClient client_;
    const std::atomic<int64_t>* limit_;
};

// Writes 100 records but publishes a write position covering only the first 50, while publishing a limit
// of 100: as if the other 50 were still being written. The reading thread must send 50 and no more. Then,
// when told, it publishes the rest, and the follower must be sent them too.
class PartlyPublishedOwnerThread : public ApplicationThread, public WalFollowerLinksInterface, public WalFollowerEventsInterface {
  public:
    static constexpr int64_t records = 100;
    static constexpr int64_t published = 50;
    std::atomic<bool> ready{false};
    std::atomic<bool> publish_the_rest{false};

    PartlyPublishedOwnerThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor, std::string directory, WalFollowingPositions& positions)
        : ApplicationThread(token, logger, reactor, "OwnerThread", owner_thread_id, make_queue_config(), make_allocator_config("OwnerPool"),
                            ApplicationThreadConfiguration{})
        , directory_(std::move(directory))
        , positions_(positions) {}

    void send_to_reading_thread(std::unique_ptr<WalReadingCommand> command) override {
        post_message(reading_thread_id, carry(get_thread_id(), std::move(command)));
    }
    void send_declined(const ConnectionID& /*to*/, const WalFollowDeclined& /*declined*/) override {}
    void send_caught_up(const ConnectionID& to, const WalFollowCaughtUp& caught_up) override {
        send_pdu(to, WalFollowCaughtUp::message_pdu_id, 0, caught_up);
    }
    void send_leader_changed(const ConnectionID& /*to*/, const WalLeaderChanged& /*changed*/) override {}
    void follower_handed_over(const ConnectionID& /*follower*/, int64_t /*through*/, int64_t /*records_sent*/, WalPosition /*resume_at*/) override {}
    void hand_over_falling_behind(const ConnectionID& /*follower*/) override {}
    void follower_cannot_be_served(const ConnectionID& /*follower*/, int64_t /*damaged_seq_no*/) override {}
    void declined_while_not_leading(const ConnectionID& /*follower*/) override {}
    void reading_stopped() override {}

  protected:
    void on_initial_event() override {
        wal_.open(directory_, segment_size);
        const uint8_t byte = 0;
        for (int64_t seq_no = 1; seq_no <= records; ++seq_no) {
            wal_.append(seq_no, 1, &byte, 1, 0, 1);
            if (seq_no == published) {
                positions_.publish_written(seq_no, wal_.write_position());
            }
        }
        end_ = wal_.write_position();
        positions_.publish_committed(records); // the limit, deliberately ahead of the published write position
        service_ = std::make_unique<WalFollowerService>(get_logger(), wal_, positions_, *this, *this);
        static_cast<void>(schedule_timer(std::chrono::microseconds{1000}, TimerType(TimerType::Recurring)));
        ready.store(true, std::memory_order_release);
    }

    void on_timer_event(TimerID /*id*/) override {
        if (publish_the_rest.load(std::memory_order_acquire) && !published_the_rest_) {
            published_the_rest_ = true;
            positions_.publish_written(records, end_);
            // Asked again, the reading thread reads on from where it stopped.
            WalFollowRequestView request{};
            request.follower_name = "follower";
            request.last_seq_no = published;
            request.wants_new_records = false;
            // The number of the follower's own request, so that its client takes this answer as the answer to it.
            request.request_number = follower_request_number_;
            service_->on_follow_request(follower_, request, Leading::Leads, FollowerLimit::LastCommitted, 0, 0);
        }
    }

    void on_framework_pdu_message(const EventMessage& message) override {
        if (message.pdu_id() != WalFollowRequest::message_pdu_id) {
            return;
        }
        BumpAllocator arena(decode_arena_buffer().data(), decode_arena_buffer().capacity());
        size_t consumed = 0;
        size_t arena_needed = 0;
        WalFollowRequestView request{};
        if (!decode(request, message.payload(), static_cast<size_t>(message.payload_size()), consumed, arena, arena_needed)) {
            return;
        }
        follower_ = message.connection_id();
        follower_request_number_ = request.request_number;
        service_->on_follow_request(message.connection_id(), request, Leading::Leads, FollowerLimit::LastCommitted, 0, 0);
    }

    void on_itc_message(const EventMessage& message) override {
        static_cast<void>(service_->on_inter_thread_message(message));
    }

  private:
    std::string directory_;
    WalFollowingPositions& positions_;
    Wal wal_;
    WalPosition end_{};
    bool published_the_rest_{false};
    ConnectionID follower_;
    int64_t follower_request_number_{0};
    std::unique_ptr<WalFollowerService> service_;
};

// Writes 100 records and publishes them all; its reading thread finds record 60 damaged
// (damaging_converter). A follower asking from before it must be sent the records before it and never
// told it caught up; one asking from 60 or later must be served and handed over as usual.
class DamagedRecordOwnerThread : public ApplicationThread, public WalFollowerLinksInterface, public WalFollowerEventsInterface {
  public:
    static constexpr int64_t records = 100;
    static constexpr int64_t damaged_record = 60;
    std::atomic<bool> ready{false};
    std::atomic<int64_t> cannot_be_served_count{0};
    std::atomic<int64_t> damaged_seq_no_reported{0};
    std::atomic<int64_t> handed_over_count{0};

    DamagedRecordOwnerThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor, std::string directory, WalFollowingPositions& positions)
        : ApplicationThread(token, logger, reactor, "OwnerThread", owner_thread_id, make_queue_config(), make_allocator_config("OwnerPool"),
                            ApplicationThreadConfiguration{})
        , directory_(std::move(directory))
        , positions_(positions) {}

    void send_to_reading_thread(std::unique_ptr<WalReadingCommand> command) override {
        post_message(reading_thread_id, carry(get_thread_id(), std::move(command)));
    }
    void send_declined(const ConnectionID& /*to*/, const WalFollowDeclined& /*declined*/) override {}
    void send_caught_up(const ConnectionID& to, const WalFollowCaughtUp& caught_up) override {
        send_pdu(to, WalFollowCaughtUp::message_pdu_id, 0, caught_up);
    }
    void send_leader_changed(const ConnectionID& /*to*/, const WalLeaderChanged& /*changed*/) override {}
    void follower_handed_over(const ConnectionID& /*follower*/, int64_t /*through*/, int64_t /*records_sent*/, WalPosition /*resume_at*/) override {
        handed_over_count.fetch_add(1, std::memory_order_acq_rel);
    }
    void hand_over_falling_behind(const ConnectionID& /*follower*/) override {}
    void follower_cannot_be_served(const ConnectionID& /*follower*/, int64_t damaged_seq_no) override {
        damaged_seq_no_reported.store(damaged_seq_no, std::memory_order_release);
        cannot_be_served_count.fetch_add(1, std::memory_order_acq_rel);
    }
    void declined_while_not_leading(const ConnectionID& /*follower*/) override {}
    void reading_stopped() override {}

  protected:
    void on_initial_event() override {
        wal_.open(directory_, segment_size);
        const uint8_t byte = 0;
        for (int64_t seq_no = 1; seq_no <= records; ++seq_no) {
            wal_.append(seq_no, 1, &byte, 1, 0, 1);
        }
        positions_.publish_written(records, wal_.write_position());
        positions_.publish_committed(records);
        service_ = std::make_unique<WalFollowerService>(get_logger(), wal_, positions_, *this, *this);
        ready.store(true, std::memory_order_release);
    }

    void on_framework_pdu_message(const EventMessage& message) override {
        if (message.pdu_id() != WalFollowRequest::message_pdu_id) {
            return;
        }
        BumpAllocator arena(decode_arena_buffer().data(), decode_arena_buffer().capacity());
        size_t consumed = 0;
        size_t arena_needed = 0;
        WalFollowRequestView request{};
        if (!decode(request, message.payload(), static_cast<size_t>(message.payload_size()), consumed, arena, arena_needed)) {
            return;
        }
        service_->on_follow_request(message.connection_id(), request, Leading::Leads, FollowerLimit::LastCommitted, 0, 0);
    }

    void on_itc_message(const EventMessage& message) override {
        static_cast<void>(service_->on_inter_thread_message(message));
    }

  private:
    std::string directory_;
    WalFollowingPositions& positions_;
    Wal wal_;
    std::unique_ptr<WalFollowerService> service_;
};

// As record_converter, but finds DamagedRecordOwnerThread::damaged_record damaged.
WalRecordConverter damaging_converter() {
    return [](int64_t seq_no, const WalEntryContents& entry, std::vector<uint8_t>& out) -> ConvertedRecord {
        if (seq_no == DamagedRecordOwnerThread::damaged_record) {
            return ConvertedRecord::damaged();
        }
        return record_converter()(seq_no, entry, out);
    };
}

bool wait_for(const std::function<bool()>& condition, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!condition()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

ReactorConfiguration reactor_configuration() {
    ReactorConfiguration configuration{};
    configuration.inactivity_check_interval_ = std::chrono::milliseconds(100);
    configuration.init_phase_timeout_ = std::chrono::milliseconds(10000);
    configuration.shutdown_timeout_ = std::chrono::milliseconds(1000);
    configuration.connect_timeout = std::chrono::milliseconds(2000);
    return configuration;
}

} // un-named namespace

TEST(WalFollowingIntegrationTest, FollowersCatchUpWhileTheWalIsWrittenAndAreHandedOverWithoutAGapOrARepeat) {
#ifdef USING_VALGRIND
    constexpr int64_t initial_records = 5000;
    constexpr int64_t write_until = 20000;
    const std::chrono::milliseconds timeout{120000};
#else
    constexpr int64_t initial_records = 20000;
    constexpr int64_t write_until = 80000;
    const std::chrono::milliseconds timeout{30000};
#endif
    constexpr int records_per_tick = 5;
    constexpr int64_t commit_lag = 25;

    LoggerWithSink logger(FwLogLevel::Info);
    const std::string directory = tests_common::make_scratch_directory("wal_following");
    WalFollowingPositions positions;

    const ServiceRegistry owner_registry;
    auto owner_reactor = std::make_unique<Reactor>(reactor_configuration(), owner_registry, logger.logger);
    owner_reactor->register_inbound_listener(NetworkEndpointConfiguration{"127.0.0.1", 0}, owner_thread_id);
    auto owner = ApplicationThread::create<OwnerThread>(logger.logger, *owner_reactor, directory, positions, initial_records, records_per_tick, commit_lag);
    auto reading = ApplicationThread::create<WalReadingThread>(logger.logger, *owner_reactor, "WalReadingThread", reading_thread_id, make_queue_config(),
                                                               make_allocator_config("ReadingPool"), owner_thread_id, directory, positions,
                                                               std::vector<WalRecordConverter>{record_converter()});
    owner_reactor->register_thread(owner);
    owner_reactor->register_thread(reading);
    ThreadWithJoinTimeout owner_reactor_thread([&]() { owner_reactor->run(); });
    ASSERT_TRUE(wait_for([&]() { return owner_reactor->is_initialized(); }, timeout));
    ASSERT_TRUE(wait_for([&]() { return owner->written.load(std::memory_order_acquire) >= initial_records; }, timeout));
    const uint16_t port = owner_reactor->get_inbound_listener_port(0);

    ServiceRegistry follower_registry;
    follower_registry.add("owner", NetworkEndpointConfiguration{"127.0.0.1", port}, NetworkEndpointConfiguration{});
    auto follower_reactor = std::make_unique<Reactor>(reactor_configuration(), follower_registry, logger.logger);
    auto served_written = ApplicationThread::create<FollowerThread>(logger.logger, *follower_reactor, ThreadID{1}, "written", nullptr);
    auto served_committed = ApplicationThread::create<FollowerThread>(logger.logger, *follower_reactor, ThreadID{2}, "committed", &owner->committed);
    follower_reactor->register_thread(served_written);
    follower_reactor->register_thread(served_committed);
    ThreadWithJoinTimeout follower_reactor_thread([&]() { follower_reactor->run(); });

    // Both catch up while writing goes on, and go on receiving new records once handed over.
    ASSERT_TRUE(wait_for([&]() { return owner->written.load(std::memory_order_acquire) >= write_until; }, timeout));
    owner->writing.store(false, std::memory_order_release);
    ASSERT_TRUE(wait_for([&]() { return owner->stopped_writing.load(std::memory_order_acquire); }, timeout));
    const int64_t final_seq_no = owner->written.load(std::memory_order_acquire);
    for (const auto& follower : {served_written, served_committed}) {
        EXPECT_TRUE(wait_for([&]() { return follower->last_accepted.load(std::memory_order_acquire) == final_seq_no; }, timeout))
            << "a follower reached " << follower->last_accepted.load() << " of " << final_seq_no;
        EXPECT_EQ(follower->accepted.load(), final_seq_no) << "every record accepted exactly once";
        EXPECT_EQ(follower->out_of_order.load(), 0);
        EXPECT_EQ(follower->gaps.load(), 0);
        EXPECT_EQ(follower->count_mismatches.load(), 0);
        EXPECT_EQ(follower->above_limit.load(), 0) << "a follower was sent a record above its limit";
        EXPECT_GE(follower->times_caught_up.load(), 1);
        // The catch-up came from the reading thread, and was a real one.
        EXPECT_GE(follower->first_catch_up_records_sent.load(), initial_records);
    }

    follower_reactor->shutdown("test complete");
    owner_reactor->shutdown("test complete");
    if (follower_reactor_thread.joinable()) {
        follower_reactor_thread.join();
    }
    if (owner_reactor_thread.joinable()) {
        owner_reactor_thread.join();
    }
    tests_common::remove_scratch_directory(directory);
}

// The reading thread reads only before the write position the owning thread has published, so that it
// reads only records completely written, even when the follower's limit is further on.
TEST(WalFollowingIntegrationTest, TheReadingThreadNeverReadsPastThePublishedWritePosition) {
    const std::chrono::milliseconds timeout{30000};
    LoggerWithSink logger(FwLogLevel::Info);
    const std::string directory = tests_common::make_scratch_directory("wal_following_partly_published");
    WalFollowingPositions positions;

    const ServiceRegistry owner_registry;
    auto owner_reactor = std::make_unique<Reactor>(reactor_configuration(), owner_registry, logger.logger);
    owner_reactor->register_inbound_listener(NetworkEndpointConfiguration{"127.0.0.1", 0}, owner_thread_id);
    auto owner = ApplicationThread::create<PartlyPublishedOwnerThread>(logger.logger, *owner_reactor, directory, positions);
    auto reading = ApplicationThread::create<WalReadingThread>(logger.logger, *owner_reactor, "WalReadingThread", reading_thread_id, make_queue_config(),
                                                               make_allocator_config("ReadingPool"), owner_thread_id, directory, positions,
                                                               std::vector<WalRecordConverter>{record_converter()});
    owner_reactor->register_thread(owner);
    owner_reactor->register_thread(reading);
    ThreadWithJoinTimeout owner_reactor_thread([&]() { owner_reactor->run(); });
    // The reactor sets up its listeners after the threads' initial events, so the port is read only once
    // it says it is initialized.
    ASSERT_TRUE(wait_for([&]() { return owner->ready.load(std::memory_order_acquire) && owner_reactor->is_initialized(); }, timeout));
    const uint16_t port = owner_reactor->get_inbound_listener_port(0);

    ServiceRegistry follower_registry;
    follower_registry.add("owner", NetworkEndpointConfiguration{"127.0.0.1", port}, NetworkEndpointConfiguration{});
    auto follower_reactor = std::make_unique<Reactor>(reactor_configuration(), follower_registry, logger.logger);
    auto follower = ApplicationThread::create<FollowerThread>(logger.logger, *follower_reactor, ThreadID{1}, "follower", nullptr);
    follower_reactor->register_thread(follower);
    ThreadWithJoinTimeout follower_reactor_thread([&]() { follower_reactor->run(); });

    // Sent the records before the published position, and told it has caught up there.
    ASSERT_TRUE(wait_for([&]() { return follower->times_caught_up.load(std::memory_order_acquire) >= 1; }, timeout));
    std::this_thread::sleep_for(std::chrono::milliseconds(200)); // time for anything more to arrive, which must not
    EXPECT_EQ(follower->last_accepted.load(), PartlyPublishedOwnerThread::published) << "records past the published write position were read";

    // Once the rest is published, it is sent too.
    owner->publish_the_rest.store(true, std::memory_order_release);
    EXPECT_TRUE(wait_for([&]() { return follower->last_accepted.load(std::memory_order_acquire) == PartlyPublishedOwnerThread::records; }, timeout))
        << "reached " << follower->last_accepted.load();
    EXPECT_EQ(follower->out_of_order.load(), 0);
    EXPECT_EQ(follower->gaps.load(), 0);

    follower_reactor->shutdown("test complete");
    owner_reactor->shutdown("test complete");
    if (follower_reactor_thread.joinable()) {
        follower_reactor_thread.join();
    }
    if (owner_reactor_thread.joinable()) {
        owner_reactor_thread.join();
    }
    tests_common::remove_scratch_directory(directory);
}

// A record the reading thread cannot read may have been one the follower needed, so a follower asking from
// before it is sent the records before it and is never told it caught up, nor handed over; the owning
// thread is told which record it was. A follower asking from that record or later is served as usual.
TEST(WalFollowingIntegrationTest, AFollowerIsNeverHandedOverPastADamagedRecordButOneAlreadyPastItIs) {
    const std::chrono::milliseconds timeout{30000};
    LoggerWithSink logger(FwLogLevel::Info);
    const std::string directory = tests_common::make_scratch_directory("wal_following_damaged_record");
    WalFollowingPositions positions;

    const ServiceRegistry owner_registry;
    auto owner_reactor = std::make_unique<Reactor>(reactor_configuration(), owner_registry, logger.logger);
    owner_reactor->register_inbound_listener(NetworkEndpointConfiguration{"127.0.0.1", 0}, owner_thread_id);
    auto owner = ApplicationThread::create<DamagedRecordOwnerThread>(logger.logger, *owner_reactor, directory, positions);
    auto reading = ApplicationThread::create<WalReadingThread>(logger.logger, *owner_reactor, "WalReadingThread", reading_thread_id, make_queue_config(),
                                                               make_allocator_config("ReadingPool"), owner_thread_id, directory, positions,
                                                               std::vector<WalRecordConverter>{damaging_converter()});
    owner_reactor->register_thread(owner);
    owner_reactor->register_thread(reading);
    ThreadWithJoinTimeout owner_reactor_thread([&]() { owner_reactor->run(); });
    ASSERT_TRUE(wait_for([&]() { return owner->ready.load(std::memory_order_acquire) && owner_reactor->is_initialized(); }, timeout));
    const uint16_t port = owner_reactor->get_inbound_listener_port(0);

    ServiceRegistry follower_registry;
    follower_registry.add("owner", NetworkEndpointConfiguration{"127.0.0.1", port}, NetworkEndpointConfiguration{});
    auto follower_reactor = std::make_unique<Reactor>(reactor_configuration(), follower_registry, logger.logger);
    auto before = ApplicationThread::create<FollowerThread>(logger.logger, *follower_reactor, ThreadID{1}, "before", nullptr, int64_t{0});
    auto past = ApplicationThread::create<FollowerThread>(logger.logger, *follower_reactor, ThreadID{2}, "past", nullptr,
                                                          int64_t{DamagedRecordOwnerThread::damaged_record});
    follower_reactor->register_thread(before);
    follower_reactor->register_thread(past);
    ThreadWithJoinTimeout follower_reactor_thread([&]() { follower_reactor->run(); });

    // Waits from here on are EXPECTs, not ASSERTs, so that a failure still reaches the shutdown below: an
    // ASSERT would return with both reactors running, and their threads would crash as the test ends.
    // The follower already past the damaged record is served the rest, and handed over.
    EXPECT_TRUE(wait_for([&]() { return past->times_caught_up.load(std::memory_order_acquire) >= 1; }, timeout));
    EXPECT_EQ(past->last_accepted.load(), DamagedRecordOwnerThread::records);
    EXPECT_EQ(past->out_of_order.load(), 0);

    // The follower from before it is sent every record before it, and the owner is told which record could not be read.
    EXPECT_TRUE(wait_for([&]() { return owner->cannot_be_served_count.load(std::memory_order_acquire) >= 1; }, timeout));
    EXPECT_EQ(owner->damaged_seq_no_reported.load(), DamagedRecordOwnerThread::damaged_record);
    std::this_thread::sleep_for(std::chrono::milliseconds(300)); // time for anything more to arrive, which must not
    EXPECT_EQ(before->last_accepted.load(), DamagedRecordOwnerThread::damaged_record - 1);
    EXPECT_EQ(before->times_caught_up.load(), 0) << "a follower was told it caught up past a record that could not be read";
    EXPECT_EQ(owner->handed_over_count.load(), 1) << "only the follower already past the damaged record may be handed over";

    follower_reactor->shutdown("test complete");
    owner_reactor->shutdown("test complete");
    if (follower_reactor_thread.joinable()) {
        follower_reactor_thread.join();
    }
    if (owner_reactor_thread.joinable()) {
        owner_reactor_thread.join();
    }
    tests_common::remove_scratch_directory(directory);
}

} // namespaces
