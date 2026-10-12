// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <pubsub_itc_fw/Witness.hpp>

#include <chrono>
#include <memory>

#include <pubsub_itc_fw/ApplicationAnnouncer.hpp>
#include <pubsub_itc_fw/ConfigurationException.hpp>
#include <pubsub_itc_fw/CpuLayout.hpp>
#include <pubsub_itc_fw/FileOpenMode.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>
#include <pubsub_itc_fw/NetworkEndpointConfiguration.hpp>
#include <pubsub_itc_fw/ProtocolType.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>
#include <pubsub_itc_fw/WitnessConfiguration.hpp>
#include <pubsub_itc_fw/WitnessConfigurationLoader.hpp>

namespace pubsub_itc_fw {

Witness::Witness(const WitnessConfiguration& config, std::unique_ptr<pubsub_itc_fw::QuillLogger> logger) : config_(config), logger_(std::move(logger)) {
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
    reactor_configuration_.metrics_configuration = config_.metrics_configuration;
    reactor_configuration_.connect_retry_warning_interval_ = config_.connect_retry_warning_interval;
    reactor_configuration_.command_allocator_configuration_.pool_name = "WitnessCommandPool";
    reactor_configuration_.command_allocator_configuration_.objects_per_pool = config_.command_queue_pool_objects_per_slab;
    reactor_configuration_.command_allocator_configuration_.initial_pools = config_.command_queue_pool_initial_slabs;
    reactor_configuration_.command_allocator_configuration_.handler_for_pool_exhausted = [this](void* /*context*/, int objects_per_pool) {
        PUBSUB_LOG(*logger_, pubsub_itc_fw::FwLogLevel::Warning, "WitnessCommandPool exhausted: chaining new pool slab ({} objects)", objects_per_pool);
    };

    reactor_ = std::make_unique<pubsub_itc_fw::Reactor>(reactor_configuration_, service_registry_, *logger_);

    reactor_->register_inbound_listener(pubsub_itc_fw::NetworkEndpointConfiguration{config_.listen_host, config_.listen_port}, pubsub_itc_fw::ThreadID{1},
                                        pubsub_itc_fw::ProtocolType{pubsub_itc_fw::ProtocolType::FrameworkPdu}, 0);

    witness_thread_ = pubsub_itc_fw::ApplicationThread::create<WitnessThread>(*logger_, *reactor_, config_);

    reactor_->register_thread(witness_thread_);

    PUBSUB_LOG((*logger_), pubsub_itc_fw::FwLogLevel::Info, "Witness: listening for arbiter connections on {}:{}", config_.listen_host, config_.listen_port);
}

int Witness::run() {
    PUBSUB_LOG_STR((*logger_), pubsub_itc_fw::FwLogLevel::Info, "Witness: starting reactor");
    const int status = reactor_->run();
    // Does not return if shutdown had to abandon a thread that would not stop: returning
    // would run the exit handlers and destroy state that thread is still using, which is
    // how a sequencer segfaulted on 2026-08-21. See docs/bug_list.md, BUG-0057.
    reactor_->abort_if_thread_abandoned();
    return status;
}

} // namespaces
