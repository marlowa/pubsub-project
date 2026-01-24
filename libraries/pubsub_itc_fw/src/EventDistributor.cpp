#if 0
// C++ headers whose names start with ‘c’
#include <cstdint>

// System C++ headers
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <stdexcept>
#include <utility>
#include <vector>

// Project headers
#include <pubsub_itc_fw/EventDistributor.hpp>
#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/Message.hpp>
#include <pubsub_itc_fw/Reactor.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>

namespace pubsub_itc_fw {

/**
 * @brief Constructs an EventDistributor.
 * @param [in] reactor A reference to the Reactor instance.
 * @param [in] logger A reference to the logger instance.
 *
 * This constructor initializes the base class `ApplicationThread` with the thread
 * name "EventDistributor" and a reference to the `Reactor` and `LoggerInterface`
 * instances.
 */
EventDistributor::EventDistributor(Reactor& reactor, LoggerInterface& logger)
    : ApplicationThread(logger, "EventDistributor", 1024, 2048, ThreadID{0}),
      reactor_(reactor) {
    // The base class constructor handles most of the initialization.
}

/**
 * @brief The main loop for the event distributor thread.
 *
 * This function is the entry point for the dedicated thread. It processes
 * messages from its internal queue until a termination event is received.
 */
void EventDistributor::run() {
    is_running_ = true;
    while (is_running_) {
        EventMessage message;
        // Attempt to dequeue a message without blocking.
        // A tight spin loop is used for illustration, but in a production
        // environment, this would be replaced with a blocking mechanism.
        if (get_queue().try_dequeue(message)) {
            process_message(message);
        }
    }

    // After the loop terminates, log the shutdown.
    PUBSUB_LOG_STR(get_logger(), LogLevel::Info, "EventDistributor thread is terminating.");
}

/**
 * @brief Publishes a message to a topic.
 * @param [in] topic The topic to publish to.
 * @param [in] message The message to publish.
 *
 * This method creates an `EventMessage` with a `Message` payload and enqueues
 * it to the `EventDistributor`'s internal queue for asynchronous processing.
 */
void EventDistributor::publish(const std::string& topic, const Message& message) {
    // TODO: Implement this to send the message to the distributor's queue.
    // The distributor will then fan it out to the subscribers.
}

/**
 * @brief Subscribes a thread to a topic.
 * @param [in] topic The topic to subscribe to.
 * @param [in] subscriber_id The ID of the subscribing thread.
 *
 * This method adds a subscriber ID to the list of subscribers for a given topic,
 * ensuring thread-safe access to the `subscriptions_` map.
 */
void EventDistributor::subscribe(const std::string& topic, ThreadID subscriber_id) {
    std::lock_guard<std::mutex> lock(subscriptions_mutex_);
    subscriptions_[topic].push_back(subscriber_id);
}

/**
 * @brief Processes a message received via the internal queue.
 * @param [in] message The message to process.
 *
 * This method handles different types of `EventMessage`s based on their `event_type_`.
 * It's responsible for fanning out messages, handling timers, and managing
 * the thread's lifecycle.
 */
void EventDistributor::process_message(EventMessage& message) {
    // A lock is needed to access the 'subscriptions_' map.
    // This is the core of the single-threaded fan-out logic.
    switch (message.event_type_) {
        case EventMessage::EventType::Message: {
            try {
                // The message payload is a `Message` object.
                const auto& message_payload = std::get<Message>(message.payload_);

                // Lock the mutex before accessing the shared subscriptions map.
                std::lock_guard<std::mutex> lock(subscriptions_mutex_);

                auto it = subscriptions_.find(message_payload.get_topic());
                if (it != subscriptions_.end()) {
                    for (ThreadID subscriber_id : it->second) {
                        // TODO: Implement the logic to post the message to the subscriber's queue.
                        // This would require a way to map ThreadID to a specific queue or a handler.
                        // The Reactor should facilitate this.
                    }
                }
            } catch (const std::bad_variant_access& e) {
                // This should not happen in a correctly designed system.
                PUBSUB_LOG_STR(get_logger(), LogLevel::Error, "Bad variant access when processing Message event: " + std::string(e.what()));
            }
            break;
        }
        case EventMessage::EventType::Termination: {
            // Log the termination event reason before shutting down.
            std::string reason = std::get<std::string>(message.payload_);
            PUBSUB_LOG_STR(get_logger(), LogLevel::Info, "Received termination message: " + reason);
            is_running_ = false;
            break;
        }
        case EventMessage::EventType::Timer: {
            // TODO: Timer handling logic.
            break;
        }
        default:
            // Log an unknown event type.
            PUBSUB_LOG_STR(get_logger(), LogLevel::Warning, "Received an unknown event type.");
            break;
    }
}

} // namespace pubsub_itc_fw
#endif
