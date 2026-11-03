// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "PeerSettingsCheck.hpp"

#include <utility>

#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>

namespace matching_engine {

PeerSettingsCheck::PeerSettingsCheck(int64_t own_capacity, int64_t own_fingerprint, int64_t peer_instance, std::string secret_file,
                                     PeerSettingsCheckLinksInterface& links, pubsub_itc_fw::QuillLogger& logger)
    : own_capacity_(own_capacity)
    , own_fingerprint_(own_fingerprint)
    , peer_instance_(peer_instance)
    , secret_file_(std::move(secret_file))
    , links_(links)
    , logger_(logger) {}

void PeerSettingsCheck::peer_settings_arrived(int64_t instance_id, int64_t region_capacity, int64_t secret_fingerprint) {
    const bool differed = known_ && differ_;
    known_ = true;
    peer_capacity_ = region_capacity;
    peer_fingerprint_ = secret_fingerprint;
    const bool secrets_differ = peer_fingerprint_ != own_fingerprint_;
    differ_ = peer_capacity_ != own_capacity_ || secrets_differ;
    if (peer_capacity_ != own_capacity_) {
        // The open-order region's capacity decides when an order is refused because the venue holds as many as it can,
        // so two engines with different capacities apply the same commands differently once one of them is full.
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Error,
                   "PeerSettingsCheck: the peer, instance {}, holds up to {} open order(s) and this instance up to {} -- the two would apply the same commands "
                   "differently, so whichever of them leads says that the other may not lead",
                   instance_id, peer_capacity_, own_capacity_);
    }
    if (secrets_differ) {
        // The secret the identifiers' keys are derived from: two engines with different secrets give the same report
        // different ExecIDs, so a member would be told of one event under two names after a failover.
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Error,
                   "PeerSettingsCheck: the peer, instance {}, holds a different identifier secret from this instance's ({}) -- the two would name the same "
                   "reports differently, so whichever of them leads says that the other may not lead. Copy the one secret file to both",
                   instance_id, secret_file_);
    }
    if (!differ_ && differed) {
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info,
                   "PeerSettingsCheck: the peer, instance {}, is now configured as this instance is (up to {} open order(s))", instance_id, own_capacity_);
    }
    // Reported whether or not this instance leads, so that the test learns the follower knows of the difference too, and
    // only once: bar_peer_if_settings_differ reports it when it bars the peer.
    if (!bar_peer_if_settings_differ() && differ_) {
        report_differ();
    }
}

bool PeerSettingsCheck::bar_peer_if_settings_differ() {
    if (!links_.leading() || !known_) {
        return false;
    }
    // Already saying what it should, so there is nothing to record or send.
    if (differ_ != links_.says_peer_may_lead()) {
        return false;
    }
    // Recorded by a majority of the voters, as rule 11's statement is, so that the peer is refused a lease even after this
    // instance has gone; and lifted once the peer says it is configured as this instance is.
    if (differ_) {
        links_.say_peer_may_not_lead();
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Error,
                   "PeerSettingsCheck: leading, and saying that the peer, instance {}, may not lead -- it holds up to {} open order(s) and this instance up to "
                   "{}, and its identifier secret is {} this instance's. Trading continues here; if this instance stops, no engine leads until the two are "
                   "configured alike",
                   peer_instance_, peer_capacity_, own_capacity_, peer_fingerprint_ == own_fingerprint_ ? "the same as" : "different from");
        report_differ();
        return true;
    }
    links_.say_peer_may_lead();
    PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info,
               "PeerSettingsCheck: leading, and saying that the peer, instance {}, may lead again -- it is configured as this instance is", peer_instance_);
    return false;
}

void PeerSettingsCheck::report_present_state() {
    if (known_ && differ_) {
        report_differ();
    }
}

void PeerSettingsCheck::report_differ() {
    const bool barred_peer = links_.leading() && !links_.says_peer_may_lead();
    links_.settings_differ(own_capacity_, peer_capacity_, barred_peer, peer_fingerprint_ != own_fingerprint_);
}

} // namespaces
