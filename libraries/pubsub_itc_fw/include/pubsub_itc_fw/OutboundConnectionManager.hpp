#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/ExpandableSlabAllocator.hpp>
#include <pubsub_itc_fw/OutboundConnection.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/ReactorConfiguration.hpp>
#include <pubsub_itc_fw/ReactorControlCommand.hpp>
#include <pubsub_itc_fw/ServiceRegistry.hpp>
#include <pubsub_itc_fw/ThreadLookupInterface.hpp>

namespace pubsub_itc_fw {

/**
 * @brief Manages all outbound TCP connections on behalf of the Reactor.
 *
 * @ingroup reactor_subsystem
 *
 * This class owns the outbound connection maps and all logic for connecting,
 * reading, writing, timing out, and tearing down outbound connections. It is
 * extracted from the Reactor to keep that class focused on orchestration.
 *
 * The Reactor constructs this manager and delegates outbound epoll events to
 * it. The manager calls back into the Reactor only via ThreadLookupInterface
 * to deliver EventMessages to ApplicationThreads.
 *
 * ConnectionID allocation:
 *   ConnectionIDs are pre-allocated by the Reactor (which maintains the shared
 *   monotonic counter) and passed into process_connect_command() as a parameter,
 *   keeping the shared ID space unified without coupling this manager to the
 *   Reactor.
 *
 * Threading model:
 *   All methods must be called from the reactor thread only.
 *
 * Ownership:
 *   Does not own the epoll file descriptor, ReactorConfiguration, QuillLogger,
 *   ExpandableSlabAllocator, ServiceRegistry, or ThreadLookupInterface. The
 *   Reactor is responsible for their lifetimes.
 */
class OutboundConnectionManager {
  public:
    ~OutboundConnectionManager() = default;

    OutboundConnectionManager(const OutboundConnectionManager&) = delete;
    OutboundConnectionManager& operator=(const OutboundConnectionManager&) = delete;

    /**
     * @brief Constructs an OutboundConnectionManager.
     *
     * @param[in] epoll_fd          The reactor's epoll file descriptor.
     * @param[in] config            Reactor configuration (connect timeout etc.).
     *                              Must outlive this object.
     * @param[in] inbound_allocator Slab allocator for inbound PDU payloads on
     *                              established connections. Must outlive this object.
     * @param[in] service_registry  Registry for resolving service names to endpoints.
     *                              Must outlive this object.
     * @param[in] thread_lookup     Interface for delivering events to threads.
     *                              Must outlive this object.
     * @param[in] logger            Logger instance. Must outlive this object.
     */
    OutboundConnectionManager(int epoll_fd, const ReactorConfiguration& config, ExpandableSlabAllocator& inbound_allocator,
                              const ServiceRegistry& service_registry, ThreadLookupInterface& thread_lookup, QuillLogger& logger);

    /**
     * @brief Initiates an outbound TCP connection.
     *
     * Resolves the service name, starts a non-blocking connect to the primary
     * endpoint, and registers the socket with epoll for EPOLLOUT. The
     * ConnectionID must be pre-allocated by the Reactor.
     *
     * @param[in] command The Connect command carrying the service name and
     *                    requesting thread ID.
     * @param[in] id      ConnectionID pre-allocated by the Reactor.
     */
    void process_connect_command(const ReactorControlCommand& command, ConnectionID id);

    /**
     * @brief Called when epoll signals EPOLLOUT on a connecting socket.
     *
     * Calls finish_connect(). On success transitions to established phase and
     * delivers ConnectionEstablished. On failure retries the secondary endpoint
     * or delivers ConnectionFailed.
     *
     * @param[in] conn The outbound connection whose connect completed.
     */
    void on_connect_ready(OutboundConnection& conn);

    /**
     * @brief Called when epoll signals EPOLLIN on an established outbound connection.
     *
     * Delegates to PduParser::receive(). On error or peer disconnect tears down
     * the connection and delivers ConnectionLost.
     *
     * @param[in] conn The outbound connection to service.
     */
    void on_data_ready(OutboundConnection& conn);

    /**
     * @brief Called when epoll signals EPOLLOUT on an established outbound
     *        connection with a partial send in flight.
     *
     * Calls PduFramer::continue_send(). On completion deallocates the slab
     * chunk, clears pending send state, and deregisters EPOLLOUT.
     *
     * @param[in] conn The outbound connection to service.
     */
    void on_write_ready(OutboundConnection& conn);

    /**
     * @brief Attempts to dispatch a SendPdu command to an outbound connection.
     *
     * @param[in] command The SendPdu command to process.
     * @return true if the ConnectionID belongs to an outbound connection
     *         (command was processed or stashed), false if not found here.
     */
    [[nodiscard]] bool process_send_pdu_command(const ReactorControlCommand& command);

