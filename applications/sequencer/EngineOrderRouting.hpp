#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <iterator>
#include <map>

#include <pubsub_itc_fw/ConnectionID.hpp>

#include <leader_follower.hpp>

namespace sequencer {

/** @brief Whether an engine connection that has just opened carries orders when nothing carries them yet. */
class ClaimIfNothingRoutedFlag {
  public:
    enum ClaimIfNothingRoutedFlagTag {
        DoNotClaimIfNothingRouted = 0, ///< The secondary: it carries orders only once it announces that it leads.
        ClaimIfNothingRouted = 1       ///< The primary: it carries orders until something says otherwise.
    };

    explicit ClaimIfNothingRoutedFlag(ClaimIfNothingRoutedFlagTag value) : value_{value} {}

    [[nodiscard]] bool is_set() const {
        return value_ == ClaimIfNothingRouted;
    }

  private:
    ClaimIfNothingRoutedFlagTag value_;
};

/**
 * @brief Which of a sequencer's connections to the matching engines carries orders, decided from what the engines say.
 *
 * A sequencer opens an order connection to each matching engine instance, the primary (instance 1)
 * and the secondary (instance 2). Orders go down exactly one of them, the active connection: the one
 * to the instance that leads. The other is the standby. Which instance leads moves with failovers,
 * and the sequencer learns it from each engine's announcement of its role and from an engine asking
 * to be brought up to date before it leads.
 *
 * This class keeps the order connection to each instance for as long as that connection is open, and
 * works the standby out from them rather than storing it. A sequencer that stored the standby in a slot
 * of its own could overwrite it and so forget an engine altogether: following, it would then drop that
 * engine's request to be brought up to date, and once it led, the venue would have no matching engine
 * while one was leading and asking (BUG-0108).
 *
 * It decides; it does not log or send. The owner logs what each call reports and sends orders down
 * active(). One thread, the sequencer's, calls every member function.
 */
class EngineOrderRouting {
  public:
    /// What an engine's announcement of its role did to the routing, so that the owner can say so.
    enum class AnnouncementOutcome {
        RefusedAsBehind,           ///< Its epoch is behind one already accepted, so it is an instance whose leadership has been superseded.
        NoOrderConnection,         ///< Accepted, but this sequencer holds no order connection to that instance.
        RoutedToLeader,            ///< The instance leads, and orders now go to it.
        AlreadyRoutedToLeader,     ///< The instance leads, and orders were already going to it.
        FollowerNotCarryingOrders, ///< The instance follows, and orders were not going to it.
        FollowerHandedToLeader,    ///< The instance follows and was carrying orders; they now go to the instance last known to lead.
        FollowerWithdrawnNoLeader, ///< The instance follows and was carrying orders; no leader is known, so orders have nowhere to go.
    };

    /**
     * @brief An order connection to an engine instance has been established.
     *
     * Orders go to it if that instance has already announced that it leads. Otherwise, with
     * ClaimIfNothingRouted, they go to it if they go nowhere yet, which is how the primary is used
     * until something says otherwise.
     */
    void connected(int64_t instance, pubsub_itc_fw::ConnectionID connection, ClaimIfNothingRoutedFlag claim) {
        connection_by_instance_[instance] = connection;
        if (announced_leader_ == instance || (claim.is_set() && !active_.is_valid())) {
            active_ = connection;
        }
    }

    /** @brief A connection has closed; it is forgotten, and if it carried orders, they go nowhere until told otherwise. */
    void lost(pubsub_itc_fw::ConnectionID connection) {
        for (auto entry = connection_by_instance_.begin(); entry != connection_by_instance_.end();) {
            entry = entry->second == connection ? connection_by_instance_.erase(entry) : std::next(entry);
        }
        if (active_ == connection) {
            active_ = pubsub_itc_fw::ConnectionID{};
        }
    }

    /**
     * @brief An engine instance has announced its role at an epoch.
     *
     * The epoch is what makes a claim safe to believe: an instance whose leadership has been superseded
     * may still announce that it leads, and its epoch is behind the one already accepted.
     */
    AnnouncementOutcome announced(int64_t instance, pubsub_itc_fw_app::Role role, int32_t epoch) {
        const bool leads = role == pubsub_itc_fw_app::Role::leader;
        if (epoch < announced_epoch_) {
            return AnnouncementOutcome::RefusedAsBehind;
        }
        announced_epoch_ = epoch;
        if (leads) {
            announced_leader_ = instance;
        } else if (announced_leader_ == instance) {
            announced_leader_ = 0;
        }
        const auto own = connection_by_instance_.find(instance);
        if (own == connection_by_instance_.end()) {
            return AnnouncementOutcome::NoOrderConnection;
        }
        if (leads) {
            if (active_ == own->second) {
                return AnnouncementOutcome::AlreadyRoutedToLeader;
            }
            active_ = own->second;
            return AnnouncementOutcome::RoutedToLeader;
        }
        if (active_ != own->second) {
            return AnnouncementOutcome::FollowerNotCarryingOrders;
        }
        const auto leader = announced_leader_ != 0 ? connection_by_instance_.find(announced_leader_) : connection_by_instance_.end();
        if (leader != connection_by_instance_.end()) {
            active_ = leader->second;
            return AnnouncementOutcome::FollowerHandedToLeader;
        }
        active_ = pubsub_itc_fw::ConnectionID{};
        return AnnouncementOutcome::FollowerWithdrawnNoLeader;
    }

    /**
     * @brief An engine asked, on this connection, to be brought up to date while this sequencer follows.
     *
     * A following sequencer does not bring it up to date, but routes to it, so that if this sequencer
     * takes the lead it sends orders to the engine that is about to lead.
     */
    void asked_to_catch_up_while_following(pubsub_itc_fw::ConnectionID connection) {
        if (is_engine_connection(connection)) {
            active_ = connection;
        }
    }

    /** @brief The engine on this connection has been brought up to date in order to lead; orders go to it. */
    void caught_up_to_lead(pubsub_itc_fw::ConnectionID connection) {
        if (is_engine_connection(connection)) {
            active_ = connection;
        }
    }

    /** @brief The connection orders go down, or an invalid one if they go nowhere. */
    [[nodiscard]] pubsub_itc_fw::ConnectionID active() const {
        return active_;
    }

    /** @brief The open order connection to an engine that is not the active one, or an invalid one if there is none. */
    [[nodiscard]] pubsub_itc_fw::ConnectionID standby() const {
        for (const auto& [instance, connection] : connection_by_instance_) {
            if (connection != active_) {
                return connection;
            }
        }
        return pubsub_itc_fw::ConnectionID{};
    }

    /** @brief Whether this is an open order connection to either engine instance. */
    [[nodiscard]] bool is_engine_connection(pubsub_itc_fw::ConnectionID connection) const {
        if (!connection.is_valid()) {
            return false;
        }
        for (const auto& [instance, own] : connection_by_instance_) {
            if (own == connection) {
                return true;
            }
        }
        return false;
    }

    /** @brief The epoch of the latest announcement accepted, or -1 before any. */
    [[nodiscard]] int32_t announced_epoch() const {
        return announced_epoch_;
    }

    /** @brief The instance that last announced it leads, or 0 if none is known to. */
    [[nodiscard]] int64_t announced_leader() const {
        return announced_leader_;
    }

  private:
    std::map<int64_t, pubsub_itc_fw::ConnectionID> connection_by_instance_;
    pubsub_itc_fw::ConnectionID active_{};
    int32_t announced_epoch_{-1};
    int64_t announced_leader_{0};
};

} // namespaces
