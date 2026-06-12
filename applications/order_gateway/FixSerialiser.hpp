#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <string>

#include <pubsub_itc_fw/WallClock.hpp>
#include "FixMessage.hpp"

namespace order_gateway {

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
     * @brief Constructs a FixSerialiser with fixed SenderCompID, TargetCompID, and an injectable clock.
     *
     * @param[in] sender_comp_id SenderCompID (tag 49) for all outbound messages.
     * @param[in] target_comp_id TargetCompID (tag 56) for all outbound messages.
     * @param[in] wall_clock     Clock used to generate SendingTime (tag 52). Must outlive this object.
     */
    FixSerialiser(std::string sender_comp_id, std::string target_comp_id, const pubsub_itc_fw::WallClock& wall_clock);

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

    /**
     * @brief Serialises with an explicit TargetCompID, overriding the one supplied at construction.
     *
     * Used when the target is known per-session (e.g. the client's SenderCompID from its Logon)
     * rather than the configured default.
     */
    [[nodiscard]] std::string serialise(const FixMessage& msg, int seq_num,
                                        const std::string& target_comp_id) const;

  private:
    /*
     * Appends a single tag=value<SOH> field to output.
     */
    static void append_field(std::string& output, int tag, const std::string& value);
    static void append_field(std::string& output, int tag, int value);

    /*
     * Computes the FIX checksum of input -- sum of all bytes modulo 256,
     * formatted as a zero-padded three-digit string.
     */
    static std::string compute_checksum(const std::string& input);

    /*
     * Returns the time from wall_clock_ formatted as YYYYMMDD-HH:MM:SS UTC.
     */
    [[nodiscard]] std::string current_utc_timestamp() const;

    std::string sender_comp_id_;
    std::string target_comp_id_;
    const pubsub_itc_fw::WallClock& wall_clock_;
};

} // namespaces
