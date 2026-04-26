// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "Arbiter.hpp"
#include "ArbiterConfigurationLoader.hpp"

#include <chrono>
#include <iostream>
#include <memory>

#include <pubsub_itc_fw/ConfigurationException.hpp>
#include <pubsub_itc_fw/FileOpenMode.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>
#include <pubsub_itc_fw/NetworkEndpointConfiguration.hpp>
#include <pubsub_itc_fw/ProtocolType.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>

namespace arbiter {

Arbiter::Arbiter(const ArbiterConfiguration& config,
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

    reactor_->register_inbound_listener(
        pubsub_itc_fw::NetworkEndpointConfiguration{config_.listen_host, config_.listen_port},
        pubsub_itc_fw::ThreadID{1},
        pubsub_itc_fw::ProtocolType{pubsub_itc_fw::ProtocolType::FrameworkPdu},
        0);

    arbiter_thread_ = pubsub_itc_fw::ApplicationThread::create<ArbiterThread>(
        *logger_, *reactor_, config_);

    reactor_->register_thread(arbiter_thread_);

    PUBSUB_LOG((*logger_), pubsub_itc_fw::FwLogLevel::Info,
               "Arbiter: listening for sequencer connections on {}:{}",
               config_.listen_host, config_.listen_port);
}

int Arbiter::run()
{
    PUBSUB_LOG_STR((*logger_), pubsub_itc_fw::FwLogLevel::Info,
                   "Arbiter: starting reactor");
    return reactor_->run();
}

} // namespace arbiter

// ============================================================
// main
// ============================================================

int main(int argc, char* argv[])
{
    if (argc != 3) {
        std::cerr << "Usage: arbiter <logfile> <config.toml>\n";
        return 1;
    }

    const std::string log_file    = argv[1];
    const std::string config_file = argv[2];

    const std::string writable_error =
        pubsub_itc_fw::QuillLogger::ensure_log_file_writable(log_file);
    if (!writable_error.empty()) {
        std::cerr << "Arbiter: " << writable_error << "\n";
        return 1;
    }

    pubsub_itc_fw::QuillLogger::block_signals_before_construction();

    auto logger = std::make_unique<pubsub_itc_fw::QuillLogger>(
        log_file,
        pubsub_itc_fw::FileOpenMode{pubsub_itc_fw::FileOpenMode::Truncate},
        pubsub_itc_fw::FwLogLevel::Info,
        pubsub_itc_fw::FwLogLevel::Info);

    arbiter::ArbiterConfiguration config;
    try {
        config = arbiter::ArbiterConfigurationLoader::load(config_file);
    } catch (const pubsub_itc_fw::ConfigurationException& ex) {
        PUBSUB_LOG((*logger), pubsub_itc_fw::FwLogLevel::Error,
                   "Arbiter: configuration error: {}", ex.what());
        return 1;
    }

    logger->set_log_level(config.applog_level);
    logger->set_syslog_level(config.syslog_level);

    try {
        arbiter::Arbiter app{config, std::move(logger)};
        return app.run();
    } catch (const std::exception& ex) {
        std::cerr << "Arbiter: fatal exception: " << ex.what() << "\n";
        return 1;
    } catch (...) {
        std::cerr << "Arbiter: unknown fatal exception\n";
        return 1;
    }
}
