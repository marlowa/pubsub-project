// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <pubsub_itc_fw/UnansweredCommands.hpp>

#include <utility>

#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>
#include <pubsub_itc_fw/PreconditionAssertion.hpp>

namespace pubsub_itc_fw {

UnansweredCommands::UnansweredCommands(size_t capacity_bytes, size_t capacity_commands, std::vector<std::string> destination_names,
                                       UnansweredCommandLinksInterface& links, QuillLogger& logger, std::string owner_name)
    : store_(capacity_bytes, capacity_commands), links_(links), logger_(logger), owner_name_(std::move(owner_name)) {
    if (destination_names.empty()) {
        throw PreconditionAssertion("UnansweredCommands: at least one destination must be named", __FILE__, __LINE__);
    }
    for (std::string& name : destination_names) {
        Destination destination;
        destination.name = std::move(name);
        destinations_.push_back(std::move(destination));
    }
    PUBSUB_LOG(logger_, FwLogLevel::Info, "{}: keeping up to {} unanswered command(s) in {} MiB, to send again after a change of leader", owner_name_,
               capacity_commands, capacity_bytes / (1024 * 1024));
}

UnansweredCommands::Kept UnansweredCommands::keep(std::string_view comp_id, std::string_view cl_ord_id, const uint8_t* envelope, size_t size) {
    const Kept kept = store_.keep(comp_id, cl_ord_id, envelope, size);
    if (kept == Kept::already_held) {
        return kept;
    }
    if (kept == Kept::full) {
        ++refused_because_full_;
        if (!full_) {
            full_ = true;
            PUBSUB_LOG(logger_, FwLogLevel::Warning,
                       "{}: the store of unanswered commands is full with {} command(s) waiting -- the service is not answering, and new commands are "
                       "refused until it does",
                       owner_name_, store_.unanswered());
        }
        return kept;
    }
    if (full_) {
        full_ = false;
        PUBSUB_LOG(logger_, FwLogLevel::Info, "{}: the store of unanswered commands has room again -- {} command(s) were refused while it was full",
                   owner_name_, refused_because_full_);
    }
    return kept;
}

bool UnansweredCommands::answered(std::string_view comp_id, std::string_view cl_ord_id) {
    return store_.answered(comp_id, cl_ord_id);
}

size_t UnansweredCommands::drop_session(std::string_view comp_id) {
    return store_.drop_session(comp_id);
}

void UnansweredCommands::on_new_leader(int32_t epoch, bool first_leader_seen) {
    if (first_leader_seen && store_.unanswered() == 0) {
        return;
    }
    // To every destination: the new leader may be any of them, and only it acts on the commands.
    start_resends(std::vector<bool>(destinations_.size(), true));
    PUBSUB_LOG(logger_, FwLogLevel::Info,
               "{}: a new leader at epoch {} -- sending again {} command(s) still unanswered, each marked as sent again, {} at a time", owner_name_, epoch,
               snapshot_.count(), PacedResend::batch_size);
}

void UnansweredCommands::on_connection_established(size_t destination, const ConnectionID& connection) {
    if (destination >= destinations_.size()) {
        throw PreconditionAssertion("UnansweredCommands: no such destination", __FILE__, __LINE__);
    }
    Destination& established = destinations_[destination];
    established.connection = connection;
    if (!established.connected_before) {
        established.connected_before = true;
        return;
    }
    // The connection replaces one that was closed, and the commands waiting to be sent on that one were discarded
    // with it (docs/bug_list.md, BUG-0118).
    std::vector<bool> wanted(destinations_.size(), false);
    wanted[destination] = true;
    start_resends(wanted);
    PUBSUB_LOG(logger_, FwLogLevel::Info,
               "{}: the {} connection {} is established again -- sending again on it {} command(s) still unanswered, each marked as sent again, {} at a time",
               owner_name_, established.name, connection.get_value(), snapshot_.count(), PacedResend::batch_size);
}

void UnansweredCommands::on_connection_lost(size_t destination) {
    if (destination >= destinations_.size()) {
        throw PreconditionAssertion("UnansweredCommands: no such destination", __FILE__, __LINE__);
    }
    Destination& lost = destinations_[destination];
    lost.connection = ConnectionID{};
    lost.resend.abandon();
}

bool UnansweredCommands::on_connection_writable(const ConnectionID& connection) {
    for (Destination& destination : destinations_) {
        if (destination.connection == connection && destination.connection.get_value() != 0 && destination.resend.active()) {
            continue_resend(destination);
            return true;
        }
    }
    return false;
}

void UnansweredCommands::start_resends(const std::vector<bool>& wanted) {
    // A resend still going is started again on the new copy, because its place in the old copy means nothing in the
    // new one. Commands it had already sent are sent again; the leader finds each in its WAL and does not sequence it
    // twice. A destination never connected has nothing to send on.
    std::vector<bool> sending(destinations_.size(), false);
    for (size_t index = 0; index < destinations_.size(); ++index) {
        sending[index] = (wanted[index] || destinations_[index].resend.active()) && destinations_[index].connection.get_value() != 0;
    }
    snapshot_.take(store_);
    for (Destination& destination : destinations_) {
        destination.resend.abandon();
    }
    for (size_t index = 0; index < destinations_.size(); ++index) {
        if (sending[index]) {
            destinations_[index].resend.start(snapshot_.count());
            continue_resend(destinations_[index]);
        }
    }
}

void UnansweredCommands::continue_resend(Destination& destination) {
    const bool more = destination.resend.send_next_batch([this, &destination](size_t index) {
        const auto [bytes, size] = snapshot_.command(index);
        links_.send_again(destination.connection, bytes, size);
    });
    if (more) {
        links_.request_writable_notification(destination.connection);
        return;
    }
    PUBSUB_LOG(logger_, FwLogLevel::Info, "{}: finished sending again {} unanswered command(s) to the {}", owner_name_, destination.resend.sent(),
               destination.name);
}

} // namespaces
