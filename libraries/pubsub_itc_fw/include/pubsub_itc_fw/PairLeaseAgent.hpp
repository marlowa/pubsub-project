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

#include <lease_messages.hpp>
#include <pubsub_itc_fw/BackgroundPromiseRecorderInterface.hpp>
#include <pubsub_itc_fw/LeaderStatement.hpp>
#include <pubsub_itc_fw/LeaseLinksInterface.hpp>
#include <pubsub_itc_fw/LeaseParticipant.hpp>
#include <pubsub_itc_fw/LeasePromiseRecord.hpp>
#include <pubsub_itc_fw/LeasePromiseStore.hpp>
#include <pubsub_itc_fw/LeaseTiming.hpp>
#include <pubsub_itc_fw/PeerStatementsFlag.hpp>
#include <pubsub_itc_fw/TestEventChannel.hpp>

namespace pubsub_itc_fw {

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
 *   - In the sequencer pair, it carries the leader's statement about whether its peer may lead on
 *     every request, records the peer's statement when it grants, echoes it on the grant, and keeps
 *     the statement it holds on disk with its promise (rule 11 in
 *     docs/availability/a_follower_behind_does_not_lead.md). The leader acts on a command its peer
 *     lacks only once its own statement is on disk and another voter has echoed it.
 *
 * It never writes to the disk itself. In the sequencer and the matching engine it runs on the thread
 * that handles orders, which must never wait for the disk. Every record is written in the background by
 * a BackgroundPromiseRecorderInterface, and the agent learns at a later tick that the write has finished.
 * A promise to the peer that the record on disk does not yet cover is refused with the reason
 * recording_promise, and granted when the peer asks again after the write has finished.
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
     * @param[in] pair_id The number of the pair this instance belongs to, which every voter in the pair is configured with.
     * @param[in] self_id This instance's id: 1 for the primary, 2 for the secondary.
     * @param[in] peer_id The other instance's id.
     * @param[in] third_voter_id The third voter's id, which appears on its grants and refusals.
     * @param[in] third_voter_name How the third voter is named in the log: "the arbiter" or "the witness".
     * @param[in] timing The lease timings, which every voter shares.
     * @param[in] started_at When this instance started.
     * @param[in] persisted_epoch The highest epoch this instance knew of when it last stopped, or zero.
     * @param[in] peer_statements Whether this pair's leader says whether its peer may lead: only the sequencer pair's does.
     */
    PairLeaseAgent(std::string owner_name, pubsub_itc_fw::QuillLogger& logger, LeaseLinksInterface& links, int32_t pair_id, int64_t self_id, int64_t peer_id,
                   int64_t third_voter_id, std::string third_voter_name, const LeaseTiming& timing, Clock::time_point started_at, int32_t persisted_epoch,
                   PeerStatementsFlag peer_statements)
        : owner_name_(std::move(owner_name))
        , logger_(logger)
        , links_(links)
        , pair_id_(pair_id)
        , self_id_(self_id)
        , peer_id_(peer_id)
        , third_voter_id_(third_voter_id)
        , third_voter_name_(std::move(third_voter_name))
        , timing_(timing)
        , participant_(self_id, timing.period, timing.drift_allowance, started_at, persisted_epoch, peer_statements)
        , random_(static_cast<std::mt19937::result_type>(self_id)) {
        // Measured from the end of the first lease period, during which neither instance may ask at all.
        if (self_id > peer_id) {
            participant_.stop(started_at, timing.period + timing.renewal_interval);
        }
    }

    /**
     * @brief Report what happens to this instance's lease to the test that started the component, through @p channel
     *        (docs/availability/test_events.md). Without it, nothing is reported.
     */
    void report_test_events_to(TestEventChannel& channel) {
        test_events_ = &channel;
    }

