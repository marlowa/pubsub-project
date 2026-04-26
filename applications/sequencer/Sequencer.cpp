// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "Sequencer.hpp"
#include "SequencerConfigurationLoader.hpp"

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

namespace sequencer {

Sequencer::Sequencer(const SequencerConfiguration& config,
                 std::unique_ptr<pubsub_itc_fw::QuillLogger> logger)
    : config_(config)
    , logger_(std::move(logger))
{
    reactor_configuration_.connect_timeout                     = std::chrono::seconds{5};
    reactor_configuration_.socket_maximum_inactivity_interval_ = std::chrono::seconds{120};
    reactor_configuration_.inactivity_check_interval_          = std::chrono::milliseconds{500};
    reactor_configuration_.shutdown_timeout_                    = std::chrono::seconds{2};

    reactor_ = std::make_unique<pubsub_itc_fw::Reactor>(
        reactor_configuration_, service_registry_, *logger_);

    // Inbound PDU listener for order PDUs from gateways.
    reactor_->register_inbound_listener(
        pubsub_itc_fw::NetworkEndpointConfiguration{config_.listen_host, config_.listen_port},
        pubsub_itc_fw::ThreadID{1},
        pubsub_itc_fw::ProtocolType{pubsub_itc_fw::ProtocolType::FrameworkPdu},
        0);

    // Inbound PDU listener for ExecutionReport PDUs from the matching engine.
    // The ME connects outbound to this port; the sequencer forwards ERs to gateways.
    reactor_->register_inbound_listener(
        pubsub_itc_fw::NetworkEndpointConfiguration{config_.er_listen_host, config_.er_listen_port},
        pubsub_itc_fw::ThreadID{1},
        pubsub_itc_fw::ProtocolType{pubsub_itc_fw::ProtocolType::FrameworkPdu},
        0);

    sequencer_thread_ = pubsub_itc_fw::ApplicationThread::create<SequencerThread>(
        *logger_, *reactor_, config_);

    reactor_->register_thread(sequencer_thread_);

    // Outbound connections are initiated from SequencerThread::on_app_ready_event()
    // via connect_to_service(). The ServiceRegistry is populated here.
    service_registry_.add("gateway",
        pubsub_itc_fw::NetworkEndpointConfiguration{
            config_.gateway_host, config_.gateway_port},
        pubsub_itc_fw::NetworkEndpointConfiguration{});
    service_registry_.add("sequencer_peer",
        pubsub_itc_fw::NetworkEndpointConfiguration{
            config_.peer_host, config_.peer_port},
        pubsub_itc_fw::NetworkEndpointConfiguration{});
    service_registry_.add("arbiter",
        pubsub_itc_fw::NetworkEndpointConfiguration{
            config_.arbiter_host, config_.arbiter_port},
        pubsub_itc_fw::NetworkEndpointConfiguration{});

    PUBSUB_LOG((*logger_), pubsub_itc_fw::FwLogLevel::Info,
               "Sequencer: order listener on {}:{} ER listener on {}:{} instance_id={}",
               config_.listen_host, config_.listen_port,
               config_.er_listen_host, config_.er_listen_port,
               config_.instance_id);
    PUBSUB_LOG((*logger_), pubsub_itc_fw::FwLogLevel::Info,
               "Sequencer: gateway={}:{} peer={}:{} arbiter={}:{}",
               config_.gateway_host, config_.gateway_port,
               config_.peer_host, config_.peer_port,
               config_.arbiter_host, config_.arbiter_port);
}

int Sequencer::run()
{
    PUBSUB_LOG_STR((*logger_), pubsub_itc_fw::FwLogLevel::Info,
                   "Sequencer: starting reactor");
    return reactor_->run();
}

} // namespace sequencer

// ============================================================
// main
// ============================================================

int main(int argc, char* argv[])
{
    if (argc != 3) {
        std::cerr << "Usage: sequencer <logfile> <config.toml>\n";
        return 1;
    }

    const std::string log_file    = argv[1];
    const std::string config_file = argv[2];

    const std::string writable_error =
        pubsub_itc_fw::QuillLogger::ensure_log_file_writable(log_file);
    if (!writable_error.empty()) {
        std::cerr << "Sequencer: " << writable_error << "\n";
        return 1;
    }

    pubsub_itc_fw::QuillLogger::block_signals_before_construction();

    auto logger = std::make_unique<pubsub_itc_fw::QuillLogger>(
        log_file,
        pubsub_itc_fw::FileOpenMode{pubsub_itc_fw::FileOpenMode::Truncate},
        pubsub_itc_fw::FwLogLevel::Info,
        pubsub_itc_fw::FwLogLevel::Info);

    sequencer::SequencerConfiguration config;
    try {
        config = sequencer::SequencerConfigurationLoader::load(config_file);
    } catch (const pubsub_itc_fw::ConfigurationException& ex) {
        PUBSUB_LOG((*logger), pubsub_itc_fw::FwLogLevel::Error,
                   "Sequencer: configuration error: {}", ex.what());
        return 1;
    }

    logger->set_log_level(config.applog_level);
    logger->set_syslog_level(config.syslog_level);

    try {
        sequencer::Sequencer app{config, std::move(logger)};
        return app.run();
    } catch (const std::exception& ex) {
        std::cerr << "Sequencer: fatal exception: " << ex.what() << "\n";
        return 1;
    } catch (...) {
        std::cerr << "Sequencer: unknown fatal exception\n";
        return 1;
    }
}
