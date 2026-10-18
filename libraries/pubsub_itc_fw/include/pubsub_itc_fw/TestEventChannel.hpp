#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/EventMessage.hpp>

#include <test_events.hpp>

namespace pubsub_itc_fw {

/// How a TestEventChannel sends a message. Implemented by the thread that owns the channel's connection.
class TestEventLinksInterface {
  public:
    virtual ~TestEventLinksInterface() = default;

    /// Send @p size bytes at @p bytes, an encoded message of type @p pdu_id, on @p connection.
    virtual void send_test_event(const ConnectionID& connection, int16_t pdu_id, const uint8_t* bytes, size_t size) = 0;
};

/**
 * @brief Sends one thread's test events to the test that started the component, when there is one.
 *
 * The design is docs/availability/test_events.md. A component whose configuration has a [test_events] section
 * registers a service of that name (service_name) and has the thread that owns this channel connect to it. When the
 * connection is made the channel says which component, instance and thread it speaks for (TestEventsHello), then
 * sends each event the thread reports, and answers the test's checkpoint requests on the same connection. Because a
 * connection delivers in order and the thread answers a request only once it has dealt with everything before it,
 * every event the thread sent before the answer has arrived by the time the answer does.
 *
 * With no connection -- no [test_events] section, or the test has gone -- each event costs one test of a flag and
 * sends nothing. The venue never waits for the test: sends are queued by the framework, which closes a connection
 * whose reader falls far behind, and the channel then sends nothing more.
 *
 * Each event is encoded into a buffer the channel owns, so sending one allocates nothing from the heap here.
 *
 * **Threading.** Everything here runs on the thread that owns the channel.
 */
class TestEventChannel {
  public:
    /// The name of the service a component registers for the test's listener.
    static constexpr std::string_view service_name{"test_events"};

    /**
     * @param[in] links     How messages are sent.
     * @param[in] component The component's name, such as "sequencer_primary".
     * @param[in] instance  The component's instance id.
     * @param[in] thread    The name of the thread that owns this channel.
     */
    TestEventChannel(TestEventLinksInterface& links, std::string component, int64_t instance, std::string thread);

    /**
     * @brief Claims @p connection if it is the connection to the test, and introduces the thread on it; otherwise
     *        reports it to the test as made (ConnectionChanged), when the connection to the test is up.
     *
     * The owner passes every connection it is told of here first, so that each one made after the connection to the
     * test is reported.
     *
     * @return Whether it was the connection to the test, so that the owner need not look at it further.
     */
    bool on_connection_established(const ConnectionID& connection);

    /**
     * @brief Notes that @p connection has gone, if it was the connection to the test, after which nothing more is
     *        sent; otherwise reports it to the test as lost (ConnectionChanged), when the connection to the test is up.
     * @return Whether it was the connection to the test. The owner says so in its Quill log: the test has failed
     *         by now, and the component carries on.
     */
    bool on_connection_lost(const ConnectionID& connection);

    /**
     * @brief Answers a checkpoint request that arrived on the connection to the test.
     * @return Whether @p message came on the connection to the test, so that the owner need not look at it further.
     */
    bool on_framework_pdu_message(const EventMessage& message);

    /// Whether the connection to the test is up.
    [[nodiscard]] bool connected() const {
        return connected_;
    }

    /**
     * @brief An instance of a pair changed its role.
     *
     * @param[in] owner    The pair's owner, as the library's configuration names it.
     * @param[in] instance The instance's id.
     * @param[in] from     The role it had.
     * @param[in] to       The role it has now.
     * @param[in] epoch    The epoch of the leadership it now has or follows.
     */
    void role_changed(std::string_view owner, int64_t instance, LeadershipRole from, LeadershipRole to, int32_t epoch);

    /**
     * @brief Something happened to an instance's lease.
     *
     * @param[in] owner      The lease agent's owner, as it names itself.
     * @param[in] instance   The instance's id.
     * @param[in] change     What happened.
     * @param[in] granted_by For a grant, the voter whose grant made the majority; otherwise Voter::NoVoter.
     * @param[in] epoch      The epoch it asked for, leads at, or stopped leading at.
     */
    void lease_changed(std::string_view owner, int64_t instance, LeaseChange change, Voter granted_by, int32_t epoch);

    /**
     * @brief This third voter granted an instance of a pair a lease.
     *
     * @param[in] pair_id  The pair.
     * @param[in] instance The instance granted the lease.
     * @param[in] epoch    The epoch of the lease.
     */
    void lease_granted_to(int32_t pair_id, int64_t instance, int32_t epoch);

    /**
     * @brief Sends an event an application defines in a DSL file of its own, for the venue's ideas the library does
     *        not name.
     *
     * The application's message ids must lie in the range set aside for test events (140 to 199) and must not be one
     * the library's `test_events.dsl` uses, so that the test can decode every message on the connection. Nothing is
     * sent while there is no connection to the test.
     *
     * @param[in] pdu_id  The message's id, as its DSL file gives it.
     * @param[in] message The message, which the generated `encode` for its type writes into the channel's buffer.
     */
    template <typename Message> void send_event(int16_t pdu_id, const Message& message) {
        if (connected_) {
            send(pdu_id, message);
        }
    }

  private:
    template <typename Message> void send(int16_t pdu_id, const Message& message) {
        size_t written = 0;
        size_t needed = 0;
        // An event too large for the buffer is not sent, and the test, waiting for it, fails; no event comes close.
        if (!encode(message, buffer_.data(), buffer_.size(), written, needed)) {
            return;
        }
        links_.send_test_event(connection_, pdu_id, buffer_.data(), written);
    }
    enum class ConnectionChange { Made, Lost };
    void connection_changed(const ConnectionID& connection, ConnectionChange change);

    TestEventLinksInterface& links_;
    std::string component_;
    int64_t instance_{0};
    std::string thread_;
    ConnectionID connection_{};
    bool connected_{false};
    std::array<uint8_t, 1024> buffer_{};
};

} // namespaces