    /**
     * @brief Keep this instance's promises as a voter, written by @p recorder, and resume with the one it recorded before restarting.
     *
     * Called once, straight after construction, by an owner that can keep a record on disk. With a
     * record from this boot of the machine, the instance does not wait out a lease period after
     * starting: it knows what it promised, so it can vote at once, and ask to lead at once if it
     * promised nothing still running. A process restarted by its supervisor within the lease period
     * can then keep the lead it held, rather than being overtaken by its peer. Without a record it
     * waits, as any voter that has forgotten must.
     *
     * @p kept_statement is the statement the record holds, whichever boot wrote it: unlike a promise,
     * it does not depend on the clock, so it is taken back even when the promise is not.
     */
    void keep_promises_in(BackgroundPromiseRecorderInterface& recorder, const std::optional<LeasePromiseStore::Record>& kept,
                          const LeaderStatement& kept_statement, Clock::time_point now) {
        recorder_ = &recorder;
        participant_.restore_recorded_statement(kept_statement);
        recorded_statement_ = kept_statement;
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
            PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info,
                       "{}: resuming with the promise recorded before restarting -- this instance was leading, so it asks to lead again at once", owner_name_);
            report(LeaseChange::ResumingLead, Voter::NoVoter, participant_.epoch());
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
        start_wanted_record();
        write_statement_if_needed();
        report_whether_this_instance_may_lead();
        Change change = Change::Nothing;
        if (!asks_to_lead_ && participant_.state() == LeaseHolder::State::Candidate) {
            // Asking when it was told to stop: the request is abandoned, so that a grant arriving for it finds no candidate.
            participant_.stop(now, wait_before_asking_again());
        }
        if (participant_.stop_if_lease_ran_out(now)) {
            PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Warning,
                       "{}: lease ran out -- neither the peer nor {} renewed it in time, so this instance stops leading (epoch={})", owner_name_,
                       third_voter_name_, participant_.epoch());
            report(LeaseChange::RanOut, Voter::NoVoter, participant_.epoch());
            change = Change::StoppedLeading;
        }

        if (participant_.acting(now) && now - last_renewal_ >= timing_.renewal_interval) {
            last_renewal_ = now;
            ask_voters(participant_.epoch(), now);
        }

        if (participant_.state() == LeaseHolder::State::Candidate && peer_is_recording_promise_ && now - last_renewal_ >= timing_.renewal_interval) {
            // The peer refused only because the promise of its vote was not yet on disk. It will grant
            // once it is, so this instance asks again now rather than waiting out the lease period.
            peer_is_recording_promise_ = false;
            last_renewal_ = now;
            PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Debug, "{}: asking again to lead at epoch {}, now that the peer has had time to record its promise",
                       owner_name_, participant_.epoch());
            ask_voters(participant_.epoch(), now);
        }

        if (participant_.state() == LeaseHolder::State::Candidate && now - asked_at_ > timing_.period) {
            if (!reported_no_leader_) {
                PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Warning,
                           "{}: no voter granted a lease within a lease period of asking to lead at epoch {} -- this group has no leader. If the leader and "
                           "every arbiter are unavailable, it stays without one until an arbiter or the peer returns, because this instance cannot tell a "
                           "dead leader from being cut off (R-0147). Asking again",
                           owner_name_, participant_.epoch());
                reported_no_leader_ = true;
                report(LeaseChange::NoLeader, Voter::NoVoter, participant_.epoch());
            } else {
                PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Debug, "{}: still no grant after asking to lead at epoch {} -- asking again", owner_name_,
                           participant_.epoch());
            }
            participant_.stop(now, wait_before_asking_again());
        }

        if (asks_to_lead_ && participant_.may_ask_to_lead(now)) {
            const int32_t epoch = participant_.begin_asking_to_lead();
            asked_at_ = now;
            peer_is_recording_promise_ = false;
            peer_refused_promised_elsewhere_ = false;
            third_voter_refused_promised_elsewhere_ = false;
            last_renewal_ = now;
            PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info, "{}: asking the peer and {} to let this instance lead at epoch {}", owner_name_,
                       third_voter_name_, epoch);
            report(LeaseChange::AskingToLead, Voter::NoVoter, epoch);
            ask_voters(epoch, now);
        }
        return change;
    }

    /**
     * @brief The peer asks for a lease. The answer is sent back on @p reply_to.
     * @return AgreedPeerLeads when the request was granted.
     */
    [[nodiscard]] Change on_request(const pubsub_itc_fw::ConnectionID& reply_to, int64_t candidate_id, int32_t epoch, int64_t request_id,
                                    const LeaderStatement& statement, Clock::time_point now) {
        // A statement newer than the one held replaces it before the request is decided, whether or not
        // the request is then granted. A request carries a statement only while its sender leads, and a
        // statement made at a later epoch was made by an instance that led at that epoch, which it could do
        // only with a majority that honoured the statements then current; so adopting it cannot let an
        // instance that lacks commands the engine acted on lead. Without this, an instance holding an older
        // statement that its peer may not lead would refuse that peer for ever, even once the peer leads
        // at a higher epoch, and so never learn the peer's newer statement (docs/bug_list.md, BUG-0152).
        if (statement.leader_id != 0 && statement.newer_than(participant_.recorded_statement())) {
            static_cast<void>(participant_.record_peer_statement(statement));
        }
        const LeaseParticipant::PeerRequestOutcome outcome = participant_.on_peer_request(candidate_id, epoch, now);
        if (outcome.gave_up_asking_to_lead) {
            PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info,
                       "{}: the peer asks to lead at epoch {}, above the epoch this instance asked for -- giving way to it", owner_name_, epoch);
        }

        // The peer's statement, recorded in memory above or here, reaches the disk in the background; the
        // grant does not wait for it (docs/availability/a_follower_behind_does_not_lead.md, 4.5). A grant
        // echoes it only if it is the statement now held.
        int64_t echoed = 0;
        if (outcome.answer.verdict == LeaseVoter::Verdict::Granted && statement.leader_id != 0) {
            echoed = participant_.record_peer_statement(statement);
        }

        // A grant the record on disk does not cover is refused, and the record that covers it is
        // started in the background: a restart would forget a promise that is not on disk. The voter
        // has made the promise in memory, which only makes it stricter until the promise runs out.
        const bool covered = outcome.answer.verdict == LeaseVoter::Verdict::Granted && promise_recorded(candidate_id);

        if (covered) {
            reported_no_leader_ = false;
            write_statement_if_needed();
            report_whether_this_instance_may_lead();
            pubsub_itc_fw::LeaseGrant grant{};
            grant.voter_instance_id = self_id_;
            grant.pair_id = pair_id_;
            grant.epoch = epoch;
            grant.request_id = request_id;
            grant.echoed_statement_number = echoed;
            links_.send_grant(reply_to, grant);
            return Change::AgreedPeerLeads;
        }

        pubsub_itc_fw::LeaseRefusal refusal{};
        refusal.voter_instance_id = self_id_;
        refusal.pair_id = pair_id_;
        refusal.highest_epoch = outcome.answer.highest_epoch;
        refusal.request_id = request_id;
        refusal.reason = outcome.answer.verdict == LeaseVoter::Verdict::Granted ? pubsub_itc_fw::LeaseRefusalReason::recording_promise
                                                                                : refusal_reason_for(outcome.answer.verdict);
        links_.send_refusal(reply_to, refusal);
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Debug, "{}: refused the peer's request to lead at epoch {} ({})", owner_name_, epoch,
                   pubsub_itc_fw::to_string(refusal.reason));
        return Change::Nothing;
    }

    /**
     * @brief A voter granted one of this instance's requests.
     * @return BecameLeader when this instance now leads.
     */
    [[nodiscard]] Change on_grant(int64_t voter_id, int32_t epoch, int64_t request_id, int64_t echoed_statement_number, Clock::time_point now) {
        const LeaseHolder::Event event = participant_.on_grant(voter_id, request_id, epoch, now);
        participant_.note_echo(epoch, echoed_statement_number);
        if (event == LeaseHolder::Event::LeaseExtended) {
            record_leading();
        }
        if (event != LeaseHolder::Event::BecameLeader) {
            return Change::Nothing;
        }
        reported_no_leader_ = false;
        peer_is_recording_promise_ = false;
        resuming_lead_until_ = Clock::time_point{};
        record_leading();
        if (participant_.statement().leader_id != 0) {
            // A new leadership starts with a statement about its peer -- in the sequencer pair that the
            // peer may not lead, and the owner may wait on an echo of that before acting on a command its
            // peer lacks -- and it is sent now rather than at the first renewal.
            last_renewal_ = now;
            ask_voters(participant_.epoch(), now);
        }
        PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info, "{}: leading at epoch {} -- granted a lease by {}, which with its own vote is a majority",
                   owner_name_, epoch, voter_name(voter_id));
        report(LeaseChange::Granted, voter_id == peer_id_ ? Voter::Peer : Voter::ThirdVoter, epoch);
        return Change::BecameLeader;
    }

    /**
     * @brief A voter refused one of this instance's requests.
     * @return StoppedLeading when this instance led and has learnt of a newer generation.
     */
    [[nodiscard]] Change on_refusal(int64_t voter_id, int32_t highest_epoch, int64_t request_id, pubsub_itc_fw::LeaseRefusalReason reason,
                                    Clock::time_point now) {
        const bool was_leading = participant_.state() == LeaseHolder::State::Leading;
        const int32_t epoch_before = participant_.epoch();
        if (reason == pubsub_itc_fw::LeaseRefusalReason::recording_promise && voter_id == peer_id_ && participant_.state() == LeaseHolder::State::Candidate) {
            peer_is_recording_promise_ = true;
        }
        // An instance resuming the lead it recorded asks again above a newer epoch at once, rather than
        // after the usual random wait. The wait keeps two candidates from colliding again, and there is
        // no second candidate: both other voters promised their votes to this instance. It matters to an
        // arbiter, which keeps no epoch on disk and so first asks below the epoch it led in; waiting
        // could let those promises run out, and the other arbiter take over.
        const Clock::duration wait = now < resuming_lead_until_ ? Clock::duration::zero() : Clock::duration{wait_before_asking_again()};
        if (reason == pubsub_itc_fw::LeaseRefusalReason::promised_elsewhere && participant_.state() == LeaseHolder::State::Candidate) {
            (voter_id == third_voter_id_ ? third_voter_refused_promised_elsewhere_ : peer_refused_promised_elsewhere_) = true;
        }
        if (peer_refused_promised_elsewhere_ && third_voter_refused_promised_elsewhere_ && participant_.state() == LeaseHolder::State::Candidate) {
            // Both other voters refused because their votes are promised elsewhere. The peer's is held for itself, so the
            // peer leads or is asking to lead; the third voter is no member of the pair and holds no vote of its own, so
            // its vote is promised to the peer. The peer therefore leads, or is about to. While this instance asks, it
            // refuses the peer every request, the renewals of a peer that leads included, and so never agrees that the
            // peer leads. Asking again after the usual short wait would refuse the peer again, and a leader renewing once
            // a second can miss every short gap for many seconds (BUG-0195). So this instance gives up the request and
            // does not ask again for a lease period: in that time it grants the leading peer's next renewal and follows,
            // or, if the peer has died, the third voter's promise runs out and this instance asks as it would have.
            // Giving up a request is always safe; it only delays a leader. Both refusals are needed: after the third
            // voter's alone, the peer may yet grant, and this instance lead.
            peer_refused_promised_elsewhere_ = false;
            third_voter_refused_promised_elsewhere_ = false;
            PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info,
                       "{}: the peer holds its own vote and {} has promised its vote to the peer -- giving up asking to lead at epoch {} for a lease period, "
                       "so that the peer's requests can be granted",
                       owner_name_, third_voter_name_, epoch_before);
            participant_.stop(now, timing_.period + wait);
            return Change::Nothing;
        }
        if (participant_.on_refusal(request_id, highest_epoch, now, wait) != LeaseHolder::Event::NewerEpochKnown) {
            PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Debug, "{}: {} refused a request ({})", owner_name_, voter_name(voter_id),
                       pubsub_itc_fw::to_string(reason));
            return Change::Nothing;
        }
        PUBSUB_LOG(logger_, was_leading ? pubsub_itc_fw::FwLogLevel::Warning : pubsub_itc_fw::FwLogLevel::Info,
                   "{}: {} has granted epoch {}, above this instance's {} -- a newer generation exists, so this instance stops {} and will ask again above it",
                   owner_name_, voter_name(voter_id), highest_epoch, epoch_before, was_leading ? "leading" : "asking");
        return was_leading ? Change::StoppedLeading : Change::Nothing;
    }

    /// Whether this instance leads, whether or not its lease has yet run out: what say_peer_may_not_lead() requires.
    [[nodiscard]] bool leading() const {
        return participant_.state() == LeaseHolder::State::Leading;
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

    /**
     * @brief Say that the peer may not lead, before having the matching engine act on a command the peer does not hold.
     *
     * The statement is written to disk in the background and sent to both other voters at once. The
     * owner acts on such a command only once may_act_without_peer() is true. Only in a pair constructed
     * with PeerStatementsFlag::SayWhetherPeerMayLead, and only while leading.
     */
    void begin_running_without_peer(Clock::time_point now) {
        participant_.begin_running_without_peer();
        statement_changed(now);
    }

    /**
     * @brief Say that the peer may lead again.
     *
     * Call only once the peer has acknowledged every record this instance holds, and after going back
     * to acting only on commands the peer holds.
     */
    void peer_holds_everything(Clock::time_point now) {
        participant_.peer_holds_everything();
        statement_changed(now);
    }

    /**
     * @brief Say that the peer may not lead, for a reason of the owner's, such as the two instances being configured differently.
     *
     * The statement is written to disk in the background and sent to both other voters at once; a voter
     * holding it refuses the peer a lease. Only in a pair constructed with
     * PeerStatementsFlag::SayWhenPeerMayNotLead or SayWhetherPeerMayLead, and only while leading.
     */
    void say_peer_may_not_lead(Clock::time_point now) {
        participant_.say_peer_may_not_lead();
        statement_changed(now);
    }

    /// Say that the peer may lead again, once the owner's reason for saying it may not has gone. As say_peer_may_not_lead.
    void say_peer_may_lead(Clock::time_point now) {
        participant_.say_peer_may_lead();
        statement_changed(now);
    }

    /**
     * @brief Whether the owner may have the matching engine act on a command its peer does not hold.
     *
     * True only while this instance leads saying its peer may not lead, that statement is on disk, and
     * a voter other than this instance has echoed it.
     */
    [[nodiscard]] bool may_act_without_peer() const {
        return participant_.may_act_without_peer() && recorder_ != nullptr && recorded_statement_.same_as(participant_.statement());
    }

    /**
     * @brief From now on this instance never asks to lead, and a request it is making is abandoned at the next tick. It
     *        goes on answering its peer's requests, so the peer may lead.
     *
     * For an owner that has found it cannot be trusted to lead, such as a matching engine whose reports differ from
     * the leader's, until its process is restarted. Not for an instance that leads: one that should stop leading lets
     * its lease run out. Fewer instances asking cannot give two leaders, so the lease rules are unaffected.
     */
    void stop_asking_to_lead() {
        asks_to_lead_ = false;
    }

    /// Whether this instance asks to lead when the rules allow it; false once stop_asking_to_lead has been called.
    [[nodiscard]] bool asks_to_lead() const {
        return asks_to_lead_;
    }

    /// Whether this instance, leading, says its peer may lead. True in a pair that makes no statements.
    [[nodiscard]] bool says_peer_may_lead() const {
        return participant_.statement().leader_id == 0 || participant_.statement().peer_may_lead;
    }

  private:
    // What happened to this instance's lease, for the test that started the component, if there is one.
    void report(LeaseChange change, Voter granted_by, int32_t epoch) {
        if (test_events_ != nullptr) {
            test_events_->lease_changed(owner_name_, self_id_, change, granted_by, epoch);
        }
    }

    // The leader's own statement changed: start writing it, and tell both other voters at once rather
    // than at the next renewal.
    void statement_changed(Clock::time_point now) {
        write_statement_if_needed();
        if (participant_.acting(now)) {
            last_renewal_ = now;
            ask_voters(participant_.epoch(), now);
        }
    }

    /**
     * @brief Start writing the record when the statement held is not the one on disk.
     *
     * The promise in the record is written unchanged. If a write is already in progress, or a record
     * is waiting to be started, this is tried again at the next tick: the record that is written next
     * carries the statement held at the moment it is started.
     */
    void write_statement_if_needed() {
        if (recorder_ == nullptr || background_in_flight_ || record_wanted_ || recorded_statement_.same_as(participant_.recorded_statement())) {
            return;
        }
        start_record(recorded_to_, recorded_until_);
    }

    // Says once, when it changes, whether this instance holds a statement that it may not lead.
    void report_whether_this_instance_may_lead() {
        const bool may_not_lead = participant_.recorded_statement().instance_that_may_not_lead() == self_id_;
        if (may_not_lead == reported_may_not_lead_) {
            return;
        }
        reported_may_not_lead_ = may_not_lead;
        if (may_not_lead) {
            PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Warning,
                       "{}: the peer, leading at epoch {}, says this instance may not lead (the peer's Quill log says why). It does not ask to lead "
                       "until the peer says otherwise",
                       owner_name_, participant_.recorded_statement().epoch);
        } else {
            PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Info, "{}: no statement that this instance may not lead is held any longer -- it may lead again",
                       owner_name_);
        }
    }

    /**
     * @brief Whether the record on disk covers the promise just made to @p candidate_id; if it does not, start the write that will.
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
        const Clock::time_point expires_at = participant_.promise_expires_at();
        if (recorded_to_ == candidate_id && expires_at <= recorded_until_) {
            refresh_in_background(candidate_id, expires_at);
            return true;
        }
        if (!write_in_progress_covers(candidate_id, expires_at)) {
            start_record(candidate_id, expires_at + promise_record_margin);
        }
        return false;
    }

    /**
     * @brief Start writing a record that names @p promised_to until @p until, with the statement held now.
     *
     * Only one write is in progress at a time. If one is, the record is kept and started at the next
     * tick after that write has finished, replacing any record kept earlier: the record asked for last
     * is the one that describes the promise this instance holds now.
     */
    void start_record(int64_t promised_to, Clock::time_point until) {
        const LeasePromiseRecord record{promised_to, until, participant_.recorded_statement()};
        if (!background_in_flight_ && recorder_->record_in_background(record)) {
            background_in_flight_ = true;
            background_record_ = record;
            background_started_at_ = Clock::now();
            record_wanted_ = false;
            return;
        }
        record_wanted_ = true;
        wanted_to_ = promised_to;
        wanted_until_ = until;
    }

    // Start the record kept by start_record() while another write was in progress, once that write has finished.
    void start_wanted_record() {
        if (recorder_ != nullptr && record_wanted_ && !background_in_flight_) {
            start_record(wanted_to_, wanted_until_);
        }
    }

    // Whether the write in progress, or the one waiting to be started, will cover a promise to
    // @p promised_to that runs out at @p expires_at, so that no other need be asked for.
    [[nodiscard]] bool write_in_progress_covers(int64_t promised_to, Clock::time_point expires_at) const {
        if (record_wanted_) {
            return wanted_to_ == promised_to && expires_at <= wanted_until_;
        }
        return background_in_flight_ && background_record_.promised_to == promised_to && expires_at <= background_record_.until;
    }

    /**
     * @brief Take the outcome of a background write, if one has finished, as what the record now says.
     *
     * Called at every tick and before the record is consulted, so a finished write is adopted within
     * one tick. A write that failed leaves the record as it was; a promise it does not cover goes on
     * being refused, and the write is asked for again the next time it is needed.
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
        const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - background_started_at_);
        if (took >= slow_promise_record_write) {
            PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Warning,
                       "{}: writing the lease promise record in the background took {} ms -- until it finished, this instance refused any promise the "
                       "record on disk did not cover",
                       owner_name_, took.count());
        }
        if (*outcome) {
            recorded_to_ = background_record_.promised_to;
            recorded_until_ = background_record_.until;
            recorded_statement_ = background_record_.statement;
            reported_write_failed_ = false;
            return;
        }
        if (!reported_write_failed_) {
            PUBSUB_LOG(logger_, pubsub_itc_fw::FwLogLevel::Error,
                       "{}: could not write the lease promise record -- this instance refuses any promise the record on disk does not cover, rather than "
                       "make a promise a restart would forget, and tries again each time one is needed",
                       owner_name_);
            reported_write_failed_ = true;
        }
    }

    /**
     * @brief Start writing a fresh record when the one held is close to no longer covering the promise.
     *
     * The record held still covers the promise, so nothing waits for this write. It is started when
     * fewer than refresh_ahead remain between the promise's expiry and the record's, which is half the
     * margin, so that in normal operation the fresh record is on disk seconds before it is needed and
     * no promise is ever refused for want of it.
     */
    void refresh_in_background(int64_t promised_to, Clock::time_point expires_at) {
        if (background_in_flight_ || record_wanted_ || recorded_until_ - expires_at > refresh_ahead) {
            return;
        }
        start_record(promised_to, expires_at + promise_record_margin);
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
     * It is written in the background, and nothing waits for it, because it is not needed for safety.
     * Rule 5 means an instance leads only once its promise to its peer has run out, so replacing that
     * promise in the record loses nothing. Until the record is written, or if it cannot be, the record
     * holds an older promise, and a restart reading it is stricter than it needs to be, never less strict.
     */
    void record_leading() {
        if (recorder_ == nullptr || participant_.state() != LeaseHolder::State::Leading) {
            return;
        }
        adopt_background_record();
        const Clock::time_point expires_at = participant_.lease_expires_at();
        if (recorded_to_ == self_id_ && expires_at <= recorded_until_) {
            refresh_in_background(self_id_, expires_at);
            return;
        }
        if (!write_in_progress_covers(self_id_, expires_at)) {
            start_record(self_id_, expires_at + promise_record_margin);
        }
    }

    static pubsub_itc_fw::LeaseRefusalReason refusal_reason_for(LeaseVoter::Verdict verdict) {
        switch (verdict) {
            case LeaseVoter::Verdict::RefusedWhileRestarting:
                return pubsub_itc_fw::LeaseRefusalReason::restarting;
            case LeaseVoter::Verdict::RefusedPromisedElsewhere:
                return pubsub_itc_fw::LeaseRefusalReason::promised_elsewhere;
            case LeaseVoter::Verdict::RefusedEpochBehind:
                return pubsub_itc_fw::LeaseRefusalReason::epoch_behind;
            case LeaseVoter::Verdict::RefusedMayNotLead:
                return pubsub_itc_fw::LeaseRefusalReason::may_not_lead;
            case LeaseVoter::Verdict::Granted:
                break;
        }
        return pubsub_itc_fw::LeaseRefusalReason::unknown;
    }

    [[nodiscard]] std::string voter_name(int64_t voter_id) const {
        return voter_id == third_voter_id_ ? third_voter_name_ : "the peer";
    }

    [[nodiscard]] std::chrono::milliseconds wait_before_asking_again() {
        const int64_t quarter = timing_.renewal_interval.count() / 4;
        return std::chrono::milliseconds{std::uniform_int_distribution<int64_t>(quarter, 3 * quarter)(random_)};
    }

    void ask_voters(int32_t epoch, Clock::time_point now) {
        pubsub_itc_fw::LeaseRequest request{};
        request.candidate_instance_id = self_id_;
        request.pair_id = pair_id_;
        request.epoch = epoch;
        if (participant_.state() == LeaseHolder::State::Leading) {
            const LeaderStatement& statement = participant_.statement();
            request.statement_leader_id = statement.leader_id;
            request.statement_epoch = statement.epoch;
            request.statement_number = statement.number;
            request.peer_may_lead = statement.peer_may_lead;
        }
        request.request_id = participant_.record_request(peer_id_, now);
        links_.send_request_to_peer(request);
        request.request_id = participant_.record_request(third_voter_id_, now);
        links_.send_request_to_third_voter(request);
    }

    std::string owner_name_;
    pubsub_itc_fw::QuillLogger& logger_;
    LeaseLinksInterface& links_;
    int32_t pair_id_;
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
    // a promise would have to be refused for want of it.
    static constexpr std::chrono::seconds refresh_ahead{promise_record_margin / 2};

    // A background write slower than this, from being started to its outcome being read, is logged. A
    // write normally takes well under a millisecond, and its outcome is read at the next tick, within
    // a tenth of a second. Half a second is long enough that nothing ordinary reaches it and short
    // enough to show a disk slow enough to make the peer ask more than once.
    static constexpr std::chrono::milliseconds slow_promise_record_write{500};

    // What writes this instance's promises, or nullptr if they are not kept.
    BackgroundPromiseRecorderInterface* recorder_{nullptr};
    TestEventChannel* test_events_{nullptr};

    // The instance named in the record, and the expiry recorded. The record covers a promise to that
    // instance that runs out before the expiry.
    int64_t recorded_to_{0};
    Clock::time_point recorded_until_{};

    // The statement the record on disk holds.
    LeaderStatement recorded_statement_{};

    // A record being written in the background, what it will say once written, and when it was started.
    bool background_in_flight_{false};
    LeasePromiseRecord background_record_{};
    Clock::time_point background_started_at_{};

    // A record asked for while another write was in progress, to be started once that write has
    // finished. See start_record().
    bool record_wanted_{false};
    int64_t wanted_to_{0};
    Clock::time_point wanted_until_{};

    // Whether the peer refused this instance's request to lead only because it was still writing the
    // promise of its vote to disk. While asking to lead, this instance then asks again at the next
    // renewal interval instead of waiting out the lease period.
    bool peer_is_recording_promise_{false};
    // Whether the peer and the third voter have each refused the present request to lead because their votes are
    // promised elsewhere (on_refusal). Cleared when a request to lead begins.
    bool peer_refused_promised_elsewhere_{false};
    bool third_voter_refused_promised_elsewhere_{false};

    // Until when this instance, restarted while it led, is resuming that lead; see on_refusal().
    Clock::time_point resuming_lead_until_{};

    // Whether the group having no leader has been reported since this instance last led or agreed
    // that its peer leads. Reported once, as a warning, rather than at every attempt.
    bool reported_no_leader_{false};

    // Whether this instance was last reported as holding a statement that it may not lead.
    bool reported_may_not_lead_{false};
    // False once the owner has said this instance is not to lead (stop_asking_to_lead).
    bool asks_to_lead_{true};

    // Whether a failure to write the record has been reported and has not since been cured.
    bool reported_write_failed_{false};
};

} // namespaces
