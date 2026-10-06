// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

/*
 * Integration, fairness and soak tests for the queue of waiting sends each connection has
 * (WaitingSendQueue), and for closing a connection whose peer does not read (docs/bug_list.md,
 * BUG-0112).
 *
 * When a connection's socket cannot take a send at once, the rest of the send is written when the
 * socket has room, and further sends for that connection wait in the connection's own queue. A peer
 * that reads slowly, or not at all, must hold up only the sends to itself. Every other connection of
 * the same process must go on being served, and so must the process's timers. A connection whose
 * queue reaches its limit is closed, and its application told the connection was lost.
 *
 * In every test one sender thread, on one reactor, sends numbered PDUs to several receivers in turn,
 * one PDU to each receiver per round, at a fixed number of rounds per millisecond, from its own timer. Each
 * receiver runs on its own reactor and records what arrives. A receiver made to stop reading pauses
 * reading on its connection, as an application would; the kernel's buffers for that connection then
 * fill, and the sender's sends to it start to wait. The sender's socket send buffers are 64 KiB, so
 * that this happens after a few thousand PDUs rather than tens of thousands.
 *
 * The receivers keep the operating system's default receive buffer. A receive buffer much smaller
 * than the largest segment loopback carries, which is about 64 KiB, stops the receiver telling the
 * sender promptly that it has room again after a stop, and the sender's kernel then waits for its own
 * 200-millisecond timer before trying again. A receiver that reads again would then catch up at about
 * a thousand PDUs a second, which is the kernel's behaviour and not what is being tested.
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/ApplicationThreadConfiguration.hpp>
#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/NetworkEndpointConfiguration.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/Reactor.hpp>
#include <pubsub_itc_fw/ReactorConfiguration.hpp>
#include <pubsub_itc_fw/ServiceRegistry.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>

#include <pubsub_itc_fw/tests_common/LoggerWithSink.hpp>
#include <pubsub_itc_fw/tests_common/TestConfigurations.hpp>

#include <leader_follower.hpp>

using pubsub_itc_fw::tests_common::LoggerWithSink;
using pubsub_itc_fw::tests_common::make_allocator_config;
using pubsub_itc_fw::tests_common::make_queue_config;

namespace pubsub_itc_fw::tests {

namespace {

constexpr uint16_t any_os_assigned_port = 0;
constexpr int small_send_buffer = 64 * 1024;

ReactorConfiguration make_reactor_config() {
    ReactorConfiguration cfg{};
    cfg.inactivity_check_interval_ = std::chrono::milliseconds(100);
    cfg.init_phase_timeout_ = std::chrono::milliseconds(5000);
    cfg.shutdown_timeout_ = std::chrono::milliseconds(1000);
    cfg.connect_timeout = std::chrono::milliseconds(2000);
    cfg.metrics_configuration.enabled = false;
    cfg.socket_send_buffer_size = small_send_buffer;
    // The receivers send nothing back, and the sender must not close their connections for that
    // during a long test.
    cfg.socket_maximum_inactivity_interval_ = std::chrono::minutes{10};
    return cfg;
}

std::string receiver_service_name(size_t index) {
    return "receiver" + std::to_string(index);
}

// Sends PDUs numbered 1 to `count` to every connection, in rounds of one PDU to each connection,
// `rounds_per_ms` rounds for every millisecond since it started. It sends from its one-millisecond
// timer, and on each tick sends as many rounds as bring it up to date: the reactor passes on one tick
// however many milliseconds have passed since the last, so counting ticks would send more slowly the
// busier the reactor was. It starts once `connections` connections are established. A connection
// that is lost is left out of later rounds.
//
// Each PDU is a WalAck, a few dozen bytes, unless `padding` is given, in which case each is an
// UndeliveredReportsRequest carrying that many bytes, so that fewer PDUs fill the kernel's buffers.
//
// It either listens for its receivers, or connects to them itself, as receiver0, receiver1 and so on.
class RoundRobinSender : public ApplicationThread {
  public:
    RoundRobinSender(ConstructorToken token, QuillLogger& logger, Reactor& reactor, size_t connections, int count, int rounds_per_ms, bool connect_out,
                     size_t padding = 0)
        : ApplicationThread(token, logger, reactor, "RoundRobinSender", ThreadID{1}, make_queue_config(), make_allocator_config("RoundRobinSenderPool"),
                            ApplicationThreadConfiguration{})
        , expected_connections_(connections)
        , count_(count)
        , rounds_per_ms_(rounds_per_ms)
        , connect_out_(connect_out)
        , padding_(padding, 'x') {}

    std::atomic<int> connections_established{0};
    std::atomic<int> connections_lost{0};
    std::atomic<int> highest_sent{0};
    std::atomic<bool> all_sent{false};

  protected:
    void on_app_ready_event() override {
        timer_ = start_recurring_timer(std::chrono::milliseconds{1});
        if (connect_out_) {
            for (size_t index = 0; index < expected_connections_; ++index) {
                connect_to_service(receiver_service_name(index));
            }
        }
    }

    void on_connection_established(ConnectionID id) override {
        live_.push_back(id);
        connections_established.fetch_add(1);
    }

    void on_connection_lost(const ConnectionID& id, const std::string&) override {
        live_.erase(std::remove_if(live_.begin(), live_.end(), [&id](const ConnectionID& live) { return live.get_value() == id.get_value(); }), live_.end());
        connections_lost.fetch_add(1);
    }

    void on_timer_event(TimerID id) override {
        if (id != timer_) {
            return;
        }
        if (static_cast<size_t>(connections_established.load()) < expected_connections_ || next_ > count_) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (started_at_ == std::chrono::steady_clock::time_point{}) {
            started_at_ = now;
        }
        const int64_t elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - started_at_).count() + 1;
        const int64_t due = std::min<int64_t>(count_, elapsed_ms * rounds_per_ms_);
        pubsub_itc_fw_app::WalAck message{};
        pubsub_itc_fw_app::UndeliveredReportsRequest padded{};
        padded.comp_id = padding_;
        for (; next_ <= due; ++next_) {
            message.seq_no = next_;
            for (const ConnectionID& connection : live_) {
                if (padding_.empty()) {
                    send_pdu(connection, pubsub_itc_fw_app::WalAck::message_pdu_id, static_cast<int64_t>(next_), message);
                } else {
                    send_pdu(connection, pubsub_itc_fw_app::UndeliveredReportsRequest::message_pdu_id, static_cast<int64_t>(next_), padded);
                }
            }
        }
        highest_sent.store(next_ - 1);
        if (next_ > count_) {
            all_sent.store(true);
        }
    }

    void on_connection_failed(const std::string&) override {}
    void on_raw_socket_message(const EventMessage&) override {}
    void on_framework_pdu_message(const EventMessage& message) override {
        release_pdu_payload(message);
    }
    void on_itc_message(const EventMessage&) override {}

  private:
    size_t expected_connections_;
    int count_;
    int rounds_per_ms_;
    std::chrono::steady_clock::time_point started_at_{};
    bool connect_out_;
    std::string padding_;
    TimerID timer_{};
    int next_{1};
    std::vector<ConnectionID> live_;
};

// Sends PDUs numbered 1 to `count` on the first connection established, one at a time: after each, it
// asks to be told when the connection can take another, and sends the next only when told. This is how
// the topic publisher paces itself to a subscriber, so that a slow subscriber's backlog stays in the
// log and not in the publisher's memory.
class PacedSender : public ApplicationThread {
  public:
    PacedSender(ConstructorToken token, QuillLogger& logger, Reactor& reactor, int count)
        : ApplicationThread(token, logger, reactor, "PacedSender", ThreadID{1}, make_queue_config(), make_allocator_config("PacedSenderPool"),
                            ApplicationThreadConfiguration{})
        , count_(count) {}

    std::atomic<int> sent{0};
    std::atomic<int> connections_lost{0};

  protected:
    void on_connection_established(ConnectionID id) override {
        if (!connection_.is_valid()) {
            connection_ = id;
            send_next();
        }
    }
    void on_connection_writable(ConnectionID) override {
        send_next();
    }
    void on_connection_lost(const ConnectionID&, const std::string&) override {
        connections_lost.fetch_add(1);
    }
    void on_connection_failed(const std::string&) override {}
    void on_raw_socket_message(const EventMessage&) override {}
    void on_framework_pdu_message(const EventMessage& message) override {
        release_pdu_payload(message);
    }
    void on_itc_message(const EventMessage&) override {}
    void on_timer_event(TimerID) override {}

  private:
    void send_next() {
        const int next = sent.load() + 1;
        if (next > count_) {
            return;
        }
        pubsub_itc_fw_app::WalAck message{};
        message.seq_no = next;
        send_pdu(connection_, pubsub_itc_fw_app::WalAck::message_pdu_id, static_cast<int64_t>(next), message);
        sent.store(next);
        request_writable_notification(connection_);
    }

    int count_;
    ConnectionID connection_{};
};

// Counts the ticks of a one-millisecond timer, and records the longest gap between two, and does nothing
// else, so that what it records shows how well its reactor serves timers, undisturbed by any work of the
// thread's own. The reactor reads a timer's descriptor and passes on one tick however many intervals
// have passed since it last read it, so the gap between ticks is how long the reactor took to come back
// to its timers.
class TickCounter : public ApplicationThread {
  public:
    TickCounter(ConstructorToken token, QuillLogger& logger, Reactor& reactor)
        : ApplicationThread(token, logger, reactor, "TickCounter", ThreadID{2}, make_queue_config(), make_allocator_config("TickCounterPool"),
                            ApplicationThreadConfiguration{}) {}

    std::atomic<int> ticks{0};

    /// Starts recording the longest gap afresh, from now.
    void restart_gap() {
        restart_requested_.store(true);
    }

    [[nodiscard]] std::chrono::microseconds longest_gap() const {
        return std::chrono::microseconds{longest_gap_us_.load()};
    }

  protected:
    void on_app_ready_event() override {
        timer_ = start_recurring_timer(std::chrono::milliseconds{1});
    }
    void on_timer_event(TimerID id) override {
        if (id != timer_) {
            return;
        }
        ticks.fetch_add(1);
        const auto now = std::chrono::steady_clock::now();
        if (restart_requested_.exchange(false)) {
            longest_gap_us_.store(0);
        } else if (last_tick_ != std::chrono::steady_clock::time_point{}) {
            const int64_t gap_us = std::chrono::duration_cast<std::chrono::microseconds>(now - last_tick_).count();
            if (gap_us > longest_gap_us_.load()) {
                longest_gap_us_.store(gap_us);
            }
        }
        last_tick_ = now;
    }
    void on_connection_established(ConnectionID) override {}
    void on_connection_failed(const std::string&) override {}
    void on_connection_lost(const ConnectionID&, const std::string&) override {}
    void on_raw_socket_message(const EventMessage&) override {}
    void on_framework_pdu_message(const EventMessage& message) override {
        release_pdu_payload(message);
    }
    void on_itc_message(const EventMessage&) override {}

  private:
    TimerID timer_{};
    std::chrono::steady_clock::time_point last_tick_{};
    std::atomic<bool> restart_requested_{false};
    std::atomic<int64_t> longest_gap_us_{0};
};

// When a receiver stops reading, and for how long.
struct ReadingPlan {
    int stop_after{0};                     ///< stop reading once this many PDUs have arrived; 0 never stops
    std::chrono::milliseconds stop_for{0}; ///< how long each stop lasts
    std::chrono::milliseconds read_for{0}; ///< for repeated stops: how long reading runs between them; 0 stops once
};

// Records every PDU that arrives on its one connection, and stops reading as its plan says. It either
// connects to the sender, or listens for it.
class Receiver : public ApplicationThread {
  public:
    Receiver(ConstructorToken token, QuillLogger& logger, Reactor& reactor, ThreadID id, ReadingPlan plan, int expected, bool connect_out)
        : ApplicationThread(token, logger, reactor, "Receiver" + std::to_string(id.get_value()), id, make_queue_config(),
                            make_allocator_config("ReceiverPool" + std::to_string(id.get_value())), ApplicationThreadConfiguration{})
        , plan_(plan)
        , expected_(expected)
        , connect_out_(connect_out) {}

    std::atomic<bool> established{false};
    std::atomic<bool> stopped_now{false};
    std::atomic<bool> lost{false};
    std::atomic<int> stops{0};

    [[nodiscard]] std::vector<int64_t> received() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return received_;
    }

    [[nodiscard]] size_t count() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return received_.size();
    }

    /// When the last expected PDU arrived; the epoch if it has not.
    [[nodiscard]] std::chrono::steady_clock::time_point completed_at() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return completed_at_;
    }

  protected:
    void on_app_ready_event() override {
        timer_ = start_recurring_timer(std::chrono::milliseconds{1});
        if (connect_out_) {
            connect_to_service("sender");
        }
    }

    void on_connection_established(ConnectionID id) override {
        connection_ = id;
        established.store(true);
    }

    void on_connection_lost(const ConnectionID&, const std::string&) override {
        lost.store(true);
    }

    void on_framework_pdu_message(const EventMessage& message) override {
        size_t arrived = 0;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            received_.push_back(message.seq_no());
            arrived = received_.size();
            if (static_cast<int>(arrived) == expected_) {
                completed_at_ = std::chrono::steady_clock::now();
            }
        }
        release_pdu_payload(message);
        if (plan_.stop_after > 0 && !started_stopping_ && static_cast<int>(arrived) >= plan_.stop_after) {
            started_stopping_ = true;
            stop();
        }
    }

    void on_timer_event(TimerID id) override {
        if (id != timer_ || !started_stopping_) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (stopped_ && now - changed_at_ >= plan_.stop_for) {
            resume_reading(connection_);
            stopped_ = false;
            stopped_now.store(false);
            changed_at_ = now;
        } else if (!stopped_ && plan_.read_for.count() > 0 && now - changed_at_ >= plan_.read_for) {
            stop();
        }
    }

    void on_connection_failed(const std::string&) override {}
    void on_raw_socket_message(const EventMessage&) override {}
    void on_itc_message(const EventMessage&) override {}

  private:
    void stop() {
        pause_reading(connection_);
        stopped_ = true;
        stopped_now.store(true);
        changed_at_ = std::chrono::steady_clock::now();
        stops.fetch_add(1);
    }

    ReadingPlan plan_;
    int expected_;
    bool connect_out_;
    TimerID timer_{};
    ConnectionID connection_{};
    bool started_stopping_{false};
    bool stopped_{false};
    std::chrono::steady_clock::time_point changed_at_{};
    mutable std::mutex mutex_;
    std::vector<int64_t> received_;
    std::chrono::steady_clock::time_point completed_at_{};
};

// True when `received` is exactly 1..count, in order.
::testing::AssertionResult exactly_once_in_order(const std::vector<int64_t>& received, int count) {
    if (static_cast<int>(received.size()) != count) {
        return ::testing::AssertionFailure() << received.size() << " PDUs arrived, expected " << count;
    }
    for (int i = 0; i < count; ++i) {
        if (received[static_cast<size_t>(i)] != i + 1) {
            return ::testing::AssertionFailure() << "PDU " << i + 1 << " arrived as number " << received[static_cast<size_t>(i)];
        }
    }
    return ::testing::AssertionSuccess();
}

// Never resumes within the life of a test.
const ReadingPlan stops_for_good{1, std::chrono::milliseconds{600000}, std::chrono::milliseconds{0}};
const ReadingPlan keeps_reading{};

} // un-named namespace

class SlowReaderIntegrationTest : public ::testing::Test {
  protected:
    void SetUp() override {
        logger_ = std::make_unique<LoggerWithSink>();
    }

    void TearDown() override {
        // Every reactor is stopped and its thread joined before any application thread or reactor is
        // destroyed: an application thread destroyed while its reactor still runs it is a fatal error.
        for (auto& [reactor, thread] : running_) {
            reactor->shutdown("test complete");
            if (thread.joinable()) {
                thread.join();
            }
        }
        running_.clear();
        threads_.clear();
        reactors_.clear();
        logger_.reset();
    }

    bool wait_for(const std::function<bool()>& predicate, int timeout_ms = 10000) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (!predicate()) {
            if (std::chrono::steady_clock::now() > deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }

    void start(Reactor& reactor) {
        running_.emplace_back(&reactor, std::thread([&reactor]() { reactor.run(); }));
    }

    Reactor& new_reactor(const ServiceRegistry& registry, const ReactorConfiguration& config) {
        reactors_.push_back(std::make_unique<Reactor>(config, registry, logger_->logger));
        return *reactors_.back();
    }

    template <typename ThreadT, typename... Args> ThreadT& new_thread(Reactor& reactor, Args&&... args) {
        auto thread = ApplicationThread::create<ThreadT>(logger_->logger, reactor, std::forward<Args>(args)...);
        reactor.register_thread(thread);
        threads_.push_back(thread);
        return *thread;
    }

    // One sender listening, and one receiver connecting to it for each plan, one at a time.
    void make_listening_sender(const std::vector<ReadingPlan>& plans, int count, int rounds_per_ms, const ReactorConfiguration& sender_config,
                               size_t padding = 0, const ReactorConfiguration& receiver_config = make_reactor_config()) {
        static const ServiceRegistry empty_registry;
        Reactor& sender_reactor = new_reactor(empty_registry, sender_config);
        sender_reactor.register_inbound_listener(NetworkEndpointConfiguration{"127.0.0.1", any_os_assigned_port}, ThreadID{1});
        sender_ = &new_thread<RoundRobinSender>(sender_reactor, plans.size(), count, rounds_per_ms, false, padding);
        tick_counter_ = &new_thread<TickCounter>(sender_reactor);
        start(sender_reactor);
        ASSERT_TRUE(wait_for([&]() { return sender_reactor.is_initialized(); }));
        const uint16_t port = sender_reactor.get_inbound_listener_port(0);

        for (size_t index = 0; index < plans.size(); ++index) {
            registries_.push_back(std::make_unique<ServiceRegistry>());
            registries_.back()->add("sender", NetworkEndpointConfiguration{"127.0.0.1", port}, NetworkEndpointConfiguration{});
            Reactor& reactor = new_reactor(*registries_.back(), receiver_config);
            receivers_.push_back(&new_thread<Receiver>(reactor, ThreadID{static_cast<int>(index) + 10}, plans[index], count, true));
            start(reactor);
            ASSERT_TRUE(wait_for([&]() { return sender_->connections_established.load() == static_cast<int>(index) + 1; }));
        }
    }

    // One sender connecting out to a listening receiver for each plan.
    void make_connecting_sender(const std::vector<ReadingPlan>& plans, int count, int rounds_per_ms) {
        registries_.push_back(std::make_unique<ServiceRegistry>());
        ServiceRegistry& sender_registry = *registries_.back();
        static const ServiceRegistry empty_registry;
        for (size_t index = 0; index < plans.size(); ++index) {
            Reactor& reactor = new_reactor(empty_registry, make_reactor_config());
            reactor.register_inbound_listener(NetworkEndpointConfiguration{"127.0.0.1", any_os_assigned_port}, ThreadID{static_cast<int>(index) + 10});
            receivers_.push_back(&new_thread<Receiver>(reactor, ThreadID{static_cast<int>(index) + 10}, plans[index], count, false));
            start(reactor);
            ASSERT_TRUE(wait_for([&]() { return reactor.is_initialized(); }));
            sender_registry.add(receiver_service_name(index), NetworkEndpointConfiguration{"127.0.0.1", reactor.get_inbound_listener_port(0)},
                                NetworkEndpointConfiguration{});
        }
        Reactor& sender_reactor = new_reactor(sender_registry, make_reactor_config());
        sender_ = &new_thread<RoundRobinSender>(sender_reactor, plans.size(), count, rounds_per_ms, true);
        start(sender_reactor);
        ASSERT_TRUE(wait_for([&]() { return sender_->connections_established.load() == static_cast<int>(plans.size()); }));
    }

    std::unique_ptr<LoggerWithSink> logger_;
    std::vector<std::unique_ptr<ServiceRegistry>> registries_;
    std::vector<std::unique_ptr<Reactor>> reactors_;
    std::vector<std::shared_ptr<ApplicationThread>> threads_;
    std::vector<std::pair<Reactor*, std::thread>> running_;
    RoundRobinSender* sender_{nullptr};
    TickCounter* tick_counter_{nullptr};
    std::vector<Receiver*> receivers_;
};

// Integration: a receiver that stops reading holds up only the sends to itself. The other receives
// everything while the first is still stopped, and the first receives everything once it reads again.
TEST_F(SlowReaderIntegrationTest, AReaderThatStopsHoldsUpOnlyItsOwnConnection) {
    constexpr int count = 20000;
    const ReadingPlan stops_for_three_seconds{1, std::chrono::milliseconds{3000}, std::chrono::milliseconds{0}};
    make_listening_sender({stops_for_three_seconds, keeps_reading}, count, 200, make_reactor_config());
    Receiver& stopped = *receivers_[0];
    Receiver& reading = *receivers_[1];

    ASSERT_TRUE(wait_for([&]() { return reading.count() == static_cast<size_t>(count); }, 2500))
        << "while receiver 0 had stopped reading, receiver 1 received only " << reading.count() << " of " << count << " PDUs";
    ASSERT_TRUE(stopped.stopped_now.load()) << "receiver 0 read again before receiver 1 finished, so this shows nothing";
    EXPECT_LT(stopped.count(), static_cast<size_t>(count)) << "receiver 0 received everything despite having stopped reading";
    EXPECT_TRUE(exactly_once_in_order(reading.received(), count));

    ASSERT_TRUE(wait_for([&]() { return stopped.count() == static_cast<size_t>(count); }, 10000))
        << "after reading again, receiver 0 received " << stopped.count() << " of " << count << " PDUs";
    EXPECT_TRUE(exactly_once_in_order(stopped.received(), count));
    EXPECT_EQ(sender_->connections_lost.load(), 0) << "a connection was closed although its queue never reached the limit";
}

// Integration: the same for connections the sender opened itself.
TEST_F(SlowReaderIntegrationTest, AReaderThatStopsHoldsUpOnlyItsOwnOutboundConnection) {
    constexpr int count = 20000;
    const ReadingPlan stops_for_three_seconds{1, std::chrono::milliseconds{3000}, std::chrono::milliseconds{0}};
    make_connecting_sender({stops_for_three_seconds, keeps_reading}, count, 200);
    Receiver& stopped = *receivers_[0];
    Receiver& reading = *receivers_[1];

    ASSERT_TRUE(wait_for([&]() { return reading.count() == static_cast<size_t>(count); }, 2500))
        << "while receiver 0 had stopped reading, receiver 1 received only " << reading.count() << " of " << count << " PDUs";
    ASSERT_TRUE(stopped.stopped_now.load()) << "receiver 0 read again before receiver 1 finished, so this shows nothing";
    EXPECT_TRUE(exactly_once_in_order(reading.received(), count));
    ASSERT_TRUE(wait_for([&]() { return stopped.count() == static_cast<size_t>(count); }, 10000));
    EXPECT_TRUE(exactly_once_in_order(stopped.received(), count));
}

// Integration: a connection whose queue of waiting sends reaches the limit is closed, the sender is told
// it was lost, and the receiver still reading receives everything.
TEST_F(SlowReaderIntegrationTest, AConnectionWhoseQueueFillsIsClosedAndTheOthersCarryOn) {
    constexpr int count = 20000;
    ReactorConfiguration sender_config = make_reactor_config();
    sender_config.connection_waiting_sends_maximum = 1000;
    make_listening_sender({stops_for_good, keeps_reading}, count, 200, sender_config);
    Receiver& reading = *receivers_[1];

    ASSERT_TRUE(wait_for([&]() { return sender_->connections_lost.load() == 1; }, 5000))
        << "the connection to the receiver that stopped reading was not closed when its queue reached 1000 sends";
    ASSERT_TRUE(wait_for([&]() { return reading.count() == static_cast<size_t>(count); }, 5000))
        << "receiver 1 received only " << reading.count() << " of " << count << " PDUs";
    EXPECT_TRUE(exactly_once_in_order(reading.received(), count));
    EXPECT_FALSE(reading.lost.load()) << "the connection to the receiver that kept reading was closed";
    EXPECT_EQ(sender_->connections_lost.load(), 1);
}

// Fairness, between peers of the same kind and between kinds of work. With one receiver stopped, the
// three that read are served alike: each receives everything, and the last finishes soon after the
// first. A timer on the sender's reactor goes on firing throughout.
TEST_F(SlowReaderIntegrationTest, ReadersThatKeepUpAreServedAlikeWhileOneHasStopped) {
    constexpr int count = 20000;
    make_listening_sender({stops_for_good, keeps_reading, keeps_reading, keeps_reading}, count, 100, make_reactor_config());

    tick_counter_->restart_gap();
    for (size_t index = 1; index < receivers_.size(); ++index) {
        ASSERT_TRUE(wait_for([&]() { return receivers_[index]->count() == static_cast<size_t>(count); }, 5000))
            << "receiver " << index << " received only " << receivers_[index]->count() << " of " << count << " PDUs";
        EXPECT_TRUE(exactly_once_in_order(receivers_[index]->received(), count)) << "receiver " << index;
    }
    const auto longest_gap_ms = std::chrono::duration_cast<std::chrono::milliseconds>(tick_counter_->longest_gap()).count();

    auto first = receivers_[1]->completed_at();
    auto last = first;
    for (size_t index = 2; index < receivers_.size(); ++index) {
        first = std::min(first, receivers_[index]->completed_at());
        last = std::max(last, receivers_[index]->completed_at());
    }
    const auto spread_ms = std::chrono::duration_cast<std::chrono::milliseconds>(last - first).count();
    EXPECT_LT(spread_ms, 250) << "the readers finished " << spread_ms << " ms apart: some were served before others";

    // With a receiver stopped, the reactor came back to its timers within 8 milliseconds every time
    // this was measured. A reactor that went on taking commands without a bound would not come back
    // for as long as the sender kept sending.
    EXPECT_LT(longest_gap_ms, 50) << "a timer on the sender's reactor went " << longest_gap_ms << " ms without a tick while one receiver had stopped reading";
    EXPECT_LT(receivers_[0]->count(), static_cast<size_t>(count)) << "the stopped receiver received everything";
}

// Integration: an application that asks to be told when a connection can take another send is told only
// once everything waiting on that connection has been written. Such a sender then never has more than
// one send waiting on its connection, so the limit is set to two here: told any sooner, while its
// receiver has stopped reading, the sender would queue a third send at once and the connection would
// be closed. Once the receiver reads again, every PDU arrives, once and in order.
//
// How many PDUs the sender manages to send during the stop is not measured. When memory is short, the
// kernel compacts a receive queue made of many small packets and opens the window a little, so a
// receiver that has stopped reading still accepts a trickle of bytes.
TEST_F(SlowReaderIntegrationTest, ASenderPacedByWritableNotificationsWaitsForAReaderThatHasStopped) {
    constexpr int count = 50000;
    ReactorConfiguration sender_config = make_reactor_config();
    sender_config.connection_waiting_sends_maximum = 2;
    static const ServiceRegistry empty_registry;
    Reactor& sender_reactor = new_reactor(empty_registry, sender_config);
    sender_reactor.register_inbound_listener(NetworkEndpointConfiguration{"127.0.0.1", any_os_assigned_port}, ThreadID{1});
    PacedSender& sender = new_thread<PacedSender>(sender_reactor, count);
    start(sender_reactor);
    ASSERT_TRUE(wait_for([&]() { return sender_reactor.is_initialized(); }));

    registries_.push_back(std::make_unique<ServiceRegistry>());
    registries_.back()->add("sender", NetworkEndpointConfiguration{"127.0.0.1", sender_reactor.get_inbound_listener_port(0)}, NetworkEndpointConfiguration{});
    Reactor& receiver_reactor = new_reactor(*registries_.back(), make_reactor_config());
    const ReadingPlan stops_for_two_seconds{1, std::chrono::milliseconds{2000}, std::chrono::milliseconds{0}};
    Receiver& receiver = new_thread<Receiver>(receiver_reactor, ThreadID{10}, stops_for_two_seconds, count, true);
    start(receiver_reactor);

    ASSERT_TRUE(wait_for([&]() { return receiver.stopped_now.load(); })) << "the receiver never stopped reading";
    // Long enough for the kernel's buffers to fill, so that the sender's sends have to wait.
    std::this_thread::sleep_for(std::chrono::milliseconds{1400});
    ASSERT_TRUE(receiver.stopped_now.load()) << "the receiver read again before the sender was measured";
    EXPECT_LT(sender.sent.load(), count) << "everything was sent before the receiver stopped, so this shows nothing";
    EXPECT_EQ(sender.connections_lost.load(), 0) << "the connection was closed: the sender was told it could send while its sends were waiting";

    ASSERT_TRUE(wait_for([&]() { return receiver.count() == static_cast<size_t>(count); }, 30000))
        << "after reading again, the receiver received " << receiver.count() << " of " << count << " PDUs";
    EXPECT_TRUE(exactly_once_in_order(receiver.received(), count));
}

// Soak: five receivers stop and start reading over and over, each to its own rhythm, and a sixth stops
// for good, while 20,000 PDUs of about a kilobyte each go to each, over about ten seconds. The receivers'
// receive buffers are fixed at 256 KiB, because left to itself the kernel grows the buffer of a
// connection that reads quickly to megabytes, and few stops would then fill it. With that, each stop,
// of 150 to 450 milliseconds, is long enough for the kernel's buffers for the connection to fill, so the
// sends to it wait. The receivers spend about half their time reading, and read far faster than the
// PDUs are sent, so none that keeps coming back falls behind for good.
TEST_F(SlowReaderIntegrationTest, ManyReadersStoppingAndStartingLoseDuplicateAndReorderNothing) {
    constexpr int count = 20000;
    constexpr size_t padding = 1000;
    ReactorConfiguration sender_config = make_reactor_config();
    sender_config.connection_waiting_sends_maximum = 5000;
    ReactorConfiguration receiver_config = make_reactor_config();
    receiver_config.socket_receive_buffer_size = 256 * 1024;
    std::vector<ReadingPlan> plans;
    for (int index = 0; index < 5; ++index) {
        plans.push_back(ReadingPlan{1, std::chrono::milliseconds{150 + 75 * index}, std::chrono::milliseconds{400}});
    }
    plans.push_back(stops_for_good);
    make_listening_sender(plans, count, 2, sender_config, padding, receiver_config);

    ASSERT_TRUE(wait_for([&]() { return sender_->connections_lost.load() == 1; }, 30000))
        << "the connection to the receiver that stopped for good was not closed when its queue reached 5000 sends";
    for (size_t index = 0; index < 5; ++index) {
        ASSERT_TRUE(wait_for([&]() { return receivers_[index]->count() == static_cast<size_t>(count); }, 60000))
            << "receiver " << index << " received " << receivers_[index]->count() << " of " << count << " PDUs";
        EXPECT_TRUE(exactly_once_in_order(receivers_[index]->received(), count)) << "receiver " << index;
        EXPECT_GT(receivers_[index]->stops.load(), 10) << "receiver " << index << " stopped too few times for this to be a soak";
        EXPECT_FALSE(receivers_[index]->lost.load()) << "receiver " << index << "'s connection was closed";
    }
    EXPECT_EQ(sender_->connections_lost.load(), 1) << "only the connection to the receiver that stopped for good should have been closed";
}

} // namespaces
