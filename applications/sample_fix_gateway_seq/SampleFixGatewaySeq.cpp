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

const std::string SampleFixGatewaySeq::log_file_name = "sample_fix_gateway_seq.log";

SampleFixGatewaySeq::SampleFixGatewaySeq(const FixGatewaySeqConfiguration& config)
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

    // Inbound RawBytes listener for FIX client connections.
    reactor_->register_inbound_listener(
        pubsub_itc_fw::NetworkEndpointConfiguration{config_.listen_host, config_.listen_port},
        pubsub_itc_fw::ThreadID{1},
        pubsub_itc_fw::ProtocolType{pubsub_itc_fw::ProtocolType::RawBytes},
        config_.raw_buffer_capacity);

    // Inbound PDU listener for ExecutionReport PDUs from the matching engine.
    // TODO: replace with pub/sub fanout when implemented.
    reactor_->register_inbound_listener(
        pubsub_itc_fw::NetworkEndpointConfiguration{config_.er_listen_host, config_.er_listen_port},
        pubsub_itc_fw::ThreadID{1},
        pubsub_itc_fw::ProtocolType{pubsub_itc_fw::ProtocolType::FrameworkPdu},
        0);

    gateway_thread_ = pubsub_itc_fw::ApplicationThread::create<FixGatewaySeqThread>(
        *logger_, *reactor_, config_);

    reactor_->register_thread(gateway_thread_);

    // Outbound PDU connections to primary and secondary sequencer are initiated
    // from FixGatewaySeqThread::on_app_ready_event() via connect_to_service().
    // The ServiceRegistry is populated here so the reactor can resolve the names.
    service_registry_.add("sequencer_primary",
        pubsub_itc_fw::NetworkEndpointConfiguration{
            config_.sequencer_primary_host, config_.sequencer_primary_port},
        pubsub_itc_fw::NetworkEndpointConfiguration{});
    service_registry_.add("sequencer_secondary",
        pubsub_itc_fw::NetworkEndpointConfiguration{
            config_.sequencer_secondary_host, config_.sequencer_secondary_port},
        pubsub_itc_fw::NetworkEndpointConfiguration{});

    PUBSUB_LOG((*logger_), pubsub_itc_fw::FwLogLevel::Info,
               "SampleFixGatewaySeq: FIX listener on {}:{}",
               config_.listen_host, config_.listen_port);
    PUBSUB_LOG((*logger_), pubsub_itc_fw::FwLogLevel::Info,
               "SampleFixGatewaySeq: ER listener on {}:{}",
               config_.er_listen_host, config_.er_listen_port);
    PUBSUB_LOG((*logger_), pubsub_itc_fw::FwLogLevel::Info,
               "SampleFixGatewaySeq: sequencer primary={}:{} secondary={}:{}",
               config_.sequencer_primary_host, config_.sequencer_primary_port,
               config_.sequencer_secondary_host, config_.sequencer_secondary_port);
}

int SampleFixGatewaySeq::run()
{
    PUBSUB_LOG_STR((*logger_), pubsub_itc_fw::FwLogLevel::Info,
                   "SampleFixGatewaySeq: starting reactor");
    return reactor_->run();
}

} // namespace sample_fix_gateway_seq

// ============================================================
// main
// ============================================================

int main(int argc, char* argv[])
{
    try {
        sample_fix_gateway_seq::FixGatewaySeqConfiguration config;

        if (argc >= 2) {
            const std::string config_file = argv[1];
            std::cout << "SampleFixGatewaySeq: loading configuration from "
                      << config_file << "\n";
            config = sample_fix_gateway_seq::FixGatewaySeqConfigurationLoader::load(config_file);
        } else {
            std::cout << "SampleFixGatewaySeq: no configuration file supplied, "
                         "using built-in defaults\n";
        }

        sample_fix_gateway_seq::SampleFixGatewaySeq gateway{config};
        return gateway.run();

    } catch (const pubsub_itc_fw::ConfigurationException& ex) {
        std::cerr << "SampleFixGatewaySeq: configuration error: " << ex.what() << "\n";
        return 1;
    } catch (const std::exception& ex) {
        std::cerr << "SampleFixGatewaySeq: fatal exception: " << ex.what() << "\n";
        return 1;
    } catch (...) {
        std::cerr << "SampleFixGatewaySeq: unknown fatal exception\n";
        return 1;
    }
}
