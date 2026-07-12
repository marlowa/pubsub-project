// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

/*
End-to-end performance harness for the topic pub/sub path.

It drives the *real* delivery mechanism -- a publisher ApplicationThread (Wal +
TopicPublisher) and a subscriber ApplicationThread (TopicSubscriberChannel), each
on its own reactor, talking over loopback TCP -- so the numbers include the reactor
loop, epoll, the socket, and the WAL cursor, not just CPU-bound encode/decode.

Two scenarios run back to back:

  1. Throughput (catch-up drain). The publisher pre-appends N records to its WAL.
     A subscriber that never acks (so WAL truncation cannot confound the figure)
     connects and drains all N. Reports records/sec and payload MB/sec over the
     first-record -> last-record interval.

     NOTE: delivery is socket-paced -- TopicPublisher::pump_data sends one TopicPage
     per on_connection_writable() and re-arms. A page batches up to max_records_per_page
     records, so the epoll writable round-trip is amortised over the whole batch. Pass
     the 4th argument to sweep the batch size: 1 restores the old one-record-per-writable
     behaviour (~120 K rec/s here); 256 reaches several million rec/s. That contrast is
     the point of the test.

  2. Latency (ack-paced ping-pong). Starting from an empty WAL, the publisher
     publishes one live record per subscriber ack, each stamped (in its payload)
     with the publisher's steady_clock time at publish. The subscriber computes the
     end-to-end publish->deliver latency per record. Because publisher and subscriber
     share one process, steady_clock is directly comparable across the two threads.
     Reports p50/p90/p99/p99.9/max.

How to run it under perf (same setup notes as fixed_pool_bench):

  perf stat ./build/libraries/pubsub_itc_fw/performance/topic_pubsub_bench

  # args (all optional): throughput_record_count  latency_ping_count  payload_size  max_records_per_page
  ./build/libraries/pubsub_itc_fw/performance/topic_pubsub_bench 200000 5000 64 256

If perf reports "Access to performance monitoring ... is limited", see the remedy
in FixedPoolBench.cpp (perf_event_paranoid).
*/

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <pubsub_itc_fw/AllocatorConfiguration.hpp>
#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/ApplicationThreadConfiguration.hpp>
#include <pubsub_itc_fw/BumpAllocator.hpp>
#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/NetworkEndpointConfiguration.hpp>
#include <pubsub_itc_fw/QueueConfiguration.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/Reactor.hpp>
#include <pubsub_itc_fw/ReactorConfiguration.hpp>
#include <pubsub_itc_fw/ReactorControlCommand.hpp>
#include <pubsub_itc_fw/ServiceRegistry.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>
#include <pubsub_itc_fw/ThreadWithJoinTimeout.hpp>
#include <pubsub_itc_fw/TopicPublisher.hpp>
#include <pubsub_itc_fw/TopicSubscriberChannel.hpp>
#include <pubsub_itc_fw/Wal.hpp>

#include <fix_equity_orders.hpp>
#include <topics.hpp>

