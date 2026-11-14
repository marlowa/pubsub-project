// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/ConnectionID.hpp>

#include <authentication.hpp>
#include <scram_crypto/ScramCrypto.hpp>

#include "ScramExchanges.hpp"

namespace authentication_service::tests {

namespace {

using pubsub_itc_fw_app::AuthenticationOutcome;

const std::string member = "MEMBER1";
const std::string password = "correct horse battery staple";
constexpr int32_t iterations = 4096;

const pubsub_itc_fw::ConnectionID gateway_connection{3, std::string("inbound:11070")};
const pubsub_itc_fw::ConnectionID other_gateway_connection{4, std::string("inbound:11070")};

const std::vector<uint8_t> client_nonce{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
const std::vector<uint8_t> server_nonce_suffix{0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF};

ScramExchanges::Credentials credentials_with_member() {
    const std::vector<uint8_t> salt{0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F};
    ScramExchanges::Credentials credentials;
    credentials[member] = scram_crypto::make_scram_credential(password, salt.data(), salt.size(), iterations);
    return credentials;
}

/// What a gateway computes from a challenge: the proof to send, and the server signature it expects back.
struct GatewaySide {
    std::vector<uint8_t> client_proof;
    std::vector<uint8_t> expected_server_signature;
};

/// Derives the proof as an order gateway does, from the password the member gave and the challenge received.
GatewaySide gateway_answers(const std::string& comp_id, const std::string& password_given, const ScramExchanges::RequestAnswer& challenge) {
    static const std::string client_key_label = "Client Key";
    static const std::string server_key_label = "Server Key";
    const std::vector<uint8_t> auth_message =
        scram_crypto::compute_auth_message(comp_id, client_nonce, challenge.server_nonce, challenge.salt.data(), challenge.salt.size(), challenge.iterations);
    const std::vector<uint8_t> salted_password =
        scram_crypto::pbkdf2_sha256(password_given, challenge.salt.data(), challenge.salt.size(), challenge.iterations);
    const std::vector<uint8_t> client_key = scram_crypto::hmac_sha256(salted_password.data(), salted_password.size(),
                                                                      reinterpret_cast<const uint8_t*>(client_key_label.data()), client_key_label.size());
    const std::vector<uint8_t> stored_key = scram_crypto::sha256(client_key.data(), client_key.size());
    const std::vector<uint8_t> client_signature = scram_crypto::hmac_sha256(stored_key.data(), stored_key.size(), auth_message.data(), auth_message.size());
    GatewaySide side;
    side.client_proof.resize(client_key.size());
    for (size_t index = 0; index < client_key.size(); ++index) {
        side.client_proof[index] = client_key[index] ^ client_signature[index];
    }
    const std::vector<uint8_t> server_key = scram_crypto::hmac_sha256(salted_password.data(), salted_password.size(),
                                                                      reinterpret_cast<const uint8_t*>(server_key_label.data()), server_key_label.size());
    side.expected_server_signature = scram_crypto::hmac_sha256(server_key.data(), server_key.size(), auth_message.data(), auth_message.size());
    return side;
}

class ScramExchangesTest : public ::testing::Test {
  protected:
    ScramExchanges::Credentials credentials_ = credentials_with_member();
    ScramExchanges::SessionPolicies session_policies_;
    ScramExchanges exchanges_{credentials_, session_policies_};

    ScramExchanges::RequestAnswer request(int64_t request_id, const std::string& comp_id = member,
                                          const pubsub_itc_fw::ConnectionID& connection_id = gateway_connection) {
        return exchanges_.on_request(connection_id, request_id, comp_id, client_nonce, server_nonce_suffix);
    }
};

} // namespaces

TEST_F(ScramExchangesTest, AKnownCompIdIsChallengedWithItsStoredSaltAndIterations) {
    const ScramExchanges::RequestAnswer answer = request(1);
    ASSERT_TRUE(answer.challenge);
    EXPECT_EQ(answer.salt, credentials_.at(member).salt);
    EXPECT_EQ(answer.iterations, iterations);
    EXPECT_EQ(exchanges_.exchanges_in_progress(), 1U);
}

TEST_F(ScramExchangesTest, TheServerNonceIsTheClientNonceFollowedByTheServicesOwnBytes) {
    const ScramExchanges::RequestAnswer answer = request(1);
    std::vector<uint8_t> expected = client_nonce;
    expected.insert(expected.end(), server_nonce_suffix.begin(), server_nonce_suffix.end());
    EXPECT_EQ(answer.server_nonce, expected);
}

TEST_F(ScramExchangesTest, AnUnknownCompIdIsNotChallengedAndStartsNoExchange) {
    const ScramExchanges::RequestAnswer answer = request(1, "NOBODY");
    EXPECT_FALSE(answer.challenge);
    EXPECT_EQ(exchanges_.exchanges_in_progress(), 0U);

    const pubsub_itc_fw_app::AuthenticationResult result = ScramExchanges::unknown_user_message(1);
    EXPECT_EQ(result.request_id, 1);
    EXPECT_EQ(result.outcome, AuthenticationOutcome::UnknownUser);
    EXPECT_EQ(result.server_signature.size, 0U);
}

TEST_F(ScramExchangesTest, AnUnknownCompIdIsRecognisedAmongManyKnownOnes) {
    // With several credentials held, a lookup that compared against the wrong map's end would find "something" for an
    // unknown comp id rather than nothing.
    for (int index = 0; index < 50; ++index) {
        credentials_["MEMBER_" + std::to_string(index)] = credentials_.at(member);
    }
    EXPECT_FALSE(request(1, "NOBODY").challenge);
    EXPECT_TRUE(request(2, "MEMBER_7").challenge);
}

TEST_F(ScramExchangesTest, TheRightPasswordIsGrantedWithTheServerSignatureTheGatewayExpects) {
    const ScramExchanges::RequestAnswer challenge = request(1);
    const GatewaySide gateway = gateway_answers(member, password, challenge);

    const ScramExchanges::ProofAnswer answer = exchanges_.on_proof(1, gateway.client_proof.data(), gateway.client_proof.size());
    ASSERT_TRUE(answer.exchange_found);
    EXPECT_EQ(answer.outcome, AuthenticationOutcome::Granted);
    EXPECT_EQ(answer.comp_id, member);
    EXPECT_EQ(answer.server_signature, gateway.expected_server_signature);
    EXPECT_EQ(exchanges_.exchanges_in_progress(), 0U) << "the exchange ends with its proof";
}

TEST_F(ScramExchangesTest, AWrongPasswordIsRefusedWithNoServerSignature) {
    const ScramExchanges::RequestAnswer challenge = request(1);
    const GatewaySide gateway = gateway_answers(member, "wrong password", challenge);

    const ScramExchanges::ProofAnswer answer = exchanges_.on_proof(1, gateway.client_proof.data(), gateway.client_proof.size());
    ASSERT_TRUE(answer.exchange_found);
    EXPECT_EQ(answer.outcome, AuthenticationOutcome::BadPassword);
    EXPECT_TRUE(answer.server_signature.empty());
    EXPECT_EQ(exchanges_.exchanges_in_progress(), 0U);
}

TEST_F(ScramExchangesTest, AProofOfTheWrongSizeIsABadPassword) {
    request(1);
    const std::vector<uint8_t> short_proof(31, 0);
    EXPECT_EQ(exchanges_.on_proof(1, short_proof.data(), short_proof.size()).outcome, AuthenticationOutcome::BadPassword);
    request(2);
    EXPECT_EQ(exchanges_.on_proof(2, nullptr, 0).outcome, AuthenticationOutcome::BadPassword);
}

TEST_F(ScramExchangesTest, AProofWithNoExchangeInProgressIsNotAnswered) {
    const std::vector<uint8_t> proof(ScramExchanges::sha256_size, 0);
    EXPECT_FALSE(exchanges_.on_proof(99, proof.data(), proof.size()).exchange_found);
}

TEST_F(ScramExchangesTest, AProofCannotBeUsedTwice) {
    const ScramExchanges::RequestAnswer challenge = request(1);
    const GatewaySide gateway = gateway_answers(member, password, challenge);
    ASSERT_EQ(exchanges_.on_proof(1, gateway.client_proof.data(), gateway.client_proof.size()).outcome, AuthenticationOutcome::Granted);
    EXPECT_FALSE(exchanges_.on_proof(1, gateway.client_proof.data(), gateway.client_proof.size()).exchange_found);
}

TEST_F(ScramExchangesTest, AProofForOneExchangeDoesNotPassAnother) {
    // The same member logs on twice at once. Each server nonce differs, so each proof is bound to its own exchange.
    const ScramExchanges::RequestAnswer first = request(1);
    const std::vector<uint8_t> other_suffix(16, 0x55);
    const ScramExchanges::RequestAnswer second = exchanges_.on_request(gateway_connection, 2, member, client_nonce, other_suffix);
    ASSERT_TRUE(first.challenge);
    ASSERT_TRUE(second.challenge);

    const GatewaySide for_first = gateway_answers(member, password, first);
    EXPECT_EQ(exchanges_.on_proof(2, for_first.client_proof.data(), for_first.client_proof.size()).outcome, AuthenticationOutcome::BadPassword);
    EXPECT_EQ(exchanges_.on_proof(1, for_first.client_proof.data(), for_first.client_proof.size()).outcome, AuthenticationOutcome::Granted);
}

TEST_F(ScramExchangesTest, LosingAConnectionEndsOnlyTheExchangesBegunOnIt) {
    request(1, member, gateway_connection);
    request(2, member, gateway_connection);
    request(3, member, other_gateway_connection);
    ASSERT_EQ(exchanges_.exchanges_in_progress(), 3U);

    exchanges_.forget_connection(gateway_connection);
    EXPECT_EQ(exchanges_.exchanges_in_progress(), 1U);
    const std::vector<uint8_t> proof(ScramExchanges::sha256_size, 0);
    EXPECT_FALSE(exchanges_.on_proof(1, proof.data(), proof.size()).exchange_found);
    EXPECT_TRUE(exchanges_.on_proof(3, proof.data(), proof.size()).exchange_found);
}

TEST_F(ScramExchangesTest, ACredentialChangedDuringAnExchangeDoesNotAffectIt) {
    const ScramExchanges::RequestAnswer challenge = request(1);
    const std::vector<uint8_t> new_salt(16, 0x77);
    credentials_[member] = scram_crypto::make_scram_credential("a new password", new_salt.data(), new_salt.size(), iterations);

    const GatewaySide gateway = gateway_answers(member, password, challenge);
    EXPECT_EQ(exchanges_.on_proof(1, gateway.client_proof.data(), gateway.client_proof.size()).outcome, AuthenticationOutcome::Granted);
}

TEST_F(ScramExchangesTest, ACredentialAddedWhileRunningIsUsedForTheNextRequest) {
    const std::vector<uint8_t> salt(16, 0x33);
    credentials_["NEWCOMER"] = scram_crypto::make_scram_credential("newcomer's password", salt.data(), salt.size(), iterations);
    const ScramExchanges::RequestAnswer challenge = request(1, "NEWCOMER");
    ASSERT_TRUE(challenge.challenge);
    const GatewaySide gateway = gateway_answers("NEWCOMER", "newcomer's password", challenge);
    EXPECT_EQ(exchanges_.on_proof(1, gateway.client_proof.data(), gateway.client_proof.size()).outcome, AuthenticationOutcome::Granted);
}

TEST_F(ScramExchangesTest, AGrantCarriesTheMembersSessionPolicy) {
    ScramExchanges::SessionPolicy policy;
    policy.cancel_on_disconnect_enabled = false;
    policy.cancel_on_disconnect_grace_period_seconds = 45;
    policy.primary_gateway_instance = 2;
    policy.backup_gateway_instance = 1;
    policy.max_place_per_second = 100;
    policy.max_amend_per_second = 50;
    policy.max_cancel_per_second = 200;
    session_policies_[member] = policy;

    const ScramExchanges::RequestAnswer challenge = request(7);
    const GatewaySide gateway = gateway_answers(member, password, challenge);
    const ScramExchanges::ProofAnswer answer = exchanges_.on_proof(7, gateway.client_proof.data(), gateway.client_proof.size());
    ASSERT_NE(answer.session_policy, nullptr);

    const pubsub_itc_fw_app::AuthenticationResult result = ScramExchanges::result_message(7, answer);
    EXPECT_EQ(result.request_id, 7);
    EXPECT_EQ(result.outcome, AuthenticationOutcome::Granted);
    ASSERT_EQ(result.server_signature.size, gateway.expected_server_signature.size());
    EXPECT_EQ(std::vector<uint8_t>(result.server_signature.data, result.server_signature.data + result.server_signature.size),
              gateway.expected_server_signature);
    EXPECT_TRUE(result.has_cancel_on_disconnect_enabled);
    EXPECT_FALSE(result.cancel_on_disconnect_enabled);
    EXPECT_TRUE(result.has_cancel_on_disconnect_grace_period_seconds);
    EXPECT_EQ(result.cancel_on_disconnect_grace_period_seconds, 45);
    EXPECT_TRUE(result.has_primary_gateway_instance);
    EXPECT_EQ(result.primary_gateway_instance, 2);
    EXPECT_TRUE(result.has_backup_gateway_instance);
    EXPECT_EQ(result.backup_gateway_instance, 1);
    EXPECT_EQ(result.max_place_per_second, 100);
    EXPECT_EQ(result.max_amend_per_second, 50);
    EXPECT_EQ(result.max_cancel_per_second, 200);
}

TEST_F(ScramExchangesTest, APolicyFieldTheMemberLeftUnsetIsNotSent) {
    ScramExchanges::SessionPolicy policy;
    policy.max_place_per_second = 10;
    session_policies_[member] = policy;

    const ScramExchanges::RequestAnswer challenge = request(1);
    const GatewaySide gateway = gateway_answers(member, password, challenge);
    const ScramExchanges::ProofAnswer answer = exchanges_.on_proof(1, gateway.client_proof.data(), gateway.client_proof.size());
    const pubsub_itc_fw_app::AuthenticationResult result = ScramExchanges::result_message(1, answer);
    EXPECT_FALSE(result.has_cancel_on_disconnect_enabled);
    EXPECT_FALSE(result.has_cancel_on_disconnect_grace_period_seconds);
    EXPECT_FALSE(result.has_primary_gateway_instance);
    EXPECT_FALSE(result.has_backup_gateway_instance);
    EXPECT_EQ(result.max_place_per_second, 10);
}

TEST_F(ScramExchangesTest, AGrantForAMemberWithNoPolicySendsNoLimitsAndNoOptionalFields) {
    const ScramExchanges::RequestAnswer challenge = request(1);
    const GatewaySide gateway = gateway_answers(member, password, challenge);
    const ScramExchanges::ProofAnswer answer = exchanges_.on_proof(1, gateway.client_proof.data(), gateway.client_proof.size());
    EXPECT_EQ(answer.session_policy, nullptr);

    const pubsub_itc_fw_app::AuthenticationResult result = ScramExchanges::result_message(1, answer);
    EXPECT_EQ(result.outcome, AuthenticationOutcome::Granted);
    EXPECT_FALSE(result.has_cancel_on_disconnect_enabled);
    EXPECT_FALSE(result.has_primary_gateway_instance);
    EXPECT_EQ(result.max_place_per_second, 0);
    EXPECT_EQ(result.max_amend_per_second, 0);
    EXPECT_EQ(result.max_cancel_per_second, 0);
}

TEST_F(ScramExchangesTest, ARefusalCarriesNoSessionPolicyEvenWhenTheMemberHasOne) {
    ScramExchanges::SessionPolicy policy;
    policy.primary_gateway_instance = 2;
    policy.max_place_per_second = 100;
    session_policies_[member] = policy;

    const ScramExchanges::RequestAnswer challenge = request(1);
    const GatewaySide gateway = gateway_answers(member, "wrong password", challenge);
    const ScramExchanges::ProofAnswer answer = exchanges_.on_proof(1, gateway.client_proof.data(), gateway.client_proof.size());
    EXPECT_EQ(answer.session_policy, nullptr);

    const pubsub_itc_fw_app::AuthenticationResult result = ScramExchanges::result_message(1, answer);
    EXPECT_EQ(result.outcome, AuthenticationOutcome::BadPassword);
    EXPECT_EQ(result.server_signature.size, 0U);
    EXPECT_FALSE(result.has_primary_gateway_instance);
    EXPECT_EQ(result.max_place_per_second, 0);
}

} // namespaces
