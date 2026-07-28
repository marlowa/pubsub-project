// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "BinaryGateway.hpp"
#include "BinaryGatewayConfigurationLoader.hpp"

#include <chrono>
#include <iostream>
#include <string>
#include <utility>

#include <pubsub_itc_fw/ApplicationAnnouncer.hpp>
#include <pubsub_itc_fw/ConfigurationException.hpp>
#include <pubsub_itc_fw/CpuLayout.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/HotPathThreadCount.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>
#include <pubsub_itc_fw/NetworkEndpointConfiguration.hpp>
#include <pubsub_itc_fw/ProtocolType.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>

namespace binary_gateway {

BinaryGateway::BinaryGateway(const BinaryGatewayConfiguration& config, std::unique_ptr<pubsub_itc_fw::QuillLogger> logger)
    : config_(config), logger_(std::move(logger)) {
    reactor_configuration_.connect_timeout = std::chrono::seconds{5};
    reactor_configuration_.socket_maximum_inactivity_interval_ = std::chrono::seconds{600};
    reactor_configuration_.inactivity_check_interval_ = std::chrono::milliseconds{500};
    reactor_configuration_.shutdown_timeout_ = std::chrono::seconds{2};
    reactor_configuration_.cpu_pinning_enabled = config_.cpu_pinning_enabled;
    reactor_configuration_.cpu_pinning_reserve_cpu0 = config_.cpu_pinning_reserve_cpu0;
    reactor_configuration_.cpu_registry_shm_path = config_.cpu_registry_shm_path;
    reactor_configuration_.cpu_registry_lock_file = config_.cpu_registry_lock_file;
    reactor_configuration_.cpu_layout_file = config_.cpu_layout_file;
    reactor_configuration_.cpu_layout_component = config_.cpu_layout_component;
    reactor_configuration_.connect_retry_warning_interval_ = config_.connect_retry_warning_interval;
    reactor_configuration_.command_allocator_configuration_.pool_name = "BinaryGatewayCommandPool";
    reactor_configuration_.command_allocator_configuration_.objects_per_pool = config_.command_queue_pool_objects_per_slab;
    reactor_configuration_.command_allocator_configuration_.initial_pools = config_.command_queue_pool_initial_slabs;
    reactor_configuration_.command_allocator_configuration_.handler_for_pool_exhausted = [this](void* /*context*/, int objects_per_pool) {
        PUBSUB_LOG(*logger_, pubsub_itc_fw::FwLogLevel::Warning, "BinaryGatewayCommandPool exhausted: chaining new pool slab ({} objects)", objects_per_pool);
    };

    reactor_ = std::make_unique<pubsub_itc_fw::Reactor>(reactor_configuration_, service_registry_, *logger_);

    // Client listener. FrameworkPdu, not RawBytes: clients speak framed PDUs, so the
    // framework delivers whole messages and there is no stream to parse.
    reactor_->register_inbound_listener(pubsub_itc_fw::NetworkEndpointConfiguration{config_.listen_host, config_.listen_port}, pubsub_itc_fw::ThreadID{1},
                                        pubsub_itc_fw::ProtocolType{pubsub_itc_fw::ProtocolType::FrameworkPdu}, 0);

    // Inbound ExecutionReport listener; the sequencer dials this endpoint.
    // BypassIdleTimeout: a long-lived quiet infrastructure link, not a client session.
    reactor_->register_inbound_listener(pubsub_itc_fw::NetworkEndpointConfiguration{config_.er_listen_host, config_.er_listen_port}, pubsub_itc_fw::ThreadID{1},
                                        pubsub_itc_fw::ProtocolType{pubsub_itc_fw::ProtocolType::FrameworkPdu}, 0,
                                        pubsub_itc_fw::IdleTimeoutFlag{pubsub_itc_fw::IdleTimeoutFlag::BypassIdleTimeout});

    gateway_thread_ = pubsub_itc_fw::ApplicationThread::create<BinaryGatewayThread>(*logger_, *reactor_, config_);
    reactor_->register_thread(gateway_thread_);

    // Outbound connections are initiated from BinaryGatewayThread::on_app_ready_event()
    // via connect_to_service(); the registry is populated here so the names resolve.
    service_registry_.add("authentication_service_primary",
                          pubsub_itc_fw::NetworkEndpointConfiguration{config_.authentication_service_host, config_.authentication_service_port},
                          pubsub_itc_fw::NetworkEndpointConfiguration{});
    if (config_.ha_enabled) {
        service_registry_.add(
            "authentication_service_secondary",
            pubsub_itc_fw::NetworkEndpointConfiguration{config_.authentication_service_secondary_host, config_.authentication_service_secondary_port},
            pubsub_itc_fw::NetworkEndpointConfiguration{});
    }
    service_registry_.add("sequencer_primary", pubsub_itc_fw::NetworkEndpointConfiguration{config_.sequencer_primary_host, config_.sequencer_primary_port},
                          pubsub_itc_fw::NetworkEndpointConfiguration{});
    if (config_.ha_enabled) {
        service_registry_.add("sequencer_secondary",
                              pubsub_itc_fw::NetworkEndpointConfiguration{config_.sequencer_secondary_host, config_.sequencer_secondary_port},
                              pubsub_itc_fw::NetworkEndpointConfiguration{});
    }

    PUBSUB_LOG((*logger_), pubsub_itc_fw::FwLogLevel::Info, "BinaryGateway: client listener on {}:{}", config_.listen_host, config_.listen_port);
    PUBSUB_LOG((*logger_), pubsub_itc_fw::FwLogLevel::Info, "BinaryGateway: ER listener on {}:{}", config_.er_listen_host, config_.er_listen_port);
    PUBSUB_LOG((*logger_), pubsub_itc_fw::FwLogLevel::Info, "BinaryGateway: authentication service {}:{} (SCRAM-SHA-256)", config_.authentication_service_host,
               config_.authentication_service_port);
    if (config_.ha_enabled) {
        PUBSUB_LOG((*logger_), pubsub_itc_fw::FwLogLevel::Info, "BinaryGateway: sequencer primary={}:{} secondary={}:{} (HA enabled)",
                   config_.sequencer_primary_host, config_.sequencer_primary_port, config_.sequencer_secondary_host, config_.sequencer_secondary_port);
    } else {
        PUBSUB_LOG((*logger_), pubsub_itc_fw::FwLogLevel::Info, "BinaryGateway: sequencer primary={}:{} (HA disabled)", config_.sequencer_primary_host,
                   config_.sequencer_primary_port);
    }
}

int BinaryGateway::run() {
    PUBSUB_LOG_STR((*logger_), pubsub_itc_fw::FwLogLevel::Info, "BinaryGateway: starting reactor");
    return reactor_->run();
}

} // namespaces

