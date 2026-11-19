// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "AuthenticationService.hpp"
#include "AuthenticationServiceConfigurationLoader.hpp"

#include <chrono>
#include <iostream>
#include <optional>

#include <pubsub_itc_fw/ApplicationAnnouncer.hpp>
#include <pubsub_itc_fw/ComponentArguments.hpp>
#include <pubsub_itc_fw/ConfigurationException.hpp>
#include <pubsub_itc_fw/CpuLayout.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>
#include <pubsub_itc_fw/NetworkEndpointConfiguration.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/Reactor.hpp>
#include <pubsub_itc_fw/TechnicalEventsSubscription.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>
#include <pubsub_itc_fw/TlsListenerConfiguration.hpp>

namespace authentication_service {

AuthenticationService::AuthenticationService(const AuthenticationServiceConfiguration& config, std::unique_ptr<pubsub_itc_fw::QuillLogger> logger)
    : config_(config), logger_(std::move(logger)) {
    reactor_configuration_.connect_timeout = std::chrono::seconds{5};
    reactor_configuration_.socket_maximum_inactivity_interval_ = config_.connection_inactivity_limit;
    reactor_configuration_.inactivity_check_interval_ = std::chrono::milliseconds{500};
    reactor_configuration_.shutdown_timeout_ = std::chrono::seconds{2};
    reactor_configuration_.cpu_pinning_enabled = config_.cpu_pinning_enabled;
    reactor_configuration_.cpu_pinning_reserve_cpu0 = config_.cpu_pinning_reserve_cpu0;
    reactor_configuration_.cpu_registry_shm_path = config_.cpu_registry_shm_path;
    reactor_configuration_.cpu_registry_lock_file = config_.cpu_registry_lock_file;
    reactor_configuration_.cpu_layout_file = config_.cpu_layout_file;
    reactor_configuration_.cpu_layout_component = config_.cpu_layout_component;
    reactor_configuration_.metrics_configuration = config_.metrics_configuration;
    reactor_configuration_.connect_retry_warning_interval_ = config_.connect_retry_warning_interval;
    reactor_configuration_.command_allocator_configuration_.pool_name = "AuthenticationServiceCommandPool";
    reactor_configuration_.command_allocator_configuration_.objects_per_pool = config_.command_queue_pool_objects_per_slab;
    reactor_configuration_.command_allocator_configuration_.initial_pools = config_.command_queue_pool_initial_slabs;
    reactor_configuration_.command_allocator_configuration_.handler_for_pool_exhausted = [this](void* /*ctx*/, int objects_per_pool) {
        PUBSUB_LOG(*logger_, pubsub_itc_fw::FwLogLevel::Warning, "AuthenticationServiceCommandPool exhausted: chaining new pool slab ({} objects)",
                   objects_per_pool);
    };

    // The matching engine publishers, for the technical_events topic (docs/venue/start_and_end_of_day.md, section 4.4).
    static_cast<void>(config_.technical_events.register_publishers(service_registry_));
    reactor_ = std::make_unique<pubsub_itc_fw::Reactor>(reactor_configuration_, service_registry_, *logger_);

    reactor_->register_inbound_listener(pubsub_itc_fw::NetworkEndpointConfiguration{config_.listen_host, config_.listen_port}, pubsub_itc_fw::ThreadID{1});

    reactor_->register_inbound_tls_listener(pubsub_itc_fw::NetworkEndpointConfiguration{config_.listen_host, config_.admin_listen_port},
                                            pubsub_itc_fw::ThreadID{1}, 65536,
                                            pubsub_itc_fw::TlsListenerConfiguration{config_.admin_tls_certificate_path, config_.admin_tls_private_key_path,
                                                                                    config_.admin_tls_ca_path, config_.admin_tls_require_client_certificate});

    authentication_thread_ = pubsub_itc_fw::ApplicationThread::create<AuthenticationThread>(*logger_, *reactor_, config_);

    reactor_->register_thread(authentication_thread_);

    PUBSUB_LOG((*logger_), pubsub_itc_fw::FwLogLevel::Info, "AuthenticationService: PDU listener on {}:{}", config_.listen_host, config_.listen_port);
    PUBSUB_LOG((*logger_), pubsub_itc_fw::FwLogLevel::Info, "AuthenticationService: TLS admin listener on {}:{}", config_.listen_host,
               config_.admin_listen_port);
}

int AuthenticationService::run() {
    PUBSUB_LOG_STR((*logger_), pubsub_itc_fw::FwLogLevel::Info, "AuthenticationService: starting reactor");
    const int status = reactor_->run();
    // Does not return if shutdown had to abandon a thread that would not stop: returning
    // would run the exit handlers and destroy state that thread is still using, which is
    // how a sequencer segfaulted on 2026-08-21. See docs/bug_list.md, BUG-0057.
    reactor_->abort_if_thread_abandoned();
    return status;
}

} // namespaces

int main(int argc, char* argv[]) {
    const std::optional<pubsub_itc_fw::ComponentArguments> arguments =
        pubsub_itc_fw::parse_component_arguments(argc, argv, "authentication_service", std::cerr);
    if (!arguments.has_value()) {
        return 1;
    }
    const std::string& log_file = arguments->log_file;
    const std::string& config_file = arguments->config_file;

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

    PUBSUB_LOG((*logger), pubsub_itc_fw::FwLogLevel::Info, "AuthenticationService: started for trading day {}", arguments->trading_day);
    config.trading_day = arguments->trading_day;
    // A authentication service started again after the end of its day has nothing to do: it exits at once for the end of day
    // (docs/venue/start_and_end_of_day.md, section 4.4).
    if (pubsub_itc_fw::TechnicalEventsSubscription::already_ended(config.last_state_file, config.trading_day)) {
        PUBSUB_LOG((*logger), pubsub_itc_fw::FwLogLevel::Info,
                   "AuthenticationService: {} says the end of trading day {} has been reached -- nothing to do, exiting for the end of day",
                   config.last_state_file, config.trading_day);
        return pubsub_itc_fw::Reactor::stopped_for_the_end_of_day_status;
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
    pubsub_itc_fw::ApplicationAnnouncer::announce(*logger, "authentication_service");

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
