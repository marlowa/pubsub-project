// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "SampleFixGateway.hpp"
#include "FixGatewayConfigurationLoader.hpp"

#include <chrono>
#include <iostream>

#include <pubsub_itc_fw/ConfigurationException.hpp>
#include <pubsub_itc_fw/FileOpenMode.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>
#include <pubsub_itc_fw/NetworkEndpointConfiguration.hpp>
#include <pubsub_itc_fw/ProtocolType.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>

namespace sample_fix_gateway {

const std::string SampleFixGateway::log_file_name = "sample_fix_gateway.log";

SampleFixGateway::SampleFixGateway(const FixGatewayConfiguration& config)
    : config_(config)
{
    // Block SIGINT and SIGTERM before spawning any threads -- including the
    // quill backend thread. The Reactor consumes these signals via signalfd.
    pubsub_itc_fw::QuillLogger::block_signals_before_construction();

    logger_ = std::make_unique<pubsub_itc_fw::QuillLogger>(
    log_file_name,
    pubsub_itc_fw::FileOpenMode{pubsub_itc_fw::FileOpenMode::Truncate},
    pubsub_itc_fw::FwLogLevel::Info,
    pubsub_itc_fw::FwLogLevel::Info);

    // Reactor configuration -- use defaults, adjusted for the sample.
    reactor_configuration_.connect_timeout                     = std::chrono::seconds{5};
    reactor_configuration_.socket_maximum_inactivity_interval_ = std::chrono::seconds{120};
    reactor_configuration_.inactivity_check_interval_          = std::chrono::milliseconds{500};
    reactor_configuration_.shutdown_timeout_                    = std::chrono::seconds{2};

    reactor_ = std::make_unique<pubsub_itc_fw::Reactor>(
        reactor_configuration_, service_registry_, *logger_);

    reactor_->register_inbound_listener(
        pubsub_itc_fw::NetworkEndpointConfiguration{config_.listen_host, config_.listen_port},
        pubsub_itc_fw::ThreadID{1},
        pubsub_itc_fw::ProtocolType{pubsub_itc_fw::ProtocolType::RawBytes},
        config_.raw_buffer_capacity);

    gateway_thread_ = pubsub_itc_fw::ApplicationThread::create<FixGatewayThread>(*logger_, *reactor_, config_);

    reactor_->register_thread(gateway_thread_);

    PUBSUB_LOG((*logger_), pubsub_itc_fw::FwLogLevel::Info,
        "SampleFixGateway: listening on {}:{}",
        config_.listen_host, config_.listen_port);
    PUBSUB_LOG((*logger_), pubsub_itc_fw::FwLogLevel::Info,
        "SampleFixGateway: SenderCompID={} TargetCompID={}",
        config_.sender_comp_id, config_.default_target_comp_id);
    PUBSUB_LOG((*logger_), pubsub_itc_fw::FwLogLevel::Info,
        "SampleFixGateway: logon timeout={}s",
        config_.logon_timeout.count());
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

int main(int argc, char* argv[])
{
    try {
        sample_fix_gateway::FixGatewayConfiguration config;

        if (argc >= 2) {
            // Load configuration from the TOML file given as the first argument.
            const std::string config_file = argv[1];
            std::cout << "SampleFixGateway: loading configuration from "
                      << config_file << "\n";
            config = sample_fix_gateway::FixGatewayConfigurationLoader::load(config_file);
        } else {
            // No config file supplied -- use compiled-in defaults and warn.
            std::cout << "SampleFixGateway: no configuration file supplied, "
                         "using built-in defaults\n";
        }

        sample_fix_gateway::SampleFixGateway gateway{config};
        return gateway.run();

    } catch (const pubsub_itc_fw::ConfigurationException& ex) {
        std::cerr << "SampleFixGateway: configuration error: " << ex.what() << "\n";
        return 1;
    } catch (const std::exception& ex) {
        std::cerr << "SampleFixGateway: fatal exception: " << ex.what() << "\n";
        return 1;
    } catch (...) {
        std::cerr << "SampleFixGateway: unknown fatal exception\n";
        return 1;
    }
}
