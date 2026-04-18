// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <pubsub_itc_fw/TomlConfiguration.hpp>

#include <charconv>
#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include <toml++/toml.hpp>

#include <fmt/format.h>

namespace pubsub_itc_fw {

// ============================================================
// Duration suffix constants
// ============================================================

static constexpr std::string_view suffix_ns  = "ns";
static constexpr std::string_view suffix_us  = "us";
static constexpr std::string_view suffix_ms  = "ms";
static constexpr std::string_view suffix_s   = "s";
static constexpr std::string_view suffix_m   = "m";
static constexpr std::string_view suffix_h   = "h";

// ============================================================
// Pimpl
// ============================================================

struct TomlConfiguration::Impl {
    toml::table table;

    // ----------------------------------------------------------------
    // Helpers for set() with dotted keys
    // ----------------------------------------------------------------

    /**
     * Splits a dotted key like "gateway.listen_port" into a vector of
     * individual key segments: {"gateway", "listen_port"}.
     */
    static std::vector<std::string> split_key(std::string_view key) {
        std::vector<std::string> parts;
        std::size_t start = 0;
        while (start < key.size()) {
            const std::size_t dot = key.find('.', start);
            if (dot == std::string_view::npos) {
                parts.emplace_back(key.substr(start));
                break;
            }
            parts.emplace_back(key.substr(start, dot - start));
            start = dot + 1;
        }
        return parts;
    }

    /**
     * Navigates to the parent table for a dotted key, creating intermediate
     * tables as needed. Returns a pointer to the parent table and the final
     * key segment, or nullptr if a non-table node exists at an intermediate path.
     */
    std::pair<toml::table*, std::string> navigate_to_parent(std::string_view key) {
        auto parts = split_key(key);
        if (parts.empty()) {
            return {nullptr, ""};
        }

        toml::table* current = &table;
        for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
            const auto& part = parts[i];
            auto* node = current->get(part);
            if (node == nullptr) {
                current->insert(part, toml::table{});
                node = current->get(part);
            }
            current = node->as_table();
            if (current == nullptr) {
                return {nullptr, ""};
            }
        }
        return {current, parts.back()};
    }

    // ----------------------------------------------------------------
    // Duration helpers
    // ----------------------------------------------------------------

    /**
     * Formats a duration value as a string with the given suffix.
     * e.g. format_duration(30, "s") -> "30s"
     */
    static std::string format_duration(int64_t count, std::string_view suffix) {
        return fmt::format("{}{}", count, suffix);
    }

    /**
     * Parses a duration string like "30s", "500ms", "1m" into a count and
     * suffix. Returns false if the string is malformed.
     */
    static bool parse_duration_string(std::string_view str,
                                      int64_t& count,
                                      std::string_view& suffix)
    {
        // Find where the numeric part ends.
        std::size_t i = 0;
        if (i < str.size() && str[i] == '-') {
            ++i;
        }
        const std::size_t num_start = (str.size() > 0 && str[0] == '-') ? 1 : 0;
        std::size_t num_end = num_start;
        while (num_end < str.size() && std::isdigit(static_cast<unsigned char>(str[num_end]))) {
            ++num_end;
        }

        if (num_end == num_start) {
            return false; // no digits
        }

        const auto num_str = str.substr(0, num_end);
        const auto result = std::from_chars(num_str.data(),
                                            num_str.data() + num_str.size(),
                                            count);
        if (result.ec != std::errc{}) {
            return false;
        }

        suffix = str.substr(num_end);
        return !suffix.empty();
    }

    /**
     * Converts a duration string to nanoseconds. Returns false with an
     * error message if the string is malformed or the suffix is unknown.
     */
    static bool duration_string_to_ns(std::string_view str,
                                      int64_t& ns_out,
                                      std::string& error)
    {
        int64_t count = 0;
        std::string_view suffix;
        if (!parse_duration_string(str, count, suffix)) {
            error = fmt::format("'{}' is not a valid duration string", str);
            return false;
        }

        if (suffix == suffix_ns) {
            ns_out = count;
        } else if (suffix == suffix_us) {
            ns_out = count * 1'000LL;
        } else if (suffix == suffix_ms) {
            ns_out = count * 1'000'000LL;
        } else if (suffix == suffix_s) {
            ns_out = count * 1'000'000'000LL;
        } else if (suffix == suffix_m) {
            ns_out = count * 60'000'000'000LL;
        } else if (suffix == suffix_h) {
            ns_out = count * 3'600'000'000'000LL;
        } else {
            error = fmt::format("'{}' has unknown duration suffix '{}' "
                                "(expected ns, us, ms, s, m, or h)", str, suffix);
            return false;
        }
        return true;
    }

