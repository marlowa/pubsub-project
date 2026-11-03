#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/PacedResend.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/UnansweredCommandStore.hpp>

namespace pubsub_itc_fw {

/**
 * @brief What a client of a replicated service supplies so that UnansweredCommands can send its commands again.
 *
 * The library keeps each command as the bytes the client gave it, and does not read them. Sending one again is the
 * client's: it decodes the copy, marks it as sent again, so that the leader can tell it from a member reusing an
 * identifier, and sends it.
 */
class UnansweredCommandLinksInterface {
  public:
    virtual ~UnansweredCommandLinksInterface() = default;

    /**
     * @brief Sends again, on @p connection, the command whose copy is @p kept, marked as sent again.
     *
     * @param[in] connection The connection to send it on.
     * @param[in] kept       The copy, exactly as it was given to UnansweredCommands::keep(). Valid only during the call.
     * @param[in] size       The copy's size in bytes.
     */
    virtual void send_again(const ConnectionID& connection, const uint8_t* kept, size_t size) = 0;

    /// Asks to be told, by a call of UnansweredCommands::on_connection_writable(), when @p connection has written
    /// everything waiting on it.
    virtual void request_writable_notification(const ConnectionID& connection) = 0;
};

/**
 * @brief The client's side of a replicated service: every command a client sends is kept until it is answered, and
 *        sent again when a new leader announces itself or a connection to an instance is established again.
 *
 * A client, such as an order gateway, sends each command to every instance of the service, and only the leader acts on
 * it. A command can be lost when the leader changes: sent while no instance was leading, or held by the old leader
 * alone. And the commands waiting to be sent on a connection are discarded when the connection is closed. So the
 * client keeps a copy of each command until it learns the answer, and sends every copy it still holds again
 * (docs/availability/commands_during_a_change_of_leader.md, sections 3.1 and 3.2; principle V6 in
 * docs/design_principles.md). Sending again is safe because the leader never sequences a command twice (V7).
 *
 * **What the client supplies.** It tells this object of each command it sends (keep()), of each answer it recognises
 * (answered()), of a new leader (on_new_leader()), and of each connection to an instance (on_connection_established(),
 * on_connection_writable()). It sends each copy again itself, through UnansweredCommandLinksInterface. When the store is
 * full, keep() says so, and the client tells its own user that the command is refused.
 *
 * **Sending again is paced.** There can be hundreds of thousands of copies. Sent all at once they would queue on the
 * connection faster than the instance reads them, and a connection whose queue of waiting sends reaches its limit is
 * closed. So the copies are taken at one moment (UnansweredSnapshot) and sent PacedResend::batch_size at a time on each
 * connection, each batch after the connection has said that everything before it is written. A command answered after
 * the copy was taken is still sent again; the leader finds it in its WAL and does not sequence it twice.
 *
 * **The instances are destinations**, named when this object is created, such as the primary and the secondary
 * sequencer, and referred to afterwards by their place in that list.
 *
 * One thread, the client's, calls every member function.
 */
class UnansweredCommands {
  public:
    using Kept = UnansweredCommandStore::Kept;

    /**
     * @param[in] capacity_bytes    The size of the block the copies are written into.
     * @param[in] capacity_commands The most copies held at once.
     * @param[in] destination_names The instances the commands are sent to, each named for the Quill log, for example
     *                              "primary sequencer".
     * @param[in] links             How a copy is sent again.
     * @param[in] logger            Where what happens is logged.
     * @param[in] owner_name        The name each log line starts with, for example the client's thread.
     */
    UnansweredCommands(size_t capacity_bytes, size_t capacity_commands, std::vector<std::string> destination_names, UnansweredCommandLinksInterface& links,
                       QuillLogger& logger, std::string owner_name);

    /**
     * @brief Keeps a copy of a command about to be sent, until it is answered.
     *
     * @param[in] comp_id   The member's comp id.
     * @param[in] cl_ord_id The command's ClOrdID.
     * @param[in] envelope  The command as it is sent, which is what is sent again.
     * @param[in] size      Its size in bytes.
     * @return Kept::kept when the copy is kept; Kept::full when there is no room, and the client refuses the command;
     *         Kept::already_held when a command with the same comp id and ClOrdID is waiting for its answer, and the
     *         client refuses it, because an answer to it could not be told from an answer to the first.
     */
    Kept keep(std::string_view comp_id, std::string_view cl_ord_id, const uint8_t* envelope, size_t size);

    /// The command (@p comp_id, @p cl_ord_id) is answered, so it is not sent again. Returns whether a copy was held.
    bool answered(std::string_view comp_id, std::string_view cl_ord_id);

    /// Forgets every unanswered copy of the member @p comp_id, whose session has ended for good. Returns how many.
    size_t drop_session(std::string_view comp_id);

    /// How many commands are waiting for their answers.
    [[nodiscard]] size_t unanswered() const {
        return store_.unanswered();
    }

    /// Whether the command (@p comp_id, @p cl_ord_id) is waiting for its answer.
    [[nodiscard]] bool holds(std::string_view comp_id, std::string_view cl_ord_id) const {
        return store_.holds(comp_id, cl_ord_id);
    }

    /**
     * @brief A new leader has announced itself, with leadership epoch @p epoch: every copy still held is sent again
     *        to every destination connected.
     *
     * @param[in] epoch             The new leader's epoch, for the Quill log.
     * @param[in] first_leader_seen Whether this is the first leader the client has heard of. The client accepts
     *                              commands before any leader has announced itself, and an instance that does not yet
     *                              lead discards them, so the copies are sent again for the first leader too, when
     *                              there are any.
     */
    void on_new_leader(int32_t epoch, bool first_leader_seen);

    /**
     * @brief The connection to destination @p destination is established, as @p connection.
     *
     * The first time, nothing is sent: the commands were sent on the connections that existed when they were kept.
     * After that, the connection replaces one that was closed, and the commands waiting on it were discarded with it,
     * so every copy still held is sent again on it.
     */
    void on_connection_established(size_t destination, const ConnectionID& connection);

    /**
     * @brief The connection to destination @p destination is lost: nothing more is sent on it, and any copies still
     *        to be sent on it are not sent.
     *
     * The commands waiting on the lost connection are discarded with it, so every copy still held is sent again when
     * on_connection_established() reports the connection established again.
     */
    void on_connection_lost(size_t destination);

    /**
     * @brief @p connection has written everything waiting on it: the next batch of copies goes, if one is due on it.
     * @return Whether @p connection is one this object is sending copies on.
     */
    bool on_connection_writable(const ConnectionID& connection);

    /// How many commands keep() has refused because the store was full, since this object was created.
    [[nodiscard]] int64_t refused_because_full() const {
        return refused_because_full_;
    }

  private:
    struct Destination {
        std::string name;
        ConnectionID connection{};
        bool connected_before{false};
        PacedResend resend;
    };

    // Takes a fresh copy of the commands still held and starts sending it, from the beginning, on every destination
    // @p wanted names, and on any whose sending of an earlier copy has not finished.
    void start_resends(const std::vector<bool>& wanted);
    // Sends the next batch on @p destination, and asks to be told when it is written, if more remain.
    void continue_resend(Destination& destination);

    UnansweredCommandStore store_;
    UnansweredSnapshot snapshot_;
    std::vector<Destination> destinations_;
    UnansweredCommandLinksInterface& links_;
    QuillLogger& logger_;
    std::string owner_name_;
    // Whether the store was full at the last command, so that it is reported when it fills and when it has room
    // again, not at every command refused.
    bool full_{false};
    int64_t refused_because_full_{0};
};

} // namespaces
