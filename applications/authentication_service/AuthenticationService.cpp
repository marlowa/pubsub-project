// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "AuthenticationService.hpp"
#include "AuthenticationServiceConfigurationLoader.hpp"

#include <chrono>
#include <iostream>

#include <pubsub_itc_fw/ConfigurationException.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>
#include <pubsub_itc_fw/NetworkEndpointConfiguration.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>
#include <pubsub_itc_fw/TlsListenerConfiguration.hpp>

namespace authentication_service {

AuthenticationService::AuthenticationService(const AuthenticationServiceConfiguration& config,
                                             std::unique_ptr<pubsub_itc_fw::QuillLogger> logger)
    : config_(config), logger_(std::move(logger)) {
    reactor_configuration_.connect_timeout = std::chrono::seconds{5};
    reactor_configuration_.socket_maximum_inactivity_interval_ = std::chrono::seconds{600};
    reactor_configuration_.inactivity_check_interval_ = std::chrono::milliseconds{500};
    reactor_configuration_.shutdown_timeout_ = std::chrono::seconds{2};
    reactor_configuration_.cpu_pinning_enabled = config_.cpu_pinning_enabled;
    reactor_configuration_.cpu_pinning_dev_mode = config_.cpu_pinning_dev_mode;
    reactor_configuration_.cpu_registry_lock_file = config_.cpu_registry_lock_file;
    reactor_configuration_.command_allocator_configuration_.pool_name = "AuthenticationServiceCommandPool";
    reactor_configuration_.command_allocator_configuration_.objects_per_pool = config_.command_queue_pool_objects_per_slab;
    reactor_configuration_.command_allocator_configuration_.initial_pools = config_.command_queue_pool_initial_slabs;
    reactor_configuration_.command_allocator_configuration_.handler_for_pool_exhausted = [this](void* /*ctx*/, int objects_per_pool) {
        PUBSUB_LOG(*logger_, pubsub_itc_fw::FwLogLevel::Warning, "AuthenticationServiceCommandPool exhausted: chaining new pool slab ({} objects)", objects_per_pool);
    };

    reactor_ = std::make_unique<pubsub_itc_fw::Reactor>(reactor_configuration_, service_registry_, *logger_);

    reactor_->register_inbound_listener(
        pubsub_itc_fw::NetworkEndpointConfiguration{config_.listen_host, config_.listen_port},
        pubsub_itc_fw::ThreadID{1});

    reactor_->register_inbound_tls_listener(
        pubsub_itc_fw::NetworkEndpointConfiguration{config_.listen_host, config_.admin_listen_port},
        pubsub_itc_fw::ThreadID{1},
        65536,
        pubsub_itc_fw::TlsListenerConfiguration{
            config_.admin_tls_certificate_path,
            config_.admin_tls_private_key_path,
            config_.admin_tls_ca_path,
            config_.admin_tls_require_client_certificate
        });

    authentication_thread_ = pubsub_itc_fw::ApplicationThread::create<AuthenticationThread>(*logger_, *reactor_, config_);

    reactor_->register_thread(authentication_thread_);

    PUBSUB_LOG((*logger_), pubsub_itc_fw::FwLogLevel::Info,
               "AuthenticationService: PDU listener on {}:{}", config_.listen_host, config_.listen_port);
    PUBSUB_LOG((*logger_), pubsub_itc_fw::FwLogLevel::Info,
               "AuthenticationService: TLS admin listener on {}:{}", config_.listen_host, config_.admin_listen_port);
}

int AuthenticationService::run() {
    PUBSUB_LOG_STR((*logger_), pubsub_itc_fw::FwLogLevel::Info, "AuthenticationService: starting reactor");
    return reactor_->run();
}

} // namespace authentication_service

// ============================================================
// main
// ============================================================

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "Usage: authentication_service <logfile> <config.toml>\n";
        return 1;
    }

    const std::string log_file = argv[1];
    const std::string config_file = argv[2];

    const std::string writable_error = pubsub_itc_fw::QuillLogger::ensure_log_file_writable(log_file);
    if (!writable_error.empty()) {
        std::cerr << "AuthenticationService: " << writable_error << "\n";
        return 1;
    }

    pubsub_itc_fw::QuillLogger::block_signals_before_construction();

    authentication_service::AuthenticationServiceConfiguration config;
    std::unique_ptr<pubsub_itc_fw::QuillLogger> logger;

    try {
        auto [loaded_config, initialized_logger] =
            authentication_service::AuthenticationServiceConfigurationLoader::load_and_init_logging(config_file, log_file);
        config = std::move(loaded_config);
        logger = std::move(initialized_logger);
    } catch (const pubsub_itc_fw::ConfigurationException& ex) {
        std::cerr << "AuthenticationService: configuration error: " << ex.what() << "\n";
        return 1;
    }

    logger->set_log_level(config.applog_level);
    logger->set_syslog_level(config.syslog_level);

    try {
        authentication_service::AuthenticationService service{config, std::move(logger)};
        return service.run();
    } catch (const std::exception& ex) {
        std::cerr << "AuthenticationService: fatal exception: " << ex.what() << "\n";
        return 1;
    } catch (...) {
        std::cerr << "AuthenticationService: unknown fatal exception\n";
        return 1;
    }
}
