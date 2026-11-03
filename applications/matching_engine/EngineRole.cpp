// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include "EngineRole.hpp"

namespace matching_engine {

MeRole EngineRole::role() const {
    switch (situation_) {
        case Situation::starting:
        case Situation::current:
            return MeRole::Unknown;
        case Situation::catching_up_to_become_current:
        case Situation::catching_up_to_lead:
        case Situation::catching_up_to_follow_again:
            return MeRole::Reconciling;
        case Situation::leading:
            return MeRole::Leader;
        case Situation::following:
            return MeRole::Follower;
    }
    return MeRole::Unknown;
}

void EngineRole::start_catching_up_to_follow_again() {
    // An instance that was leading has become a follower before it asks to follow again, so it goes back to following.
    follow_again_returns_to_ = situation_ == Situation::following || situation_ == Situation::leading ? Situation::following : Situation::current;
    situation_ = Situation::catching_up_to_follow_again;
}

EngineRole::Completed EngineRole::catch_up_completed_now() {
    catch_up_completed_ = true;
    switch (situation_) {
        case Situation::catching_up_to_follow_again:
            situation_ = follow_again_returns_to_;
            return Completed::following_again;
        case Situation::catching_up_to_lead:
            situation_ = Situation::current;
            return Completed::to_lead;
        default:
            situation_ = Situation::current;
            return Completed::became_current;
    }
}

} // namespaces