namespace pubsub_itc_fw {
namespace {

constexpr size_t wal_segment_size = 64UL * 1024UL * 1024UL; // one large segment: keep rollover out of the measurement
constexpr int never_ack = 1 << 30;                          // ack interval so large the subscriber never acks

enum class BenchMode { Throughput, Latency };

int64_t steady_now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

QueueConfiguration make_queue_config() {
    QueueConfiguration cfg{};
    cfg.low_watermark = 1;
    cfg.high_watermark = 64;
    return cfg;
}

AllocatorConfiguration make_allocator_config(const std::string& name) {
    AllocatorConfiguration cfg{};
    cfg.pool_name = name;
    cfg.objects_per_pool = 64;
    cfg.initial_pools = 1;
    return cfg;
}

ReactorConfiguration make_reactor_config() {
    ReactorConfiguration cfg{};
    cfg.inactivity_check_interval_ = std::chrono::milliseconds(100);
    cfg.init_phase_timeout_ = std::chrono::milliseconds(5000);
    cfg.shutdown_timeout_ = std::chrono::milliseconds(1000);
    cfg.connect_timeout = std::chrono::milliseconds(2000);
    return cfg;
}

std::string make_wal_dir() {
    std::string tmpl = "/dev/shm/topic_pubsub_bench_XXXXXX";
    if (::mkdtemp(tmpl.data()) == nullptr) {
        std::cerr << "mkdtemp failed for the WAL directory\n";
        std::exit(1);
    }
    return tmpl;
}

bool wait_for(const std::function<bool()>& pred, int timeout_ms = 60000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!pred()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

// Publisher: owns a Wal + TopicPublisher for topic "orders".
// Throughput mode pre-appends record_count records at startup.
// Latency mode starts empty and publishes one record per ack.
class BenchPublisherThread : public ApplicationThread, public TopicPublisherHost {
  public:
    BenchPublisherThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor, BenchMode mode, std::string wal_dir, int record_count, int payload_size,
                         int max_records_per_page)
        : ApplicationThread(token, logger, reactor, "BenchPublisher", ThreadID{2}, make_queue_config(), make_allocator_config("BenchPubPool"),
                            ApplicationThreadConfiguration{})
        , mode_(mode)
        , record_count_(record_count)
        , payload_size_(payload_size)
        , wal_directory_(wal_dir)
        , publisher_(
              *this, "orders", [](int16_t pdu_id) { return pdu_id == pubsub_itc_fw_app::NewOrderSingle::message_pdu_id; }, std::move(wal_dir), 0,
              static_cast<size_t>(max_records_per_page)) {}

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
        wal_.open(wal_directory_, wal_segment_size);
        payload_buffer_.assign(static_cast<size_t>(payload_size_), 0);
        if (mode_ == BenchMode::Throughput) {
            for (int seq = 1; seq <= record_count_; ++seq) {
                append_and_notify(seq);
            }
        }
        // Latency mode: the WAL starts empty; the first record is published when the
        // subscriber's data channel subscribes (see on_framework_pdu_message).
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
            if (mode_ == BenchMode::Latency && view.role == pubsub_itc_fw_app::TopicChannelRole::Data && !latency_started_) {
                latency_started_ = true;
                publish_next_ping();
            }
        } else if (message.pdu_id() == pubsub_itc_fw_app::TopicAck::message_pdu_id) {
            pubsub_itc_fw_app::TopicAckView view{};
            if (!pubsub_itc_fw_app::decode(view, payload, size, consumed, arena, arena_needed)) {
                return;
            }
            publisher_.on_ack(connection_id, view);
            if (mode_ == BenchMode::Latency && latency_started_) {
                publish_next_ping();
            }
        }
    }

    void on_itc_message(const EventMessage&) override {}

  private:
    void publish_next_ping() {
        if (pings_published_ >= record_count_) {
            return;
        }
        ++pings_published_;
        append_and_notify(pings_published_);
    }

    // Append one record (payload stamped with the current steady_clock ns) and wake subscribers.
    void append_and_notify(int seq) {
        const int64_t send_ns = steady_now_ns();
        std::memcpy(payload_buffer_.data(), &send_ns, sizeof(send_ns));
        wal_.append(static_cast<int64_t>(seq), pubsub_itc_fw_app::NewOrderSingle::message_pdu_id, payload_buffer_.data(), payload_size_, send_ns);
        publisher_.notify_record_appended(static_cast<int64_t>(seq), pubsub_itc_fw_app::NewOrderSingle::message_pdu_id);
    }

    BenchMode mode_;
    int record_count_;
    int payload_size_;
    bool latency_started_ = false;
    int pings_published_ = 0;
    std::vector<uint8_t> payload_buffer_;
    std::string wal_directory_;
    Wal wal_;
    TopicPublisher publisher_;
};

