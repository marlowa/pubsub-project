#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint> // IWYU pragma: keep
#include <string>
#include <string_view>

#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>

#include <leader_follower.hpp>

#include "TradingHaltState.hpp"
#include "VoidFromWal.hpp"

namespace sequencer {

/**
 * @brief What the sequencer thread supplies so that HaltAndVoidRequests can read the facts it decides from, write
 *        commands into the WAL, and answer the one who asked.
 */
class HaltAndVoidRequestsLinksInterface {
  public:
    virtual ~HaltAndVoidRequestsLinksInterface() = default;

    /// Whether this instance leads, and so writes commands.
    [[nodiscard]] virtual bool leading() const = 0;
    /// Whether orders are routed to the matching engine on @p connection.
    [[nodiscard]] virtual bool orders_routed_to(const pubsub_itc_fw::ConnectionID& connection) const = 0;
    /// Whether a matching engine takes orders now.
    [[nodiscard]] virtual bool an_engine_takes_orders() const = 0;
    /// Whether a matching engine other than instance @p instance_id has announced that it leads.
    [[nodiscard]] virtual bool another_engine_leads(int64_t instance_id) const = 0;
    /// Whether trading is halted, and by which TechnicalEvent, as the WAL says.
    [[nodiscard]] virtual const TradingHaltState& trading_halt() const = 0;
    /// The highest member's command a report in the WAL answers.
    [[nodiscard]] virtual int64_t highest_command_answered() const = 0;
    /// The last record released to the engines.
    [[nodiscard]] virtual int64_t released_through() const = 0;
    /// The last record in the WAL.
    [[nodiscard]] virtual int64_t last_seq_no() const = 0;
    /// The number the next record written will have.
    [[nodiscard]] virtual int64_t next_seq_no() const = 0;
    /// The trading day at wall time @p now_ns.
    [[nodiscard]] virtual int32_t trading_day(int64_t now_ns) = 0;
    /// Whether a void in the WAL already names command @p seq_no.
    [[nodiscard]] virtual bool voided_already(int64_t seq_no) const = 0;
    /// What the WAL holds at @p seq_no; @p found is false if it holds no such record.
    [[nodiscard]] virtual CommandInRecord command_in_wal_at(int64_t seq_no, bool& found) = 0;

    /// Writes @p event into the WAL as a command of the sequencer's own. @return false if it could not be encoded.
    virtual bool write_technical_event(const pubsub_itc_fw_app::TechnicalEvent& event) = 0;
    /// Writes @p command into the WAL as a command of the sequencer's own. @return false if it could not be encoded.
    virtual bool write_void_commands(const pubsub_itc_fw_app::VoidCommands& command) = 0;
    /// Writes @p command into the WAL as a command of the sequencer's own. @return false if it could not be encoded.
    virtual bool write_cancel_book(const pubsub_itc_fw_app::CancelBook& command) = 0;

