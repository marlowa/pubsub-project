#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <tuple>

#include <pubsub_itc_fw/ConfigurationException.hpp>

namespace pubsub_itc_fw {

/**
 * @brief A strongly-typed wrapper around a TOML configuration store.
 *
 * TomlConfiguration provides two ways to populate itself:
 *
 *   load_file(path)    -- parses a TOML file from disk
 *   load_string(toml)  -- parses TOML from a string (useful in unit tests)
 *   set(key, value)    -- sets individual values programmatically (useful in unit tests)
 *
 * Values are retrieved via two families of overloaded accessors:
 *
 *   get_required()        -- returns std::tuple<bool, std::string>. The bool is
 *                            true on success. On failure the bool is false and the
 *                            string contains a human-readable error message. The
 *                            out-parameter is not modified on failure.
 *                            All overloads are [[nodiscard]].
 *
 *   get_required_except() -- same semantics but throws ConfigurationException
 *                            on failure instead of returning an error tuple.
 *                            Intended for use inside a try block where a
 *                            succession of fetches is performed without
 *                            cluttering the code with tuple checks.
 *
 * Supported types:
 *   std::string, bool, int32_t, int64_t, double,
 *   std::chrono::nanoseconds, std::chrono::microseconds,
 *   std::chrono::milliseconds, std::chrono::seconds,
 *   std::chrono::minutes, std::chrono::hours
 *
 * Integer types:
 *   int32_t is appropriate for port numbers (valid range 1-65535), small
 *   counts, and enum-like values. int64_t is appropriate for large values
 *   such as buffer sizes and file sizes. No unsigned integer types are
 *   supported -- use int32_t or int64_t and validate the range in application
 *   code where the semantics are understood.
 *
 * Duration values in TOML files must be expressed as a quoted string with a
 * suffix indicating the unit:
 *
 *   "30s"    -- 30 seconds
 *   "500ms"  -- 500 milliseconds
 *   "1m"     -- 1 minute
 *   "2h"     -- 2 hours
 *   "100us"  -- 100 microseconds
 *   "500ns"  -- 500 nanoseconds
 *
 * Conversions between duration types are permitted when lossless. Converting
 * from a coarser to a finer unit (e.g. seconds to milliseconds) is always
 * lossless. Converting from a finer to a coarser unit (e.g. milliseconds to
 * seconds) is only permitted when the stored value is exactly divisible by
 * the conversion factor -- otherwise get_required() returns false and
 * get_required_except() throws ConfigurationException.
 *
 * Nested keys use dotted path notation matching TOML's own section syntax:
 *
 *   config.get_required("gateway.sender_comp_id", value)
 *
 * corresponds to:
 *
 *   [gateway]
 *   sender_comp_id = "GATEWAY"
 *
 * Key lookup is case-sensitive, consistent with the TOML specification.
 *
 * The toml++ library is an implementation detail and is not exposed in this
 * header. Application code never needs to include toml++/toml.hpp directly.
 */
class TomlConfiguration {
public:
    TomlConfiguration();
    ~TomlConfiguration();

    TomlConfiguration(const TomlConfiguration&) = delete;
    TomlConfiguration& operator=(const TomlConfiguration&) = delete;

    // ----------------------------------------------------------------
    // Population
    // ----------------------------------------------------------------

    /**
     * @brief Parses a TOML file from disk and replaces the current contents.
     *
     * On parse error the configuration is left unchanged and the error
     * string includes the filename and line number of the offending line.
     *
     * @param[in] path Path to the TOML file.
     * @return {true, ""} on success. {false, error_message} on failure.
     */
    [[nodiscard]] std::tuple<bool, std::string> load_file(std::string_view path);

    /**
     * @brief Parses TOML from a string and replaces the current contents.
     *
     * Useful in unit tests where a full file is not required. On parse
     * error the configuration is left unchanged.
     *
     * @param[in] toml_content The TOML-formatted string to parse.
     * @return {true, ""} on success. {false, error_message} on failure.
     */
    [[nodiscard]] std::tuple<bool, std::string> load_string(std::string_view toml_content);

    /**
     * @brief Sets a value at the given dotted key path.
     *
     * Creates intermediate tables as needed. Overwrites any existing
     * value at the given path. Useful in unit tests.
     *
     * Duration values are stored as a string with the appropriate suffix
     * (e.g. std::chrono::seconds{30} is stored as "30s").
     *
     * @param[in] key   Dotted key path (e.g. "gateway.sender_comp_id").
     * @param[in] value The value to set.
     */
    void set(std::string_view key, const std::string& value);
    void set(std::string_view key, bool value);
    void set(std::string_view key, int32_t value);
    void set(std::string_view key, int64_t value);
    void set(std::string_view key, double value);
    void set(std::string_view key, std::chrono::nanoseconds value);
    void set(std::string_view key, std::chrono::microseconds value);
    void set(std::string_view key, std::chrono::milliseconds value);
    void set(std::string_view key, std::chrono::seconds value);
    void set(std::string_view key, std::chrono::minutes value);
    void set(std::string_view key, std::chrono::hours value);

    // ----------------------------------------------------------------
    // Error-returning accessors
    // ----------------------------------------------------------------

    [[nodiscard]] std::tuple<bool, std::string> get_required(std::string_view key, std::string& value) const;
    [[nodiscard]] std::tuple<bool, std::string> get_required(std::string_view key, bool& value) const;
    [[nodiscard]] std::tuple<bool, std::string> get_required(std::string_view key, int32_t& value) const;
    [[nodiscard]] std::tuple<bool, std::string> get_required(std::string_view key, int64_t& value) const;
    [[nodiscard]] std::tuple<bool, std::string> get_required(std::string_view key, double& value) const;
    [[nodiscard]] std::tuple<bool, std::string> get_required(std::string_view key, std::chrono::nanoseconds& value) const;
    [[nodiscard]] std::tuple<bool, std::string> get_required(std::string_view key, std::chrono::microseconds& value) const;
    [[nodiscard]] std::tuple<bool, std::string> get_required(std::string_view key, std::chrono::milliseconds& value) const;
    [[nodiscard]] std::tuple<bool, std::string> get_required(std::string_view key, std::chrono::seconds& value) const;
    [[nodiscard]] std::tuple<bool, std::string> get_required(std::string_view key, std::chrono::minutes& value) const;
    [[nodiscard]] std::tuple<bool, std::string> get_required(std::string_view key, std::chrono::hours& value) const;

    // ----------------------------------------------------------------
    // Exception-throwing accessors
    // ----------------------------------------------------------------

    void get_required_except(std::string_view key, std::string& value) const;
    void get_required_except(std::string_view key, bool& value) const;
    void get_required_except(std::string_view key, int32_t& value) const;
    void get_required_except(std::string_view key, int64_t& value) const;
    void get_required_except(std::string_view key, double& value) const;
    void get_required_except(std::string_view key, std::chrono::nanoseconds& value) const;
    void get_required_except(std::string_view key, std::chrono::microseconds& value) const;
    void get_required_except(std::string_view key, std::chrono::milliseconds& value) const;
    void get_required_except(std::string_view key, std::chrono::seconds& value) const;
    void get_required_except(std::string_view key, std::chrono::minutes& value) const;
    void get_required_except(std::string_view key, std::chrono::hours& value) const;

private:
    // Pimpl idiom: keeps toml++/toml.hpp entirely out of this header.
    // Application code that includes TomlConfiguration.hpp never sees toml++.
    struct Impl;
    Impl* impl_;
};

} // namespace pubsub_itc_fw
