// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "MatchingEnginePublisher.hpp"
#include "MatchingEnginePublisherConfigurationLoader.hpp"

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

namespace matching_engine_publisher {

MatchingEnginePublisher::MatchingEnginePublisher(MatchingEnginePublisherConfiguration config,
                                                  std::unique_ptr<pubsub_itc_fw::QuillLogger> logger)
    : config_(std::move(config)), logger_(std::move(logger)) {

    reactor_configuration_.connect_timeout                 = std::chrono::seconds{5};
    reactor_configuration_.inactivity_check_interval_      = std::chrono::milliseconds{500};
    reactor_configuration_.shutdown_timeout_               = std::chrono::seconds{2};
    reactor_configuration_.cpu_pinning_enabled             = config_.cpu_pinning_enabled;
    reactor_configuration_.cpu_pinning_reserve_cpu0        = config_.cpu_pinning_reserve_cpu0;
    reactor_configuration_.cpu_registry_lock_file          = config_.cpu_registry_lock_file;
    reactor_configuration_.connect_retry_warning_interval_ = config_.connect_retry_warning_interval;
    reactor_configuration_.command_allocator_configuration_.pool_name        = "MepCommandPool";
    reactor_configuration_.command_allocator_configuration_.objects_per_pool = config_.command_queue_pool_objects_per_slab;
    reactor_configuration_.command_allocator_configuration_.initial_pools    = config_.command_queue_pool_initial_slabs;
    reactor_configuration_.command_allocator_configuration_.handler_for_pool_exhausted =
        [this](void*, int objects_per_pool) {
            PUBSUB_LOG(*logger_, pubsub_itc_fw::FwLogLevel::Warning,
                       "MepCommandPool exhausted: chaining new pool slab ({} objects)", objects_per_pool);
        };

    reactor_ = std::make_unique<pubsub_itc_fw::Reactor>(reactor_configuration_, service_registry_, *logger_);

    // Inbound listener: "orders" topic subscribers.
    reactor_->register_inbound_listener(
        pubsub_itc_fw::NetworkEndpointConfiguration{config_.listen_host, config_.orders_listen_port},
        pubsub_itc_fw::ThreadID{1},
        pubsub_itc_fw::ProtocolType{pubsub_itc_fw::ProtocolType::FrameworkPdu}, 0,
        pubsub_itc_fw::IdleTimeoutFlag{pubsub_itc_fw::IdleTimeoutFlag::BypassIdleTimeout});

    // Inbound listener: "execution_reports" topic subscribers.
    reactor_->register_inbound_listener(
        pubsub_itc_fw::NetworkEndpointConfiguration{config_.listen_host, config_.er_listen_port},
        pubsub_itc_fw::ThreadID{1},
        pubsub_itc_fw::ProtocolType{pubsub_itc_fw::ProtocolType::FrameworkPdu}, 0,
        pubsub_itc_fw::IdleTimeoutFlag{pubsub_itc_fw::IdleTimeoutFlag::BypassIdleTimeout});

    // Inbound listener: HA peer.
    if (config_.ha_enabled) {
        reactor_->register_inbound_listener(
            pubsub_itc_fw::NetworkEndpointConfiguration{config_.peer_listen_host, config_.peer_listen_port},
            pubsub_itc_fw::ThreadID{1},
            pubsub_itc_fw::ProtocolType{pubsub_itc_fw::ProtocolType::FrameworkPdu}, 0,
            pubsub_itc_fw::IdleTimeoutFlag{pubsub_itc_fw::IdleTimeoutFlag::BypassIdleTimeout});
    }

    thread_ = pubsub_itc_fw::ApplicationThread::create<MatchingEnginePublisherThread>(
        *logger_, *reactor_, config_);
    reactor_->register_thread(thread_);

    // Outbound: sequencer WAL subscriber listeners.
    service_registry_.add("sequencer",
        pubsub_itc_fw::NetworkEndpointConfiguration{config_.sequencer_wal_host, config_.sequencer_wal_port},
        pubsub_itc_fw::NetworkEndpointConfiguration{});
    service_registry_.add("sequencer_secondary",
        pubsub_itc_fw::NetworkEndpointConfiguration{config_.sequencer_wal_secondary_host,
                                                     config_.sequencer_wal_secondary_port},
        pubsub_itc_fw::NetworkEndpointConfiguration{});

    // Outbound: HA.
    if (config_.ha_enabled) {
        service_registry_.add("peer",
            pubsub_itc_fw::NetworkEndpointConfiguration{config_.peer_host, config_.peer_port},
            pubsub_itc_fw::NetworkEndpointConfiguration{});
        service_registry_.add("arbiter_primary",
            pubsub_itc_fw::NetworkEndpointConfiguration{config_.arbiter_primary_host,
                                                         config_.arbiter_primary_port},
            pubsub_itc_fw::NetworkEndpointConfiguration{});
        service_registry_.add("arbiter_secondary",
            pubsub_itc_fw::NetworkEndpointConfiguration{config_.arbiter_secondary_host,
                                                         config_.arbiter_secondary_port},
            pubsub_itc_fw::NetworkEndpointConfiguration{});
    }

    PUBSUB_LOG(*logger_, pubsub_itc_fw::FwLogLevel::Info,
               "MatchingEnginePublisher: orders={}:{} er={}:{} sequencer_wal={}:{} sequencer_wal_secondary={}:{} instance_id={}",
               config_.listen_host, config_.orders_listen_port,
               config_.listen_host, config_.er_listen_port,
               config_.sequencer_wal_host, config_.sequencer_wal_port,
               config_.sequencer_wal_secondary_host, config_.sequencer_wal_secondary_port,
               config_.instance_id);
}

int MatchingEnginePublisher::run() const {
    PUBSUB_LOG_STR(*logger_, pubsub_itc_fw::FwLogLevel::Info, "MatchingEnginePublisher: starting reactor");
    return reactor_->run();
}

} // namespaces