    /// Sends the matching engine on @p connection the voids the WAL holds whose records are committed.
    virtual void send_voids(const pubsub_itc_fw::ConnectionID& connection) = 0;
    /// Sends VoidDeclined for command @p seq_no, with @p reason, on @p connection.
    virtual void decline_void(const pubsub_itc_fw::ConnectionID& connection, int64_t seq_no, pubsub_itc_fw_app::VoidDeclinedReason reason) = 0;
    /// Sends the operator on @p connection an OperatorHaltAnswer.
    virtual void answer_operator(const pubsub_itc_fw::ConnectionID& connection, pubsub_itc_fw_app::OperatorHaltOutcome outcome, int64_t technical_event_seq_no,
                                 const std::string& explanation) = 0;
};

/**
 * @brief The requests to halt trading or void commands, and the halt the leader writes when the venue cannot recover by
 *        itself (docs/venue/dividing_the_sequencer_and_matching_engine.md, section 6.14).
 *
 * Only the leader acts on any of them. Each is decided here from the facts the WAL holds, whether a matching engine
 * takes orders, and which engine orders are routed to; the commands decided on are written into the WAL by the thread,
 * as commands of the sequencer's own, and sent to the engines as orders are.
 *
 * | Request | Done |
 * |---------|------|
 * | an operator asks to halt | a TechnicalEvent that halts, reason DeclaredByOperator, cancelling nothing |
 * | an operator asks to lift the halt | a TechnicalEvent that does not halt, reason LiftedByOperator, only if an engine takes orders and the WAL holds a report
 * answering the last member's command before the halt; otherwise declined, saying why | | a matching engine that cannot account for the book asks to halt | a
 * TechnicalEvent that halts, with the reason the engine gave; only for the engine orders are routed to | | a matching engine that died applying a command asks
 * for it to be voided | if it is voided already, the voids are sent; otherwise VoidDeclined, saying why, if another engine has announced that it leads, if a
 * report in the WAL answers it or a later command, if the WAL does not hold it, or if it is not a member's new order or cancel; otherwise a VoidCommands naming
 * it | | no engine could be trading within the longest recovery the venue does by itself | a VoidCommands naming every new order held, a CancelBook, and a
 * TechnicalEvent that halts, in that order |
 *
 * Every operator request is answered, and the request and the answer are logged at Warning with the operator's name.
 *
 * Times on the wall clock are nanoseconds since the Unix epoch. One thread, the sequencer's, calls every member
 * function.
 */
class HaltAndVoidRequests {
  public:
    HaltAndVoidRequests(HaltAndVoidRequestsLinksInterface& links, pubsub_itc_fw::QuillLogger& logger);

    /// Operator @p operator_name, on @p connection, asks for trading to be halted if @p halt, or no longer halted.
    void operator_request(const pubsub_itc_fw::ConnectionID& connection, std::string_view operator_name, bool halt, int64_t now_ns);

    /// Matching engine instance @p instance_id, at @p epoch, on @p connection, cannot account for the orders the venue
    /// holds, and asks for trading to be halted for @p reason. It asks again every second until it has applied a halt.
    void engine_halt_request(const pubsub_itc_fw::ConnectionID& connection, int64_t instance_id, int32_t epoch, pubsub_itc_fw_app::TechnicalEventReason reason,
                             int64_t now_ns);

    /// Matching engine instance @p instance_id, on @p connection, died applying command @p seq_no and asks for it to be
    /// voided. It asks again every second until it is answered.
    void void_request(const pubsub_itc_fw::ConnectionID& connection, int64_t instance_id, int64_t seq_no);

    /**
     * @brief Halts trading because no engine could be trading within the longest recovery the venue does by itself
     *        (docs/availability/voided_commands.md, section 5.3, R-0117).
     *
     * Writes a VoidCommands naming every new order held, so that the engine that next leads rejects each instead of
     * applying it; a CancelBook that cancels every order on the book, each reported to its member; and a TechnicalEvent
     * that halts trading. They are held like the orders before them, and sent to the engine that next leads, in order.
     */
    void recovery_halt(int64_t now_ns);

  private:
    void answer(const pubsub_itc_fw::ConnectionID& connection, std::string_view operator_name, pubsub_itc_fw_app::OperatorHaltOutcome outcome,
                int64_t technical_event_seq_no, const std::string& explanation);
    void decline(const pubsub_itc_fw::ConnectionID& connection, int64_t instance_id, int64_t seq_no, pubsub_itc_fw_app::VoidDeclinedReason reason,
                 const char* why);
    void void_held_orders_at_the_halt();
    // Whether a TechnicalEvent halts trading or lifts the halt.
    enum class Trading { halts, lifts };
    // A TechnicalEvent in the open phase, the only phase the venue has until the start and the end of the trading day are
    // built.
    [[nodiscard]] pubsub_itc_fw_app::TechnicalEvent technical_event(Trading trading, pubsub_itc_fw_app::TechnicalEventReason reason, int64_t now_ns);

    HaltAndVoidRequestsLinksInterface& links_;
    pubsub_itc_fw::QuillLogger& logger_;
};

} // namespaces
