// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <string>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/ConfigurationException.hpp>
#include <pubsub_itc_fw/MetricKey.hpp>

using pubsub_itc_fw::ConfigurationException;
using pubsub_itc_fw::MetricKey;

namespace {

// Every rejection test asserts the message mentions the offending key as well as the type
// of the exception. A metric key comes from a configuration file, so the operator reading
// the message needs to know which entry to go and fix; a bare "invalid metric key" would
// be true and useless.
void expect_rejected(const char* full_name, const std::string& expected_message_fragment) {
    try {
        const MetricKey key(full_name);
        FAIL() << "expected ConfigurationException for key '" << (full_name == nullptr ? "(null)" : full_name) << "' but construction succeeded";
    } catch (const ConfigurationException& exception) {
        const std::string message(exception.what());
        EXPECT_NE(message.find(expected_message_fragment), std::string::npos)
            << "message did not explain the problem.\n  expected to contain: " << expected_message_fragment << "\n  actual message     : " << message;
        if (full_name != nullptr && *full_name != '\0') {
            EXPECT_NE(message.find(full_name), std::string::npos) << "message did not name the offending key.\n  actual message: " << message;
        }
    }
}

} // un-named namespace

// -- The worked example from the design ----------------------------------------

// The example the whole design was specified against, kept as a test so the mapping from
// dotted key to time series cannot drift silently.
TEST(MetricKeyTest, SplitsTheWorkedExampleIntoItsPrometheusParts) {
    const MetricKey key("pubsub.gateway.binary.socket_latency_seconds");

    EXPECT_EQ(key.application(), "pubsub");
    EXPECT_EQ(key.component(), "gateway");
    EXPECT_EQ(key.scope(), "binary");
    EXPECT_EQ(key.metric_name(), "socket_latency_seconds");
    EXPECT_TRUE(key.has_scope());
    EXPECT_EQ(key.full_name(), "pubsub.gateway.binary.socket_latency_seconds");
}

// -- Token counts --------------------------------------------------------------

// The shortest legal key. Scope is absent, and absent must mean an empty scope string so
// the caller omits the label rather than emitting scope="".
TEST(MetricKeyTest, ThreeTokensMeansNoScope) {
    const MetricKey key("pubsub.gateway.orders_total");

    EXPECT_EQ(key.application(), "pubsub");
    EXPECT_EQ(key.component(), "gateway");
    EXPECT_EQ(key.metric_name(), "orders_total");
    EXPECT_EQ(key.scope(), "");
    EXPECT_FALSE(key.has_scope());
}

TEST(MetricKeyTest, FourTokensGivesTheScope) {
    const MetricKey key("pubsub.gateway.binary.orders_total");

    EXPECT_EQ(key.application(), "pubsub");
    EXPECT_EQ(key.component(), "gateway");
    EXPECT_EQ(key.scope(), "binary");
    EXPECT_TRUE(key.has_scope());
    EXPECT_EQ(key.metric_name(), "orders_total");
}

TEST(MetricKeyTest, RejectsFewerThanThreeTokens) {
    expect_rejected("", "empty");
    expect_rejected("metric_total", "3 or 4");
    expect_rejected("gateway.orders_total", "3 or 4");
}

// Scope is a single token, so a fifth token is the error -- and this is also the only way a
// dotted scope can present itself. The message has to say that, because the operator who
// wrote "binary.tcp" as a scope will otherwise read "found 5 tokens" and not see why.
TEST(MetricKeyTest, RejectsMoreThanFourTokens) {
    expect_rejected("pubsub.gateway.binary.tcp.socket_latency_seconds", "3 or 4");
    expect_rejected("pubsub.gateway.binary.tcp.socket_latency_seconds", "may not contain a '.'");
    expect_rejected("app.component.one.two.three.four.five.metric_total", "3 or 4");
}

// -- Character set -------------------------------------------------------------

TEST(MetricKeyTest, AcceptsLettersDigitsAndUnderscoresInTokens) {
    const MetricKey key("pubsub2.gateway_a.api_v1_auth.socket_latency_seconds");

    EXPECT_EQ(key.application(), "pubsub2");
    EXPECT_EQ(key.component(), "gateway_a");
    EXPECT_EQ(key.scope(), "api_v1_auth");
    EXPECT_EQ(key.metric_name(), "socket_latency_seconds");
}

// A token may be a single character, including a single underscore, and a token may be all
// digits as long as it is not the metric name.
TEST(MetricKeyTest, AcceptsSingleCharacterAndAllDigitTokens) {
    const MetricKey key("a.b.500.metric_total");

    EXPECT_EQ(key.application(), "a");
    EXPECT_EQ(key.component(), "b");
    EXPECT_EQ(key.scope(), "500");
    EXPECT_EQ(key.metric_name(), "metric_total");

    const MetricKey underscore_scope("a.b._.metric_total");
    EXPECT_EQ(underscore_scope.scope(), "_");
}

TEST(MetricKeyTest, RejectsCharactersOutsideTheAllowedSet) {
    expect_rejected("pubsub.gate-way.orders_total", "contains '-'");
    expect_rejected("pubsub.gateway.orders total", "contains ' '");
    expect_rejected("pubsub.gateway.orders/total", "contains '/'");
    expect_rejected("pubsub.gateway.orders+total", "contains '+'");
    expect_rejected("pubsub.gateway.orders\ttotal", "only letters, digits and '_' are allowed");
}

