#pragma once

// C++ headers whose names start with ‘c’
// (None directly here)

// System C++ headers
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

// Project headers
#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/Message.hpp>
#include <pubsub_itc_fw/Reactor.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>

namespace pubsub_itc_fw {

// Forward declaration to break circular dependency with Reactor
class Reactor;

/**
 * @brief Handles fanout of messages to all interested subscribers.
 *
 * This class is the core of the framework's pubsub mechanism. It receives
 * messages from publishers and distributes them to all registered subscribers.
 * It is a single-threaded entity that processes messages from its own
 * dedicated message queue.
 */
class EventDistributor  : public ApplicationThread {
  public:
    /**
     * @brief Constructs an EventDistributor.
     * @param [in] reactor A reference to the Reactor instance.
     * @param [in] logger A reference to the logger instance.
     */
    EventDistributor(Reactor& reactor, LoggerInterface& logger);

    /**
     * @brief The main loop for the event distributor thread.
     */
    void run() override;

    /**
     * @brief Publishes a message to a topic.
     *
     * This method is called by publishers to send a message to the distributor's
     * queue for asynchronous processing.
     *
     * @param [in] topic The topic to publish to.
     * @param [in] message The message to publish.
     * @param [in] originating_thread_id The ID of the thread that published the message.
     */
    void publish(const std::string& topic, const Message& message, ThreadID originating_thread_id);

    /**
     * @brief Subscribes a thread to a topic.
     *
     * This method is called by a client to register its thread ID as a subscriber
     * for a given topic.
     *
     * @param [in] topic The topic to subscribe to.
     * @param [in] subscriber_id The ID of the subscribing thread.
     */
    void subscribe(const std::string& topic, ThreadID subscriber_id);

    /**
     * @brief Unsubscribes a thread from a topic.
     * @param [in] topic The topic to unsubscribe from.
     * @param [in] subscriber_id The ID of the unsubscribing thread.
     */
    void unsubscribe(const std::string& topic, ThreadID subscriber_id);

  private:
    /**
     * @brief Processes a message received via the internal queue.
     * @param [in] message The message to process.
     */
    void process_message(EventMessage& message);

    Reactor& reactor_;
    std::map<std::string, std::vector<ThreadID>> subscriptions_;
    std::mutex subscriptions_mutex_;
};

} // namespace pubsub_itc_fw
