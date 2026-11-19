#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <pubsub_itc_fw/CommandIdentityInterface.hpp>
#include <pubsub_itc_fw/WalEntryContents.hpp>

namespace pubsub_itc_fw::tests {

/// The type the tests give a record holding a command.
constexpr int16_t test_command_pdu_id = 1001;
/// The type the tests give a record that is not a command.
constexpr int16_t test_other_pdu_id = 1002;
/// The type the tests give a record that answers a command: its bytes are the command's record number, 8 bytes.
constexpr int16_t test_answer_pdu_id = 1003;

/// The bytes of a test record answering the command in record @p command_seq_no.
inline std::vector<uint8_t> test_answer_bytes(int64_t command_seq_no) {
    std::vector<uint8_t> bytes(sizeof(command_seq_no));
    std::memcpy(bytes.data(), &command_seq_no, sizeof(command_seq_no));
    return bytes;
}

/// The bytes of a test record holding the command @p identity: "CMD:" followed by the identity.
inline std::vector<uint8_t> test_command_bytes(std::string_view identity) {
    const std::string text = "CMD:" + std::string(identity);
    return std::vector<uint8_t>(text.begin(), text.end());
}

/**
 * @brief The tests' way of reading a command's identity: a record of type test_command_pdu_id whose bytes are
 *        "CMD:" followed by the identity; and a record of type test_answer_pdu_id answers the command whose record
 *        number its bytes hold.
 */
class TestCommandIdentity : public CommandIdentityInterface {
  public:
    [[nodiscard]] RecordIdentity identity_of(const WalEntryContents& entry, uint8_t* /*scratch*/, char* identity) const override {
        constexpr std::string_view prefix = "CMD:";
        if (entry.pdu_id == test_answer_pdu_id && entry.size == sizeof(int64_t)) {
            int64_t answers = 0;
            std::memcpy(&answers, entry.bytes, sizeof(answers));
            return RecordIdentity{0, answers};
        }
        if (entry.pdu_id != test_command_pdu_id || entry.size <= prefix.size()) {
            return {};
        }
        if (std::memcmp(entry.bytes, prefix.data(), prefix.size()) != 0) {
            return {};
        }
        const size_t size = entry.size - prefix.size();
        if (size > max_identity_size) {
            return {};
        }
        std::memcpy(identity, entry.bytes + prefix.size(), size);
        return RecordIdentity{size, 0};
    }
};

} // namespaces
