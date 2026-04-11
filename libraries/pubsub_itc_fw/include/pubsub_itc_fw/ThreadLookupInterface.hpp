#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <pubsub_itc_fw/ThreadID.hpp>

namespace pubsub_itc_fw {

class ApplicationThread;

/**
 * @brief Interface for looking up a live ApplicationThread by its ThreadID.
 *
 * @ingroup reactor_subsystem
 *
 * This interface exposes the single capability that InboundConnectionManager
 * and OutboundConnectionManager require from the Reactor: the ability to
 * obtain a non-owning pointer to an ApplicationThread for event delivery.
 *
 * The Reactor implements this interface. The managers hold a reference to it
 * and call get_fast_path_thread() when they need to enqueue an EventMessage
 * (ConnectionEstablished, ConnectionLost, ConnectionFailed, FrameworkPdu, etc.)
 * to a target thread.
 *
 * Separating this capability into an interface means the managers have no
 * dependency on the Reactor class itself, keeping them independently testable.
 *
 * Threading model:
 *   get_fast_path_thread() is safe to call from the reactor thread without
 *   locking, provided the reactor lifecycle contract is observed: fast_path_threads_
 *   is written only during initialisation and shutdown, and read only while running.
 */
class ThreadLookupInterface {
public:
    /**
     * @brief Virtual destructor.
     */
    virtual ~ThreadLookupInterface() = default;

    /**
     * @brief Returns a non-owning pointer to the ApplicationThread with the
     *        given ThreadID, or nullptr if no such thread is registered.
     *
     * @param[in] id The ThreadID to look up.
     * @return A non-owning pointer to the thread, or nullptr if not found.
     */
    [[nodiscard]] virtual ApplicationThread* get_fast_path_thread(ThreadID id) const = 0;
};

} // namespace pubsub_itc_fw