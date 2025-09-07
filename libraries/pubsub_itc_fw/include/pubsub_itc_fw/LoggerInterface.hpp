#pragma once

// C++ headers whose names start with ‘c’
// (None directly here)

// System C++ headers
#include <string>

// Third party headers
// (None directly here)

// Project headers
// (None directly here)

namespace pubsub_itc_fw {

/**
 * @brief An abstract interface for logging.
 *
 * This class defines the public API for the framework's logging subsystem.
 * It is designed to be a lightweight, dependency-free interface that
 * allows for a concrete logging implementation to be injected at runtime.
 */
class LoggerInterface {
  public:
    /**
     * @brief Virtual destructor to allow for proper cleanup of derived classes.
     */
    virtual ~LoggerInterface() = default;

    /**
     * @brief Logs an informational message.
     * @param [in] message The string message to log.
     */
    virtual void info(const std::string& message) const = 0;

    /**
     * @brief Logs a warning message.
     * @param [in] message The string message to log.
     */
    virtual void warning(const std::string& message) const = 0;

    /**
     * @brief Logs an error message.
     * @param [in] message The string message to log.
     */
    virtual void error(const std::string& message) const = 0;

    /**
     * @brief Logs a critical message and optionally initiates a shutdown.
     * @param [in] message The string message to log.
     */
    virtual void critical(const std::string& message) const = 0;
};

} // namespace pubsub_itc_fw