// Subscriber: owns a TopicSubscriberChannel and either counts
// records (throughput) or records per-record latency (latency).
class BenchSubscriberThread : public ApplicationThread, public TopicSubscriberChannelHost {
  public:
    std::atomic<bool> done{false};
    std::atomic<int> received{0};
    std::atomic<int64_t> first_recv_ns{0};
    std::atomic<int64_t> last_recv_ns{0};
    std::vector<int64_t> latencies_ns; // latency mode only; written on the subscriber thread

    BenchSubscriberThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor, BenchMode mode, int expected, int /*payload_size*/)
        : ApplicationThread(token, logger, reactor, "BenchSubscriber", ThreadID{1}, make_queue_config(), make_allocator_config("BenchSubPool"),
                            ApplicationThreadConfiguration{})
        , mode_(mode)
        , expected_(expected)
        , channel_(
              *this, "bench_sub", "orders", 0, [this](int64_t seq_no, int16_t, const uint8_t* payload, size_t size) { on_record(seq_no, payload, size); },
              mode == BenchMode::Latency ? 1 : never_ack) {
        if (mode_ == BenchMode::Latency) {
            latencies_ns.reserve(static_cast<size_t>(expected_));
        }
    }

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

    void on_connection_lost(const ConnectionID&, const std::string&) override {}

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
            channel_.on_page(payload, size, arena);
        }
    }

    void on_itc_message(const EventMessage&) override {}

  private:
    void on_record(int64_t /*seq_no*/, const uint8_t* payload, size_t size) {
        const int64_t now_ns = steady_now_ns();
        const int count = received.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (count == 1) {
            first_recv_ns.store(now_ns, std::memory_order_release);
        }
        if (mode_ == BenchMode::Latency && size >= sizeof(int64_t)) {
            int64_t send_ns = 0;
            std::memcpy(&send_ns, payload, sizeof(send_ns));
            latencies_ns.push_back(now_ns - send_ns);
        }
        if (count == expected_) {
            last_recv_ns.store(now_ns, std::memory_order_release);
            done.store(true, std::memory_order_release);
            ReactorControlCommand cmd(ReactorControlCommand::CommandTag::Disconnect);
            cmd.connection_id_ = connection_id_;
            get_reactor().enqueue_control_command(cmd);
        }
    }

    BenchMode mode_;
    int expected_;
    ConnectionID connection_id_;
    TopicSubscriberChannel channel_;
};

// A running publisher: its reactor, the reactor thread, and the listen port.
struct PublisherHarness {
    std::unique_ptr<Reactor> reactor;
    std::shared_ptr<BenchPublisherThread> thread;
    std::unique_ptr<ThreadWithJoinTimeout> reactor_thread;
    uint16_t listen_port = 0;
};

std::unique_ptr<PublisherHarness> start_publisher(QuillLogger& logger, BenchMode mode, const std::string& wal_dir, int record_count, int payload_size,
                                                  int max_records_per_page) {
    auto harness = std::make_unique<PublisherHarness>();
    const ServiceRegistry registry;
    harness->reactor = std::make_unique<Reactor>(make_reactor_config(), registry, logger);
    harness->reactor->register_inbound_listener(NetworkEndpointConfiguration{"127.0.0.1", 0}, ThreadID{2});
    harness->thread =
        ApplicationThread::create<BenchPublisherThread>(logger, *harness->reactor, mode, wal_dir, record_count, payload_size, max_records_per_page);
    harness->reactor->register_thread(harness->thread);
    Reactor& reactor_ref = *harness->reactor;
    harness->reactor_thread = std::make_unique<ThreadWithJoinTimeout>([&reactor_ref]() { reactor_ref.run(); });
    if (!wait_for([&reactor_ref]() { return reactor_ref.is_initialized(); })) {
        std::cerr << "publisher reactor did not initialize\n";
        std::exit(1);
    }
    harness->listen_port = harness->reactor->get_inbound_listener_port(0);
    return harness;
}

