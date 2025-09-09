#pragma once

// C headers including posix API headers
// (None directly here)

// C++ headers whose names start with ‘c’
// (None directly here)

// System C++ headers
#include <string>

// Third party headers
// (None directly here)

// Project headers
#include <pubsub_itc_fw/EventDistributor.hpp>
#include <pubsub_itc_fw/Message.hpp>

namespace pubsub_itc_fw {

/**
 * @brief A client-side interface for pubsub operations.
 *
 * This class provides a high-level API for publishing and subscribing to topics
 * within the framework. It acts as a proxy, abstracting away the underlying
 * communication with the central `EventDistributor`. This class is typically
 * owned by an `ApplicationThread` subclass that needs to interact with the
 * pubsub service.
 */
class PubSubClient final {
  public:
    /**
     * @brief Constructs a PubSubClient with a reference to the EventDistributor.
     * @param [in] distributor A reference to the EventDistributor.
     */
    explicit PubSubClient(EventDistributor& distributor);

    /**
     * @brief Publishes a message to a specific topic.
     * @param [in] topic The topic to publish the message to.
     * @param [in] message The message to publish.
     */
    void publish(const std::string& topic, const Message& message);

    /**
     * @brief Subscribes to a specific topic.
     * @param [in] topic The topic to subscribe to.
     */
    void subscribe(const std::string& topic);

    /**
     * @brief Unsubscribes from a specific topic.
     * @param [in] topic The topic to unsubscribe from.
     */
    void unsubscribe(const std::string& topic);

  private:
    // A non-owning reference is used here to ensure that the PubSubClient
    // always has a valid reference to the EventDistributor service.
    EventDistributor& distributor_;
};

} // namespace pubsub_itc_fw
