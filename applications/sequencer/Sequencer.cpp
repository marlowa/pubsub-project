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

const std::string Sequencer::log_file_name = "sequencer.log";

Sequencer::Sequencer(const SequencerConfiguration& config)
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

    // Inbound PDU listener for order PDUs from gateways.
    reactor_->register_inbound_listener(
        pubsub_itc_fw::NetworkEndpointConfiguration{config_.listen_host, config_.listen_port},
        pubsub_itc_fw::ThreadID{1},
        pubsub_itc_fw::ProtocolType{pubsub_itc_fw::ProtocolType::FrameworkPdu},
        0);

    sequencer_thread_ = pubsub_itc_fw::ApplicationThread::create<SequencerThread>(
        *logger_, *reactor_, config_);

    reactor_->register_thread(sequencer_thread_);

    // Outbound connections are initiated from SequencerThread::on_app_ready_event()
    // via connect_to_service(). The ServiceRegistry is populated here.
    service_registry_.add("matching_engine",
        pubsub_itc_fw::NetworkEndpointConfiguration{
            config_.matching_engine_host, config_.matching_engine_port},
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
               "Sequencer: listening for gateway PDUs on {}:{} instance_id={}",
               config_.listen_host, config_.listen_port, config_.instance_id);
    PUBSUB_LOG((*logger_), pubsub_itc_fw::FwLogLevel::Info,
               "Sequencer: matching engine={}:{} peer={}:{} arbiter={}:{}",
               config_.matching_engine_host, config_.matching_engine_port,
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
    try {
        sequencer::SequencerConfiguration config;

        if (argc >= 2) {
            const std::string config_file = argv[1];
            std::cout << "Sequencer: loading configuration from " << config_file << "\n";
            config = sequencer::SequencerConfigurationLoader::load(config_file);
        } else {
            std::cout << "Sequencer: no configuration file supplied, using built-in defaults\n";
        }

        sequencer::Sequencer app{config};
        return app.run();

    } catch (const pubsub_itc_fw::ConfigurationException& ex) {
        std::cerr << "Sequencer: configuration error: " << ex.what() << "\n";
        return 1;
    } catch (const std::exception& ex) {
        std::cerr << "Sequencer: fatal exception: " << ex.what() << "\n";
        return 1;
    } catch (...) {
        std::cerr << "Sequencer: unknown fatal exception\n";
        return 1;
    }
}
