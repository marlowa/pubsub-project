// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <pubsub_itc_fw/PreconditionAssertion.hpp>
#include <pubsub_itc_fw/ServiceRegistry.hpp>

namespace pubsub_itc_fw::tests {

class ServiceRegistryTest : public ::testing::Test {
protected:
    ServiceRegistry registry_;
};

TEST_F(ServiceRegistryTest, EmptyOnConstruction)
{
    EXPECT_TRUE(registry_.empty());
    EXPECT_EQ(registry_.size(), 0);
}

TEST_F(ServiceRegistryTest, AddSingleServiceWithPrimaryOnly)
{
    registry_.add("joe", {"192.168.1.10", 5001}, {});
    EXPECT_EQ(registry_.size(), 1);
    EXPECT_FALSE(registry_.empty());
}

TEST_F(ServiceRegistryTest, LookupKnownServiceReturnsPrimaryEndpoint)
{
    registry_.add("joe", {"192.168.1.10", 5001}, {});

    auto [endpoints, error] = registry_.lookup("joe");

    EXPECT_TRUE(error.empty());
    EXPECT_EQ(endpoints.primary.host, "192.168.1.10");
    EXPECT_EQ(endpoints.primary.port, 5001);
}

TEST_F(ServiceRegistryTest, LookupKnownServiceReturnsSecondaryEndpoint)
{
    registry_.add("joe", {"192.168.1.10", 5001}, {"192.168.1.11", 5001});

    auto [endpoints, error] = registry_.lookup("joe");

    EXPECT_TRUE(error.empty());
    EXPECT_EQ(endpoints.secondary.host, "192.168.1.11");
    EXPECT_EQ(endpoints.secondary.port, 5001);
}

TEST_F(ServiceRegistryTest, LookupUnknownServiceReturnsError)
{
    auto [endpoints, error] = registry_.lookup("unknown");

    EXPECT_FALSE(error.empty());
}

TEST_F(ServiceRegistryTest, LookupUnknownServiceReturnsDefaultEndpoints)
{
    auto [endpoints, error] = registry_.lookup("unknown");

    EXPECT_EQ(endpoints.primary.port, 0);
    EXPECT_EQ(endpoints.secondary.port, 0);
}

TEST_F(ServiceRegistryTest, AddMultipleServicesAllLookupCorrectly)
{
    registry_.add("joe",  {"192.168.1.10", 5001}, {"192.168.1.11", 5001});
    registry_.add("mary", {"192.168.1.10", 5002}, {"192.168.1.11", 5002});
    registry_.add("fred", {"192.168.1.10", 5003}, {"192.168.1.11", 5003});

    EXPECT_EQ(registry_.size(), 3);

    auto [joe_endpoints, joe_error] = registry_.lookup("joe");
    EXPECT_TRUE(joe_error.empty());
    EXPECT_EQ(joe_endpoints.primary.port, 5001);

    auto [mary_endpoints, mary_error] = registry_.lookup("mary");
    EXPECT_TRUE(mary_error.empty());
    EXPECT_EQ(mary_endpoints.primary.port, 5002);

    auto [fred_endpoints, fred_error] = registry_.lookup("fred");
    EXPECT_TRUE(fred_error.empty());
    EXPECT_EQ(fred_endpoints.primary.port, 5003);
}

TEST_F(ServiceRegistryTest, AddDuplicateNameThrows)
{
    registry_.add("joe", {"192.168.1.10", 5001}, {});
    EXPECT_THROW(registry_.add("joe", {"192.168.1.10", 5002}, {}), PreconditionAssertion);
}

TEST_F(ServiceRegistryTest, AddEmptyNameThrows)
{
    EXPECT_THROW(registry_.add("", {"192.168.1.10", 5001}, {}), PreconditionAssertion);
}

TEST_F(ServiceRegistryTest, ServiceWithNoSecondaryHasZeroPortOnSecondary)
{
    registry_.add("joe", {"192.168.1.10", 5001}, {});

    auto [endpoints, error] = registry_.lookup("joe");

    EXPECT_TRUE(error.empty());
    EXPECT_EQ(endpoints.secondary.port, 0);
}

TEST_F(ServiceRegistryTest, LookupIsCaseSensitive)
{
    registry_.add("joe", {"192.168.1.10", 5001}, {});

    auto [endpoints, error] = registry_.lookup("Joe");

    EXPECT_FALSE(error.empty());
}

TEST_F(ServiceRegistryTest, AddDuplicateAfterThrowDoesNotCorruptRegistry)
{
    registry_.add("joe", {"192.168.1.10", 5001}, {});

    EXPECT_THROW(registry_.add("joe", {"10.0.0.1", 9999}, {}), PreconditionAssertion);

    // Original entry must be intact after the failed add.
    auto [endpoints, error] = registry_.lookup("joe");
    EXPECT_TRUE(error.empty());
    EXPECT_EQ(endpoints.primary.host, "192.168.1.10");
    EXPECT_EQ(endpoints.primary.port, 5001);
}

TEST_F(ServiceRegistryTest, IPv6AddressRoundTrips)
{
    registry_.add("joe", {"::1", 5001}, {"fe80::1", 5001});

    auto [endpoints, error] = registry_.lookup("joe");

    EXPECT_TRUE(error.empty());
    EXPECT_EQ(endpoints.primary.host, "::1");
    EXPECT_EQ(endpoints.secondary.host, "fe80::1");
}

TEST_F(ServiceRegistryTest, HostnameRoundTrips)
{
    registry_.add("joe", {"primary.example.com", 5001}, {"secondary.example.com", 5001});

    auto [endpoints, error] = registry_.lookup("joe");

    EXPECT_TRUE(error.empty());
    EXPECT_EQ(endpoints.primary.host, "primary.example.com");
    EXPECT_EQ(endpoints.secondary.host, "secondary.example.com");
}

} // namespace pubsub_itc_fw
