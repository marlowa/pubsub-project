// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "MatchingEngine.hpp"
#include "MatchingEngineConfiguration.hpp"
#include "MatchingEngineConfigurationLoader.hpp"

#include <chrono>
#include <iostream>

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

namespace matching_engine {

MatchingEngine::MatchingEngine(MatchingEngineConfiguration config, std::unique_ptr<pubsub_itc_fw::QuillLogger> logger)
    : config_(std::move(config)), logger_(std::move(logger)) {
    reactor_configuration_.connect_timeout = std::chrono::seconds{5};
    reactor_configuration_.socket_maximum_inactivity_interval_ = std::chrono::seconds{600};
    reactor_configuration_.inactivity_check_interval_ = std::chrono::milliseconds{500};
    reactor_configuration_.shutdown_timeout_ = std::chrono::seconds{2};
    reactor_configuration_.cpu_pinning_enabled = config_.cpu_pinning_enabled;
    reactor_configuration_.cpu_pinning_reserve_cpu0 = config_.cpu_pinning_reserve_cpu0;
    reactor_configuration_.cpu_registry_lock_file = config_.cpu_registry_lock_file;
    reactor_configuration_.connect_retry_warning_interval_ = config_.connect_retry_warning_interval;
    reactor_configuration_.command_allocator_configuration_.pool_name = "MatchingEngineCommandPool";
    reactor_configuration_.command_allocator_configuration_.objects_per_pool = config_.command_queue_pool_objects_per_slab;
    reactor_configuration_.command_allocator_configuration_.initial_pools = config_.command_queue_pool_initial_slabs;
    reactor_configuration_.command_allocator_configuration_.handler_for_pool_exhausted = [this](void* /*ctx*/, int objects_per_pool) {
        PUBSUB_LOG(*logger_, pubsub_itc_fw::FwLogLevel::Warning, "MatchingEngineCommandPool exhausted: chaining new pool slab ({} objects)", objects_per_pool);
    };

    reactor_ = std::make_unique<pubsub_itc_fw::Reactor>(reactor_configuration_, service_registry_, *logger_);

    // Inbound PDU listener for SequencedMessage PDUs from the sequencer.
    reactor_->register_inbound_listener(pubsub_itc_fw::NetworkEndpointConfiguration{config_.listen_host, config_.listen_port}, pubsub_itc_fw::ThreadID{1},
                                        pubsub_itc_fw::ProtocolType{pubsub_itc_fw::ProtocolType::FrameworkPdu}, 0);

    matching_engine_thread_ = pubsub_itc_fw::ApplicationThread::create<MatchingEngineThread>(*logger_, *reactor_, config_);

    reactor_->register_thread(matching_engine_thread_);

    // Outbound PDU connections to both sequencer ER inbound listeners.
    // ERs are sent to both; the leader routes them to the gateway, the follower discards.
    service_registry_.add("sequencer_er", pubsub_itc_fw::NetworkEndpointConfiguration{config_.sequencer_er_host, config_.sequencer_er_port},
                          pubsub_itc_fw::NetworkEndpointConfiguration{});
    service_registry_.add("sequencer_er_secondary",
                          pubsub_itc_fw::NetworkEndpointConfiguration{config_.sequencer_er_secondary_host, config_.sequencer_er_secondary_port},
                          pubsub_itc_fw::NetworkEndpointConfiguration{});

    PUBSUB_LOG((*logger_), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngine: listening for sequenced PDUs on {}:{}", config_.listen_host, config_.listen_port);
    PUBSUB_LOG((*logger_), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngine: primary ER connection to sequencer at {}:{}", config_.sequencer_er_host,
               config_.sequencer_er_port);
    PUBSUB_LOG((*logger_), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngine: secondary ER connection to sequencer at {}:{}",
               config_.sequencer_er_secondary_host, config_.sequencer_er_secondary_port);
}

int MatchingEngine::run() {
    PUBSUB_LOG_STR((*logger_), pubsub_itc_fw::FwLogLevel::Info, "MatchingEngine: starting reactor");
    return reactor_->run();
}

} // namespaces

// ============================================================
// main
// ============================================================

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "Usage: matching_engine <logfile> <config.toml>\n";
        return 1;
    }

    const std::string log_file = argv[1];
    const std::string config_file = argv[2];

    const std::string writable_error = pubsub_itc_fw::QuillLogger::ensure_log_file_writable(log_file);
    if (!writable_error.empty()) {
        std::cerr << "MatchingEngine: " << writable_error << "\n";
        return 1;
    }

    pubsub_itc_fw::QuillLogger::block_signals_before_construction();

    auto logger = std::make_unique<pubsub_itc_fw::QuillLogger>(log_file, pubsub_itc_fw::FileOpenMode{pubsub_itc_fw::FileOpenMode::Truncate},
                                                               pubsub_itc_fw::FwLogLevel::Info, pubsub_itc_fw::FwLogLevel::Info);
    pubsub_itc_fw::ApplicationAnnouncer::announce(*logger, "matching_engine");

    matching_engine::MatchingEngineConfiguration config;
    try {
        config = matching_engine::MatchingEngineConfigurationLoader::load(config_file);
    } catch (const pubsub_itc_fw::ConfigurationException& ex) {
        PUBSUB_LOG((*logger), pubsub_itc_fw::FwLogLevel::Error, "MatchingEngine: configuration error: {}", ex.what());
        return 1;
    }

    logger->set_log_level(config.applog_level);
    logger->set_syslog_level(config.syslog_level);

    try {
        matching_engine::MatchingEngine app{config, std::move(logger)};
        return app.run();
    } catch (const std::exception& ex) {
        std::cerr << "MatchingEngine: fatal exception: " << ex.what() << "\n";
        return 1;
    } catch (...) {
        std::cerr << "MatchingEngine: unknown fatal exception\n";
        return 1;
    }
}
