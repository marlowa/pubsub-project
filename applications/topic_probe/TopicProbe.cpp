// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

/*
topic_probe -- a tiny diagnostic topic subscriber (a minimal stand-in for TAP).

It connects to a publisher (e.g. the MEP) and subscribes to one or more topics over
the data channel using the reusable pubsub_itc_fw::TopicSubscriberThread -- one
subscription per topic, each on that topic's port -- and prints each record it
receives to stdout, tagged with its topic name. Use it to watch orders / execution
reports stream out of the MEP live while driving the Java fix-test-client.

Usage:
  topic_probe [--topics=a,b,... | all] [--host H] [--from-seq-no N]

  --topics      comma-separated topic names, or 'all' (the default) for every topic
                the generated registry knows. Names: orders, execution_reports.
  --host        default 127.0.0.1
  --from-seq-no default 0 -- replay everything each publisher still retains, then
                stream live. Give a higher number to start nearer the head.

Ports are the MEP primary's deployed defaults (orders -> 11040, execution_reports ->
11041). Stop with Ctrl-C. If the publisher instance is a follower it answers
TopicNotLeader; point the probe at the leader instance's port instead.
*/

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <unistd.h>

#include <argparse/argparse.hpp>

#include <fix_orders.hpp>
#include <topics_registry.hpp>

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
#include <pubsub_itc_fw/ServiceRegistry.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>
#include <pubsub_itc_fw/TopicSubscriberThread.hpp>

namespace topic_probe {
namespace {

pubsub_itc_fw::QueueConfiguration make_queue_config() {
    pubsub_itc_fw::QueueConfiguration cfg{};
    cfg.low_watermark = 1;
    cfg.high_watermark = 64;
    return cfg;
}

pubsub_itc_fw::AllocatorConfiguration make_allocator_config() {
    pubsub_itc_fw::AllocatorConfiguration cfg{};
    cfg.pool_name = "ProbePool";
    cfg.objects_per_pool = 64;
    cfg.initial_pools = 1;
    return cfg;
}

pubsub_itc_fw::ReactorConfiguration make_reactor_config() {
    pubsub_itc_fw::ReactorConfiguration cfg{};
    cfg.inactivity_check_interval_ = std::chrono::milliseconds(500);
    cfg.init_phase_timeout_ = std::chrono::seconds(5);
    cfg.shutdown_timeout_ = std::chrono::seconds(2);
    cfg.connect_timeout = std::chrono::seconds(5);
    return cfg;
}

namespace app = pubsub_itc_fw_app;

const char* pdu_name(int16_t pdu_id) {
    switch (pdu_id) {
        case app::NewOrderSingle::message_pdu_id:
            return "NewOrderSingle";
        case app::OrderCancelRequest::message_pdu_id:
            return "OrderCancelRequest";
        case app::ExecutionReport::message_pdu_id:
            return "ExecutionReport";
        default:
            return "pdu";
    }
}

// Decodes the payload as View and prints the generated structured dump. Returns
// false (caller falls back to a hex preview) if the record does not decode.
template <typename View> bool decode_and_dump(const uint8_t* payload, size_t payload_size) {
    View view{};
    uint8_t arena_buffer[8192];
    pubsub_itc_fw::BumpAllocator arena(arena_buffer, sizeof(arena_buffer));
    size_t bytes_consumed = 0;
    size_t arena_bytes_needed = 0;
    if (!app::decode(view, payload, payload_size, bytes_consumed, arena, arena_bytes_needed)) {
        return false;
    }
    const std::string dump = app::to_string(view);
    std::printf(" %s\n", dump.c_str());
    return true;
}

void print_hex_preview(const uint8_t* payload, size_t payload_size) {
    std::printf(" [hex]");
    const size_t preview = payload_size < 32 ? payload_size : 32;
    for (size_t i = 0; i < preview; ++i) {
        std::printf(" %02x", payload[i]);
    }
    if (preview < payload_size) {
        std::printf(" ...");
    }
    std::printf("\n");
}

void print_record(const std::string& topic_name, int64_t seq_no, int16_t pdu_id, const uint8_t* payload, size_t payload_size) {
    std::printf(">>> [%s] seq=%lld pdu=%s(%d) size=%zu:", topic_name.c_str(), static_cast<long long>(seq_no), pdu_name(pdu_id), static_cast<int>(pdu_id),
                payload_size);

    // Dispatch on the DSL pdu id to the matching generated view; the structured
    // dump (field=value ...) comes from the generated to_string, so it stays in
    // step with fix_orders.dsl automatically. Unknown/undecodable records
    // fall back to a hex preview.
    bool decoded = false;
    switch (pdu_id) {
        case app::NewOrderSingle::message_pdu_id:
            decoded = decode_and_dump<app::NewOrderSingleView>(payload, payload_size);
            break;
        case app::OrderCancelRequest::message_pdu_id:
            decoded = decode_and_dump<app::OrderCancelRequestView>(payload, payload_size);
            break;
        case app::ExecutionReport::message_pdu_id:
            decoded = decode_and_dump<app::ExecutionReportView>(payload, payload_size);
            break;
        default:
            break;
    }
    if (!decoded) {
        print_hex_preview(payload, payload_size);
    }
    std::fflush(stdout);
}

// Subscriber thread: one data-channel subscription to one topic. Each probed topic gets
// its own ProbeThread (distinct ThreadID) and its own service registry entry (keyed by the
// topic name) pointing at that topic's port, so one probe can watch several topics at once.
class ProbeThread : public pubsub_itc_fw::TopicSubscriberThread {
  public:
    ProbeThread(ConstructorToken token, pubsub_itc_fw::QuillLogger& logger, pubsub_itc_fw::Reactor& reactor, std::string topic_name,
                pubsub_itc_fw::ThreadID thread_id, int64_t from_seq_no)
        : TopicSubscriberThread(token, logger, reactor, "ProbeThread-" + topic_name, thread_id, make_queue_config(), make_allocator_config(),
                                pubsub_itc_fw::ApplicationThreadConfiguration{}, topic_name /* service name = topic name */,
                                "probe-" + topic_name + "-" + std::to_string(::getpid()), topic_name, from_seq_no) {}

