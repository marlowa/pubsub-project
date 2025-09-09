#pragma once

// C++ headers whose names start with ‘c’
// (None directly here)

// System C++ headers
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <stdexcept>
#include <functional>

// Third party headers
// (None directly here)

// Project headers
#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/Timer.hpp>
#include <pubsub_itc_fw/LoggerInterface.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>
#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/ThreadWithJoinTimeout.hpp>

namespace pubsub_itc_fw {

// Forward declaration
class Reactor;

/**
 * @brief The application-level hub for fanout messaging and timer events.
 *
 * This class runs on its own dedicated thread and is responsible for managing
 * subscriptions, fanning out messages to subscribers, and managing timers.
 * It is a key component for decoupling publishers from subscribers.
 */
class EventDistributor final : public ApplicationThread {
  public:
    /**
     * @brief Destructor.
     */
    ~EventDistributor() override;

    /**
     * @brief Constructs an EventDistributor.
     * @param [in] logger A reference to the logger instance.
     * @param [in] thread_name The name of the thread.
     * @param [in] low_watermark The low watermark for the message queue.
     * @param [in] high_watermark The high watermark for the message queue.
     * @param [in] for_client_use A user-defined pointer for client-specific data.
     * @param [in] gone_below_low_watermark_handler A handler for when the queue size drops below the low watermark.
     * @param [in] gone_above_high_watermark_handler A handler for when the queue size exceeds the high watermark.
     */
    EventDistributor(const LoggerInterface& logger,
                     const std::string& thread_name,
                     int low_watermark,
                     int high_watermark,
                     void* for_client_use,
                     std::function<void(void* for_client_use)> gone_below_low_watermark_handler,
                     std::function<void(void* for_client_use)> gone_above_high_watermark_handler);

    /**
     * @brief The main loop for the event distributor thread.
     */
    void run() override;

    /**
     * @brief Publishes a message to a topic.
     * @param [in] topic The topic to publish to.
     * @param [in] message The message to publish.
     */
    void publish(const std::string& topic, const Message& message);

    /**
     * @brief Subscribes a thread to a topic.
     * @param [in] topic The topic to subscribe to.
     * @param [in] subscriber_id The ID of the subscribing thread.
     */
    void subscribe(const std::string& topic, ThreadID subscriber_id);

    /**
     * @brief Handles the deallocation of a message popped from the queue.
     * @param [in] message The message to deallocate.
     */
    void do_pop_message_deallocate(EventMessage& message) override;

  protected:
    /**
     * @brief Processes a message received via the internal queue.
     * @param [in] message The message to process.
     */
    void process_message(EventMessage& message) override;

  private:
    std::mutex subscriptions_mutex_;
    std::unordered_map<std::string, std::vector<ThreadID>> subscriptions_;
    std::unordered_map<std::string, std::unique_ptr<Timer>> timers_;
};

} // namespace pubsub_itc_fw
