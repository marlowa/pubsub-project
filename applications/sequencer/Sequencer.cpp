// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "Sequencer.hpp"
#include "SequencerConfigurationLoader.hpp"
#include "SequencerThread.hpp"

#include <chrono>
#include <iostream>
#include <string>

#include <pubsub_itc_fw/ApplicationAnnouncer.hpp>
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

Sequencer::Sequencer(SequencerConfiguration config, std::unique_ptr<pubsub_itc_fw::QuillLogger> logger)
    : config_(std::move(config)), logger_(std::move(logger)) {
    reactor_configuration_.connect_timeout = std::chrono::seconds{5};
    reactor_configuration_.inactivity_check_interval_ = std::chrono::milliseconds{500};
    reactor_configuration_.shutdown_timeout_ = std::chrono::seconds{2};
    reactor_configuration_.cpu_pinning_enabled = config_.cpu_pinning_enabled;
    reactor_configuration_.cpu_pinning_reserve_cpu0 = config_.cpu_pinning_reserve_cpu0;
    reactor_configuration_.cpu_registry_lock_file = config_.cpu_registry_lock_file;
    reactor_configuration_.command_allocator_configuration_.pool_name = "SequencerCommandPool";
    reactor_configuration_.command_allocator_configuration_.objects_per_pool = config_.command_queue_pool_objects_per_slab;
    reactor_configuration_.command_allocator_configuration_.initial_pools = config_.command_queue_pool_initial_slabs;
    reactor_configuration_.command_allocator_configuration_.handler_for_pool_exhausted = [this](void* /*ctx*/, int objects_per_pool) {
        PUBSUB_LOG(*logger_, pubsub_itc_fw::FwLogLevel::Warning, "SequencerCommandPool exhausted: chaining new pool slab ({} objects)", objects_per_pool);
    };

    reactor_ = std::make_unique<pubsub_itc_fw::Reactor>(reactor_configuration_, service_registry_, *logger_);

    // Inbound PDU listener for order PDUs from gateways.
    // Exempt from idle timeout: the gateway connection is long-lived and may be
    // legitimately quiet during periods of low order flow.
    reactor_->register_inbound_listener(pubsub_itc_fw::NetworkEndpointConfiguration{config_.listen_host, config_.listen_port}, pubsub_itc_fw::ThreadID{1},
                                        pubsub_itc_fw::ProtocolType{pubsub_itc_fw::ProtocolType::FrameworkPdu}, 0, pubsub_itc_fw::IdleTimeoutFlag{pubsub_itc_fw::IdleTimeoutFlag::BypassIdleTimeout});

    // Inbound PDU listener for ExecutionReport PDUs from the matching engine.
    // Exempt from idle timeout: the ME connection is long-lived and quiet between bursts.
    reactor_->register_inbound_listener(pubsub_itc_fw::NetworkEndpointConfiguration{config_.er_listen_host, config_.er_listen_port}, pubsub_itc_fw::ThreadID{1},
                                        pubsub_itc_fw::ProtocolType{pubsub_itc_fw::ProtocolType::FrameworkPdu}, 0, pubsub_itc_fw::IdleTimeoutFlag{pubsub_itc_fw::IdleTimeoutFlag::BypassIdleTimeout});

    // Inbound PDU listener for peer-to-peer leader-follower protocol PDUs.
    // Exempt from idle timeout: the peer connection is long-lived; heartbeats are
    // application-level and do not produce TCP data on every interval.
    reactor_->register_inbound_listener(pubsub_itc_fw::NetworkEndpointConfiguration{config_.peer_listen_host, config_.peer_listen_port},
                                        pubsub_itc_fw::ThreadID{1}, pubsub_itc_fw::ProtocolType{pubsub_itc_fw::ProtocolType::FrameworkPdu}, 0, pubsub_itc_fw::IdleTimeoutFlag{pubsub_itc_fw::IdleTimeoutFlag::BypassIdleTimeout});

    sequencer_thread_ = pubsub_itc_fw::ApplicationThread::create<SequencerThread>(*logger_, *reactor_, config_);

    reactor_->register_thread(sequencer_thread_);

    // Outbound connections are initiated from SequencerThread::on_app_ready_event()
    // via connect_to_service(). The ServiceRegistry is populated here.
    service_registry_.add("gateway", pubsub_itc_fw::NetworkEndpointConfiguration{config_.gateway_host, config_.gateway_port},
                          pubsub_itc_fw::NetworkEndpointConfiguration{});
    service_registry_.add("matching_engine", pubsub_itc_fw::NetworkEndpointConfiguration{config_.matching_engine_host, config_.matching_engine_port},
                          pubsub_itc_fw::NetworkEndpointConfiguration{});
    service_registry_.add("arbiter_primary", pubsub_itc_fw::NetworkEndpointConfiguration{config_.arbiter_primary_host, config_.arbiter_primary_port},
                          pubsub_itc_fw::NetworkEndpointConfiguration{});
    service_registry_.add("arbiter_secondary", pubsub_itc_fw::NetworkEndpointConfiguration{config_.arbiter_secondary_host, config_.arbiter_secondary_port},
                          pubsub_itc_fw::NetworkEndpointConfiguration{});
    service_registry_.add("peer", pubsub_itc_fw::NetworkEndpointConfiguration{config_.peer_host, config_.peer_port},
                          pubsub_itc_fw::NetworkEndpointConfiguration{});

    PUBSUB_LOG((*logger_), pubsub_itc_fw::FwLogLevel::Info, "Sequencer: order listener on {}:{} ER listener on {}:{} instance_id={}", config_.listen_host,
               config_.listen_port, config_.er_listen_host, config_.er_listen_port, config_.instance_id);
    PUBSUB_LOG((*logger_), pubsub_itc_fw::FwLogLevel::Info,
               "Sequencer: gateway={}:{} matching_engine={}:{} arbiter_primary={}:{} arbiter_secondary={}:{} peer_listen={}:{} peer={}:{}",
               config_.gateway_host, config_.gateway_port, config_.matching_engine_host, config_.matching_engine_port, config_.arbiter_primary_host,
               config_.arbiter_primary_port, config_.arbiter_secondary_host, config_.arbiter_secondary_port, config_.peer_listen_host,
               config_.peer_listen_port, config_.peer_host, config_.peer_port);
}

