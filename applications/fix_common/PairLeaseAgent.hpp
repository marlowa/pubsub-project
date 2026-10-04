#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <utility>

#include <pubsub_itc_fw/ConnectionID.hpp>
#include <pubsub_itc_fw/FwLogLevel.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>

#include <LeaseLinksInterface.hpp>
#include <LeaseParticipant.hpp>
#include <LeasePromiseRecorderInterface.hpp>
#include <LeasePromiseStore.hpp>
#include <LeaseTiming.hpp>
#include <PeerStatementsFlag.hpp>
#include <leader_follower.hpp>

namespace fix_common {

/**
 * @brief Everything an instance of a pair does to decide whether it leads, apart from reaching the other voters.
 *
 * The sequencer, the matching engine, the matching engine publisher and the arbiter each own one.
 * Keeping it in one place means all four apply the lease rules, and report what they do, in exactly
 * the same way. The rules themselves are in LeaseParticipant; this class drives them:
 *
 *   - It asks both other voters to let this instance lead whenever the rules allow it to ask.
 *   - While leading it asks both to renew, every renewal interval.
 *   - It gives up a request to lead that has gone unanswered for a lease period, and waits a random
 *     time before asking again, so that the two instances do not keep asking at the same moment.
 *   - It answers the peer's requests.
 *   - It says when this instance starts leading, stops leading, or agrees that its peer leads, so
 *     that the owner can change what it does.
 *
 * Primary and secondary are only names, but the primary is preferred when both start together and
 * neither has any other claim. The secondary gives it one renewal interval's head start, counted from the end of
 * the first lease period.
 *
 * The owner calls on_tick() every LeaseTiming::tick_interval, and passes in each lease PDU it receives.
 */
class PairLeaseAgent {
  public:
    using Clock = std::chrono::steady_clock;

    /// A change in what this instance may do, which the owner must act on.
    enum class Change {
        Nothing,        ///< nothing the owner needs to act on
        BecameLeader,   ///< this instance now leads and may act as leader
        StoppedLeading, ///< this instance led and no longer does: it must stop acting as leader at once
        AgreedPeerLeads ///< this instance granted its peer a lease, and so agrees that the peer leads
    };

    /**
     * @param[in] owner_name The name the owner logs under, such as "SequencerThread".
     * @param[in] logger Where to log.
     * @param[in] links How to reach the other two voters.
     * @param[in] group The pair this instance belongs to.
     * @param[in] self_id This instance's id: 1 for the primary, 2 for the secondary.
     * @param[in] peer_id The other instance's id.
     * @param[in] third_voter_id The third voter's id, which appears on its grants and refusals.
     * @param[in] third_voter_name How the third voter is named in the log: "the arbiter" or "the witness".
     * @param[in] timing The lease timings, which every voter shares.
     * @param[in] started_at When this instance started.
     * @param[in] persisted_epoch The highest epoch this instance knew of when it last stopped, or zero.
     */
    PairLeaseAgent(std::string owner_name, pubsub_itc_fw::QuillLogger& logger, LeaseLinksInterface& links, pubsub_itc_fw_app::ComponentGroup group,
                   int64_t self_id, int64_t peer_id, int64_t third_voter_id, std::string third_voter_name, const LeaseTiming& timing,
                   Clock::time_point started_at, int32_t persisted_epoch)
        : owner_name_(std::move(owner_name))
        , logger_(logger)
        , links_(links)
        , group_(group)
        , self_id_(self_id)
        , peer_id_(peer_id)
        , third_voter_id_(third_voter_id)
        , third_voter_name_(std::move(third_voter_name))
        , timing_(timing)
        , participant_(self_id, timing.period, timing.drift_allowance, started_at, persisted_epoch, PeerStatementsFlag{PeerStatementsFlag::PeerAlwaysMayLead})
        , random_(static_cast<std::mt19937::result_type>(self_id)) {
        // Measured from the end of the first lease period, during which neither instance may ask at all.
        if (self_id > peer_id) {
            participant_.stop(started_at, timing.period + timing.renewal_interval);
        }
    }

