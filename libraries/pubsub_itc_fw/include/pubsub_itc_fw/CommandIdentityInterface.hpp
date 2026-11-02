#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>
#include <string_view>

#include <pubsub_itc_fw/WalEntryContents.hpp>

namespace pubsub_itc_fw {

/**
 * @brief How the replicated WAL reads a command's identity from a record, so that it can tell whether
 *        the WAL already holds a command (docs/availability/one_check_against_sequencing_twice.md).
 *
 * A command is placed in the WAL at most once, judged by its identity (principle V7 in
 * docs/design_principles.md). What a command's identity is belongs to the application: for the venue it is
 * the member's comp id, the protocol the command arrived by, and its `ClOrdID`. The application gives it to
 * the WAL as bytes, and the WAL compares those bytes exactly. Two commands are the same command when, and
 * only when, their identities are the same bytes, so the application must write an identity in a way no two
 * different commands can share: where it is made of several values, each but the last with its length.
 *
 * The WAL calls identity_of() on the thread that owns it, for each record it writes, and on the thread that
 * builds its record of identifiers at startup, for each record that thread reads. The two calls can be made
 * at the same time, so an implementation must use nothing but its arguments.
 */
class CommandIdentityInterface {
  public:
    /// The largest identity the WAL accepts, in bytes. A longer one is treated as no identity.
    static constexpr size_t max_identity_size = 1024;

    /// The size of the scratch space the WAL gives identity_of() for decoding a record.
    static constexpr size_t scratch_size = 64 * 1024;

    virtual ~CommandIdentityInterface() = default;

    /**
     * @brief The identity of the command a WAL record holds.
     * @param[in]  entry    The record, as WalEntryContents describes it.
     * @param[in]  scratch  Space the implementation may use while it decodes the record; scratch_size bytes.
     * @param[out] identity Where the identity is written; max_identity_size bytes.
     * @return The number of bytes written to @p identity, or zero when the record is not a command that can
     *         arrive twice, or does not say whose it is.
     */
    [[nodiscard]] virtual size_t identity_of(const WalEntryContents& entry, uint8_t* scratch, char* identity) const = 0;
};

} // namespaces
