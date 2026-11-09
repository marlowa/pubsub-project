// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

/*
 * Integration, fairness and soak tests for the limit on how many of a connection's messages may wait in its
 * application thread's queue (docs/framework/bounding_messages_waiting.md, BUG-0234).
 *
 * A sender sends numbered PDUs as fast as its connection takes them: a batch at a time, asking after each batch to be
 * told when the connection can take more, as the sequencer's reading thread sends a matching engine the commands it
 * lacks. A receiver takes each PDU slowly, by spinning for a few microseconds, so that it falls behind. Without a limit
 * the reactor reads everything the network delivers into the receiver's queue; with one it stops reading the
 * connection once that many are waiting, and the rest stay in the sender's socket.
 *
 * What is measured is the count the limit keeps, read by the receiving thread itself as it takes each PDU. A run
 * "without a limit" sets one so large that reading is never stopped, so that both runs are measured the same way.
 *
 * The last tests are for docs/bug_list.md BUG-0238: a connection without a limit, flooded by its peer, must not hold its
 * reactor in one read, so the reactor goes on serving the timers of another thread it runs.
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

#include <replication_messages.hpp>

using pubsub_itc_fw::tests_common::LoggerWithSink;
using pubsub_itc_fw::tests_common::make_allocator_config;
using pubsub_itc_fw::tests_common::make_queue_config;

namespace pubsub_itc_fw::tests {

namespace {

constexpr uint16_t any_os_assigned_port = 0;
// A limit so large that reading is never stopped, so that a run without a limit is measured as a run with one is.
constexpr int64_t no_effective_limit = 1'000'000'000;

ReactorConfiguration make_reactor_config() {
    ReactorConfiguration cfg{};
    cfg.inactivity_check_interval_ = std::chrono::milliseconds(100);
    cfg.init_phase_timeout_ = std::chrono::milliseconds(5000);
    cfg.shutdown_timeout_ = std::chrono::milliseconds(1000);
    cfg.connect_timeout = std::chrono::milliseconds(2000);
    cfg.metrics_configuration.enabled = false;
    cfg.socket_maximum_inactivity_interval_ = std::chrono::minutes{10};
    return cfg;
}

// Spins for about `micros` microseconds: a thread taking each message slowly, without giving up its core.
void spin_for(int micros) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds{micros};
    while (std::chrono::steady_clock::now() < until) {}
}

// Sends PDUs numbered 1 to `count` on the first connection established, 256 at a time, asking after each batch to be
// told when the connection can take more. With `stamp`, each PDU's number is replaced by the time it was sent, in
// steady-clock nanoseconds, so that the receiver can tell how long it waited; the PDUs are then sent `interval` apart,
// one at a time. It listens, or connects to "receiver".
class Sender : public ApplicationThread {
  public:
    Sender(ConstructorToken token, QuillLogger& logger, Reactor& reactor, ThreadID id, int count, bool connect_out, bool stamp = false,
           std::chrono::milliseconds interval = std::chrono::milliseconds{0})
        : ApplicationThread(token, logger, reactor, "Sender" + std::to_string(id.get_value()), id, make_queue_config(),
                            make_allocator_config("SenderPool" + std::to_string(id.get_value())), ApplicationThreadConfiguration{})
        , count_(count)
        , connect_out_(connect_out)
        , stamp_(stamp)
        , interval_(interval) {}

    std::atomic<int> sent{0};
    std::atomic<bool> established{false};
    std::atomic<int> connections_lost{0};
    /// Until set, an unstamped sender waits after connecting instead of sending, so that a test can have the receiver set
    /// its limits first. Set to true from the start unless the test clears it.
    std::atomic<bool> go{true};

  protected:
    void on_app_ready_event() override {
        if (stamp_) {
            timer_ = start_recurring_timer(interval_);
        } else {
            go_timer_ = start_recurring_timer(std::chrono::milliseconds{1});
        }
        if (connect_out_) {
            connect_to_service("receiver");
        }
    }
    void on_connection_established(ConnectionID id) override {
        if (connection_.is_valid()) {
            return;
        }
        connection_ = id;
        established.store(true);
        if (!stamp_ && go.load()) {
            started_ = true;
            send_batch();
        }
    }
    void on_connection_writable(ConnectionID) override {
        send_batch();
    }
    void on_timer_event(TimerID id) override {
        if (id == go_timer_) {
            if (!started_ && connection_.is_valid() && go.load()) {
                started_ = true;
                send_batch();
            }
            return;
        }
        if (id != timer_ || !connection_.is_valid() || sent.load() >= count_) {
            return;
        }
        pubsub_itc_fw::WalAck message{};
        message.seq_no = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        send_pdu(connection_, pubsub_itc_fw::WalAck::message_pdu_id, message.seq_no, message);
        sent.fetch_add(1);
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

  private:
    void send_batch() {
        int next = sent.load();
        const int end = std::min(count_, next + 256);
        pubsub_itc_fw::WalAck message{};
        for (; next < end; ++next) {
            message.seq_no = next + 1;
            send_pdu(connection_, pubsub_itc_fw::WalAck::message_pdu_id, static_cast<int64_t>(next + 1), message);
        }
        sent.store(next);
        if (next < count_) {
            request_writable_notification(connection_);
        }
    }

    int count_;
    bool connect_out_;
    bool stamp_;
    std::chrono::milliseconds interval_;
    TimerID timer_{};
    TimerID go_timer_{};
    bool started_{false};
    ConnectionID connection_{};
};

// How a receiver takes its PDUs: how long it spins on each, and, for a soak, how often that changes.
struct TakingPlan {
    int slow_micros{20};                       ///< spin on each PDU while slow
    int fast_micros{0};                        ///< spin on each PDU while fast
    std::chrono::milliseconds switch_every{0}; ///< alternate slow and fast this often; 0 is always slow
};

// Receives on every connection made to it or by it, limits each to the limit given for it, takes each PDU as its plan
// says, and records, for each connection, the PDUs in order, the most messages ever waiting, and, for a connection whose
// PDUs carry the time they were sent, the longest wait. It also records the longest gap between ticks of its timer.
class Receiver : public ApplicationThread {
  public:
    struct PerConnection {
        ConnectionID id{};
        int64_t limit{0};
        std::vector<int64_t> received;
        int64_t most_waiting{0};
        int64_t longest_wait_ns{0};
        bool stamped{false};
    };

    // `limits[n]` is the limit for the nth connection made, 0 for none; `stamped[n]` says whether its PDUs carry times.
    Receiver(ConstructorToken token, QuillLogger& logger, Reactor& reactor, ThreadID id, std::vector<int64_t> limits, std::vector<bool> stamped,
             TakingPlan plan, std::vector<std::string> connect_to = {})
        : ApplicationThread(token, logger, reactor, "Receiver" + std::to_string(id.get_value()), id, make_queue_config(),
                            make_allocator_config("ReceiverPool" + std::to_string(id.get_value())), ApplicationThreadConfiguration{})
        , limits_(std::move(limits))
        , stamped_(std::move(stamped))
        , plan_(plan)
        , connect_to_(std::move(connect_to)) {}

    std::atomic<int> connections{0};
    std::atomic<int64_t> longest_tick_gap_us{0};
    std::atomic<int> switches{0};

    [[nodiscard]] PerConnection connection(size_t index) const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return index < per_connection_.size() ? per_connection_[index] : PerConnection{};
    }

    [[nodiscard]] size_t count(size_t index) const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return index < per_connection_.size() ? per_connection_[index].received.size() : 0;
    }

    /// The count the limit keeps for connection `index` now, read on the receiving thread at its next tick.
    [[nodiscard]] int64_t waiting_at_last_tick(size_t index) const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return index < waiting_at_tick_.size() ? waiting_at_tick_[index] : -2;
    }

  protected:
    void on_app_ready_event() override {
        timer_ = start_recurring_timer(std::chrono::milliseconds{1});
        for (const std::string& service : connect_to_) {
            connect_to_service(service);
        }
    }

    void on_connection_established(ConnectionID id) override {
        const std::lock_guard<std::mutex> lock(mutex_);
        const size_t index = per_connection_.size();
        PerConnection entry;
        entry.id = id;
        entry.limit = index < limits_.size() ? limits_[index] : 0;
        entry.stamped = index < stamped_.size() && stamped_[index];
        if (entry.limit > 0) {
            limit_messages_waiting(id, entry.limit);
        }
        per_connection_.push_back(entry);
        waiting_at_tick_.push_back(-1);
        connections.fetch_add(1);
    }

    void on_framework_pdu_message(const EventMessage& message) override {
        const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            for (PerConnection& entry : per_connection_) {
                if (entry.id == message.connection_id()) {
                    entry.received.push_back(message.seq_no());
                    entry.most_waiting = std::max(entry.most_waiting, messages_waiting(entry.id));
                    if (entry.stamped) {
                        entry.longest_wait_ns = std::max(entry.longest_wait_ns, now_ns - message.seq_no());
                    }
                    break;
                }
            }
        }
        release_pdu_payload(message);
        spin_for(slow_ ? plan_.slow_micros : plan_.fast_micros);
    }

    void on_timer_event(TimerID id) override {
        if (id != timer_) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (last_tick_ != std::chrono::steady_clock::time_point{}) {
            const int64_t gap = std::chrono::duration_cast<std::chrono::microseconds>(now - last_tick_).count();
            if (gap > longest_tick_gap_us.load()) {
                longest_tick_gap_us.store(gap);
            }
        }
        last_tick_ = now;
        if (plan_.switch_every.count() > 0 && now - switched_at_ >= plan_.switch_every) {
            slow_ = !slow_;
            switched_at_ = now;
            switches.fetch_add(1);
        }
        const std::lock_guard<std::mutex> lock(mutex_);
        for (size_t index = 0; index < per_connection_.size(); ++index) {
            waiting_at_tick_[index] = messages_waiting(per_connection_[index].id);
        }
    }

    void on_connection_lost(const ConnectionID&, const std::string&) override {}
    void on_connection_failed(const std::string&) override {}
    void on_raw_socket_message(const EventMessage&) override {}
    void on_itc_message(const EventMessage&) override {}

  private:
    std::vector<int64_t> limits_;
    std::vector<bool> stamped_;
    TakingPlan plan_;
    std::vector<std::string> connect_to_;
    TimerID timer_{};
    std::chrono::steady_clock::time_point last_tick_{};
    std::chrono::steady_clock::time_point switched_at_{std::chrono::steady_clock::now()};
    bool slow_{true};
    mutable std::mutex mutex_;
    std::vector<PerConnection> per_connection_;
    std::vector<int64_t> waiting_at_tick_;
};

// Counts the ticks of a one-millisecond timer and records the longest gap between two, and does nothing else, so that
// the gap shows how promptly its reactor comes back to its timers.
class TickCounter : public ApplicationThread {
  public:
    TickCounter(ConstructorToken token, QuillLogger& logger, Reactor& reactor)
        : ApplicationThread(token, logger, reactor, "TickCounter", ThreadID{2}, make_queue_config(), make_allocator_config("TickCounterPool"),
                            ApplicationThreadConfiguration{}) {}

    std::atomic<int64_t> longest_gap_us{0};
    std::atomic<int> ticks{0};

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
        if (last_ != std::chrono::steady_clock::time_point{}) {
            const int64_t gap = std::chrono::duration_cast<std::chrono::microseconds>(now - last_).count();
            if (gap > longest_gap_us.load()) {
                longest_gap_us.store(gap);
            }
        }
        last_ = now;
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
    std::chrono::steady_clock::time_point last_{};
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

} // namespaces

class MessagesWaitingIntegrationTest : public ::testing::Test {
  protected:
    void SetUp() override {
        logger_ = std::make_unique<LoggerWithSink>();
    }

    void TearDown() override {
        // Every reactor is stopped and its thread joined before any application thread or reactor is destroyed.
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

    bool wait_for(const std::function<bool()>& predicate, int timeout_ms = 30000) {
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

    Reactor& new_reactor(const ServiceRegistry& registry) {
        reactors_.push_back(std::make_unique<Reactor>(make_reactor_config(), registry, logger_->logger));
        return *reactors_.back();
    }

    template <typename ThreadT, typename... Args> ThreadT& new_thread(Reactor& reactor, Args&&... args) {
        auto thread = ApplicationThread::create<ThreadT>(logger_->logger, reactor, std::forward<Args>(args)...);
        reactor.register_thread(thread);
        threads_.push_back(thread);
        return *thread;
    }

    // A receiver listening, and one sender connecting to it for each entry of `senders`: (count, stamped, interval).
    Receiver& make_listening_receiver(const std::vector<int64_t>& limits, const std::vector<std::tuple<int, bool, std::chrono::milliseconds>>& senders,
                                      TakingPlan plan, bool with_tick_counter = false) {
        static const ServiceRegistry empty_registry;
        Reactor& receiver_reactor = new_reactor(empty_registry);
        if (with_tick_counter) {
            tick_counter_ = &new_thread<TickCounter>(receiver_reactor);
        }
        receiver_reactor.register_inbound_listener(NetworkEndpointConfiguration{"127.0.0.1", any_os_assigned_port}, ThreadID{1});
        std::vector<bool> stamped;
        for (const auto& sender : senders) {
            stamped.push_back(std::get<1>(sender));
        }
        Receiver& receiver = new_thread<Receiver>(receiver_reactor, ThreadID{1}, limits, stamped, plan);
        start(receiver_reactor);
        EXPECT_TRUE(wait_for([&]() { return receiver_reactor.is_initialized(); }));
        const uint16_t port = receiver_reactor.get_inbound_listener_port(0);
        for (size_t index = 0; index < senders.size(); ++index) {
            registries_.push_back(std::make_unique<ServiceRegistry>());
            registries_.back()->add("receiver", NetworkEndpointConfiguration{"127.0.0.1", port}, NetworkEndpointConfiguration{});
            Reactor& reactor = new_reactor(*registries_.back());
            const auto& [count, stamp, interval] = senders[index];
            senders_.push_back(&new_thread<Sender>(reactor, ThreadID{static_cast<int>(index) + 10}, count, true, stamp, interval));
            senders_.back()->go.store(false);
            start(reactor);
            EXPECT_TRUE(wait_for([&]() { return receiver.connections.load() == static_cast<int>(index) + 1; }));
        }
        release_senders();
        return receiver;
    }

    // A sender listening, and a receiver connecting to it: the receiver's connection is outbound.
    Receiver& make_connecting_receiver(int64_t limit, int count, TakingPlan plan) {
        static const ServiceRegistry empty_registry;
        Reactor& sender_reactor = new_reactor(empty_registry);
        sender_reactor.register_inbound_listener(NetworkEndpointConfiguration{"127.0.0.1", any_os_assigned_port}, ThreadID{10});
        senders_.push_back(&new_thread<Sender>(sender_reactor, ThreadID{10}, count, false));
        senders_.back()->go.store(false);
        start(sender_reactor);
        EXPECT_TRUE(wait_for([&]() { return sender_reactor.is_initialized(); }));
        registries_.push_back(std::make_unique<ServiceRegistry>());
        registries_.back()->add("sender", NetworkEndpointConfiguration{"127.0.0.1", sender_reactor.get_inbound_listener_port(0)},
                                NetworkEndpointConfiguration{});
        Reactor& receiver_reactor = new_reactor(*registries_.back());
        Receiver& receiver = new_thread<Receiver>(receiver_reactor, ThreadID{1}, std::vector<int64_t>{limit}, std::vector<bool>{false}, plan,
                                                  std::vector<std::string>{"sender"});
        start(receiver_reactor);
        EXPECT_TRUE(wait_for([&]() { return receiver.connections.load() == 1; }));
        release_senders();
        return receiver;
    }

    // Lets the senders start, once the receiver has had time to set its limits. A connection flooding before its limit
    // takes effect is read without one, which is docs/bug_list.md BUG-0238 and is not what these tests are about.
    void release_senders() {
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        for (Sender* sender : senders_) {
            sender->go.store(true);
        }
    }

    std::unique_ptr<LoggerWithSink> logger_;
    std::vector<std::unique_ptr<ServiceRegistry>> registries_;
    std::vector<std::unique_ptr<Reactor>> reactors_;
    std::vector<std::shared_ptr<ApplicationThread>> threads_;
    std::vector<std::pair<Reactor*, std::thread>> running_;
    std::vector<Sender*> senders_;
    TickCounter* tick_counter_{nullptr};
};

constexpr int flood = 100'000;
const auto unstamped = std::make_tuple(flood, false, std::chrono::milliseconds{0});

// The control for every test below: without an effective limit, a receiver taking each PDU in 20 microseconds falls far
// behind a sender over loopback, and the reactor reads the backlog into its queue. If this did not happen, the tests
// with a limit would prove nothing.
TEST_F(MessagesWaitingIntegrationTest, WithoutAnEffectiveLimitTheBacklogMovesIntoTheThreadsQueue) {
    Receiver& receiver = make_listening_receiver({no_effective_limit}, {unstamped}, TakingPlan{});
    ASSERT_TRUE(wait_for([&]() { return receiver.count(0) == static_cast<size_t>(flood); }));
    EXPECT_GT(receiver.connection(0).most_waiting, 640) << "the receiver never fell more than ten times a limit of 64 behind, so the tests with that limit "
                                                           "would not show it doing anything";
}

TEST_F(MessagesWaitingIntegrationTest, AnInboundConnectionNeverHasMoreThanItsLimitWaiting) {
    Receiver& receiver = make_listening_receiver({64}, {unstamped}, TakingPlan{});
    ASSERT_TRUE(wait_for([&]() { return receiver.count(0) == static_cast<size_t>(flood); }))
        << "received " << receiver.count(0) << " of " << flood << ": reading was left stopped";
    const Receiver::PerConnection result = receiver.connection(0);
    EXPECT_LE(result.most_waiting, 64);
    EXPECT_GT(result.most_waiting, 32) << "the receiver never reached the limit, so the limit was never tested";
    EXPECT_TRUE(exactly_once_in_order(result.received, flood));
    ASSERT_TRUE(wait_for([&]() { return receiver.waiting_at_last_tick(0) == 0; }, 2000)) << "the count did not return to zero";
}

TEST_F(MessagesWaitingIntegrationTest, AnOutboundConnectionNeverHasMoreThanItsLimitWaiting) {
    Receiver& receiver = make_connecting_receiver(64, flood, TakingPlan{});
    ASSERT_TRUE(wait_for([&]() { return receiver.count(0) == static_cast<size_t>(flood); }))
        << "received " << receiver.count(0) << " of " << flood << ": reading was left stopped";
    const Receiver::PerConnection result = receiver.connection(0);
    EXPECT_LE(result.most_waiting, 64);
    EXPECT_GT(result.most_waiting, 32) << "the receiver never reached the limit, so the limit was never tested";
    EXPECT_TRUE(exactly_once_in_order(result.received, flood));
}

TEST_F(MessagesWaitingIntegrationTest, TheSmallestLimitIsHeldAndNothingIsLeftStopped) {
    Receiver& receiver = make_listening_receiver({16}, {unstamped}, TakingPlan{2, 0, std::chrono::milliseconds{0}});
    ASSERT_TRUE(wait_for([&]() { return receiver.count(0) == static_cast<size_t>(flood); }))
        << "received " << receiver.count(0) << " of " << flood << ": reading was left stopped";
    EXPECT_LE(receiver.connection(0).most_waiting, 16);
    EXPECT_TRUE(exactly_once_in_order(receiver.connection(0).received, flood));
}

// Fairness between connections: a flood on a limited connection does not keep a trickle on another connection waiting
// behind it, nor the thread's own timer. The trickle's PDUs carry the time they were sent.
TEST_F(MessagesWaitingIntegrationTest, AFloodOnALimitedConnectionDoesNotHoldUpAnotherConnectionOrTheTimer) {
    const auto trickle = std::make_tuple(500, true, std::chrono::milliseconds{1});
    Receiver& receiver = make_listening_receiver({64, 0}, {unstamped, trickle}, TakingPlan{});
    ASSERT_TRUE(wait_for([&]() { return receiver.count(0) == static_cast<size_t>(flood) && receiver.count(1) == 500U; }));
    EXPECT_LT(receiver.connection(1).longest_wait_ns, 20'000'000) << "a trickled PDU waited behind the flood for more than 20 ms";
    EXPECT_LT(receiver.longest_tick_gap_us.load(), 20'000) << "the receiver's timer went more than 20 ms without a tick";
}

// The same without an effective limit, to show the test above can tell the difference: the same measurement exceeds the
// 20 ms that test allows.
TEST_F(MessagesWaitingIntegrationTest, WithoutAnEffectiveLimitAFloodHoldsUpAnotherConnection) {
    const auto trickle = std::make_tuple(500, true, std::chrono::milliseconds{1});
    Receiver& receiver = make_listening_receiver({no_effective_limit, 0}, {unstamped, trickle}, TakingPlan{});
    ASSERT_TRUE(wait_for([&]() { return receiver.count(0) == static_cast<size_t>(flood) && receiver.count(1) == 500U; }));
    EXPECT_GT(receiver.connection(1).longest_wait_ns, 20'000'000)
        << "without a limit a trickled PDU was expected to wait behind the flood for longer than the 20 ms the test with a limit allows";
}

// Fairness between limited connections: two floods into one thread, each limited, are both served throughout.
TEST_F(MessagesWaitingIntegrationTest, TwoLimitedFloodsIntoOneThreadAreBothServed) {
    Receiver& receiver = make_listening_receiver({64, 64}, {unstamped, unstamped}, TakingPlan{});
    ASSERT_TRUE(wait_for([&]() { return receiver.count(0) == static_cast<size_t>(flood) || receiver.count(1) == static_cast<size_t>(flood); }, 60000));
    // When the first finishes, the second must be well under way, not waiting for its turn.
    const size_t behind = std::min(receiver.count(0), receiver.count(1));
    EXPECT_GT(behind, static_cast<size_t>(flood) / 2) << "one flood was served while the other waited: the senders had sent " << senders_[0]->sent.load()
                                                      << " and " << senders_[1]->sent.load() << ", the receiver had " << receiver.count(0) << " and "
                                                      << receiver.count(1) << ", and the counts waiting were " << receiver.waiting_at_last_tick(0) << " and "
                                                      << receiver.waiting_at_last_tick(1);
    ASSERT_TRUE(wait_for([&]() { return receiver.count(0) == static_cast<size_t>(flood) && receiver.count(1) == static_cast<size_t>(flood); }, 60000));
    EXPECT_LE(receiver.connection(0).most_waiting, 64);
    EXPECT_LE(receiver.connection(1).most_waiting, 64);
    EXPECT_TRUE(exactly_once_in_order(receiver.connection(0).received, flood));
    EXPECT_TRUE(exactly_once_in_order(receiver.connection(1).received, flood));
}

// Soak: three floods of a million PDUs each, limited at 16, 256 and 4096, and a trickle on an unlimited connection,
// into one thread that alternates every 50 ms between taking each PDU in 20 microseconds and taking it at once, so that
// each limit is reached and left many times. Nothing may be lost, repeated or reordered, no limit exceeded, no
// connection left stopped, and every count must return to zero.
TEST_F(MessagesWaitingIntegrationTest, AThreadAlternatingSlowAndFastLosesNothingAndLeavesNothingStopped) {
    constexpr int soak = 1'000'000;
    const auto big = std::make_tuple(soak, false, std::chrono::milliseconds{0});
    const auto trickle = std::make_tuple(2'000, true, std::chrono::milliseconds{2});
    Receiver& receiver = make_listening_receiver({16, 256, 4096, 0}, {big, big, big, trickle}, TakingPlan{20, 0, std::chrono::milliseconds{50}});
    ASSERT_TRUE(wait_for(
        [&]() {
            return receiver.count(0) == static_cast<size_t>(soak) && receiver.count(1) == static_cast<size_t>(soak) &&
                   receiver.count(2) == static_cast<size_t>(soak) && receiver.count(3) == 2'000U;
        },
        180000))
        << "received " << receiver.count(0) << ", " << receiver.count(1) << ", " << receiver.count(2) << " of " << soak << ", and " << receiver.count(3)
        << " of 2000 trickled";
    const int64_t limits[] = {16, 256, 4096};
    for (size_t index = 0; index < 3; ++index) {
        const Receiver::PerConnection result = receiver.connection(index);
        EXPECT_LE(result.most_waiting, limits[index]) << "connection " << index;
        EXPECT_TRUE(exactly_once_in_order(result.received, soak)) << "connection " << index;
        ASSERT_TRUE(wait_for([&]() { return receiver.waiting_at_last_tick(index) == 0; }, 2000)) << "connection " << index;
    }
    EXPECT_GT(receiver.switches.load(), 20) << "the thread changed speed too few times for this to be a soak";
    EXPECT_EQ(senders_[0]->connections_lost.load() + senders_[1]->connections_lost.load() + senders_[2]->connections_lost.load(), 0);
}

// BUG-0238: a flood on a connection without a limit, into a thread that takes each PDU at once, must not hold the reactor
// in one read: another thread on the same reactor goes on getting its timer every millisecond or so.
TEST_F(MessagesWaitingIntegrationTest, AFloodWithoutALimitDoesNotStopItsReactorServingAnotherThreadsTimer) {
    constexpr int big_flood = 1'000'000;
    const auto big = std::make_tuple(big_flood, false, std::chrono::milliseconds{0});
    Receiver& receiver = make_listening_receiver({0}, {big}, TakingPlan{0, 0, std::chrono::milliseconds{0}}, true);
    ASSERT_TRUE(wait_for([&]() { return receiver.count(0) == static_cast<size_t>(big_flood); }, 120000));
    EXPECT_TRUE(exactly_once_in_order(receiver.connection(0).received, big_flood));
    EXPECT_GT(tick_counter_->ticks.load(), 100) << "the flood was too short for the timer to be tested";
    EXPECT_LT(tick_counter_->longest_gap_us.load(), 20'000) << "the reactor went more than 20 ms without ringing another thread's timer";
}

// BUG-0238: two floods without a limit into one reactor are both served throughout, neither held up by the other.
TEST_F(MessagesWaitingIntegrationTest, TwoFloodsWithoutALimitAreBothServedThroughout) {
    constexpr int big_flood = 500'000;
    const auto big = std::make_tuple(big_flood, false, std::chrono::milliseconds{0});
    Receiver& receiver = make_listening_receiver({0, 0}, {big, big}, TakingPlan{0, 0, std::chrono::milliseconds{0}});
    ASSERT_TRUE(wait_for([&]() { return receiver.count(0) == static_cast<size_t>(big_flood) || receiver.count(1) == static_cast<size_t>(big_flood); }, 120000));
    const size_t behind = std::min(receiver.count(0), receiver.count(1));
    EXPECT_GT(behind, static_cast<size_t>(big_flood) / 2)
        << "one flood was served while the other waited: " << receiver.count(0) << " and " << receiver.count(1);
    ASSERT_TRUE(wait_for([&]() { return receiver.count(0) == static_cast<size_t>(big_flood) && receiver.count(1) == static_cast<size_t>(big_flood); }, 120000));
}

} // namespaces