// Prometheus permits a colon in a metric name but reserves it by convention for recording
// rules, so a metric emitting one would collide with that convention.
TEST(MetricKeyTest, RejectsColonEvenThoughPrometheusPermitsItInNames) {
    expect_rejected("pubsub.gateway.job:orders_total", "contains ':'");
}

// A byte above 0x7F is the case that makes the obvious isalnum() implementation undefined
// on a signed-char platform. It must be a clean rejection, not a crash or an accident.
TEST(MetricKeyTest, RejectsHighBitBytesCleanly) {
    expect_rejected("pubsub.gateway.orders\xC3\xA9_total", "only letters, digits and '_' are allowed");
    expect_rejected("pubsub.gate\xFFway.orders_total", "only letters, digits and '_' are allowed");
}

// -- Separator placement -------------------------------------------------------

// A leading dot, a trailing dot and a doubled dot are all one underlying fault -- an empty
// token -- and all three must be caught, since each is an easy typo in a config file.
TEST(MetricKeyTest, RejectsEmptyTokensFromMisplacedSeparators) {
    expect_rejected(".pubsub.gateway.orders_total", "is empty");
    expect_rejected("pubsub.gateway.orders_total.", "is empty");
    expect_rejected("pubsub..gateway.orders_total", "is empty");
    expect_rejected("pubsub.gateway..orders_total", "is empty");
}

TEST(MetricKeyTest, RejectsAPathOfSeparatorsAlone) {
    expect_rejected("..", "is empty");
    expect_rejected("...", "is empty");
}

// "a.b" is two tokens and fails the count; "a.b." is three tokens, the last of which is
// empty. Distinguishing them matters because the fixes differ -- add a token, or delete a
// stray dot.
TEST(MetricKeyTest, DistinguishesTooFewTokensFromATrailingSeparator) {
    expect_rejected("a.b", "3 or 4");
    expect_rejected("a.b.", "is empty");
}

// -- The metric name is stricter than the other tokens -------------------------

// The leaf becomes the metric name, and Prometheus requires those to match
// [a-zA-Z_:][a-zA-Z0-9_:]*. This is the one rule that applies to a single token.
TEST(MetricKeyTest, RejectsAMetricNameStartingWithADigit) {
    expect_rejected("pubsub.gateway.5xx_total", "may not start with a digit");
    expect_rejected("pubsub.gateway.scope.404_total", "may not start with a digit");
    expect_rejected("pubsub.gateway.0", "may not start with a digit");
}

// The same restriction must NOT be applied to the other tokens: they become label values,
// where Prometheus places no such constraint. This is the test that would fail if the rule
// were applied to every token out of misplaced consistency.
TEST(MetricKeyTest, AllowsALeadingDigitEverywhereExceptTheMetricName) {
    const MetricKey key("2fa.5xx.404.latency_seconds");

    EXPECT_EQ(key.application(), "2fa");
    EXPECT_EQ(key.component(), "5xx");
    EXPECT_EQ(key.scope(), "404");
    EXPECT_EQ(key.metric_name(), "latency_seconds");
}

TEST(MetricKeyTest, AllowsAMetricNameStartingWithAnUnderscore) {
    const MetricKey key("pubsub.gateway._internal_total");

    EXPECT_EQ(key.metric_name(), "_internal_total");
}

// -- Null and lifetime ---------------------------------------------------------

TEST(MetricKeyTest, RejectsANullPath) {
    expect_rejected(nullptr, "null");
}

// The key must own its strings. Constructing from a buffer that is then overwritten proves
// it copied rather than retained views into the caller's memory -- the failure mode this
// would otherwise have is a dangling read long after construction.
TEST(MetricKeyTest, CopiesTheCallersBufferRatherThanReferencingIt) {
    char buffer[] = "pubsub.gateway.binary.socket_latency_seconds";
    const MetricKey key(buffer);
    for (char& character : buffer) {
        character = 'X';
    }
    buffer[sizeof(buffer) - 1] = '\0';

    EXPECT_EQ(key.application(), "pubsub");
    EXPECT_EQ(key.component(), "gateway");
    EXPECT_EQ(key.scope(), "binary");
    EXPECT_EQ(key.metric_name(), "socket_latency_seconds");
    EXPECT_EQ(key.full_name(), "pubsub.gateway.binary.socket_latency_seconds");
}

// -- Scale ---------------------------------------------------------------------

// Token length is unbounded even though the token COUNT is not, so a long single token
// must work rather than trip an undocumented cap.
TEST(MetricKeyTest, AcceptsLongTokens) {
    const std::string long_token(4096, 'a');
    const std::string full_name = long_token + "." + long_token + "." + long_token + "." + long_token + "_total";

    const MetricKey key(full_name.c_str());

    EXPECT_EQ(key.application(), long_token);
    EXPECT_EQ(key.component(), long_token);
    EXPECT_EQ(key.scope(), long_token);
    EXPECT_EQ(key.metric_name(), long_token + "_total");
}
