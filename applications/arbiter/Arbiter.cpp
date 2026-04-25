// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "Arbiter.hpp"
#include "ArbiterConfigurationLoader.hpp"

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

namespace arbiter {

const std::string Arbiter::log_file_name = "arbiter.log";

Arbiter::Arbiter(const ArbiterConfiguration& config)
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

    // Inbound PDU listener for ArbitrationReport PDUs from sequencer instances.
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
    try {
        arbiter::ArbiterConfiguration config;

        if (argc >= 2) {
            const std::string config_file = argv[1];
            std::cout << "Arbiter: loading configuration from " << config_file << "\n";
            config = arbiter::ArbiterConfigurationLoader::load(config_file);
        } else {
            std::cout << "Arbiter: no configuration file supplied, using built-in defaults\n";
        }

        arbiter::Arbiter app{config};
        return app.run();

    } catch (const pubsub_itc_fw::ConfigurationException& ex) {
        std::cerr << "Arbiter: configuration error: " << ex.what() << "\n";
        return 1;
    } catch (const std::exception& ex) {
        std::cerr << "Arbiter: fatal exception: " << ex.what() << "\n";
        return 1;
    } catch (...) {
        std::cerr << "Arbiter: unknown fatal exception\n";
        return 1;
    }
}
