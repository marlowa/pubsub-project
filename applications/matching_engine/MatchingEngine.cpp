// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "MatchingEngine.hpp"
#include "MatchingEngineConfigurationLoader.hpp"

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

namespace matching_engine {

const std::string MatchingEngine::log_file_name = "matching_engine.log";

MatchingEngine::MatchingEngine(const MatchingEngineConfiguration& config)
    : config_(config)
{
    pubsub_itc_fw::QuillLogger::block_signals_before_construction();

    logger_ = std::make_unique<pubsub_itc_fw::QuillLogger>(
        log_file_name,
        pubsub_itc_fw::FileOpenMode{pubsub_itc_fw::FileOpenMode::Truncate},
        pubsub_itc_fw::FwLogLevel::Info,
        pubsub_itc_fw::FwLogLevel::Info);

    reactor_configuration_.connect_timeout                     = std::chrono::seconds{5};
    reactor_configuration_.socket_maximum_inactivity_interval_ = std::chrono::seconds{120};
    reactor_configuration_.inactivity_check_interval_          = std::chrono::milliseconds{500};
    reactor_configuration_.shutdown_timeout_                    = std::chrono::seconds{2};

    reactor_ = std::make_unique<pubsub_itc_fw::Reactor>(
        reactor_configuration_, service_registry_, *logger_);

    // Inbound PDU listener for SequencedMessage PDUs from the sequencer.
    reactor_->register_inbound_listener(
        pubsub_itc_fw::NetworkEndpointConfiguration{config_.listen_host, config_.listen_port},
        pubsub_itc_fw::ThreadID{1},
        pubsub_itc_fw::ProtocolType{pubsub_itc_fw::ProtocolType::FrameworkPdu},
        0);

    matching_engine_thread_ = pubsub_itc_fw::ApplicationThread::create<MatchingEngineThread>(
        *logger_, *reactor_, config_);

    reactor_->register_thread(matching_engine_thread_);

    // Outbound PDU connection to the gateway is initiated from
    // MatchingEngineThread::on_app_ready_event() via connect_to_service().
    // TODO: replace with pub/sub fanout when implemented.
    service_registry_.add("gateway",
        pubsub_itc_fw::NetworkEndpointConfiguration{
            config_.gateway_host, config_.gateway_port},
        pubsub_itc_fw::NetworkEndpointConfiguration{});

    PUBSUB_LOG((*logger_), pubsub_itc_fw::FwLogLevel::Info,
               "MatchingEngine: listening for sequenced PDUs on {}:{}",
               config_.listen_host, config_.listen_port);
    PUBSUB_LOG((*logger_), pubsub_itc_fw::FwLogLevel::Info,
               "MatchingEngine: gateway ER connection to {}:{} (TODO: replace with pub/sub)",
               config_.gateway_host, config_.gateway_port);
}

int MatchingEngine::run()
{
    PUBSUB_LOG_STR((*logger_), pubsub_itc_fw::FwLogLevel::Info,
                   "MatchingEngine: starting reactor");
    return reactor_->run();
}

} // namespace matching_engine

// ============================================================
// main
// ============================================================

int main(int argc, char* argv[])
{
    try {
        matching_engine::MatchingEngineConfiguration config;

        if (argc >= 2) {
            const std::string config_file = argv[1];
            std::cout << "MatchingEngine: loading configuration from " << config_file << "\n";
            config = matching_engine::MatchingEngineConfigurationLoader::load(config_file);
        } else {
            std::cout << "MatchingEngine: no configuration file supplied, using built-in defaults\n";
        }

        matching_engine::MatchingEngine app{config};
        return app.run();

    } catch (const pubsub_itc_fw::ConfigurationException& ex) {
        std::cerr << "MatchingEngine: configuration error: " << ex.what() << "\n";
        return 1;
    } catch (const std::exception& ex) {
        std::cerr << "MatchingEngine: fatal exception: " << ex.what() << "\n";
        return 1;
    } catch (...) {
        std::cerr << "MatchingEngine: unknown fatal exception\n";
        return 1;
    }
}