void print_throughput(int record_count, int payload_size, int max_records_per_page, int64_t first_ns, int64_t last_ns) {
    const int64_t elapsed_ns = last_ns - first_ns;
    const double elapsed_s = static_cast<double>(elapsed_ns) / 1e9;
    // Interval spans first-record -> last-record, i.e. record_count - 1 gaps.
    const double records_per_sec = elapsed_ns > 0 ? static_cast<double>(record_count - 1) / elapsed_s : 0.0;
    const double payload_bytes = static_cast<double>(record_count) * static_cast<double>(payload_size);
    const double payload_mb_per_sec = elapsed_ns > 0 ? payload_bytes / static_cast<double>(elapsed_ns) * 1e9 / 1e6 : 0.0;
    const double ns_per_record = record_count > 1 ? static_cast<double>(elapsed_ns) / static_cast<double>(record_count - 1) : 0.0;

    std::cout << "topic_pubsub_bench / throughput (catch-up drain, one page per writable):\n";
    std::cout << "  records:            " << record_count << "\n";
    std::cout << "  payload_size:       " << payload_size << " bytes\n";
    std::cout << "  max_records/page:   " << max_records_per_page << "\n";
    std::cout << "  elapsed:            " << elapsed_ns << " ns\n";
    std::cout << "  throughput:         " << records_per_sec << " records/sec\n";
    std::cout << "  payload throughput: " << payload_mb_per_sec << " MB/sec\n";
    std::cout << "  per record:         " << ns_per_record << " ns\n";
}

int64_t percentile(const std::vector<int64_t>& sorted, double fraction) {
    if (sorted.empty()) {
        return 0;
    }
    size_t index = static_cast<size_t>(fraction * static_cast<double>(sorted.size() - 1));
    return sorted[index];
}

void print_latency(std::vector<int64_t> samples) {
    std::cout << "topic_pubsub_bench / latency (ack-paced ping-pong, publish->deliver):\n";
    if (samples.empty()) {
        std::cout << "  (no samples)\n";
        return;
    }
    std::sort(samples.begin(), samples.end());
    int64_t sum = 0;
    for (int64_t s : samples) {
        sum += s;
    }
    const double mean = static_cast<double>(sum) / static_cast<double>(samples.size());
    std::cout << "  samples:  " << samples.size() << "\n";
    std::cout << "  min:      " << samples.front() << " ns\n";
    std::cout << "  mean:     " << mean << " ns\n";
    std::cout << "  p50:      " << percentile(samples, 0.50) << " ns\n";
    std::cout << "  p90:      " << percentile(samples, 0.90) << " ns\n";
    std::cout << "  p99:      " << percentile(samples, 0.99) << " ns\n";
    std::cout << "  p99.9:    " << percentile(samples, 0.999) << " ns\n";
    std::cout << "  max:      " << samples.back() << " ns\n";
}

void run_throughput(QuillLogger& logger, int record_count, int payload_size, int max_records_per_page) {
    const std::string wal_dir = make_wal_dir();
    auto publisher = start_publisher(logger, BenchMode::Throughput, wal_dir, record_count, payload_size, max_records_per_page);

    ServiceRegistry registry;
    registry.add("publisher", NetworkEndpointConfiguration{"127.0.0.1", publisher->listen_port}, NetworkEndpointConfiguration{});
    auto subscriber_reactor = std::make_unique<Reactor>(make_reactor_config(), registry, logger);
    auto subscriber = ApplicationThread::create<BenchSubscriberThread>(logger, *subscriber_reactor, BenchMode::Throughput, record_count, payload_size);
    subscriber_reactor->register_thread(subscriber);
    Reactor& subscriber_reactor_ref = *subscriber_reactor;
    ThreadWithJoinTimeout subscriber_reactor_thread([&subscriber_reactor_ref]() { subscriber_reactor_ref.run(); });

    if (!wait_for([&subscriber]() { return subscriber->done.load(std::memory_order_acquire); })) {
        std::cerr << "throughput scenario timed out (received " << subscriber->received.load() << " of " << record_count << ")\n";
    } else {
        print_throughput(record_count, payload_size, max_records_per_page, subscriber->first_recv_ns.load(), subscriber->last_recv_ns.load());
    }

    subscriber_reactor->shutdown("bench complete");
    publisher->reactor->shutdown("bench complete");
    if (subscriber_reactor_thread.joinable()) {
        subscriber_reactor_thread.join();
    }
    if (publisher->reactor_thread->joinable()) {
        publisher->reactor_thread->join();
    }
    std::filesystem::remove_all(wal_dir);
}