    /**
     * @brief Attempts to dispatch a SendRaw command to an outbound connection.
     *
     * OutboundConnection uses PduProtocolHandler and does not support raw-bytes
     * connections in the current implementation. This method returns false for
     * all connection IDs so the Reactor can try the inbound manager.
     *
     * @param[in] command The SendRaw command to process.
     * @return false always, since outbound connections do not support SendRaw.
     */
    [[nodiscard]] bool process_send_raw_command(const ReactorControlCommand& command);

    /**
     * @brief Advances the inbound MirroredBuffer tail for a RawBytesProtocolHandler
     *        connection by the number of bytes the application has consumed.
     *
     * Called by the Reactor in response to a CommitRawBytes command. Looks up
     * the connection by ID and forwards the call to
     * ProtocolHandlerInterface::commit_bytes(). For PduProtocolHandler connections
     * this is a no-op. If the connection ID is not found, returns false so the
     * Reactor can log an appropriate warning.
     *
     * @param[in] id             The ConnectionID of the raw-bytes connection.
     * @param[in] bytes_consumed Number of bytes the application has finished processing.
     * @return true if the ConnectionID belongs to an outbound connection, false otherwise.
     */
    [[nodiscard]] bool process_commit_raw_bytes(ConnectionID id, int64_t bytes_consumed);

    /**
     * @brief Drains the pending_send_ slot if one is waiting.
     *
     * Called by the Reactor at the start of process_control_commands() before
     * draining the command queue.
     *
     * @return true if pending_send_ was empty or successfully processed,
     *         false if it is still blocked.
     */
    [[nodiscard]] bool drain_pending_send();

    /**
     * @brief Attempts to tear down an outbound connection by application request.
     *
     * @param[in] id The ConnectionID to disconnect.
     * @return true if the connection was found and torn down, false otherwise.
     */
    [[nodiscard]] bool process_disconnect_command(ConnectionID id);

    /**
     * @brief Checks all pending connect retries and re-issues any that are due.
     *
     * Called by the Reactor alongside check_for_timed_out_connections() on each
     * housekeeping sweep.
     *
     * @param[in] next_id_fn Callable returning the next ConnectionID to allocate.
     */
    void retry_failed_connections(const std::function<ConnectionID()>& next_id_fn);

    /**
     * @brief Checks all connecting outbound connections against connect_timeout
     *        and tears down any that have exceeded it, delivering ConnectionFailed.
     */
    void check_for_timed_out_connections();

    /**
     * @brief Returns a non-owning pointer to an outbound connection by fd,
     *        or nullptr if not found. Used by the Reactor's epoll dispatch.
     */
    [[nodiscard]] OutboundConnection* find_by_fd(int fd) const;

    /**
     * @brief Returns a non-owning pointer to an outbound connection by
     *        ConnectionID, or nullptr if not found.
     *
     * TEST SEAM: exists solely for unit tests that drive the manager directly
     * without a running reactor. Must not be used by application code.
     */
    [[nodiscard]] OutboundConnection* find_by_id(ConnectionID id) const;

    /**
     * @brief Tears down an outbound connection.
     *
     * Deregisters from epoll, frees any in-flight slab chunk, and optionally
     * delivers ConnectionLost to the requesting thread.
     *
     * @param[in] id                 ConnectionID of the connection to tear down.
     * @param[in] reason             Human-readable reason for logging and event delivery.
     * @param[in] deliver_lost_event If true, delivers ConnectionLost to the requesting thread.
     */
    void teardown_connection(ConnectionID id, const std::string& reason, bool deliver_lost_event);

  private:
    int epoll_fd_;
    const ReactorConfiguration& config_;
    ExpandableSlabAllocator& inbound_allocator_;
    const ServiceRegistry& service_registry_;
    ThreadLookupInterface& thread_lookup_;
    QuillLogger& logger_;

    std::unordered_map<ConnectionID, std::unique_ptr<OutboundConnection>> connections_;
    std::unordered_map<int, OutboundConnection*> connections_by_fd_;

    // Pending connect retries. Keyed by service name. Each entry holds the
    // original Connect command and the time after which the retry should fire.
    // This is a temporary workaround for the TCP rendezvous problem pending
    // WAL-based pub/sub. See ReactorConfiguration::connect_retry_interval_.
    struct PendingRetry {
        PendingRetry() : command(ReactorControlCommand::CommandTag::Connect) {}
        PendingRetry(ReactorControlCommand cmd, std::chrono::steady_clock::time_point when)
            : command(std::move(cmd)), retry_after(when) {}

        ReactorControlCommand command;
        std::chrono::steady_clock::time_point retry_after;
    };
    std::unordered_map<std::string, PendingRetry> pending_retries_;

    std::optional<ReactorControlCommand> pending_send_;
};

} // namespace pubsub_itc_fw
