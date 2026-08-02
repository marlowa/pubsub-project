// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cctype>
#include <cstddef>

#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include <pubsub_itc_fw/ConfigurationException.hpp>
#include <pubsub_itc_fw/MetricKey.hpp>

namespace pubsub_itc_fw {

namespace {

constexpr char token_separator = '.';

// application.component.metricName, with an optional single scope token between the
// component and the metric name. Scope is one token rather than a list, so the count is
// bounded at both ends -- which is also what makes a dot inside a scope inexpressible
// rather than something needing its own check.
constexpr size_t minimum_token_count = 3;
constexpr size_t maximum_token_count = 4;
constexpr size_t scope_token_index = 2;

// Compared explicitly rather than by calling isalnum(). Two reasons, and this is the one
// place in the file where they apply: the input here is unvalidated, so a byte above 0x7F
// reaches this function, and isalnum takes an int and is undefined for a negative value --
// which is what such a byte becomes where char is signed. isalnum is also one of the
// classifiers a locale may extend, so what counts as alphanumeric would not be fixed.
//
// std::isdigit below is a different case: it runs only on a character already known to be
// in the set this function admits, and unlike isalnum the standard pins it to '0'-'9'.
bool is_token_character(char character) {
    return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') || character == '_';
}

// Names the offending token by position as well as by value: a key may repeat a token,
// and "token 3" tells the operator where to look in a way the value alone does not.
[[noreturn]] void throw_token_error(std::string_view full_name, size_t token_index, std::string_view reason) {
    throw ConfigurationException(fmt::format("MetricKey: invalid metric key '{}': token {} {}", full_name, token_index + 1, reason));
}

std::vector<std::string_view> split_on_separator(std::string_view full_name) {
    std::vector<std::string_view> tokens;
    size_t token_start = 0;
    for (size_t index = 0; index <= full_name.size(); ++index) {
        if (index == full_name.size() || full_name[index] == token_separator) {
            tokens.push_back(full_name.substr(token_start, index - token_start));
            token_start = index + 1;
        }
    }
    return tokens;
}

} // un-named namespace

MetricKey MetricKey::compose(const std::string& application, const std::string& component, const std::string& scope, const std::string& metric_name) {
    // Assembled and then parsed, rather than validated field by field. The dotted form is
    // the single definition of what a key may be, so composing one and handing it to the
    // parsing constructor means there is no second set of rules to drift.
    //
    // A part containing a dot therefore fails as "too many tokens" rather than with a
    // bespoke message, which is the same thing an operator writing it in configuration
    // would see.
    std::string assembled = application;
    assembled += token_separator;
    assembled += component;
    if (!scope.empty()) {
        assembled += token_separator;
        assembled += scope;
    }
    assembled += token_separator;
    assembled += metric_name;
    return MetricKey(assembled.c_str());
}

MetricKey::MetricKey(const char* full_name) {
    if (full_name == nullptr) {
        throw ConfigurationException("MetricKey: metric key is null");
    }

    const std::string_view full_name_view(full_name);
    if (full_name_view.empty()) {
        throw ConfigurationException("MetricKey: metric key is empty");
    }

    const std::vector<std::string_view> tokens = split_on_separator(full_name_view);

    // Checked before the per-token rules so that "a.b" reports the real problem -- the wrong
    // number of parts -- rather than passing every token check and then failing obscurely
    // later. The upper bound is what a dotted scope trips: "app.component.a.b.metric" is
    // not a scope containing a dot, it is five tokens, and the message says so.
    if (tokens.size() < minimum_token_count || tokens.size() > maximum_token_count) {
        throw ConfigurationException(fmt::format("MetricKey: invalid metric key '{}': needs {} or {} dot-separated tokens "
                                                 "(<application>.<component>[.<scope>].<metricName>), found {}. "
                                                 "Scope is a single token and may not contain a '.'",
                                                 full_name_view, minimum_token_count, maximum_token_count, tokens.size()));
    }

    for (size_t index = 0; index < tokens.size(); ++index) {
        const std::string_view token = tokens[index];
        // An empty token is how a leading dot, a trailing dot and a doubled dot all
        // present themselves, so one check covers the three.
        if (token.empty()) {
            throw_token_error(full_name_view, index, "is empty (check for a leading, trailing or doubled '.')");
        }
        for (const char character : token) {
            if (!is_token_character(character)) {
                throw_token_error(full_name_view, index, fmt::format("contains '{}'; only letters, digits and '_' are allowed", character));
            }
        }
    }

    // The leaf alone becomes the Prometheus metric name, and Prometheus requires those to
    // match [a-zA-Z_:][a-zA-Z0-9_:]*. Every other token becomes a label value, where a
    // leading digit is perfectly legal -- so this rule belongs to the leaf and nowhere
    // else. Rejecting it here turns a metric the exposition would refuse into a
    // configuration error naming the key.
    const std::string_view metric_name_token = tokens.back();
    if (std::isdigit(static_cast<unsigned char>(metric_name_token.front())) != 0) {
        throw_token_error(full_name_view, tokens.size() - 1,
                          fmt::format("is the metric name '{}', which may not start with a digit "
                                      "(Prometheus metric names must match [a-zA-Z_][a-zA-Z0-9_]*)",
                                      metric_name_token));
    }

    full_name_ = std::string(full_name_view);
    application_ = std::string(tokens[0]);
    component_ = std::string(tokens[1]);
    metric_name_ = std::string(metric_name_token);

    // Present only in the four-token form. Left empty otherwise, which the caller reads as
    // "omit the scope label" -- a token can never be empty, so that cannot be ambiguous.
    if (tokens.size() == maximum_token_count) {
        scope_ = std::string(tokens[scope_token_index]);
    }
}

} // namespaces
