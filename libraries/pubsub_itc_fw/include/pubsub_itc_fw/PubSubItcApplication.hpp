#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.

// C++ headers whose names start with ‘c’
// (None directly here)

// System C++ headers
#include <memory>
#include <string>
#include <vector>

// Third party headers
// (None directly here)

// Project headers
#include <pubsub_itc_fw/Reactor.hpp>
#include <pubsub_itc_fw/ReactorConfiguration.hpp>
#include <pubsub_itc_fw/LoggerInterface.hpp>
#include <pubsub_itc_fw/EventDistributor.hpp>
#include <pubsub_itc_fw/ApplicationThread.hpp>

namespace pubsub_itc_fw {

/**
 * @brief The top-level orchestrator for the PubSub ITC framework.
 *
 * This abstract class is the main entry point and controls the lifecycle of the
 * entire application. It is responsible for creating and wiring together all
 * core components. Subclasses must implement the `create_threads` method to
 * define their specific application threads.
 */
class PubSubItcApplication {
  public:
    /**
     * @brief Virtual destructor to ensure proper cleanup of derived classes.
     */
    virtual ~PubSubItcApplication() = default;

    /**
     * @brief Constructs the application orchestrator.
     * @param [in] config_file_path The path to the configuration file.
     * @param [in] logger A unique pointer to the concrete logger implementation.
     */
    PubSubItcApplication(const std::string& config_file_path, std::unique_ptr<LoggerInterface> logger);

    /**
     * @brief Runs the main application loop.
     */
    void run();

  private:
    /**
     * @brief Initializes and registers all application threads.
     *
     * This is a pure virtual function that must be implemented by concrete
     * subclasses to define the specific threads for the application.
     */
    virtual void create_threads() = 0;

  protected:
    std::unique_ptr<ReactorConfiguration> config_;
    std::unique_ptr<LoggerInterface> logger_;
    std::unique_ptr<Reactor> reactor_;
    std::unique_ptr<EventDistributor> distributor_;
    std::vector<std::unique_ptr<ApplicationThread>> threads_;
};

} // namespace pubsub_itc_fw
