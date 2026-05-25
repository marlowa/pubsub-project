// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "Sequencer.hpp"
#include "SequencerThread.hpp"
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
#include <utility>

namespace sequencer {

Sequencer::Sequencer(SequencerConfiguration  config, std::unique_ptr<pubsub_itc_fw::QuillLogger> logger) : config_(std::move(config)), logger_(std::move(logger)) {
    reactor_configuration_.connect_timeout = std::chrono::seconds{5};
    reactor_configuration_.socket_maximum_inactivity_interval_ = std::chrono::seconds{600};
    reactor_configuration_.inactivity_check_interval_ = std::chrono::milliseconds{500};
    reactor_configuration_.shutdown_timeout_ = std::chrono::seconds{2};
    reactor_configuration_.cpu_pinning_enabled = config_.cpu_pinning_enabled;
    reactor_configuration_.cpu_pinning_dev_mode = config_.cpu_pinning_dev_mode;
    reactor_configuration_.cpu_registry_lock_file = config_.cpu_registry_lock_file;
    reactor_configuration_.command_allocator_configuration_.pool_name = "SequencerCommandPool";
    reactor_configuration_.command_allocator_configuration_.objects_per_pool = config_.command_queue_pool_objects_per_slab;
    reactor_configuration_.command_allocator_configuration_.initial_pools = config_.command_queue_pool_initial_slabs;
    reactor_configuration_.command_allocator_configuration_.handler_for_pool_exhausted = [this](void* /*ctx*/, int objects_per_pool) {
        PUBSUB_LOG(*logger_, pubsub_itc_fw::FwLogLevel::Warning,
                   "SequencerCommandPool exhausted: chaining new pool slab ({} objects)", objects_per_pool);
    };

    reactor_ = std::make_unique<pubsub_itc_fw::Reactor>(reactor_configuration_, service_registry_, *logger_);

    // Inbound PDU listener for order PDUs from gateways.
    reactor_->register_inbound_listener(pubsub_itc_fw::NetworkEndpointConfiguration{config_.listen_host, config_.listen_port}, pubsub_itc_fw::ThreadID{1},
                                        pubsub_itc_fw::ProtocolType{pubsub_itc_fw::ProtocolType::FrameworkPdu}, 0);

    // Inbound PDU listener for ExecutionReport PDUs from the matching engine.
    // The ME connects outbound to this port; the sequencer forwards ERs to gateways.
    reactor_->register_inbound_listener(pubsub_itc_fw::NetworkEndpointConfiguration{config_.er_listen_host, config_.er_listen_port}, pubsub_itc_fw::ThreadID{1},
                                        pubsub_itc_fw::ProtocolType{pubsub_itc_fw::ProtocolType::FrameworkPdu}, 0);

    // Inbound PDU listener for peer-to-peer leader-follower protocol PDUs.
    // The peer sequencer connects to this port; primary uses 7003, secondary 7004.
    reactor_->register_inbound_listener(pubsub_itc_fw::NetworkEndpointConfiguration{config_.peer_listen_host, config_.peer_listen_port}, pubsub_itc_fw::ThreadID{1},
                                        pubsub_itc_fw::ProtocolType{pubsub_itc_fw::ProtocolType::FrameworkPdu}, 0);

    sequencer_thread_ = pubsub_itc_fw::ApplicationThread::create<SequencerThread>(*logger_, *reactor_, config_);

    reactor_->register_thread(sequencer_thread_);

    // Outbound connections are initiated from SequencerThread::on_app_ready_event()
    // via connect_to_service(). The ServiceRegistry is populated here.
    service_registry_.add("gateway", pubsub_itc_fw::NetworkEndpointConfiguration{config_.gateway_host, config_.gateway_port},
                          pubsub_itc_fw::NetworkEndpointConfiguration{});
    service_registry_.add("matching_engine", pubsub_itc_fw::NetworkEndpointConfiguration{config_.matching_engine_host, config_.matching_engine_port},
                          pubsub_itc_fw::NetworkEndpointConfiguration{});
    service_registry_.add("arbiter", pubsub_itc_fw::NetworkEndpointConfiguration{config_.arbiter_host, config_.arbiter_port},
                          pubsub_itc_fw::NetworkEndpointConfiguration{});
    service_registry_.add("peer", pubsub_itc_fw::NetworkEndpointConfiguration{config_.peer_host, config_.peer_port},
                          pubsub_itc_fw::NetworkEndpointConfiguration{});

    PUBSUB_LOG((*logger_), pubsub_itc_fw::FwLogLevel::Info, "Sequencer: order listener on {}:{} ER listener on {}:{} instance_id={}", config_.listen_host,
               config_.listen_port, config_.er_listen_host, config_.er_listen_port, config_.instance_id);
    PUBSUB_LOG((*logger_), pubsub_itc_fw::FwLogLevel::Info,
               "Sequencer: gateway={}:{} matching_engine={}:{} arbiter={}:{} peer_listen={}:{} peer={}:{}",
               config_.gateway_host, config_.gateway_port, config_.matching_engine_host, config_.matching_engine_port,
               config_.arbiter_host, config_.arbiter_port,
               config_.peer_listen_host, config_.peer_listen_port, config_.peer_host, config_.peer_port);
}

int Sequencer::run()const {
    PUBSUB_LOG_STR((*logger_), pubsub_itc_fw::FwLogLevel::Info, "Sequencer: starting reactor");
    return reactor_->run();
}

} // namespace sequencer

// ============================================================
// main
// ============================================================

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "Usage: sequencer <logfile> <config.toml>\n";
        return 1;
    }

    const std::string log_file = argv[1];
    const std::string config_file = argv[2];

    const std::string writable_error = pubsub_itc_fw::QuillLogger::ensure_log_file_writable(log_file);
    if (!writable_error.empty()) {
        std::cerr << "Sequencer: " << writable_error << "\n";
        return 1;
    }

    pubsub_itc_fw::QuillLogger::block_signals_before_construction();

    auto logger = std::make_unique<pubsub_itc_fw::QuillLogger>(log_file, pubsub_itc_fw::FileOpenMode{pubsub_itc_fw::FileOpenMode::Truncate},
                                                               pubsub_itc_fw::FwLogLevel::Info, pubsub_itc_fw::FwLogLevel::Info);

    sequencer::SequencerConfiguration config;
    try {
        config = sequencer::SequencerConfigurationLoader::load(config_file);
    } catch (const pubsub_itc_fw::ConfigurationException& ex) {
        PUBSUB_LOG((*logger), pubsub_itc_fw::FwLogLevel::Error, "Sequencer: configuration error: {}", ex.what());
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