    /**
     * Converts nanoseconds to a target duration, checking for lossless
     * conversion. Returns false with an error message if lossy.
     */
    template <typename Duration>
    static bool ns_to_duration(int64_t ns,
                                std::string_view original_str,
                                Duration& out,
                                std::string& error)
    {
        using namespace std::chrono;
        const auto target_ns = duration_cast<nanoseconds>(Duration{1}).count();

        if (target_ns <= 1) {
            // Target is nanoseconds or finer -- always lossless.
            out = duration_cast<Duration>(nanoseconds{ns});
            return true;
        }

        if (ns % target_ns != 0) {
            error = fmt::format("'{}' cannot be converted to the requested "
                                "duration type without loss of precision",
                                original_str);
            return false;
        }

        out = Duration{ns / target_ns};
        return true;
    }

    // ----------------------------------------------------------------
    // Node lookup
    // ----------------------------------------------------------------

    /**
     * Looks up a node by dotted key path using toml::at_path.
     * Returns nullptr if not found.
     */
    const toml::node* find_node(std::string_view key) const {
        return toml::at_path(table, key).node();
    }

    /**
     * Looks up a string value at key and returns it in duration_str.
     * Returns {false, error} if the key is missing or not a string.
     */
    std::tuple<bool, std::string> get_duration_string(
        std::string_view key, std::string& duration_str) const
    {
        const auto* node = find_node(key);
        if (!node) {
            return {false, fmt::format("required key '{}' not found in configuration", key)};
        }
        const auto* v = node->as_string();
        if (!v) {
            return {false, fmt::format("key '{}' has wrong type "
                                       "(expected duration string e.g. \"30s\")", key)};
        }
        duration_str = v->get();
        return {true, ""};
    }
};

// ============================================================
// TomlConfiguration
// ============================================================

TomlConfiguration::TomlConfiguration()
    : impl_(new Impl{})
{
}

TomlConfiguration::~TomlConfiguration()
{
    delete impl_;
}

// ----------------------------------------------------------------
// Population
// ----------------------------------------------------------------

std::tuple<bool, std::string> TomlConfiguration::load_file(std::string_view path)
{
    try {
        auto result = toml::parse_file(path);
        impl_->table = std::move(result);
        return {true, ""};
    } catch (const toml::parse_error& ex) {
        const auto& src = ex.source();
        std::string error = fmt::format("{}:{}: {}",
            src.path ? src.path->c_str() : std::string(path),
            src.begin.line,
            ex.description());
        return {false, error};
    }
}

std::tuple<bool, std::string> TomlConfiguration::load_string(std::string_view toml_content)
{
    try {
        auto result = toml::parse(toml_content);
        impl_->table = std::move(result);
        return {true, ""};
    } catch (const toml::parse_error& ex) {
        const auto& src = ex.source();
        std::string error = fmt::format("line {}: {}",
            src.begin.line,
            ex.description());
        return {false, error};
    }
}

// ----------------------------------------------------------------
// set() overloads
// ----------------------------------------------------------------

void TomlConfiguration::set(std::string_view key, const std::string& value)
{
    auto [parent, leaf] = impl_->navigate_to_parent(key);
    if (parent) {
        parent->insert_or_assign(leaf, value);
    }
}

void TomlConfiguration::set(std::string_view key, bool value)
{
    auto [parent, leaf] = impl_->navigate_to_parent(key);
    if (parent) {
        parent->insert_or_assign(leaf, value);
    }
}

void TomlConfiguration::set(std::string_view key, int32_t value)
{
    auto [parent, leaf] = impl_->navigate_to_parent(key);
    if (parent) {
        parent->insert_or_assign(leaf, static_cast<int64_t>(value));
    }
}

void TomlConfiguration::set(std::string_view key, int64_t value)
{
    auto [parent, leaf] = impl_->navigate_to_parent(key);
    if (parent) {
        parent->insert_or_assign(leaf, value);
    }
}

void TomlConfiguration::set(std::string_view key, double value)
{
    auto [parent, leaf] = impl_->navigate_to_parent(key);
    if (parent) {
        parent->insert_or_assign(leaf, value);
    }
}

void TomlConfiguration::set(std::string_view key, std::chrono::nanoseconds value)
{
    auto [parent, leaf] = impl_->navigate_to_parent(key);
    if (parent) {
        parent->insert_or_assign(leaf,
            Impl::format_duration(value.count(), suffix_ns));
    }
}

void TomlConfiguration::set(std::string_view key, std::chrono::microseconds value)
{
    auto [parent, leaf] = impl_->navigate_to_parent(key);
    if (parent) {
        parent->insert_or_assign(leaf,
            Impl::format_duration(value.count(), suffix_us));
    }
}

void TomlConfiguration::set(std::string_view key, std::chrono::milliseconds value)
{
    auto [parent, leaf] = impl_->navigate_to_parent(key);
    if (parent) {
        parent->insert_or_assign(leaf,
            Impl::format_duration(value.count(), suffix_ms));
    }
}

