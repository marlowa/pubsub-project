#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <functional>
#include <string>

#include <pubsub_itc_fw/ConnectionID.hpp>

#include <topics.hpp>

namespace pubsub_itc_fw {

/**
 * @brief What TopicControlChannel needs its owning ApplicationThread to do.
 */
class TopicControlChannelHost {
  public:
    virtual ~TopicControlChannelHost() = default;

    virtual void topic_control_send_subscribe_request(ConnectionID connection_id, const pubsub_itc_fw_app::TopicSubscribeRequest& request) = 0;
};

/**
 * @brief Subscriber side of the control channel: the second connection.
 *
 * A subscriber opens a control connection alongside its data connection (see
 * TopicSubscriberChannel). On connect it registers the control channel with a
 * role=Control TopicSubscribeRequest carrying the same subscriber_id, so the
 * publisher can correlate the pair. It then receives out-of-band signals -- for now
 * TopicLagged (the subscriber has fallen too far behind) -- and delivers them to a
 * sink. The control connection carries no bulk data, so signals arrive promptly even
 * when the data connection is backed up.
 *
 * Not thread-safe: all calls must come from the owning ApplicationThread.
 */
class TopicControlChannel {
  public:
    // Called when a TopicLagged is received: (reason, oldest_retained_seq_no).
    using LaggedSink = std::function<void(const std::string& reason, int64_t oldest_retained_seq_no)>;

    TopicControlChannel(TopicControlChannelHost& host, std::string subscriber_id, std::string topic_name, LaggedSink lagged_sink)
        : host_(host), subscriber_id_(std::move(subscriber_id)), topic_name_(std::move(topic_name)), lagged_sink_(std::move(lagged_sink)) {}

    /// Register the control channel once its connection is up.
    void on_connected(ConnectionID connection_id) {
        connection_id_ = connection_id;
        pubsub_itc_fw_app::TopicSubscribeRequest request{};
        request.subscriber_id = subscriber_id_;
        request.topic_name = topic_name_;
        request.from_seq_no = 0; // ignored on the control channel
        request.role = pubsub_itc_fw_app::TopicChannelRole::Control;
        host_.topic_control_send_subscribe_request(connection_id, request);
    }

    void on_lagged(const pubsub_itc_fw_app::TopicLaggedView& view) {
        lagged_sink_(std::string(view.reason), view.oldest_retained_seq_no);
    }

  private:
    TopicControlChannelHost& host_;
    std::string subscriber_id_;
    std::string topic_name_;
    LaggedSink lagged_sink_;
    ConnectionID connection_id_;
};

} // namespaces