int Sequencer::run() const {
    PUBSUB_LOG_STR((*logger_), pubsub_itc_fw::FwLogLevel::Info, "Sequencer: starting reactor");
    return reactor_->run();
}

} // namespace sequencer

// ============================================================
// main
// ============================================================

int main(int argc, char* argv[]) {
    if (argc < 3 || argc > 4) {
        std::cerr << "Usage: sequencer <logfile> <config.toml> [--replay]\n";
        std::cerr << "  --replay  Read the WAL and replay all records to the matching engine,\n";
        std::cerr << "            then exit.  No gateway or HA connections are opened.\n";
        return 1;
    }

    const std::string log_file = argv[1];
    const std::string config_file = argv[2];
    const bool replay_mode = (argc == 4 && std::string(argv[3]) == "--replay");

    const std::string writable_error = pubsub_itc_fw::QuillLogger::ensure_log_file_writable(log_file);
    if (!writable_error.empty()) {
        std::cerr << "Sequencer: " << writable_error << "\n";
        return 1;
    }

    pubsub_itc_fw::QuillLogger::block_signals_before_construction();

    auto logger = std::make_unique<pubsub_itc_fw::QuillLogger>(log_file, pubsub_itc_fw::FileOpenMode{pubsub_itc_fw::FileOpenMode::Truncate},
                                                               pubsub_itc_fw::FwLogLevel::Info, pubsub_itc_fw::FwLogLevel::Info);
    pubsub_itc_fw::ApplicationAnnouncer::announce(*logger, "sequencer");

    sequencer::SequencerConfiguration config;
    try {
        config = sequencer::SequencerConfigurationLoader::load(config_file);
    } catch (const pubsub_itc_fw::ConfigurationException& ex) {
        PUBSUB_LOG((*logger), pubsub_itc_fw::FwLogLevel::Error, "Sequencer: configuration error: {}", ex.what());
        return 1;
    }

    if (replay_mode) {
        config.replay_mode = true;
        PUBSUB_LOG_STR((*logger), pubsub_itc_fw::FwLogLevel::Info, "Sequencer: --replay flag set -- WAL replay mode enabled");
    }

    logger->set_log_level(config.applog_level);
    logger->set_syslog_level(config.syslog_level);

    try {
        const sequencer::Sequencer app{config, std::move(logger)};
        return app.run();
    } catch (const std::exception& ex) {
        std::cerr << "Sequencer: fatal exception: " << ex.what() << "\n";
        return 1;
    } catch (...) {
        std::cerr << "Sequencer: unknown fatal exception\n";
        return 1;
    }
}
