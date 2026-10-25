// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file IdentifierToPositionMain.cpp
 * @brief identifier_to_position: the position in the WAL that a matching engine's OrderID or ExecID was made from.
 *
 *   identifier_to_position --secret-file <file> --date <yyyy-mm-dd> <identifier>...
 *
 * The matching engine makes every OrderID and ExecID from a position in the sequencer's WAL: the sequence number of a
 * command and, for a report, its place among that command's reports, scrambled with keys derived from the venue's
 * secret and the trading date (docs/availability/deterministic_matching_engine.md, section 3). Given the secret and
 * the date, this program turns each identifier back, so that support staff can find the command behind a report a
 * member quotes. It reads the secret from the file the engines read, so it needs the same access to it.
 *
 * For each identifier it prints one line: what it names, the sequence number, and for an ExecID the place. It exits
 * 0 when every identifier was turned back, 1 when any was not an OrderID or an ExecID, and 2 when it could not start.
 */

#include <cstdio>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include "ReportIdentifiers.hpp"

namespace {

int usage() {
    fmt::print(stderr, "usage: identifier_to_position --secret-file <file> --date <yyyy-mm-dd> <identifier>...\n");
    return 2;
}

// The date written yyyy-mm-dd, as the number yyyymmdd, or nothing if it is not written so.
std::optional<int32_t> trading_date_of_text(std::string_view text) {
    if (text.size() != 10 || text[4] != '-' || text[7] != '-') {
        return std::nullopt;
    }
    int32_t value = 0;
    for (size_t index = 0; index < text.size(); ++index) {
        if (index == 4 || index == 7) {
            continue;
        }
        if (text[index] < '0' || text[index] > '9') {
            return std::nullopt;
        }
        value = value * 10 + (text[index] - '0');
    }
    return value;
}

} // un-named namespace

int main(int argc, char** argv) {
    std::string secret_file;
    std::optional<int32_t> trading_date;
    std::vector<std::string> identifiers_given;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--secret-file" && index + 1 < argc) {
            secret_file = argv[++index];
        } else if (argument == "--date" && index + 1 < argc) {
            trading_date = trading_date_of_text(argv[++index]);
            if (!trading_date.has_value()) {
                fmt::print(stderr, "identifier_to_position: the date must be written yyyy-mm-dd\n");
                return 2;
            }
        } else {
            identifiers_given.emplace_back(argument);
        }
    }
    if (secret_file.empty() || !trading_date.has_value() || identifiers_given.empty()) {
        return usage();
    }

    std::optional<matching_engine::ReportIdentifiers> identifiers;
    try {
        identifiers.emplace(matching_engine::ReportIdentifiers::read_secret_file(secret_file));
    } catch (const std::runtime_error& error) {
        fmt::print(stderr, "identifier_to_position: {}\n", error.what());
        return 2;
    }
    identifiers->use_trading_date(*trading_date);

    int exit_status = 0;
    for (const std::string& identifier : identifiers_given) {
        const auto position = identifiers->position_of(identifier);
        if (!position.has_value()) {
            fmt::print("{}: not an OrderID or an ExecID of the matching engine (a letter O or E and 16 hexadecimal digits)\n", identifier);
            exit_status = 1;
            continue;
        }
        if (position->kind == matching_engine::ReportIdentifiers::Kind::order) {
            fmt::print("{}: the order placed by the new order with sequence number {}\n", identifier, position->sequence_number);
        } else {
            fmt::print("{}: report {} of the command with sequence number {}, counting from 0\n", identifier, position->place, position->sequence_number);
        }
    }
    return exit_status;
}
