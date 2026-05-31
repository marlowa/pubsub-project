// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "SampleFixGatewaySeq.hpp"
#include "FixGatewaySeqConfigurationLoader.hpp"

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

namespace sample_fix_gateway_seq {

SampleFixGatewaySeq::SampleFixGatewaySeq(const FixGatewaySeqConfiguration& config, std::unique_ptr<pubsub_itc_fw::QuillLogger> logger)
    : config_(config), logger_(std::move(logger)) {
    reactor_configuration_.connect_timeout = std::chrono::seconds{5};
    reactor_configuration_.socket_maximum_inactivity_interval_ = std::chrono::seconds{600};
    reactor_configuration_.inactivity_check_interval_ = std::chrono::milliseconds{500};
    reactor_configuration_.shutdown_timeout_ = std::chrono::seconds{2};
    reactor_configuration_.cpu_pinning_enabled = config_.cpu_pinning_enabled;
    reactor_configuration_.cpu_pinning_dev_mode = config_.cpu_pinning_dev_mode;
    reactor_configuration_.cpu_registry_lock_file = config_.cpu_registry_lock_file;
    reactor_configuration_.command_allocator_configuration_.pool_name = "FixGatewaySeqCommandPool";
    reactor_configuration_.command_allocator_configuration_.objects_per_pool = config_.command_queue_pool_objects_per_slab;
    reactor_configuration_.command_allocator_configuration_.initial_pools = config_.command_queue_pool_initial_slabs;
    reactor_configuration_.command_allocator_configuration_.handler_for_pool_exhausted = [this](void* /*ctx*/, int objects_per_pool) {
        PUBSUB_LOG(*logger_, pubsub_itc_fw::FwLogLevel::Warning, "FixGatewaySeqCommandPool exhausted: chaining new pool slab ({} objects)", objects_per_pool);
    };

    reactor_ = std::make_unique<pubsub_itc_fw::Reactor>(reactor_configuration_, service_registry_, *logger_);

    // Inbound RawBytes listener for FIX client connections.
    reactor_->register_inbound_listener(pubsub_itc_fw::NetworkEndpointConfiguration{config_.listen_host, config_.listen_port}, pubsub_itc_fw::ThreadID{1},
                                        pubsub_itc_fw::ProtocolType{pubsub_itc_fw::ProtocolType::RawBytes}, config_.raw_buffer_capacity);

    // Inbound PDU listener for ExecutionReport PDUs from the matching engine.
    // TODO: replace with pub/sub fanout when implemented.
    reactor_->register_inbound_listener(pubsub_itc_fw::NetworkEndpointConfiguration{config_.er_listen_host, config_.er_listen_port}, pubsub_itc_fw::ThreadID{1},
                                        pubsub_itc_fw::ProtocolType{pubsub_itc_fw::ProtocolType::FrameworkPdu}, 0);

    gateway_thread_ = pubsub_itc_fw::ApplicationThread::create<FixGatewaySeqThread>(*logger_, *reactor_, config_);

    reactor_->register_thread(gateway_thread_);

    // Outbound PDU connections are initiated from FixGatewaySeqThread::on_app_ready_event()
    // via connect_to_service(). The ServiceRegistry is populated here so the reactor can
    // resolve the names.
    service_registry_.add("authentication_service_primary",
                          pubsub_itc_fw::NetworkEndpointConfiguration{config_.authentication_service_host, config_.authentication_service_port},
                          pubsub_itc_fw::NetworkEndpointConfiguration{});
    if (config_.ha_enabled) {
        service_registry_.add("authentication_service_secondary",
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

    PUBSUB_LOG((*logger_), pubsub_itc_fw::FwLogLevel::Info, "SampleFixGatewaySeq: FIX listener on {}:{}", config_.listen_host, config_.listen_port);
    PUBSUB_LOG((*logger_), pubsub_itc_fw::FwLogLevel::Info, "SampleFixGatewaySeq: ER listener on {}:{}", config_.er_listen_host, config_.er_listen_port);
    if (config_.ha_enabled) {
        PUBSUB_LOG((*logger_), pubsub_itc_fw::FwLogLevel::Info, "SampleFixGatewaySeq: authentication service primary={}:{} secondary={}:{} (HA enabled)",
                   config_.authentication_service_host, config_.authentication_service_port,
                   config_.authentication_service_secondary_host, config_.authentication_service_secondary_port);
    } else {
        PUBSUB_LOG((*logger_), pubsub_itc_fw::FwLogLevel::Info, "SampleFixGatewaySeq: authentication service primary={}:{}", config_.authentication_service_host, config_.authentication_service_port);
    }
    if (config_.ha_enabled) {
        PUBSUB_LOG((*logger_), pubsub_itc_fw::FwLogLevel::Info, "SampleFixGatewaySeq: sequencer primary={}:{} secondary={}:{} (HA enabled)",
                   config_.sequencer_primary_host, config_.sequencer_primary_port, config_.sequencer_secondary_host, config_.sequencer_secondary_port);
    } else {
        PUBSUB_LOG((*logger_), pubsub_itc_fw::FwLogLevel::Info, "SampleFixGatewaySeq: sequencer primary={}:{} (HA disabled)", config_.sequencer_primary_host,
                   config_.sequencer_primary_port);
    }
}

int SampleFixGatewaySeq::run() {
    PUBSUB_LOG_STR((*logger_), pubsub_itc_fw::FwLogLevel::Info, "SampleFixGatewaySeq: starting reactor");
    return reactor_->run();
}

} // namespace sample_fix_gateway_seq

// ============================================================
// main
// ============================================================

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "Usage: sample_fix_gateway_seq <logfile> <config.toml>\n";
        return 1;
    }

    const std::string log_file = argv[1];
    const std::string config_file = argv[2];

    const std::string writable_error = pubsub_itc_fw::QuillLogger::ensure_log_file_writable(log_file);
    if (!writable_error.empty()) {
        std::cerr << "SampleFixGatewaySeq: " << writable_error << "\n";
        return 1;
    }

    pubsub_itc_fw::QuillLogger::block_signals_before_construction();

    sample_fix_gateway_seq::FixGatewaySeqConfiguration config;
    std::unique_ptr<pubsub_itc_fw::QuillLogger> logger;

    try {
        auto [loaded_config, initialized_logger] = sample_fix_gateway_seq::FixGatewaySeqConfigurationLoader::load_and_init_logging(config_file, log_file);
        config = std::move(loaded_config);
        logger = std::move(initialized_logger);
    } catch (const pubsub_itc_fw::ConfigurationException& ex) {
        std::cerr << fmt::format("SampleFixGatewaySeq: configuration error: {}\n", ex.what());
        return 1;
    }

    logger->set_log_level(config.applog_level);
    logger->set_syslog_level(config.syslog_level);

    try {
        sample_fix_gateway_seq::SampleFixGatewaySeq app{config, std::move(logger)};
        return app.run();
    } catch (const std::exception& ex) {
        std::cerr << "SampleFixGatewaySeq: fatal exception: " << ex.what() << "\n";
        return 1;
    } catch (...) {
        std::cerr << "SampleFixGatewaySeq: unknown fatal exception\n";
        return 1;
    }
}