void TomlConfiguration::set(std::string_view key, std::chrono::seconds value)
{
    auto [parent, leaf] = impl_->navigate_to_parent(key);
    if (parent) {
        parent->insert_or_assign(leaf,
            Impl::format_duration(value.count(), suffix_s));
    }
}

void TomlConfiguration::set(std::string_view key, std::chrono::minutes value)
{
    auto [parent, leaf] = impl_->navigate_to_parent(key);
    if (parent) {
        parent->insert_or_assign(leaf,
            Impl::format_duration(value.count(), suffix_m));
    }
}

void TomlConfiguration::set(std::string_view key, std::chrono::hours value)
{
    auto [parent, leaf] = impl_->navigate_to_parent(key);
    if (parent) {
        parent->insert_or_assign(leaf,
            Impl::format_duration(value.count(), suffix_h));
    }
}

// ----------------------------------------------------------------
// get_required() helpers
// ----------------------------------------------------------------

namespace {

std::string not_found_error(std::string_view key)
{
    return fmt::format("required key '{}' not found in configuration", key);
}

std::string wrong_type_error(std::string_view key, std::string_view expected_type)
{
    return fmt::format("key '{}' has wrong type (expected {})", key, expected_type);
}

} // anonymous namespace

// ----------------------------------------------------------------
// get_required() overloads
// ----------------------------------------------------------------

std::tuple<bool, std::string> TomlConfiguration::get_required(
    std::string_view key, std::string& value) const
{
    const auto* node = impl_->find_node(key);
    if (!node) {
        return {false, not_found_error(key)};
    }
    const auto* v = node->as_string();
    if (!v) {
        return {false, wrong_type_error(key, "string")};
    }
    value = v->get();
    return {true, ""};
}

std::tuple<bool, std::string> TomlConfiguration::get_required(
    std::string_view key, bool& value) const
{
    const auto* node = impl_->find_node(key);
    if (!node) {
        return {false, not_found_error(key)};
    }
    const auto* v = node->as_boolean();
    if (!v) {
        return {false, wrong_type_error(key, "boolean")};
    }
    value = v->get();
    return {true, ""};
}

std::tuple<bool, std::string> TomlConfiguration::get_required(
    std::string_view key, int32_t& value) const
{
    const auto* node = impl_->find_node(key);
    if (!node) {
        return {false, not_found_error(key)};
    }
    const auto* v = node->as_integer();
    if (!v) {
        return {false, wrong_type_error(key, "integer")};
    }
    const int64_t raw = v->get();
    if (raw < INT32_MIN || raw > INT32_MAX) {
        return {false, fmt::format("key '{}': value {} is out of range for int32_t",
                                   key, raw)};
    }
    value = static_cast<int32_t>(raw);
    return {true, ""};
}

std::tuple<bool, std::string> TomlConfiguration::get_required(
    std::string_view key, int64_t& value) const
{
    const auto* node = impl_->find_node(key);
    if (!node) {
        return {false, not_found_error(key)};
    }
    const auto* v = node->as_integer();
    if (!v) {
        return {false, wrong_type_error(key, "integer")};
    }
    value = v->get();
    return {true, ""};
}

std::tuple<bool, std::string> TomlConfiguration::get_required(
    std::string_view key, double& value) const
{
    const auto* node = impl_->find_node(key);
    if (!node) {
        return {false, not_found_error(key)};
    }
    const auto* v = node->as_floating_point();
    if (!v) {
        return {false, wrong_type_error(key, "floating point")};
    }
    value = v->get();
    return {true, ""};
}

// Duration get_required helper -- shared logic now lives in Impl::get_duration_string

std::tuple<bool, std::string> TomlConfiguration::get_required(
    std::string_view key, std::chrono::nanoseconds& value) const
{
    std::string str;
    auto [ok, err] = impl_->get_duration_string(key, str);
    if (!ok) return {false, err};

    int64_t ns = 0;
    if (!Impl::duration_string_to_ns(str, ns, err)) {
        return {false, fmt::format("key '{}': {}", key, err)};
    }
    value = std::chrono::nanoseconds{ns};
    return {true, ""};
}

std::tuple<bool, std::string> TomlConfiguration::get_required(
    std::string_view key, std::chrono::microseconds& value) const
{
    std::string str;
    auto [ok, err] = impl_->get_duration_string(key, str);
    if (!ok) return {false, err};

    int64_t ns = 0;
    if (!Impl::duration_string_to_ns(str, ns, err)) {
        return {false, fmt::format("key '{}': {}", key, err)};
    }
    return Impl::ns_to_duration(ns, str, value, err)
        ? std::make_tuple(true, std::string{})
        : std::make_tuple(false, fmt::format("key '{}': {}", key, err));
}

