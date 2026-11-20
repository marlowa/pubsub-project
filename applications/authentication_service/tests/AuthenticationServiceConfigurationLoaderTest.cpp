// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/ConfigurationException.hpp>
#include <pubsub_itc_fw/TomlConfiguration.hpp>
#include <pubsub_itc_fw/tests_common/ScratchDirectory.hpp>

#include "AuthenticationServiceConfigurationLoader.hpp"

namespace authentication_service::tests {

namespace {

// Everything the loader requires, as in applications/authentication_service/authentication_service_a.toml with the
// dev environment's values, except pinning, which is off so that no CPU layout file is needed. Each test changes one
// thing, so nothing else can be the reason it fails.
const std::string network_section = R"(
[network]
listen_host = "127.0.0.1"
listen_port = 11070
)";

const std::string inactivity_limit = "connection_inactivity_limit = \"600s\"\n";

const std::string remaining_sections = R"(
[admin]
listen_port                    = 11072
tls_certificate_path           = "admin.crt"
tls_private_key_path           = "admin.key"

[logging]
applog_level     = "info"
syslog_level     = "critical"
mode             = "none"
max_file_size    = 10240000
max_backup_files = 10

[reactor]
cpu_pinning_enabled            = false
cpu_pinning_reserve_cpu0       = true
connect_retry_warning_interval = "15m"

[event_queue_pool]
objects_per_slab = 64
initial_slabs    = 1

[command_queue_pool]
objects_per_slab = 64
initial_slabs    = 1
[technical_events]
primary_host = "127.0.0.1"
primary_port = 11046
secondary_host = ""
secondary_port = 0

[end_of_day]
last_state_file = "/run/venue/authentication_service_a.last_state"
)";

const std::string stored_key_hex(64, 'a');
const std::string server_key_hex(64, 'b');
const std::string salt_hex(32, 'c');

/// One credential entry for comp_id, followed by any extra keys given.
std::string credential(const std::string& comp_id, const std::string& extra_keys = "") {
    return "[[credential]]\ncomp_id = \"" + comp_id + "\"\nstored_key = \"" + stored_key_hex + "\"\nserver_key = \"" + server_key_hex + "\"\nsalt = \"" +
           salt_hex + "\"\niterations = 4096\n" + extra_keys + "\n";
}

class AuthenticationServiceConfigurationLoaderTest : public ::testing::Test {
  protected:
    void SetUp() override {
        directory_ = pubsub_itc_fw::tests_common::make_scratch_directory("authentication_service_configuration");
    }
    void TearDown() override {
        pubsub_itc_fw::tests_common::remove_scratch_directory(directory_);
    }

    /// Writes credentials.toml beside the configuration, then loads a configuration made of the given parts.
    AuthenticationServiceConfiguration load(const std::string& credentials_content, const std::string& network = network_section + inactivity_limit,
                                            const std::string& first_lines = "credentials_file = \"credentials.toml\"\n") {
        std::ofstream(directory_ + "/credentials.toml") << credentials_content;
        pubsub_itc_fw::TomlConfiguration toml;
        const auto [ok, error] = toml.load_string(first_lines + network + remaining_sections);
        EXPECT_TRUE(ok) << error;
        return AuthenticationServiceConfigurationLoader::load(toml, directory_ + "/authentication_service_a.toml");
    }

    std::string directory_;
};

} // namespaces

