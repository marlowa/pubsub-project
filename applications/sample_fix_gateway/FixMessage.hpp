#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <string>
#include <string_view>
#include <unordered_map>

namespace sample_fix_gateway {

/**
 * @brief A simple container for a single FIX message's tag/value pairs.
 *
 * FixMessage holds the fields of one complete FIX message as a map from
 * integer tag number to string value. It does not validate field presence
 * or types -- that is the responsibility of the code that consumes the message.
 *
 * This is intentionally minimal and scoped to the sample FIX gateway. It is
 * not a general-purpose FIX message implementation.
 *
 * FIX tag numbers referenced in this sample:
 *   8  -- BeginString (e.g. "FIXT.1.1")
 *   9  -- BodyLength
 *   10 -- Checksum
 *   11 -- ClOrdID
 *   14 -- CumQty
 *   17 -- ExecID
 *   34 -- MsgSeqNum
 *   35 -- MsgType
 *   37 -- OrderID
 *   38 -- OrderQty
 *   39 -- OrdStatus
 *   40 -- OrdType
 *   44 -- Price
 *   49 -- SenderCompID
 *   52 -- SendingTime
 *   54 -- Side
 *   55 -- Symbol
 *   56 -- TargetCompID
 *   58 -- Text
 *   98 -- EncryptMethod
 *   108 -- HeartBtInt
 *   150 -- ExecType
 *   151 -- LeavesQty
 */
class FixMessage {
  public:
    FixMessage() = default;

    /**
     * @brief Sets a field by tag number and string value.
     */
    void set(int tag, const std::string& value) {
        fields_[tag] = value;
    }

    /**
     * @brief Sets a field by tag number and integer value.
     */
    void set(int tag, int value) {
        fields_[tag] = std::to_string(value);
    }

    /**
     * @brief Returns the value for the given tag, or empty string if not present.
     */
    [[nodiscard]] const std::string& get(int tag) const {
        static const std::string empty;
        auto it = fields_.find(tag);
        return (it != fields_.end()) ? it->second : empty;
    }

    /**
     * @brief Returns true if the given tag is present in the message.
     */
    [[nodiscard]] bool has(int tag) const {
        return fields_.count(tag) != 0;
    }

    /**
     * @brief Returns the MsgType field (tag 35), or empty string if not present.
     */
    [[nodiscard]] const std::string& msg_type() const {
        return get(35);
    }

    /**
     * @brief Clears all fields. Used to reset the message for reuse.
     */
    void clear() {
        fields_.clear();
    }

    /**
     * @brief Returns the number of fields in the message.
     */
    [[nodiscard]] int size() const {
        return static_cast<int>(fields_.size());
    }

  private:
    std::unordered_map<int, std::string> fields_;
};

// Commonly used MsgType values
namespace MsgType {
static const std::string Logon = "A";
static const std::string Logout = "5";
static const std::string Heartbeat = "0";
static const std::string TestRequest = "1";
static const std::string ResendRequest = "2";
static const std::string Reject = "3";
static const std::string SequenceReset = "4";
static const std::string NewOrderSingle = "D";
static const std::string ExecutionReport = "8";
} // namespace MsgType

// Commonly used tag numbers
namespace Tag {
static constexpr int BeginString = 8;
static constexpr int BodyLength = 9;
static constexpr int MsgType = 35;
static constexpr int SenderCompID = 49;
static constexpr int TargetCompID = 56;
static constexpr int MsgSeqNum = 34;
static constexpr int SendingTime = 52;
static constexpr int EncryptMethod = 98;
static constexpr int HeartBtInt = 108;
static constexpr int Checksum = 10;
static constexpr int ClOrdID = 11;
static constexpr int OrderID = 37;
static constexpr int ExecID = 17;
static constexpr int ExecType = 150;
static constexpr int OrdStatus = 39;
static constexpr int Symbol = 55;
static constexpr int Side = 54;
static constexpr int OrderQty = 38;
static constexpr int Price = 44;
static constexpr int OrdType = 40;
static constexpr int CumQty = 14;
static constexpr int LeavesQty = 151;
static constexpr int Text = 58;
} // namespace Tag

/**
 * @brief The expected byte sequence at the start of every inbound FIX 5.0SP2
 *        / FIXT 1.1 message stream.
 *
 * Any inbound connection whose first bytes do not match this preamble is not
 * a valid FIX FIXT.1.1 client and should be disconnected immediately.
 *
 * The length is computed at compile time via string_view::size() -- no magic
 * numbers are needed in code that checks the preamble.
 */
static constexpr std::string_view expected_preamble = "8=FIXT.1.1\x01";

} // namespace sample_fix_gateway
