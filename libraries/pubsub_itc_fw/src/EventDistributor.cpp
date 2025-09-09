/**
 * @file EventDistributor.cpp
 * @brief Implements the EventDistributor class.
 */

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

// Third party headers
// (None directly here)

// Project headers
#include <pubsub_itc_fw/EventDistributor.hpp>
#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/Message.hpp>
#include <pubsub_itc_fw/Reactor.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>
#include <pubsub_itc_fw/LoggerInterface.hpp>

namespace pubsub_itc_fw {

EventDistributor::EventDistributor(const LoggerInterface& logger,
                                   const std::string& thread_name,
                                   int low_watermark,
                                   int high_watermark,
                                   void* for_client_use,
                                   std::function<void(void* for_client_use)> gone_below_low_watermark_handler,
                                   std::function<void(void* for_client_use)> gone_above_high_watermark_handler)
    : ApplicationThread(logger,
                        thread_name,
                        low_watermark,
                        high_watermark,
                        for_client_use,
                        std::move(gone_below_low_watermark_handler),
                        std::move(gone_above_high_watermark_handler)) {
    // Constructor handles initialization via the base class.
}

EventDistributor::~EventDistributor() {
    // No specific cleanup needed here beyond what the base class handles.
}

void EventDistributor::run() {
    // The main loop for the EventDistributor thread. This loop runs
    // on its own dedicated thread, processing events from its internal queue.
    // The `run_internal` method from the base class handles the core loop logic.
    run_internal();
}

void EventDistributor::publish(const std::string& topic, const Message& message) {
    // We now construct the EventMessage in a single line using a valid payload.
    EventMessage event_message(EventMessage::EventType::Message, std::make_pair(topic, message), get_thread_id());
    push(std::move(event_message));
}

void EventDistributor::subscribe(const std::string& topic, ThreadID subscriber_id) {
    std::lock_guard<std::mutex> lock(subscriptions_mutex_);
    subscriptions_[topic].push_back(subscriber_id);
}

void EventDistributor::do_pop_message_deallocate(EventMessage& message) {
    // In this specific implementation, we don't have a custom allocator, so we do nothing.
    // The memory for the message is managed by the queue itself.
}

void EventDistributor::process_message(EventMessage& message) {
    // The lock is needed to access the 'subscriptions_' map.
    // This is the core of the single-threaded fan-out logic.
    switch (message.event_type_) {
        case EventMessage::EventType::Message: {
            // The message payload is a pair of topic and message.
            auto* message_payload = std::get_if<std::pair<std::string, Message>>(&message.payload_);
            if (!message_payload) {
                PUBSUB_LOG(get_logger(), LogLevel::Error, "{}", "EventDistributor: Failed to get message payload.");
                return;
            }

            const auto& topic = message_payload->first;
            const auto& msg = message_payload->second;

            std::lock_guard<std::mutex> lock(subscriptions_mutex_);
            auto it = subscriptions_.find(topic);
            if (it != subscriptions_.end()) {
                for (ThreadID subscriber_id : it->second) {
                    // TODO: Post the message to the subscriber's queue.
                    // This will require the EventDistributor to have a way to look up a subscriber's queue
                    // by their ThreadID, likely via the Reactor.
                }
            }
            break;
        }
        case EventMessage::EventType::Termination: {
            PUBSUB_LOG(get_logger(), LogLevel::Info, "{}", "EventDistributor: Received termination message.");
            shutdown("Termination message received.");
            break;
        }
        case EventMessage::EventType::Timer: {
            // TODO: Timer handling logic.
            // This would involve finding the correct timer based on the message payload
            // and executing its handler.
            break;
        }
        default:
            PUBSUB_LOG(get_logger(), LogLevel::Warning, "{}", "EventDistributor: Unhandled event type.");
            break;
    }
}

} // namespace pubsub_itc_fw