TEST_F(AuthenticationServiceConfigurationLoaderTest, ReadsTheListenerAndTheInactivityLimit) {
    const AuthenticationServiceConfiguration config = load(credential("MEMBER1"));
    EXPECT_EQ(config.listen_host, "127.0.0.1");
    EXPECT_EQ(config.listen_port, 11'070);
    EXPECT_EQ(config.admin_listen_port, 11'072);
    EXPECT_EQ(config.connection_inactivity_limit, std::chrono::seconds(600));
}

TEST_F(AuthenticationServiceConfigurationLoaderTest, ReadsAnInactivityLimitGivenInMinutes) {
    const AuthenticationServiceConfiguration config = load(credential("MEMBER1"), network_section + "connection_inactivity_limit = \"5m\"\n");
    EXPECT_EQ(config.connection_inactivity_limit, std::chrono::seconds(300));
}

TEST_F(AuthenticationServiceConfigurationLoaderTest, RefusesAConfigurationWithNoInactivityLimit) {
    EXPECT_THROW(load(credential("MEMBER1"), network_section), pubsub_itc_fw::ConfigurationException);
}

TEST_F(AuthenticationServiceConfigurationLoaderTest, RefusesAnInactivityLimitBelowOneSecond) {
    EXPECT_THROW(load(credential("MEMBER1"), network_section + "connection_inactivity_limit = \"0s\"\n"), pubsub_itc_fw::ConfigurationException);
}

TEST_F(AuthenticationServiceConfigurationLoaderTest, TakesTheCredentialsFileRelativeToTheConfiguration) {
    const AuthenticationServiceConfiguration config = load(credential("MEMBER1"));
    EXPECT_EQ(config.credentials_file, directory_ + "/credentials.toml");
    EXPECT_EQ(config.admin_tls_certificate_path, directory_ + "/admin.crt");
}

TEST_F(AuthenticationServiceConfigurationLoaderTest, LeavesTheOptionalAdministrationKeysAtTheirDefaultsWhenAbsent) {
    const AuthenticationServiceConfiguration config = load(credential("MEMBER1"));
    EXPECT_EQ(config.admin_tls_ca_path, "");
    EXPECT_FALSE(config.admin_tls_require_client_certificate);
}

TEST_F(AuthenticationServiceConfigurationLoaderTest, ReadsTheOptionalAdministrationKeysWhenGiven) {
    std::ofstream(directory_ + "/credentials.toml") << credential("MEMBER1");
    pubsub_itc_fw::TomlConfiguration toml;
    std::string text = "credentials_file = \"credentials.toml\"\n" + network_section + inactivity_limit + remaining_sections;
    const std::string admin_port_line = "listen_port                    = 11072\n";
    text.replace(text.find(admin_port_line), admin_port_line.size(), admin_port_line + "tls_ca_path = \"ca.crt\"\ntls_require_client_certificate = true\n");
    const auto [ok, error] = toml.load_string(text);
    ASSERT_TRUE(ok) << error;
    const AuthenticationServiceConfiguration with_keys = AuthenticationServiceConfigurationLoader::load(toml, directory_ + "/authentication_service_a.toml");
    EXPECT_EQ(with_keys.admin_tls_ca_path, directory_ + "/ca.crt");
    EXPECT_TRUE(with_keys.admin_tls_require_client_certificate);
}

TEST_F(AuthenticationServiceConfigurationLoaderTest, ReadsEachCredential) {
    const AuthenticationServiceConfiguration config = load(credential("MEMBER1") + credential("MEMBER2"));
    ASSERT_EQ(config.credentials.size(), 2U);
    const scram_crypto::ScramCredential& member = config.credentials.at("MEMBER1");
    EXPECT_EQ(member.iterations, 4096);
    EXPECT_EQ(member.stored_key.size(), 32U);
    EXPECT_EQ(member.stored_key[0], 0xAA);
    EXPECT_EQ(member.server_key[0], 0xBB);
    EXPECT_EQ(member.salt.size(), 16U);
}

TEST_F(AuthenticationServiceConfigurationLoaderTest, ACredentialWithNoProvisioningHasNoSessionPolicy) {
    const AuthenticationServiceConfiguration config = load(credential("MEMBER1"));
    EXPECT_EQ(config.session_policies.count("MEMBER1"), 0U);
}

TEST_F(AuthenticationServiceConfigurationLoaderTest, ReadsACredentialsProvisioning) {
    const AuthenticationServiceConfiguration config =
        load(credential("MEMBER1", "cancel_on_disconnect_enabled = false\ncancel_on_disconnect_grace_period = \"45s\"\n"
                                   "primary_gateway_instance = 2\nbackup_gateway_instance = 1\n"
                                   "max_place_per_second = 100\nmax_amend_per_second = 50\nmax_cancel_per_second = 200\n"));
    const AuthenticationServiceConfiguration::SessionPolicy& policy = config.session_policies.at("MEMBER1");
    EXPECT_EQ(policy.cancel_on_disconnect_enabled, false);
    EXPECT_EQ(policy.cancel_on_disconnect_grace_period_seconds, 45);
    EXPECT_EQ(policy.primary_gateway_instance, 2);
    EXPECT_EQ(policy.backup_gateway_instance, 1);
    EXPECT_EQ(policy.max_place_per_second, 100);
    EXPECT_EQ(policy.max_amend_per_second, 50);
    EXPECT_EQ(policy.max_cancel_per_second, 200);
}

TEST_F(AuthenticationServiceConfigurationLoaderTest, RefusesABackupGatewayInstanceWithNoPrimary) {
    EXPECT_THROW(load(credential("MEMBER1", "backup_gateway_instance = 1\n")), pubsub_itc_fw::ConfigurationException);
}

TEST_F(AuthenticationServiceConfigurationLoaderTest, RefusesABackupGatewayInstanceTheSameAsThePrimary) {
    EXPECT_THROW(load(credential("MEMBER1", "primary_gateway_instance = 1\nbackup_gateway_instance = 1\n")), pubsub_itc_fw::ConfigurationException);
}

TEST_F(AuthenticationServiceConfigurationLoaderTest, RefusesAThrottleLimitAboveTheMostPermitted) {
    EXPECT_THROW(load(credential("MEMBER1", "max_place_per_second = 100001\n")), pubsub_itc_fw::ConfigurationException);
}

TEST_F(AuthenticationServiceConfigurationLoaderTest, RefusesANegativeGracePeriod) {
    EXPECT_THROW(load(credential("MEMBER1", "cancel_on_disconnect_grace_period = \"-1s\"\n")), pubsub_itc_fw::ConfigurationException);
}

TEST_F(AuthenticationServiceConfigurationLoaderTest, RefusesAStoredKeyOfTheWrongLength) {
    std::string entry = credential("MEMBER1");
    entry.replace(entry.find(stored_key_hex), stored_key_hex.size(), std::string(62, 'a'));
    EXPECT_THROW(load(entry), pubsub_itc_fw::ConfigurationException);
}

TEST_F(AuthenticationServiceConfigurationLoaderTest, RefusesAKeyThatIsNotHexadecimal) {
    std::string entry = credential("MEMBER1");
    entry.replace(entry.find(server_key_hex), 2, "zz");
    EXPECT_THROW(load(entry), pubsub_itc_fw::ConfigurationException);
}

TEST_F(AuthenticationServiceConfigurationLoaderTest, RefusesAMissingCredentialsFile) {
    pubsub_itc_fw::TomlConfiguration toml;
    const auto [ok, error] = toml.load_string("credentials_file = \"absent.toml\"\n" + network_section + inactivity_limit + remaining_sections);
    ASSERT_TRUE(ok) << error;
    EXPECT_THROW(AuthenticationServiceConfigurationLoader::load(toml, directory_ + "/authentication_service_a.toml"), pubsub_itc_fw::ConfigurationException);
}

} // namespaces