    /**
     * @brief Keep this instance's promises as a voter in @p recorder, and resume with the one it recorded before restarting.
     *
     * Called once, straight after construction, by an owner that can keep a record on disk. With a
     * record from this boot of the machine, the instance does not wait out a lease period after
     * starting: it knows what it promised, so it can vote at once, and ask to lead at once if it
     * promised nothing still running. A process restarted by its supervisor within the lease period
     * can then keep the lead it held, rather than being overtaken by its peer. Without a record it
     * waits, as any voter that has forgotten must.
     */
    void keep_promises_in(LeasePromiseRecorderInterface& recorder, const std::optional<LeasePromiseStore::Record>& kept, Clock::time_point now) {
        recorder_ = &recorder;
        if (!kept.has_value()) {
            PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info,
                       "{}: no promise recorded during this boot of the machine -- waiting one lease period before voting or asking to lead", owner_name_);
            return;
        }
        // A vote this instance gave itself while leading is not carried across the restart. It was
        // a vote for a process that has now died, together with the lease it held, so it protects
        // nothing; kept, it would only have this instance refuse a peer that has taken over.
        const bool was_leading = kept->promised_to == self_id_ && kept->until > now;
        participant_.resume_with_kept_promise(was_leading ? 0 : kept->promised_to, was_leading ? now : kept->until, now);
        recorded_to_ = kept->promised_to;
        recorded_until_ = kept->until;
        const auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(kept->until - now).count();
        if (was_leading) {
            // It was leading when it stopped. The secondary's head start exists so that the primary wins
            // when both start together from cold; it must not cost a leading secondary its lead.
            participant_.stop(now, Clock::duration::zero());
            resuming_lead_until_ = kept->until;
            // TEST CONTRACT -- ha_test.py matches this text. The wording is an interface: change it and the test breaks, silently and elsewhere.
            PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info,
                       "{}: resuming with the promise recorded before restarting -- this instance was leading, so it asks to lead again at once", owner_name_);
        } else if (kept->promised_to != 0 && kept->until > now) {
            PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info,
                       "{}: resuming with the promise recorded before restarting -- the vote is promised to the peer for {} ms more, so this instance "
                       "does not ask to lead before then",
                       owner_name_, remaining_ms);
        } else {
            PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info,
                       "{}: resuming with the promise recorded before restarting -- no promise is still running, so this instance votes at once", owner_name_);
        }
    }

    /// Apply the lease rules. Called every LeaseTiming::tick_interval.
    [[nodiscard]] Change on_tick(Clock::time_point now) {
        adopt_background_record();
        Change change = Change::Nothing;
        if (participant_.stop_if_lease_ran_out(now)) {
            // TEST CONTRACT -- ha_test.py matches this text. The wording is an interface: change it and the test breaks, silently and elsewhere.
            PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Warning,
                       "{}: lease ran out -- neither the peer nor {} renewed it in time, so this instance stops leading (epoch={})", owner_name_,
                       third_voter_name_, participant_.epoch());
            change = Change::StoppedLeading;
        }

        if (participant_.acting(now) && now - last_renewal_ >= timing_.renewal_interval) {
            last_renewal_ = now;
            ask_voters(participant_.epoch(), now);
        }

        if (participant_.state() == LeaseHolder::State::Candidate && now - asked_at_ > timing_.period) {
            if (!reported_no_leader_) {
                // TEST CONTRACT -- ha_test.py matches this text. The wording is an interface: change it and the test breaks, silently and elsewhere.
                PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Warning,
                           "{}: no voter granted a lease within a lease period of asking to lead at epoch {} -- this group has no leader. If the leader and "
                           "every arbiter are unavailable, it stays without one until an arbiter or the peer returns, because this instance cannot tell a "
                           "dead leader from being cut off (R-0147). Asking again",
                           owner_name_, participant_.epoch());
                reported_no_leader_ = true;
            } else {
                PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Debug, "{}: still no grant after asking to lead at epoch {} -- asking again", owner_name_,
                           participant_.epoch());
            }
            participant_.stop(now, wait_before_asking_again());
        }

        if (participant_.may_ask_to_lead(now)) {
            const int32_t epoch = participant_.begin_asking_to_lead();
            asked_at_ = now;
            last_renewal_ = now;
            PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info, "{}: asking the peer and {} to let this instance lead at epoch {}", owner_name_,
                       third_voter_name_, epoch);
            ask_voters(epoch, now);
        }
        return change;
    }

    /**
     * @brief The peer asks for a lease. The answer is sent back on @p reply_to.
     * @return AgreedPeerLeads when the request was granted.
     */
    [[nodiscard]] Change on_request(const pubsub_itc_fw::ConnectionID& reply_to, int64_t candidate_id, int32_t epoch, int64_t request_id,
                                    Clock::time_point now) {
        const LeaseParticipant::PeerRequestOutcome outcome = participant_.on_peer_request(candidate_id, epoch, now);
        if (outcome.gave_up_asking_to_lead) {
            PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info,
                       "{}: the peer asks to lead at epoch {}, above the epoch this instance asked for -- giving way to it", owner_name_, epoch);
        }

        if (outcome.answer.verdict == LeaseVoter::Verdict::Granted && !promise_recorded(candidate_id)) {
            // The promise could not be made durable, so it is not made: a restart would forget it.
            PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Error,
                       "{}: could not record the promise of this instance's vote to the peer -- refusing the peer's request rather than making a promise "
                       "a restart would forget",
                       owner_name_);
            pubsub_itc_fw_app::LeaseRefusal refusal{};
            refusal.voter_instance_id = self_id_;
            refusal.group = group_;
            refusal.highest_epoch = outcome.answer.highest_epoch;
            refusal.request_id = request_id;
            refusal.reason = pubsub_itc_fw_app::LeaseRefusalReason::unknown;
            links_.send_refusal(reply_to, refusal);
            return Change::Nothing;
        }

        if (outcome.answer.verdict == LeaseVoter::Verdict::Granted) {
            reported_no_leader_ = false;
            pubsub_itc_fw_app::LeaseGrant grant{};
            grant.voter_instance_id = self_id_;
            grant.group = group_;
            grant.epoch = epoch;
            grant.request_id = request_id;
            links_.send_grant(reply_to, grant);
            return Change::AgreedPeerLeads;
        }

        pubsub_itc_fw_app::LeaseRefusal refusal{};
        refusal.voter_instance_id = self_id_;
        refusal.group = group_;
        refusal.highest_epoch = outcome.answer.highest_epoch;
        refusal.request_id = request_id;
        refusal.reason = refusal_reason_for(outcome.answer.verdict);
        links_.send_refusal(reply_to, refusal);
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Debug, "{}: refused the peer's request to lead at epoch {} ({})", owner_name_, epoch,
                   pubsub_itc_fw_app::to_string(refusal.reason));
        return Change::Nothing;
    }

    /**
     * @brief A voter granted one of this instance's requests.
     * @return BecameLeader when this instance now leads.
     */
    [[nodiscard]] Change on_grant(int64_t voter_id, int32_t epoch, int64_t request_id, Clock::time_point now) {
        const LeaseHolder::Event event = participant_.on_grant(voter_id, request_id, epoch, now);
        if (event == LeaseHolder::Event::LeaseExtended) {
            record_leading();
        }
        if (event != LeaseHolder::Event::BecameLeader) {
            return Change::Nothing;
        }
        reported_no_leader_ = false;
        resuming_lead_until_ = Clock::time_point{};
        record_leading();
        // TEST CONTRACT -- ha_test.py matches this text. The wording is an interface: change it and the test breaks, silently and elsewhere.
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info, "{}: leading at epoch {} -- granted a lease by {}, which with its own vote is a majority",
                   owner_name_, epoch, voter_name(voter_id));
        return Change::BecameLeader;
    }

    /**
     * @brief A voter refused one of this instance's requests.
     * @return StoppedLeading when this instance led and has learnt of a newer generation.
     */
    [[nodiscard]] Change on_refusal(int64_t voter_id, int32_t highest_epoch, int64_t request_id, pubsub_itc_fw_app::LeaseRefusalReason reason,
                                    Clock::time_point now) {
        const bool was_leading = participant_.state() == LeaseHolder::State::Leading;
        const int32_t epoch_before = participant_.epoch();
        // An instance resuming the lead it recorded asks again above a newer epoch at once, rather than
        // after the usual random wait. The wait keeps two candidates from colliding again, and there is
        // no second candidate: both other voters promised their votes to this instance. It matters to an
        // arbiter, which keeps no epoch on disk and so first asks below the epoch it led in; waiting
        // could let those promises run out, and the other arbiter take over.
        const Clock::duration wait = now < resuming_lead_until_ ? Clock::duration::zero() : Clock::duration{wait_before_asking_again()};
        if (participant_.on_refusal(request_id, highest_epoch, now, wait) != LeaseHolder::Event::NewerEpochKnown) {
            PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Debug, "{}: {} refused a request ({})", owner_name_, voter_name(voter_id),
                       pubsub_itc_fw_app::to_string(reason));
            return Change::Nothing;
        }
        PUBSUB_LOG(logger_, was_leading ? pubsub_itc_fw::FwLogLevel::Warning : pubsub_itc_fw::FwLogLevel::Info,
                   "{}: {} has granted epoch {}, above this instance's {} -- a newer generation exists, so this instance stops {} and will ask again above it",
                   owner_name_, voter_name(voter_id), highest_epoch, epoch_before, was_leading ? "leading" : "asking");
        return was_leading ? Change::StoppedLeading : Change::Nothing;
    }

    /// Whether this instance leads and holds an unexpired lease, and so may act as leader.
    [[nodiscard]] bool acting(Clock::time_point now) const {
        return participant_.acting(now);
    }

    /// The epoch this instance leads in, or asks to lead in.
    [[nodiscard]] int32_t epoch() const {
        return participant_.epoch();
    }

    /// When this instance's latest lease runs out, as it counts it: somewhat earlier than the voters count it.
    [[nodiscard]] Clock::time_point lease_expires_at() const {
        return participant_.lease_expires_at();
    }

    /// The highest epoch this instance has granted, led in, or learnt of. The owner keeps it on disk.
    [[nodiscard]] int32_t highest_epoch() const {
        return participant_.highest_epoch();
    }

  private:
    /**
     * @brief Make durable the promise just made to @p candidate_id, when the record does not already cover it.
     *
     * The record is written with an expiry some way beyond the true one, so that a follower granting
     * its leader a renewal every second rewrites it only now and then rather than at every renewal. A
     * recorded expiry later than the true one only makes the instance stricter after a restart.
     */
    [[nodiscard]] bool promise_recorded(int64_t candidate_id) {
        // The record covers the promise only if it names the same instance. A record naming this
        // instance itself, left from when it led, must be replaced: read back after a restart, it would
        // let the instance ask to lead while its vote was in fact promised to the peer.
        if (recorder_ == nullptr) {
            return true;
        }
        adopt_background_record();
        if (recorded_to_ == candidate_id && participant_.promise_expires_at() <= recorded_until_) {
            refresh_in_background(candidate_id, participant_.promise_expires_at());
            return true;
        }
        const Clock::time_point until = participant_.promise_expires_at() + promise_record_margin;
        // record() waits for any background write and discards its outcome, so it is forgotten here.
        background_in_flight_ = false;
        if (!write_promise_record(candidate_id, until)) {
            return false;
        }
        recorded_to_ = candidate_id;
        recorded_until_ = until;
        return true;
    }

    /**
     * @brief Take the outcome of a background write, if one has finished, as what the record now says.
     *
     * Called at every tick and before the record is consulted, so a finished write is adopted within
     * one tick. A write that failed leaves the record as it was; the promise is then written on the
     * lease thread when it is needed, as without a background writer.
     */
    void adopt_background_record() {
        if (!background_in_flight_ || recorder_ == nullptr) {
            return;
        }
        const std::optional<bool> outcome = recorder_->background_result();
        if (!outcome.has_value()) {
            return;
        }
        background_in_flight_ = false;
        if (*outcome) {
            recorded_to_ = background_promised_to_;
            recorded_until_ = background_until_;
        } else {
            PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Warning,
                       "{}: could not refresh the lease promise record in the background -- it will be written when it is needed", owner_name_);
        }
    }

    /**
     * @brief Start writing a fresh record in the background when the one held is close to no longer covering the promise.
     *
     * The record held still covers the promise, so nothing waits for this write. It is started when
     * fewer than refresh_ahead remain between the promise's expiry and the record's, which is half the
     * margin, so that in normal operation the fresh record is on disk seconds before it is needed and
     * the lease thread never writes it itself. A recorder that cannot write in the background declines,
     * and the record is then written on the lease thread when it is needed.
     */
    void refresh_in_background(int64_t promised_to, Clock::time_point expires_at) {
        if (background_in_flight_ || recorded_until_ - expires_at > refresh_ahead) {
            return;
        }
        const Clock::time_point until = expires_at + promise_record_margin;
        if (recorder_->record_in_background(promised_to, until)) {
            background_in_flight_ = true;
            background_promised_to_ = promised_to;
            background_until_ = until;
        }
    }

    /**
     * @brief Write a promise record, and say so when writing it took long enough to put leases at risk.
     *
     * The record is written to disk and synced before the vote it records is given, on the thread
     * that handles leases, so for as long as the write takes, this instance answers no lease request
     * and renews nothing. A write slower than slow_promise_record_write is logged with how long it
     * took, because a write of seconds lets leases run out across the venue (BUG-0107).
     */
    [[nodiscard]] bool write_promise_record(int64_t promised_to, Clock::time_point until) {
        const auto started = Clock::now();
        const bool written = recorder_->record(promised_to, until);
        const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started);
        if (took >= slow_promise_record_write) {
            PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Warning,
                       "{}: writing the lease promise record took {} ms, during which this instance answered no lease request and renewed nothing", owner_name_,
                       took.count());
        }
        return written;
    }

    /**
     * @brief Record, while leading, that this instance's vote is promised to itself until its lease runs out.
     *
     * The record lets a leader whose process is restarted within the lease period know that it was
     * leading, so that it asks to lead again at once instead of waiting; its peer and the arbiter
     * grant it, because their votes are promised to it. The vote it gave itself is not carried
     * across the restart (see keep_promises_in()). The record is written with the same margin as a
     * promise to the peer, so a leader renewing every second rewrites it only now and then.
     *
     * Rule 5 means an instance leads only once its promise to its peer has run out, so replacing that
     * promise in the record loses nothing. A record that cannot be written is not fatal: the record
     * then holds an older promise, and a restart reading it is stricter than it needs to be, never less
     * strict.
     */
    void record_leading() {
        if (recorder_ == nullptr || participant_.state() != LeaseHolder::State::Leading) {
            return;
        }
        adopt_background_record();
        if (recorded_to_ == self_id_ && participant_.lease_expires_at() <= recorded_until_) {
            refresh_in_background(self_id_, participant_.lease_expires_at());
            return;
        }
        const Clock::time_point until = participant_.lease_expires_at() + promise_record_margin;
        // record() waits for any background write and discards its outcome, so it is forgotten here.
        background_in_flight_ = false;
        if (write_promise_record(self_id_, until)) {
            recorded_to_ = self_id_;
            recorded_until_ = until;
        } else {
            PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Warning,
                       "{}: could not record that this instance leads -- if its process is restarted it will wait longer than it needs to before "
                       "asking to lead again",
                       owner_name_);
        }
    }

    static pubsub_itc_fw_app::LeaseRefusalReason refusal_reason_for(LeaseVoter::Verdict verdict) {
        switch (verdict) {
            case LeaseVoter::Verdict::RefusedWhileRestarting:
                return pubsub_itc_fw_app::LeaseRefusalReason::restarting;
            case LeaseVoter::Verdict::RefusedPromisedElsewhere:
                return pubsub_itc_fw_app::LeaseRefusalReason::promised_elsewhere;
            case LeaseVoter::Verdict::RefusedEpochBehind:
                return pubsub_itc_fw_app::LeaseRefusalReason::epoch_behind;
            case LeaseVoter::Verdict::RefusedMayNotLead:
                return pubsub_itc_fw_app::LeaseRefusalReason::may_not_lead;
            case LeaseVoter::Verdict::Granted:
                break;
        }
        return pubsub_itc_fw_app::LeaseRefusalReason::unknown;
    }

    [[nodiscard]] std::string voter_name(int64_t voter_id) const {
        return voter_id == third_voter_id_ ? third_voter_name_ : "the peer";
    }

    [[nodiscard]] std::chrono::milliseconds wait_before_asking_again() {
        const int64_t quarter = timing_.renewal_interval.count() / 4;
        return std::chrono::milliseconds{std::uniform_int_distribution<int64_t>(quarter, 3 * quarter)(random_)};
    }

    void ask_voters(int32_t epoch, Clock::time_point now) {
        pubsub_itc_fw_app::LeaseRequest request{};
        request.candidate_instance_id = self_id_;
        request.group = group_;
        request.epoch = epoch;
        request.request_id = participant_.record_request(peer_id_, now);
        links_.send_request_to_peer(request);
        request.request_id = participant_.record_request(third_voter_id_, now);
        links_.send_request_to_third_voter(request);
    }

    std::string owner_name_;
    pubsub_itc_fw::QuillLogger& logger_;
    LeaseLinksInterface& links_;
    pubsub_itc_fw_app::ComponentGroup group_;
    int64_t self_id_;
    int64_t peer_id_;
    int64_t third_voter_id_;
    std::string third_voter_name_;
    LeaseTiming timing_;
    LeaseParticipant participant_;
    std::mt19937 random_;
    Clock::time_point last_renewal_{};
    Clock::time_point asked_at_{};

    // How far beyond the true expiry a recorded promise is written. See promise_recorded().
    static constexpr std::chrono::seconds promise_record_margin{10};

    // How close the promise's expiry may come to the record's before a fresh record is written in
    // the background: half the margin, five seconds, which leaves a write that long to finish before
    // the lease thread would have to write it itself.
    static constexpr std::chrono::seconds refresh_ahead{promise_record_margin / 2};

    // A promise record write slower than this is logged. A write normally takes well under a
    // millisecond; fifty is long enough that nothing ordinary reaches it and short enough to show
    // the writes that hold up leases.
    static constexpr std::chrono::milliseconds slow_promise_record_write{50};

    // Where this instance's promises are kept, or nullptr if they are not kept.
    LeasePromiseRecorderInterface* recorder_{nullptr};

    // The instance named in the record, and the expiry recorded. The record covers a promise to that
    // instance that runs out before the expiry.
    int64_t recorded_to_{0};
    Clock::time_point recorded_until_{};

    // A record being written in the background, and what it will say once written. See refresh_in_background().
    bool background_in_flight_{false};
    int64_t background_promised_to_{0};
    Clock::time_point background_until_{};

    // Until when this instance, restarted while it led, is resuming that lead; see on_refusal().
    Clock::time_point resuming_lead_until_{};

    // Whether the group having no leader has been reported since this instance last led or agreed
    // that its peer leads. Reported once, as a warning, rather than at every attempt.
    bool reported_no_leader_{false};
};

} // namespaces