  protected:
    void on_pubsub_message(const pubsub_itc_fw::EventMessage& message) override {
        print_record(topic_name(), message.seq_no(), message.pdu_id(), message.payload(), static_cast<size_t>(message.payload_size()));
    }

    void on_connection_established(pubsub_itc_fw::ConnectionID id) override {
        std::printf("--- connected; subscribing to topic '%s'\n", topic_name().c_str());
        std::fflush(stdout);
        TopicSubscriberThread::on_connection_established(id);
    }

    void on_connection_lost(const pubsub_itc_fw::ConnectionID& id, const std::string& reason) override {
        std::printf("--- connection lost: %s\n", reason.c_str());
        std::fflush(stdout);
        TopicSubscriberThread::on_connection_lost(id, reason);
    }
};

int default_port_for_topic(const std::string& topic_name) {
    if (topic_name == "orders") {
        return 11040;
    }
    if (topic_name == "execution_reports") {
        return 11041;
    }
    return 0;
}

// Resolve the --topics value ("all" or a comma-separated list) to a list of topic names.
// "all" expands to every topic in the generated registry, so new topics are covered
// automatically. Unknown names are rejected. Returns an empty list on error (message
// already printed).
std::vector<std::string> resolve_topics(const std::string& topics_arg) {
    std::vector<std::string> topics;
    if (topics_arg == "all") {
        for (const app::Topic topic : app::all_topics) {
            topics.emplace_back(app::to_string(topic));
        }
        return topics;
    }
    size_t start = 0;
    while (start <= topics_arg.size()) {
        const size_t comma = topics_arg.find(',', start);
        const size_t end = comma == std::string::npos ? topics_arg.size() : comma;
        std::string name = topics_arg.substr(start, end - start);
        // Trim surrounding whitespace so "orders, execution_reports" works.
        const size_t first = name.find_first_not_of(" \t");
        const size_t last = name.find_last_not_of(" \t");
        name = first == std::string::npos ? std::string() : name.substr(first, last - first + 1);
        if (!name.empty()) {
            app::Topic topic{};
            if (!app::topic_from_name(name, topic)) {
                std::cerr << "topic_probe: unknown topic '" << name << "' (use --topics=all or a comma-separated list)\n";
                return {};
            }
            topics.push_back(name);
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    if (topics.empty()) {
        std::cerr << "topic_probe: no topics given in --topics\n";
    }
    return topics;
}

} // un-named namespace
} // namespaces

int main(int argc, char** argv) {
    using namespace topic_probe;

    argparse::ArgumentParser program("topic_probe");
    program.add_description("Subscribe to one or more publisher topics (default: all) and print each record.");
    program.add_argument("--topics")
        .default_value(std::string("all"))
        .help("comma-separated topic names, or 'all' (default) for every known topic: orders, execution_reports");
    program.add_argument("--host").default_value(std::string("127.0.0.1")).help("publisher host");
    program.add_argument("--from-seq-no")
        .scan<'i', long long>()
        .default_value(0LL)
        .help("start cursor; 0 (default) replays retained history then streams live; a higher value starts nearer the head");

    try {
        program.parse_args(argc, argv);
    } catch (const std::exception& err) {
        std::cerr << err.what() << "\n" << program;
        return 1;
    }

    const std::string host = program.get<std::string>("--host");
    const int64_t from_seq_no = static_cast<int64_t>(program.get<long long>("--from-seq-no"));

    const std::vector<std::string> topics = resolve_topics(program.get<std::string>("--topics"));
    if (topics.empty()) {
        return 1; // resolve_topics printed the reason
    }
    for (const std::string& topic_name : topics) {
        if (default_port_for_topic(topic_name) <= 0) {
            std::cerr << "topic_probe: no known port for topic '" << topic_name << "'.\n";
            return 1;
        }
    }

    // Report exactly which topics (and ports) this probe is subscribing to.
    std::printf("topic_probe: subscribing to %zu topic(s) from seq_no=%lld (Ctrl-C to stop):\n", topics.size(), static_cast<long long>(from_seq_no));
    for (const std::string& topic_name : topics) {
        std::printf("  - %s (%s:%d)\n", topic_name.c_str(), host.c_str(), default_port_for_topic(topic_name));
    }
    std::fflush(stdout);

    pubsub_itc_fw::QuillLogger::block_signals_before_construction();
    pubsub_itc_fw::QuillLogger logger(pubsub_itc_fw::FwLogLevel::Warning, [](const std::string&) {});

    // One service registry entry and one ProbeThread per topic. The service is keyed by the
    // topic name (each ProbeThread connects to its own), pointing at that topic's port.
    pubsub_itc_fw::ServiceRegistry registry;
    for (const std::string& topic_name : topics) {
        registry.add(topic_name, pubsub_itc_fw::NetworkEndpointConfiguration{host, static_cast<uint16_t>(default_port_for_topic(topic_name))},
                     pubsub_itc_fw::NetworkEndpointConfiguration{});
    }

    pubsub_itc_fw::Reactor reactor(make_reactor_config(), registry, logger);
    std::vector<std::shared_ptr<ProbeThread>> threads;
    int thread_number = pubsub_itc_fw::system_thread_id_value + 1; // thread ids start at 1 (0 is the system thread)
    for (const std::string& topic_name : topics) {
        auto thread = pubsub_itc_fw::ApplicationThread::create<ProbeThread>(logger, reactor, topic_name, pubsub_itc_fw::ThreadID{thread_number}, from_seq_no);
        reactor.register_thread(thread);
        threads.push_back(thread);
        ++thread_number;
    }

    return reactor.run();
}