std::tuple<bool, std::string> TomlConfiguration::get_required(
    std::string_view key, std::chrono::milliseconds& value) const
{
    std::string str;
    auto [ok, err] = impl_->get_duration_string(key, str);
    if (!ok) return {false, err};

    int64_t ns = 0;
    if (!Impl::duration_string_to_ns(str, ns, err)) {
        return {false, fmt::format("key '{}': {}", key, err)};
    }
    return Impl::ns_to_duration(ns, str, value, err)
        ? std::make_tuple(true, std::string{})
        : std::make_tuple(false, fmt::format("key '{}': {}", key, err));
}

std::tuple<bool, std::string> TomlConfiguration::get_required(
    std::string_view key, std::chrono::seconds& value) const
{
    std::string str;
    auto [ok, err] = impl_->get_duration_string(key, str);
    if (!ok) return {false, err};

    int64_t ns = 0;
    if (!Impl::duration_string_to_ns(str, ns, err)) {
        return {false, fmt::format("key '{}': {}", key, err)};
    }
    return Impl::ns_to_duration(ns, str, value, err)
        ? std::make_tuple(true, std::string{})
        : std::make_tuple(false, fmt::format("key '{}': {}", key, err));
}

std::tuple<bool, std::string> TomlConfiguration::get_required(
    std::string_view key, std::chrono::minutes& value) const
{
    std::string str;
    auto [ok, err] = impl_->get_duration_string(key, str);
    if (!ok) return {false, err};

    int64_t ns = 0;
    if (!Impl::duration_string_to_ns(str, ns, err)) {
        return {false, fmt::format("key '{}': {}", key, err)};
    }
    return Impl::ns_to_duration(ns, str, value, err)
        ? std::make_tuple(true, std::string{})
        : std::make_tuple(false, fmt::format("key '{}': {}", key, err));
}

std::tuple<bool, std::string> TomlConfiguration::get_required(
    std::string_view key, std::chrono::hours& value) const
{
    std::string str;
    auto [ok, err] = impl_->get_duration_string(key, str);
    if (!ok) return {false, err};

    int64_t ns = 0;
    if (!Impl::duration_string_to_ns(str, ns, err)) {
        return {false, fmt::format("key '{}': {}", key, err)};
    }
    return Impl::ns_to_duration(ns, str, value, err)
        ? std::make_tuple(true, std::string{})
        : std::make_tuple(false, fmt::format("key '{}': {}", key, err));
}

// ----------------------------------------------------------------
// get_required_except() overloads -- delegate to get_required()
// ----------------------------------------------------------------

void TomlConfiguration::get_required_except(std::string_view key, std::string& value) const
{
    auto [ok, err] = get_required(key, value);
    if (!ok) throw ConfigurationException(err);
}

void TomlConfiguration::get_required_except(std::string_view key, bool& value) const
{
    auto [ok, err] = get_required(key, value);
    if (!ok) throw ConfigurationException(err);
}

void TomlConfiguration::get_required_except(std::string_view key, int32_t& value) const
{
    auto [ok, err] = get_required(key, value);
    if (!ok) throw ConfigurationException(err);
}

void TomlConfiguration::get_required_except(std::string_view key, int64_t& value) const
{
    auto [ok, err] = get_required(key, value);
    if (!ok) throw ConfigurationException(err);
}

void TomlConfiguration::get_required_except(std::string_view key, double& value) const
{
    auto [ok, err] = get_required(key, value);
    if (!ok) throw ConfigurationException(err);
}

void TomlConfiguration::get_required_except(std::string_view key, std::chrono::nanoseconds& value) const
{
    auto [ok, err] = get_required(key, value);
    if (!ok) throw ConfigurationException(err);
}

void TomlConfiguration::get_required_except(std::string_view key, std::chrono::microseconds& value) const
{
    auto [ok, err] = get_required(key, value);
    if (!ok) throw ConfigurationException(err);
}

void TomlConfiguration::get_required_except(std::string_view key, std::chrono::milliseconds& value) const
{
    auto [ok, err] = get_required(key, value);
    if (!ok) throw ConfigurationException(err);
}

void TomlConfiguration::get_required_except(std::string_view key, std::chrono::seconds& value) const
{
    auto [ok, err] = get_required(key, value);
    if (!ok) throw ConfigurationException(err);
}

void TomlConfiguration::get_required_except(std::string_view key, std::chrono::minutes& value) const
{
    auto [ok, err] = get_required(key, value);
    if (!ok) throw ConfigurationException(err);
}

void TomlConfiguration::get_required_except(std::string_view key, std::chrono::hours& value) const
{
    auto [ok, err] = get_required(key, value);
    if (!ok) throw ConfigurationException(err);
}

} // namespace pubsub_itc_fw
