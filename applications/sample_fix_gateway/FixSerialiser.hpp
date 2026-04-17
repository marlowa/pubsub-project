#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <string>

#include "FixMessage.hpp"

namespace sample_fix_gateway {

/**
 * @brief Builds outbound FIX 5.0SP2 / FIXT 1.1 messages as raw byte strings.
 *
 * FixSerialiser takes a FixMessage containing the application fields and
 * produces a complete, wire-ready FIX message string with correct BeginString
 * (tag 8), BodyLength (tag 9), and Checksum (tag 10) fields prepended and
 * appended automatically.
 *
 * The caller is responsible for setting all required application fields on
 * the FixMessage before calling serialise(). The session-level fields
 * (tag 8, 9, 34, 49, 52, 56, 10) are managed by this class and must not
 * be set by the caller.
 *
 * Field delimiter:
 *   FIX fields are separated by SOH (ASCII 0x01).
 *
 * Usage:
 *   FixSerialiser ser("GATEWAY", "CLIENT");
 *
 *   FixMessage msg;
 *   msg.set(Tag::MsgType, MsgType::Logon);
 *   msg.set(Tag::EncryptMethod, 0);
 *   msg.set(Tag::HeartBtInt, 30);
 *   std::string wire = ser.serialise(msg, seq_num);
 */
class FixSerialiser {
public:
    /**
     * @brief Constructs a FixSerialiser with fixed SenderCompID and TargetCompID.
     *
     * @param[in] sender_comp_id SenderCompID (tag 49) for all outbound messages.
     * @param[in] target_comp_id TargetCompID (tag 56) for all outbound messages.
     */
    FixSerialiser(std::string sender_comp_id, std::string target_comp_id);

    /**
     * @brief Serialises a FixMessage to a complete wire-ready FIX byte string.
     *
     * Prepends BeginString (tag 8), BodyLength (tag 9), MsgType (tag 35),
     * SenderCompID (tag 49), TargetCompID (tag 56), MsgSeqNum (tag 34), and
     * SendingTime (tag 52). Appends Checksum (tag 10).
     *
     * The MsgType field must be set on msg before calling this method.
     *
     * @param[in] msg     The message containing application-layer fields.
     * @param[in] seq_num The outbound sequence number for this message.
     * @return A complete FIX message string ready for transmission.
     */
    [[nodiscard]] std::string serialise(const FixMessage& msg, int seq_num) const;

private:
    /*
     * Appends a single tag=value<SOH> field to buf.
     */
    static void append_field(std::string& buf, int tag, const std::string& value);
    static void append_field(std::string& buf, int tag, int value);

    /*
     * Computes the FIX checksum of buf -- sum of all bytes modulo 256,
     * formatted as a zero-padded three-digit string.
     */
    static std::string compute_checksum(const std::string& buf);

    /*
     * Returns the current UTC time formatted as YYYYMMDD-HH:MM:SS.
     */
    static std::string current_utc_timestamp();

    std::string sender_comp_id_;
    std::string target_comp_id_;
};

} // namespace sample_fix_gateway
