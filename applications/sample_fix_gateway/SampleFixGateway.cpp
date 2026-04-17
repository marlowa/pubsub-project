// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "SampleFixGateway.hpp"

#include <chrono>
#include <iostream>

#include <pubsub_itc_fw/FileOpenMode.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>
#include <pubsub_itc_fw/NetworkEndpointConfiguration.hpp>
#include <pubsub_itc_fw/ProtocolType.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>

namespace sample_fix_gateway {

// Configuration constants
const std::string SampleFixGateway::listen_host    = "127.0.0.1";
const std::string SampleFixGateway::sender_comp_id = "GATEWAY";
const std::string SampleFixGateway::target_comp_id = "CLIENT";
const std::string SampleFixGateway::logger_name    = "sample_fix_gateway";

SampleFixGateway::SampleFixGateway()
{
    pubsub_itc_fw::QuillLogger::block_signals_before_construction();
    // Create logger -- log to file, with Info level for file and console,
    // Warning for syslog.
    logger_ = std::make_unique<pubsub_itc_fw::QuillLogger>(
        "sample_fix_gateway.log",
        pubsub_itc_fw::FileOpenMode{pubsub_itc_fw::FileOpenMode::Truncate},
        pubsub_itc_fw::FwLogLevel::Info,
        pubsub_itc_fw::FwLogLevel::Warning,
        pubsub_itc_fw::FwLogLevel::Info);

    // Configure reactor -- use defaults, just tighten timeouts for the sample.
    reactor_configuration_.connect_timeout                   = std::chrono::seconds{5};
    reactor_configuration_.socket_maximum_inactivity_interval_ = std::chrono::seconds{60};
    reactor_configuration_.inactivity_check_interval_          = std::chrono::milliseconds{500};
    reactor_configuration_.shutdown_timeout_                    = std::chrono::seconds{2};

    // Create reactor.
    reactor_ = std::make_unique<pubsub_itc_fw::Reactor>(
        reactor_configuration_, service_registry_, *logger_);

    // Register inbound RawBytes listener -- fix8 will connect here.
    reactor_->register_inbound_listener(
        pubsub_itc_fw::NetworkEndpointConfiguration{listen_host, listen_port},
        pubsub_itc_fw::ThreadID{1},
        pubsub_itc_fw::ProtocolType{pubsub_itc_fw::ProtocolType::RawBytes},
        raw_buffer_capacity);

    // Create and register the gateway thread.
    gateway_thread_ = std::make_shared<FixGatewayThread>(
        *logger_, *reactor_, sender_comp_id, target_comp_id);

    reactor_->register_thread(gateway_thread_);

    PUBSUB_LOG((*logger_), pubsub_itc_fw::FwLogLevel::Info,
        "SampleFixGateway: listening on {}:{}", listen_host, listen_port);
    PUBSUB_LOG((*logger_), pubsub_itc_fw::FwLogLevel::Info,
        "SampleFixGateway: SenderCompID={} TargetCompID={}",
        sender_comp_id, target_comp_id);
}

int SampleFixGateway::run()
{
    PUBSUB_LOG_STR((*logger_), pubsub_itc_fw::FwLogLevel::Info,
        "SampleFixGateway: starting reactor");

    return reactor_->run();
}

} // namespace sample_fix_gateway

// ============================================================
// main
// ============================================================

int main()
{

    try {
        sample_fix_gateway::SampleFixGateway gateway;
        return gateway.run();
    } catch (const std::exception& ex) {
        std::cerr << "SampleFixGateway: fatal exception: " << ex.what() << "\n";
        return 1;
    } catch (...) {
        std::cerr << "SampleFixGateway: unknown fatal exception\n";
        return 1;
    }
}
