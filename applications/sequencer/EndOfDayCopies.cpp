// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "EndOfDayCopies.hpp"

#include <algorithm>
#include <optional>
#include <string>

#include <fmt/format.h>

namespace sequencer {

EndOfDayCopies::EndOfDayCopies(int32_t publishers_waited_for, int64_t longest_wait_ns)
    : publishers_waited_for_(publishers_waited_for), longest_wait_ns_(longest_wait_ns) {}

void EndOfDayCopies::start(int64_t begin_seq_no, int64_t now_utc_ns) {
    if (begin_seq_no == begin_seq_no_) {
        return;
    }
    begin_seq_no_ = begin_seq_no;
    wait_started_utc_ns_ = now_utc_ns;
}

void EndOfDayCopies::publisher_holds(const pubsub_itc_fw::ConnectionID& connection, int64_t last_seq_no) {
    for (PublisherAnswer& answer : publisher_answers_) {
        if (answer.connection == connection) {
            answer.last_seq_no = last_seq_no;
            return;
        }
    }
    publisher_answers_.push_back(PublisherAnswer{connection, last_seq_no});
}

void EndOfDayCopies::publisher_gone(const pubsub_itc_fw::ConnectionID& connection) {
    publisher_answers_.erase(std::remove_if(publisher_answers_.begin(), publisher_answers_.end(),
                                            [&connection](const PublisherAnswer& answer) { return answer.connection == connection; }),
                             publisher_answers_.end());
}

int32_t EndOfDayCopies::publishers_holding_the_begin() const {
    return static_cast<int32_t>(std::count_if(publisher_answers_.begin(), publisher_answers_.end(),
                                              [this](const PublisherAnswer& answer) { return answer.last_seq_no >= begin_seq_no_; }));
}

EndOfDayCopies::Assessment EndOfDayCopies::assess(std::optional<int64_t> follower_sequencer_holds_through, int64_t now_utc_ns) const {
    const bool follower_complete = !follower_sequencer_holds_through.has_value() || *follower_sequencer_holds_through >= begin_seq_no_;
    const int32_t publishers_complete = publishers_holding_the_begin();
    const bool publishers_all_complete = publishers_complete >= publishers_waited_for_;

    const std::string follower_words = !follower_sequencer_holds_through.has_value()
                                           ? std::string("there is no follower sequencer, because high availability is off")
                                           : fmt::format("the follower sequencer holds every record up to {}", *follower_sequencer_holds_through);
    std::string publisher_words;
    for (const PublisherAnswer& answer : publisher_answers_) {
        publisher_words += fmt::format("; the publisher on connection {} holds every record up to {}", answer.connection.get_value(), answer.last_seq_no);
    }
    const std::string state = fmt::format("the end-of-day begin is at seq={}: {}; {} of the {} publisher(s) waited for hold it{}", begin_seq_no_,
                                          follower_words, publishers_complete, publishers_waited_for_, publisher_words);

    if (follower_complete && publishers_all_complete) {
        return Assessment{Verdict::EveryCopyComplete, state};
    }
    const int64_t waited_ns = now_utc_ns - wait_started_utc_ns_;
    if (waited_ns >= longest_wait_ns_) {
        return Assessment{Verdict::LimitPassed, fmt::format("{}; the limit of {} second(s) has passed", state, longest_wait_ns_ / 1'000'000'000)};
    }
    return Assessment{Verdict::Waiting, state};
}

void EndOfDayCopies::forget() {
    begin_seq_no_ = 0;
    wait_started_utc_ns_ = 0;
    publisher_answers_.clear();
}

} // namespaces