int main(int argc, char* argv[]) {
    if (pubsub_itc_fw::answer_hot_path_thread_count_query(argc, argv, binary_gateway::BinaryGateway::hot_path_thread_count)) {
        return 0;
    }

    if (argc != 3) {
        std::cerr << "Usage: binary_gateway <logfile> <config.toml>\n";
        return 1;
    }

    const std::string log_file = argv[1];
    const std::string config_file = argv[2];

    const std::string writable_error = pubsub_itc_fw::QuillLogger::ensure_log_file_writable(log_file);
    if (!writable_error.empty()) {
        std::cerr << "BinaryGateway: " << writable_error << "\n";
        return 1;
    }

    pubsub_itc_fw::QuillLogger::block_signals_before_construction();

    binary_gateway::BinaryGatewayConfiguration config;
    std::unique_ptr<pubsub_itc_fw::QuillLogger> logger;

    try {
        auto [loaded_config, initialised_logger] = binary_gateway::BinaryGatewayConfigurationLoader::load_and_init_logging(config_file, log_file);
        config = std::move(loaded_config);
        logger = std::move(initialised_logger);
    } catch (const pubsub_itc_fw::ConfigurationException& ex) {
        std::cerr << fmt::format("BinaryGateway: configuration error: {}\n", ex.what());
        return 1;
    }

    logger->set_log_level(config.applog_level);
    logger->set_syslog_level(config.syslog_level);

    // Background by default: mask the whole process to the shared tier before it
    // creates any thread, so every thread inherits it and the Reactor only has to
    // promote the few that were allocated dedicated cores.
    if (config.cpu_pinning_enabled) {
        const auto [masked, mask_error] = pubsub_itc_fw::apply_background_affinity(config.cpu_layout_file, config.cpu_layout_component);
        if (!masked) {
            PUBSUB_LOG((*logger), pubsub_itc_fw::FwLogLevel::Error, "CPU pinning: {}", mask_error);
            return 1;
        }
    }
    pubsub_itc_fw::ApplicationAnnouncer::announce(*logger, "binary_gateway");

    try {
        binary_gateway::BinaryGateway app{config, std::move(logger)};
        return app.run();
    } catch (const std::exception& ex) {
        std::cerr << "BinaryGateway: fatal exception: " << ex.what() << "\n";
        return 1;
    } catch (...) {
        std::cerr << "BinaryGateway: unknown fatal exception\n";
        return 1;
    }
}