void run_latency(QuillLogger& logger, int ping_count, int payload_size, int max_records_per_page) {
    const std::string wal_dir = make_wal_dir();
    // In the ping-pong only one record is ever available per writable, so the page cap
    // does not change the latency picture; it is passed only for construction symmetry.
    auto publisher = start_publisher(logger, BenchMode::Latency, wal_dir, ping_count, payload_size, max_records_per_page);

    ServiceRegistry registry;
    registry.add("publisher", NetworkEndpointConfiguration{"127.0.0.1", publisher->listen_port}, NetworkEndpointConfiguration{});
    auto subscriber_reactor = std::make_unique<Reactor>(make_reactor_config(), registry, logger);
    auto subscriber = ApplicationThread::create<BenchSubscriberThread>(logger, *subscriber_reactor, BenchMode::Latency, ping_count, payload_size);
    subscriber_reactor->register_thread(subscriber);
    Reactor& subscriber_reactor_ref = *subscriber_reactor;
    ThreadWithJoinTimeout subscriber_reactor_thread([&subscriber_reactor_ref]() { subscriber_reactor_ref.run(); });

    if (!wait_for([&subscriber]() { return subscriber->done.load(std::memory_order_acquire); })) {
        std::cerr << "latency scenario timed out (received " << subscriber->received.load() << " of " << ping_count << ")\n";
    }

    subscriber_reactor->shutdown("bench complete");
    publisher->reactor->shutdown("bench complete");
    if (subscriber_reactor_thread.joinable()) {
        subscriber_reactor_thread.join();
    }
    if (publisher->reactor_thread->joinable()) {
        publisher->reactor_thread->join();
    }
    print_latency(subscriber->latencies_ns);
    std::filesystem::remove_all(wal_dir);
}

} // un-named namespace
} // namespaces

int main(int argc, char** argv) {
    int throughput_count = argc > 1 ? std::atoi(argv[1]) : 100000;
    int latency_pings = argc > 2 ? std::atoi(argv[2]) : 5000;
    int payload_size = argc > 3 ? std::atoi(argv[3]) : 64;
    // Records per TopicPage. 1 restores the old one-record-per-writable behaviour (for A/B).
    int max_records_per_page = argc > 4 ? std::atoi(argv[4]) : 256;
    if (throughput_count < 2) {
        throughput_count = 2;
    }
    if (latency_pings < 1) {
        latency_pings = 1;
    }
    if (payload_size < static_cast<int>(sizeof(int64_t))) {
        payload_size = static_cast<int>(sizeof(int64_t));
    }
    if (max_records_per_page < 1) {
        max_records_per_page = 1;
    }

    // Error threshold: keep benign reactor teardown warnings out of the bench output.
    pubsub_itc_fw::QuillLogger logger(pubsub_itc_fw::FwLogLevel::Error, [](const std::string&) {});

    pubsub_itc_fw::run_throughput(logger, throughput_count, payload_size, max_records_per_page);
    std::cout << "\n";
    pubsub_itc_fw::run_latency(logger, latency_pings, payload_size, max_records_per_page);
    return 0;
}
