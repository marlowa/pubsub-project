// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

// The witness program: reads its configuration and runs the library's Witness, which is the voting
// service described in docs/venue/witness.md.

#include <iostream>
#include <memory>
#include <string>
#include <utility>

#include <pubsub_itc_fw/ApplicationAnnouncer.hpp>
#include <pubsub_itc_fw/ConfigurationException.hpp>
#include <pubsub_itc_fw/CpuLayout.hpp>
#include <pubsub_itc_fw/FileOpenMode.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/Witness.hpp>
#include <pubsub_itc_fw/WitnessConfiguration.hpp>
#include <pubsub_itc_fw/WitnessConfigurationLoader.hpp>

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "Usage: witness <logfile> <config.toml>\n";
        return 1;
    }

    const std::string log_file = argv[1];
    const std::string config_file = argv[2];

    const std::string writable_error = pubsub_itc_fw::QuillLogger::ensure_log_file_writable(log_file);
    if (!writable_error.empty()) {
        std::cerr << "Witness: " << writable_error << "\n";
        return 1;
    }

    pubsub_itc_fw::QuillLogger::block_signals_before_construction();

    auto logger = std::make_unique<pubsub_itc_fw::QuillLogger>(log_file, pubsub_itc_fw::FileOpenMode{pubsub_itc_fw::FileOpenMode::Truncate},
                                                               pubsub_itc_fw::FwLogLevel::Info, pubsub_itc_fw::FwLogLevel::Info);
    pubsub_itc_fw::ApplicationAnnouncer::announce(*logger, "witness");

    pubsub_itc_fw::WitnessConfiguration config;
    try {
        config = pubsub_itc_fw::WitnessConfigurationLoader::load(config_file);
    } catch (const pubsub_itc_fw::ConfigurationException& ex) {
        PUBSUB_LOG((*logger), pubsub_itc_fw::FwLogLevel::Error, "Witness: configuration error: {}", ex.what());
        return 1;
    }

    // Refuse to run in a venue that has disowned high availability. Not launching this component
    // is what devenv.py already does; refusing is what protects the case it does not cover -- a
    // hand-started process, or a supervisor with a stale manifest. With high availability off there is no arbiter pair, so there is no tie to break.
    //
    // An operator who sees witness running will believe the venue has the mechanism it
    // names. Being present and idle is the failure worth preventing here, not being absent.
    // See docs/bug_list.md, BUG-0061.
    if (!config.ha_enabled) {
        // TEST CONTRACT -- ha_test.py matches this text (scenarios 44 and 45), because what it checks is that an operator is told why.
        PUBSUB_LOG_STR((*logger), pubsub_itc_fw::FwLogLevel::Error,
                       "Witness: refusing to start -- this venue has [ha] enabled = false in its environment, so there is nothing to "
                       "witness. Set [ha] enabled = true and redeploy, or do not start this component.");
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

    try {
        pubsub_itc_fw::Witness app{config, std::move(logger)};
        return app.run();
    } catch (const std::exception& ex) {
        std::cerr << "Witness: fatal exception: " << ex.what() << "\n";
        return 1;
    } catch (...) {
        std::cerr << "Witness: unknown fatal exception\n";
        return 1;
    }
}
