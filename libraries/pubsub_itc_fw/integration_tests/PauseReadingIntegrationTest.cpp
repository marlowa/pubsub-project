// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

/*
 * Integration, fairness and soak tests for ApplicationThread::pause_reading() and resume_reading().
 *
 * An application thread can ask the reactor to stop reading from one of its connections for a
 * while, and to start again. The sequencer does this when its storage for orders waiting on a voter's
 * confirmation is filling (docs/availability/a_follower_behind_does_not_lead.md, 4.3). It is a change
 * to how the reactor decides what to watch on each socket, so it is tested with real reactors, real
 * application threads and real sockets, for fairness between peers and between kinds of work, and
 * over a soak with many pauses, not only as a unit.
 *
 * In every test a sender thread on one reactor sends a numbered burst of small PDUs, and a receiver
 * thread on another reactor records what arrives on each connection. The receiver pauses a connection
 * itself, from its own thread, as an application would, and resumes it from its own timer.
 *
 * Messages the reactor read before the pause reached it are still delivered: the reactor reads all a
 * socket has whenever it reads, and passes on every complete message. So a test measures what arrives
 * during the pause from a moment shortly after the pause, not from the pause itself.
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
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
const std::string receiver_service = "receiver";
const std::string sender_service = "sender";

// Which side of a connection opens it.
enum class Opener { ThisThread, ThePeer };

ReactorConfiguration make_reactor_config() {
    ReactorConfiguration cfg{};
    cfg.inactivity_check_interval_ = std::chrono::milliseconds(100);
    cfg.init_phase_timeout_ = std::chrono::milliseconds(5000);
    cfg.shutdown_timeout_ = std::chrono::milliseconds(1000);
    cfg.connect_timeout = std::chrono::milliseconds(2000);
    cfg.metrics_configuration.enabled = false;
    return cfg;
}

// Sends `count` numbered PDUs, 1 to count, as soon as a connection is established, whichever side
// opened it. Optionally opens the connection itself.
class BurstSender : public ApplicationThread {
  public:
    BurstSender(ConstructorToken token, QuillLogger& logger, Reactor& reactor, ThreadID id, int count, Opener opener)
        : ApplicationThread(token, logger, reactor, "BurstSender" + std::to_string(id.get_value()), id, make_queue_config(),
                            make_allocator_config("BurstSenderPool" + std::to_string(id.get_value())), ApplicationThreadConfiguration{})
        , count_(count)
        , opener_(opener) {}

    std::atomic<bool> burst_sent{false};

  protected:
    void on_app_ready_event() override {
        if (opener_ == Opener::ThisThread) {
            connect_to_service(receiver_service);
        }
    }

    void on_connection_established(ConnectionID id) override {
        pubsub_itc_fw_app::WalAck message{};
        for (int i = 1; i <= count_; ++i) {
            message.seq_no = i;
            send_pdu(id, pubsub_itc_fw_app::WalAck::message_pdu_id, static_cast<int64_t>(i), message);
        }
        burst_sent.store(true, std::memory_order_release);
    }

    void on_connection_failed(const std::string&) override {}
    void on_connection_lost(const ConnectionID&, const std::string&) override {}
    void on_raw_socket_message(const EventMessage&) override {}
    void on_framework_pdu_message(const EventMessage& message) override {
        release_pdu_payload(message);
    }
    void on_itc_message(const EventMessage&) override {}
    void on_timer_event(TimerID) override {}

  private:
    int count_;
    Opener opener_;
};

// What a receiver does with reading on one connection.
struct PausePlan {
    int pause_after{0};                      ///< pause once this many PDUs have arrived on the connection; 0 never
    std::chrono::milliseconds pause_for{0};  ///< how long each pause lasts
    std::chrono::milliseconds resume_for{0}; ///< for repeated pauses: how long reading runs between them; 0 pauses once
};

// Records every PDU that arrives, per connection, and pauses the connections its plan names.
class PausingReceiver : public ApplicationThread {
  public:
    PausingReceiver(ConstructorToken token, QuillLogger& logger, Reactor& reactor, Opener opener)
        : ApplicationThread(token, logger, reactor, "PausingReceiver", ThreadID{9}, make_queue_config(), make_allocator_config("PausingReceiverPool"),
                            ApplicationThreadConfiguration{})
        , opener_(opener) {}

    // Connections in the order they were established, and the plan for the nth of them.
    void set_plan(size_t connection_index, PausePlan plan) {
        plans_[connection_index] = plan;
    }

    std::atomic<int> connections_established{0};
    std::atomic<int> timer_ticks{0};
    std::atomic<int> pauses{0};
    std::atomic<bool> paused_now{false};

    // Snapshot of what has arrived on connection `index`, safe to call from the test thread.
    [[nodiscard]] std::vector<int64_t> received_on(size_t index) const {
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto found = received_.find(index);
        return found == received_.end() ? std::vector<int64_t>{} : found->second;
    }

    [[nodiscard]] size_t count_on(size_t index) const {
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto found = received_.find(index);
        return found == received_.end() ? 0 : found->second.size();
    }

  protected:
    void on_app_ready_event() override {
        timer_ = start_recurring_timer(std::chrono::milliseconds{1});
        if (opener_ == Opener::ThisThread) {
            connect_to_service(sender_service);
        }
    }

    void on_connection_established(ConnectionID id) override {
        const size_t index = static_cast<size_t>(connections_established.load());
        index_of_[id.get_value()] = index;
        connection_of_[index] = id;
        connections_established.fetch_add(1);
    }

    void on_framework_pdu_message(const EventMessage& message) override {
        const size_t index = index_of_[message.connection_id().get_value()];
        size_t arrived = 0;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            received_[index].push_back(message.seq_no());
            arrived = received_[index].size();
        }
        release_pdu_payload(message);
        const auto plan = plans_.find(index);
        if (plan != plans_.end() && plan->second.pause_after > 0 && !started_pausing_[index] && static_cast<int>(arrived) >= plan->second.pause_after) {
            started_pausing_[index] = true;
            pause(index);
        }
    }

    void on_timer_event(TimerID id) override {
        if (id != timer_) {
            return;
        }
        timer_ticks.fetch_add(1);
        const auto now = std::chrono::steady_clock::now();
        for (auto& [index, plan] : plans_) {
            if (paused_[index] && now - changed_at_[index] >= plan.pause_for) {
                resume_reading(connection_of_[index]);
                paused_[index] = false;
                paused_now.store(false);
                changed_at_[index] = now;
            } else if (!paused_[index] && started_pausing_[index] && plan.resume_for.count() > 0 && now - changed_at_[index] >= plan.resume_for) {
                pause(index);
            }
        }
    }

    void on_connection_failed(const std::string&) override {}
    void on_connection_lost(const ConnectionID&, const std::string&) override {}
    void on_raw_socket_message(const EventMessage&) override {}
    void on_itc_message(const EventMessage&) override {}

  private:
    void pause(size_t index) {
        pause_reading(connection_of_[index]);
        paused_[index] = true;
        paused_now.store(true);
        changed_at_[index] = std::chrono::steady_clock::now();
        pauses.fetch_add(1);
    }

    Opener opener_;
    TimerID timer_{};
    std::map<size_t, PausePlan> plans_;
    std::map<int64_t, size_t> index_of_;
    std::map<size_t, ConnectionID> connection_of_;
    std::map<size_t, bool> paused_;
    std::map<size_t, bool> started_pausing_;
    std::map<size_t, std::chrono::steady_clock::time_point> changed_at_;
    mutable std::mutex mutex_;
    std::map<size_t, std::vector<int64_t>> received_;
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

} // un-named namespace

class PauseReadingIntegrationTest : public ::testing::Test {
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
            for (auto& entry : running_) {
                if (entry.first->is_finished()) {
                    return false;
                }
            }
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

    // Creates a reactor the fixture owns until TearDown.
    Reactor& new_reactor(const ServiceRegistry& registry) {
        reactors_.push_back(std::make_unique<Reactor>(make_reactor_config(), registry, logger_->logger));
        return *reactors_.back();
    }

    // Creates an application thread the fixture owns until TearDown, registered with its reactor.
    template <typename ThreadT, typename... Args> ThreadT& new_thread(Reactor& reactor, Args&&... args) {
        auto thread = ApplicationThread::create<ThreadT>(logger_->logger, reactor, std::forward<Args>(args)...);
        reactor.register_thread(thread);
        threads_.push_back(thread);
        return *thread;
    }

    // A receiver listening for senders, which connect to it.
    struct InboundSetup {
        Reactor* receiver_reactor{nullptr};
        PausingReceiver* receiver{nullptr};
    };

    InboundSetup make_inbound(const std::vector<int>& burst_sizes, const std::map<size_t, PausePlan>& plans) {
        InboundSetup setup;
        static const ServiceRegistry empty_registry;
        setup.receiver_reactor = &new_reactor(empty_registry);
        setup.receiver_reactor->register_inbound_listener(NetworkEndpointConfiguration{"127.0.0.1", any_os_assigned_port}, ThreadID{9});
        auto receiver = ApplicationThread::create<PausingReceiver>(logger_->logger, *setup.receiver_reactor, Opener::ThePeer);
        for (const auto& [index, plan] : plans) {
            receiver->set_plan(index, plan);
        }
        setup.receiver_reactor->register_thread(receiver);
        threads_.push_back(receiver);
        setup.receiver = receiver.get();
        start(*setup.receiver_reactor);
        EXPECT_TRUE(wait_for([&]() { return setup.receiver_reactor->is_initialized(); }));
        const uint16_t port = setup.receiver_reactor->get_inbound_listener_port(0);

        int sender_number = 1;
        for (const int burst : burst_sizes) {
            registries_.push_back(std::make_unique<ServiceRegistry>());
            registries_.back()->add(receiver_service, NetworkEndpointConfiguration{"127.0.0.1", port}, NetworkEndpointConfiguration{});
            Reactor& reactor = new_reactor(*registries_.back());
            new_thread<BurstSender>(reactor, ThreadID{sender_number}, burst, Opener::ThisThread);
            start(reactor);
            // One at a time, so that the receiver's connection index matches the sender's position.
            EXPECT_TRUE(wait_for([&]() { return setup.receiver->connections_established.load() == sender_number; }));
            ++sender_number;
        }
        return setup;
    }

    std::unique_ptr<LoggerWithSink> logger_;
    std::vector<std::unique_ptr<ServiceRegistry>> registries_;
    std::vector<std::unique_ptr<Reactor>> reactors_;
    std::vector<std::shared_ptr<ApplicationThread>> threads_;
    std::vector<std::pair<Reactor*, std::thread>> running_;
};

// Large enough that the kernel's buffers and the reactor's reads cannot take it all before the pause
// takes effect, so a pause that works must hold some of it back.
constexpr int large_burst = 200000;

TEST_F(PauseReadingIntegrationTest, APausedInboundConnectionDeliversNothingUntilItIsResumed) {
    auto setup = make_inbound({large_burst}, {{0, PausePlan{1, std::chrono::milliseconds{1500}, std::chrono::milliseconds{0}}}});
    PausingReceiver& receiver = *setup.receiver;
    ASSERT_TRUE(wait_for([&]() { return receiver.paused_now.load(); })) << "the receiver never paused";
    // Messages read before the pause reached the reactor still arrive; after that, nothing.
    std::this_thread::sleep_for(std::chrono::milliseconds{200});
    const size_t early = receiver.count_on(0);
    std::this_thread::sleep_for(std::chrono::milliseconds{800});
    const size_t later = receiver.count_on(0);
    ASSERT_TRUE(receiver.paused_now.load()) << "the pause ended before it was measured";
    EXPECT_EQ(later, early) << "PDUs kept arriving on a connection whose reading was paused";
    EXPECT_LT(later, static_cast<size_t>(large_burst)) << "the whole burst arrived before the pause, so the pause held nothing back";
    ASSERT_TRUE(wait_for([&]() { return receiver.count_on(0) == static_cast<size_t>(large_burst); }, 20000))
        << "after resuming, " << receiver.count_on(0) << " of " << large_burst << " PDUs arrived";
    EXPECT_TRUE(exactly_once_in_order(receiver.received_on(0), large_burst));
}

TEST_F(PauseReadingIntegrationTest, APausedOutboundConnectionDeliversNothingUntilItIsResumed) {
    // The receiver opens the connection; the sender listens and sends as soon as it is accepted.
    static const ServiceRegistry empty_registry;
    Reactor& sender_reactor = new_reactor(empty_registry);
    sender_reactor.register_inbound_listener(NetworkEndpointConfiguration{"127.0.0.1", any_os_assigned_port}, ThreadID{1});
    new_thread<BurstSender>(sender_reactor, ThreadID{1}, large_burst, Opener::ThePeer);
    start(sender_reactor);
    ASSERT_TRUE(wait_for([&]() { return sender_reactor.is_initialized(); }));

    registries_.push_back(std::make_unique<ServiceRegistry>());
    registries_.back()->add(sender_service, NetworkEndpointConfiguration{"127.0.0.1", sender_reactor.get_inbound_listener_port(0)},
                            NetworkEndpointConfiguration{});
    Reactor& receiver_reactor = new_reactor(*registries_.back());
    auto receiver_owner = ApplicationThread::create<PausingReceiver>(logger_->logger, receiver_reactor, Opener::ThisThread);
    receiver_owner->set_plan(0, PausePlan{1, std::chrono::milliseconds{1500}, std::chrono::milliseconds{0}});
    receiver_reactor.register_thread(receiver_owner);
    threads_.push_back(receiver_owner);
    PausingReceiver* receiver = receiver_owner.get();
    start(receiver_reactor);

    ASSERT_TRUE(wait_for([&]() { return receiver->paused_now.load(); })) << "the receiver never paused";
    std::this_thread::sleep_for(std::chrono::milliseconds{200});
    const size_t early = receiver->count_on(0);
    std::this_thread::sleep_for(std::chrono::milliseconds{800});
    const size_t later = receiver->count_on(0);
    ASSERT_TRUE(receiver->paused_now.load()) << "the pause ended before it was measured";
    EXPECT_EQ(later, early) << "PDUs kept arriving on an outbound connection whose reading was paused";
    EXPECT_LT(later, static_cast<size_t>(large_burst));
    ASSERT_TRUE(wait_for([&]() { return receiver->count_on(0) == static_cast<size_t>(large_burst); }, 20000));
    EXPECT_TRUE(exactly_once_in_order(receiver->received_on(0), large_burst));
}

TEST_F(PauseReadingIntegrationTest, OtherConnectionsAndTimersAreServedWhileOneIsPaused) {
    // Fairness between peers and between kinds of work: with connection 0 paused, connection 1's whole
    // burst arrives, and the receiver's one-millisecond timer goes on firing.
    constexpr int second_burst = 50000;
    auto setup = make_inbound({large_burst}, {{0, PausePlan{1, std::chrono::milliseconds{4000}, std::chrono::milliseconds{0}}}});
    PausingReceiver& receiver = *setup.receiver;
    ASSERT_TRUE(wait_for([&]() { return receiver.paused_now.load(); }));
    std::this_thread::sleep_for(std::chrono::milliseconds{200});
    const size_t held_at = receiver.count_on(0);
    const int ticks_before = receiver.timer_ticks.load();

    registries_.push_back(std::make_unique<ServiceRegistry>());
    registries_.back()->add(receiver_service, NetworkEndpointConfiguration{"127.0.0.1", setup.receiver_reactor->get_inbound_listener_port(0)},
                            NetworkEndpointConfiguration{});
    Reactor& second_reactor = new_reactor(*registries_.back());
    new_thread<BurstSender>(second_reactor, ThreadID{2}, second_burst, Opener::ThisThread);
    start(second_reactor);

    ASSERT_TRUE(wait_for([&]() { return receiver.count_on(1) == static_cast<size_t>(second_burst); }, 3000))
        << "while connection 0 was paused, only " << receiver.count_on(1) << " of connection 1's " << second_burst << " PDUs arrived";
    ASSERT_TRUE(receiver.paused_now.load()) << "connection 0's pause ended before connection 1 finished, so this shows nothing";
    EXPECT_EQ(receiver.count_on(0), held_at) << "connection 0 delivered PDUs while paused";
    EXPECT_TRUE(exactly_once_in_order(receiver.received_on(1), second_burst));
    EXPECT_GT(receiver.timer_ticks.load() - ticks_before, 50) << "the receiver's timer stopped firing while a connection was paused";
    ASSERT_TRUE(wait_for([&]() { return receiver.count_on(0) == static_cast<size_t>(large_burst); }, 20000));
    EXPECT_TRUE(exactly_once_in_order(receiver.received_on(0), large_burst));
}

TEST_F(PauseReadingIntegrationTest, ManyPausesAndResumesLoseDuplicateAndReorderNothing) {
    // Soak: the receiver pauses for 2 ms and reads for 2 ms, over and over, for the whole of a long
    // burst, so that pausing and resuming meet the reactor at every point in reading a socket.
    auto setup = make_inbound({large_burst}, {{0, PausePlan{1, std::chrono::milliseconds{2}, std::chrono::milliseconds{2}}}});
    PausingReceiver& receiver = *setup.receiver;
    ASSERT_TRUE(wait_for([&]() { return receiver.count_on(0) == static_cast<size_t>(large_burst); }, 60000))
        << receiver.count_on(0) << " of " << large_burst << " PDUs arrived";
    EXPECT_GT(receiver.pauses.load(), 100) << "too few pauses for this to be a soak";
    EXPECT_TRUE(exactly_once_in_order(receiver.received_on(0), large_burst));
}

} // namespaces
