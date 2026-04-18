#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <stdexcept>
#include <string>

namespace pubsub_itc_fw {

/**
 * @brief Exception thrown when a configuration error is detected.
 *
 * A configuration error is a deployment error -- something wrong in a
 * configuration file or in the way the application has populated a
 * TomlConfiguration. It is distinct from:
 *
 *   PreconditionAssertion -- a programming error (wrong API usage)
 *   PubSubItcException    -- a runtime framework error
 *
 * ConfigurationException carries a human-readable message describing
 * what went wrong, including the key name and the offending value where
 * relevant. It does not carry separate structured fields -- the message
 * string is intended to be logged or displayed directly to the operator.
 */
class ConfigurationException : public std::runtime_error {
public:
    explicit ConfigurationException(const std::string& message)
        : std::runtime_error(message)
    {}
};

} // namespace pubsub_itc_fw
