#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <cstddef>
#include <cstdint> // IWYU pragma: keep
#include <optional>
#include <string>
#include <string_view>

#include "Ff1Cipher.hpp"

namespace matching_engine {

/**
 * @brief Makes the matching engine's OrderID and ExecID values from positions in the WAL, and turns them back.
 *
 * A report is named by two numbers: the sequence number in the WAL of the command that caused it, and
 * its place among that command's reports, 0 for the first. An order is named by the sequence number of
 * the new order that placed it. The two numbers are packed into 64 bits, 36 for the sequence number
 * and 28 for the place, scrambled with FF1 as 16 hexadecimal digits, and written after a letter that
 * says what they name: "O" for an OrderID and "E" for an ExecID. Every engine given the same secret
 * and the same commands therefore gives every report the same identifier, the identifiers are unique
 * for the day, and a member cannot read the venue's sequence numbers from them.
 *
 * The keys come from a secret held on both engine machines and the trading date the sequencer writes
 * on each command. A day key is HMAC-SHA256 of the date, written as yyyy-mm-dd, keyed with the
 * secret; the key for each kind of identifier is HMAC-SHA256 of its name, "OrderID" or "ExecID",
 * keyed with the day key. See docs/availability/deterministic_matching_engine.md, section 3.
 *
 * Not thread-safe, because the ciphers it holds are not.
 */
class ReportIdentifiers {
  public:
    /// How many bytes the secret has.
    static constexpr size_t secret_size = 32;
    using Secret = std::array<uint8_t, secret_size>;

    /// The sequence numbers an identifier can hold: from 0 to one below this.
    static constexpr int64_t sequence_number_limit = int64_t{1} << 36;
    /// The places an identifier can hold: from 0 to one below this.
    static constexpr int64_t place_limit = int64_t{1} << 28;

    /// How many characters an identifier has: a letter and 16 hexadecimal digits.
    static constexpr size_t identifier_length = 17;
    using Buffer = std::array<char, identifier_length>;

    /// What an identifier names.
    enum class Kind { order, report };

    /// What an identifier was made from.
    struct Position {
        Kind kind{Kind::report};
        int64_t sequence_number{0};
        int64_t place{0};
    };

    /// @param[in] secret The secret the keys are derived from.
    explicit ReportIdentifiers(const Secret& secret);

    /**
     * @brief Reads a secret from a file holding it as 64 hexadecimal digits, which may be followed by white space.
     * @param[in] path The file.
     * @throws std::runtime_error naming the file and what is wrong with it, if it cannot be read or does not hold that.
     */
    static Secret read_secret_file(const std::string& path);

    /**
     * @brief Makes the keys for a trading date, unless they are the ones in use already.
     * @param[in] trading_date The date as the number yyyymmdd. Zero, meaning a command that carried no date, is
     *                         accepted and gives keys of its own, so that every engine refuses such a command alike.
     */
    void use_trading_date(int32_t trading_date);

    /// The trading date whose keys are in use, or -1 before use_trading_date has been called.
    [[nodiscard]] int32_t trading_date() const {
        return trading_date_;
    }

    /// Whether a sequence number and a place can be held in an identifier.
    [[nodiscard]] static bool fits(int64_t sequence_number, int64_t place) {
        return sequence_number >= 0 && sequence_number < sequence_number_limit && place >= 0 && place < place_limit;
    }

    /**
     * @brief The OrderID of the order placed by the new order with this sequence number.
     * @param[out] buffer          Where it is written; the view returned points into it.
     * @param[in]  placed_at_seq_no The sequence number of the new order that placed the order.
     * @throws PreconditionAssertion if no trading date is in use, or if the number does not fit.
     */
    std::string_view order_id(Buffer& buffer, int64_t placed_at_seq_no);

    /**
     * @brief The ExecID of a report.
     * @param[out] buffer          Where it is written; the view returned points into it.
     * @param[in]  sequence_number The sequence number of the command that caused the report.
     * @param[in]  place           The report's place among that command's reports, 0 for the first.
     * @throws PreconditionAssertion if no trading date is in use, or if the numbers do not fit.
     */
    std::string_view exec_id(Buffer& buffer, int64_t sequence_number, int64_t place);

    /**
     * @brief What an identifier made with the keys in use was made from.
     * @param[in] identifier An OrderID or an ExecID.
     * @return The position, or nothing if it is not a letter O or E followed by 16 hexadecimal digits.
     * @throws PreconditionAssertion if no trading date is in use.
     */
    std::optional<Position> position_of(std::string_view identifier);

    /**
     * @brief A number that tells two secrets apart without showing either: the first 8 bytes of HMAC-SHA256 of the
     * text "fingerprint", keyed with the secret.
     *
     * The two engines exchange it when they connect, as one of the settings they must share.
     */
    [[nodiscard]] int64_t secret_fingerprint() const;

  private:
    std::string_view make(Buffer& buffer, char letter, Ff1Cipher& cipher, int64_t sequence_number, int64_t place);

    Secret secret_{};
    int32_t trading_date_{-1};
    std::optional<Ff1Cipher> order_cipher_;
    std::optional<Ff1Cipher> report_cipher_;
};

} // namespaces