// ============================================================
// main
// ============================================================

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "Usage: matching_engine_publisher <logfile> <config.toml>\n";
        return 1;
    }

    const std::string log_file    = argv[1];
    const std::string config_file = argv[2];

    const std::string writable_error = pubsub_itc_fw::QuillLogger::ensure_log_file_writable(log_file);
    if (!writable_error.empty()) {
        std::cerr << "MatchingEnginePublisher: " << writable_error << "\n";
        return 1;
    }

    pubsub_itc_fw::QuillLogger::block_signals_before_construction();

    auto logger = std::make_unique<pubsub_itc_fw::QuillLogger>(
        log_file,
        pubsub_itc_fw::FileOpenMode{pubsub_itc_fw::FileOpenMode::Truncate},
        pubsub_itc_fw::FwLogLevel::Info,
        pubsub_itc_fw::FwLogLevel::Info);
    pubsub_itc_fw::ApplicationAnnouncer::announce(*logger, "matching_engine_publisher");

    matching_engine_publisher::MatchingEnginePublisherConfiguration config;
    try {
        config = matching_engine_publisher::MatchingEnginePublisherConfigurationLoader::load(config_file);
    } catch (const pubsub_itc_fw::ConfigurationException& ex) {
        PUBSUB_LOG(*logger, pubsub_itc_fw::FwLogLevel::Error,
                   "MatchingEnginePublisher: configuration error: {}", ex.what());
        return 1;
    }

    logger->set_log_level(config.applog_level);
    logger->set_syslog_level(config.syslog_level);

    try {
        const matching_engine_publisher::MatchingEnginePublisher app{config, std::move(logger)};
        return app.run();
    } catch (const std::exception& ex) {
        std::cerr << "MatchingEnginePublisher: fatal exception: " << ex.what() << "\n";
        return 1;
    } catch (...) {
        std::cerr << "MatchingEnginePublisher: unknown fatal exception\n";
        return 1;
    }
}
