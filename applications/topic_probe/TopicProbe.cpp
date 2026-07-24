// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

/*
topic_probe -- a tiny diagnostic topic subscriber (a minimal stand-in for TAP).

It connects to a publisher's topic port (e.g. the MEP), subscribes to one topic
over the data channel using the reusable pubsub_itc_fw::TopicSubscriberThread,
and prints each record it receives to stdout. Use it to watch orders / execution
reports stream out of the MEP live while driving the Java fix-test-client.

Usage:
  topic_probe <topic_name> [host] [port] [from_seq_no]

  topic_name   orders | execution_reports (any recognised topic)
  host         default 127.0.0.1
  port         default by topic: orders -> 11040, execution_reports -> 11041
               (the MEP primary's deployed ports; override for the secondary)
  from_seq_no  default 0 -- replay everything the publisher still retains, then
               stream live. Give a higher number to start nearer the head.

Stop with Ctrl-C. If the publisher instance is a follower it answers
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

#include <unistd.h>

#include <argparse/argparse.hpp>

#include <fix_orders.hpp>

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

// Subscriber thread: one data-channel subscription to one topic.
class ProbeThread : public pubsub_itc_fw::TopicSubscriberThread {
  public:
    ProbeThread(ConstructorToken token, pubsub_itc_fw::QuillLogger& logger, pubsub_itc_fw::Reactor& reactor, std::string topic_name, int64_t from_seq_no)
        : TopicSubscriberThread(token, logger, reactor, "ProbeThread", pubsub_itc_fw::ThreadID{1}, make_queue_config(), make_allocator_config(),
                                pubsub_itc_fw::ApplicationThreadConfiguration{}, "publisher", "probe-" + topic_name + "-" + std::to_string(::getpid()),
                                topic_name, from_seq_no) {}

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

} // un-named namespace
} // namespaces

int main(int argc, char** argv) {
    using namespace topic_probe;

    argparse::ArgumentParser program("topic_probe");
    program.add_description("Subscribe to one topic on a publisher (e.g. the MEP) and print each record.");
    program.add_argument("topic").help("topic name: orders | execution_reports (any recognised topic)");
    program.add_argument("--host").default_value(std::string("127.0.0.1")).help("publisher host");
    program.add_argument("--port").scan<'i', int>().help("publisher port (default by topic: orders=11040, execution_reports=11041)");
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

    const std::string topic_name = program.get<std::string>("topic");
    const std::string host = program.get<std::string>("--host");
    const int port = program.present<int>("--port").value_or(default_port_for_topic(topic_name));
    const int64_t from_seq_no = static_cast<int64_t>(program.get<long long>("--from-seq-no"));

    if (port <= 0) {
        std::cerr << "topic_probe: no default port for topic '" << topic_name << "'; pass --port explicitly.\n";
        return 1;
    }

    std::printf("topic_probe: subscribing to '%s' at %s:%d from seq_no=%lld (Ctrl-C to stop)\n", topic_name.c_str(), host.c_str(), port,
                static_cast<long long>(from_seq_no));
    std::fflush(stdout);

    pubsub_itc_fw::QuillLogger::block_signals_before_construction();
    pubsub_itc_fw::QuillLogger logger(pubsub_itc_fw::FwLogLevel::Warning, [](const std::string&) {});

    pubsub_itc_fw::ServiceRegistry registry;
    registry.add("publisher", pubsub_itc_fw::NetworkEndpointConfiguration{host, static_cast<uint16_t>(port)}, pubsub_itc_fw::NetworkEndpointConfiguration{});

    pubsub_itc_fw::Reactor reactor(make_reactor_config(), registry, logger);
    auto thread = pubsub_itc_fw::ApplicationThread::create<ProbeThread>(logger, reactor, topic_name, from_seq_no);
    reactor.register_thread(thread);

    return reactor.run();
}
