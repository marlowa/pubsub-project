// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

/*
 * A randomised simulation of one pair of instances and the third voter, all using the lease classes
 * the components use. It is the C++ counterpart of docs/availability/tla/MajorityLeaseHA.tla: the
 * model checker established that the rules are right, and this establishes that the code implements
 * them.
 *
 * Messages take anything from no time to longer than a lease period to arrive, and some are lost.
 * Links fail and recover. Either instance and the third voter crash and restart, forgetting
 * everything but what is kept on disk: an instance's epoch, its log, and the leader's statement it
 * holds; and the arbiter's statement, which the arbiter keeps on disk and copies to the passive
 * arbiter. After every step the simulation checks that two instances are never both acting as leader,
 * and that no epoch is ever led by two instances.
 *
 * The leader also writes commands to its log, copies its log to its peer, and has the matching engine
 * act on commands. Copying to the peer sometimes stalls, and then the leader runs without its peer
 * under rule 11 (docs/availability/a_follower_behind_does_not_lead.md): it says that its peer may not
 * lead, and acts on commands its peer lacks only once another voter has echoed that statement. The
 * simulation checks that an instance starting to act as leader holds every command the engine has
 * acted on. A voter here records a statement durably before it grants; the design lets the follower
 * write it in the background, and findings.md section 12.5 describes the risk that leaves.
 *
 * Some runs are deliberately broken, because a check that cannot fail shows nothing. In one the third
 * voter's clock runs further ahead than the drift allowance covers, so it forgets its promises early,
 * and two leaders must be found acting at once. In others one part of rule 11 is removed, and an
 * instance must be found leading without a command the engine acted on.
 */

#include <chrono>
#include <cstdint>
#include <iostream>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <gtest/gtest.h>

#include <LeaderStatement.hpp>
#include <LeaseHolder.hpp>
#include <LeaseParticipant.hpp>
#include <LeaseVoter.hpp>
#include <PeerStatementsFlag.hpp>

namespace {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

constexpr milliseconds tick{50};
constexpr milliseconds lease_period{3000};
constexpr milliseconds drift_allowance{250};
constexpr milliseconds renewal_interval{1000};
constexpr milliseconds give_up_asking_after{2000};
constexpr int64_t arbiter_id = 3;
// The simulated pair is the sequencer pair, the one whose leader says whether its peer may lead.
const fix_common::PeerStatementsFlag statements{fix_common::PeerStatementsFlag::SayWhetherPeerMayLead};

struct Options {
    bool failures{true};
    bool slow_and_lossy_messages{true};
    bool arbiter_down_for_good{false};
    // How far ahead of the instances' clocks the arbiter's clock runs over one lease period. Up to the
    // drift allowance the lease rules absorb it; beyond it, the arbiter forgets promises too early.
    milliseconds arbiter_clock_gain_per_lease{0};
    // Each of these is part of rule 11, and is true in the design. Setting one false breaks the rule.
    bool leader_waits_for_an_echo{true};
    bool arbiter_keeps_its_statement{true};
    bool instances_keep_their_statement{true};
};

// An instance's log: the commands it holds, kept on disk, in the order it received them.
struct Log {
    std::vector<int64_t> entries;
    std::set<int64_t> held;

    void add(int64_t command) {
        if (held.insert(command).second) {
            entries.push_back(command);
        }
    }

    bool holds(int64_t command) const {
        return held.count(command) != 0;
    }
};

class PairSimulation {
  public:
    PairSimulation(uint32_t seed, const Options& options) : random_(seed), options_(options) {
        for (int i = 0; i < 2; ++i) {
            instances_[i].emplace(i + 1, lease_period, drift_allowance, now_, 0, statements);
        }
        if (!options_.arbiter_down_for_good) {
            start_arbiter();
        }
    }

    /// Runs @p steps ticks. Returns a description of the first violation, or an empty string.
    std::string run(int steps) {
        for (int step = 0; step < steps; ++step) {
            now_ += tick;
            if (options_.failures) {
                inject_failures();
            }
            deliver_due_messages();
            for (int i = 0; i < 2; ++i) {
                act(i);
            }
            if (acting(0) && acting(1)) {
                return fmt::format("step {}: instances 1 and 2 are both acting as leader, at epochs {} and {}", step, instances_[0]->epoch(),
                                   instances_[1]->epoch());
            }
            check_the_leader_holds_every_acted_command(step);
            if (!violation_.empty()) {
                return violation_;
            }
            if (acting(0) || acting(1)) {
                ++steps_with_a_leader_;
            }
            last_step_had_leader_ = acting(0) || acting(1);
        }
        return {};
    }

    int steps_with_a_leader() const {
        return steps_with_a_leader_;
    }

    int elections() const {
        return elections_;
    }

    bool last_step_had_leader() const {
        return last_step_had_leader_;
    }

    /// Crash an instance and keep it down, for scenarios that need a particular failure.
    void crash_for_good(int instance_index) {
        crash_instance(instance_index);
        kept_down_[instance_index] = true;
    }

    int acting_instance() const {
        for (int i = 0; i < 2; ++i) {
            if (acting(i)) {
                return i + 1;
            }
        }
        return 0;
    }

    /// Commands the engine acted on that the peer of the acting leader did not hold at the time.
    int64_t commands_acted_on_without_the_peer() const {
        return commands_acted_on_without_the_peer_;
    }

    /// Times an instance began acting as leader after the other had had the engine act on commands.
    int changes_of_leader_after_acting() const {
        return changes_of_leader_after_acting_;
    }

  private:
    enum class Kind { Request, Grant, Refusal };

    struct Message {
        Kind kind{Kind::Request};
        int64_t from{0};
        int64_t to{0};
        int32_t epoch{0};
        int64_t request_id{0};
        Clock::time_point deliver_at{};
        fix_common::LeaderStatement statement{}; ///< on a request from a leader; leader_id is zero otherwise
        int64_t echoed{0};                       ///< on a grant: the statement number the voter recorded
    };

    bool acting(int i) const {
        return instances_[i].has_value() && instances_[i]->acting(now_);
    }

    // The arbiter's clock, which may run ahead of the instances' clocks.
    Clock::time_point arbiter_now() const {
        const auto elapsed = now_.time_since_epoch();
        const auto gain = elapsed * options_.arbiter_clock_gain_per_lease.count() / lease_period.count();
        return now_ + std::chrono::duration_cast<Clock::duration>(gain);
    }

    void start_arbiter() {
        arbiter_.emplace(lease_period, arbiter_now(), 0);
        if (options_.arbiter_keeps_its_statement) {
            arbiter_->restore_recorded_statement(arbiter_statement_on_disk_);
        } else {
            arbiter_statement_on_disk_ = fix_common::LeaderStatement{};
        }
    }

    void start_instance(int i) {
        instances_[i].emplace(i + 1, lease_period, drift_allowance, now_, stored_epoch_[i], statements);
        if (options_.instances_keep_their_statement) {
            instances_[i]->restore_recorded_statement(stored_statement_[i]);
        } else {
            stored_statement_[i] = fix_common::LeaderStatement{};
        }
    }

    static int link_of(int64_t a, int64_t b) {
        if ((a == 1 && b == 2) || (a == 2 && b == 1)) {
            return 0;
        }
        if ((a == 1 && b == 3) || (a == 3 && b == 1)) {
            return 1;
        }
        return 2;
    }

    bool link_up(int64_t a, int64_t b) const {
        return now_ >= link_down_until_[link_of(a, b)];
    }

    bool running(int64_t id) const {
        if (id == arbiter_id) {
            return arbiter_.has_value();
        }
        return instances_[id - 1].has_value();
    }

    int random_between(int low, int high) {
        return std::uniform_int_distribution<int>(low, high)(random_);
    }

    bool chance(double probability) {
        return std::uniform_real_distribution<double>(0.0, 1.0)(random_) < probability;
    }

    milliseconds wait_before_asking_again() {
        return milliseconds{random_between(200, 800)};
    }

    void send(const Message& unsent) {
        if (!link_up(unsent.from, unsent.to)) {
            return;
        }
        milliseconds delay{random_between(0, 20)};
        if (options_.slow_and_lossy_messages) {
            if (chance(0.05)) {
                return;
            }
            const int band = random_between(0, 99);
            if (band >= 95) {
                delay = milliseconds{random_between(2000, 5000)};
            } else if (band >= 80) {
                delay = milliseconds{random_between(200, 2000)};
            } else {
                delay = milliseconds{random_between(0, 200)};
            }
        }
        Message message = unsent;
        message.deliver_at = now_ + delay;
        in_flight_.push_back(message);
    }

    void crash_instance(int i) {
        instances_[i].reset();
        // A crashed process's connections close, so what was on its way to it is lost with them.
        drop_messages_to(i + 1);
    }

    void drop_messages_to(int64_t id) {
        std::vector<Message> kept;
        for (const Message& message : in_flight_) {
            if (message.to != id) {
                kept.push_back(message);
            }
        }
        in_flight_.swap(kept);
    }

    void inject_failures() {
        for (int i = 0; i < 2; ++i) {
            if (instances_[i].has_value() && chance(0.002)) {
                crash_instance(i);
            } else if (!instances_[i].has_value() && !kept_down_[i] && chance(0.02)) {
                start_instance(i);
            }
        }
        if (!options_.arbiter_down_for_good) {
            if (arbiter_.has_value() && chance(0.002)) {
                arbiter_.reset();
                drop_messages_to(arbiter_id);
            } else if (!arbiter_.has_value() && chance(0.02)) {
                start_arbiter();
            }
        }
        if (now_ >= copying_stalled_until_ && chance(0.003)) {
            copying_stalled_until_ = now_ + milliseconds{random_between(200, 3000)};
        }
        for (int link = 0; link < 3; ++link) {
            if (now_ >= link_down_until_[link] && chance(0.003)) {
                link_down_until_[link] = now_ + milliseconds{random_between(500, 8000)};
                std::vector<Message> kept;
                for (const Message& message : in_flight_) {
                    if (link_of(message.from, message.to) != link) {
                        kept.push_back(message);
                    }
                }
                in_flight_.swap(kept);
            }
        }
    }

    void deliver_due_messages() {
        std::vector<Message> due;
        std::vector<Message> later;
        for (const Message& message : in_flight_) {
            (message.deliver_at <= now_ ? due : later).push_back(message);
        }
        in_flight_.swap(later);
        for (const Message& message : due) {
            if (running(message.to) && link_up(message.from, message.to)) {
                deliver(message);
            }
        }
    }

    void deliver(const Message& message) {
        if (message.kind == Kind::Request) {
            answer_request(message);
            return;
        }
        fix_common::LeaseParticipant& instance = *instances_[message.to - 1];
        const int i = static_cast<int>(message.to - 1);
        if (message.kind == Kind::Grant) {
            if (instance.on_grant(message.from, message.request_id, message.epoch, now_) == fix_common::LeaseHolder::Event::BecameLeader) {
                ++elections_;
                stored_statement_[i] = instance.recorded_statement();
                copied_through_[i] = 0;
                next_to_act_[i] = 0;
                const auto recorded = leader_of_epoch_.emplace(message.epoch, message.to);
                if (!recorded.second && recorded.first->second != message.to) {
                    violation_ = fmt::format("epoch {} was led by instance {} and then by instance {}", message.epoch, recorded.first->second, message.to);
                }
            }
            instance.note_echo(message.epoch, message.echoed);
        } else {
            static_cast<void>(instance.on_refusal(message.request_id, message.epoch, now_, wait_before_asking_again()));
        }
        stored_epoch_[message.to - 1] = instance.highest_epoch();
    }

    // A voter grants or refuses a request. Granting a leader's request, it records the statement the
    // request carries, durably, before the grant is sent, and the grant echoes the number it holds.
    void answer_request(const Message& message) {
        fix_common::LeaseVoter::Answer answer;
        int64_t echoed = 0;
        const bool carries_statement = message.statement.leader_id != 0;
        if (message.to == arbiter_id) {
            answer = arbiter_->consider(message.from, message.epoch, arbiter_now());
            if (answer.verdict == fix_common::LeaseVoter::Verdict::Granted && carries_statement) {
                arbiter_->record_statement(message.statement);
                arbiter_statement_on_disk_ = arbiter_->recorded_statement();
                echoed = arbiter_->holds_statement(message.statement) ? message.statement.number : 0;
            }
        } else {
            const int i = static_cast<int>(message.to - 1);
            fix_common::LeaseParticipant& voter = *instances_[i];
            answer = voter.on_peer_request(message.from, message.epoch, now_).answer;
            stored_epoch_[i] = voter.highest_epoch();
            if (answer.verdict == fix_common::LeaseVoter::Verdict::Granted && carries_statement) {
                echoed = voter.record_peer_statement(message.statement);
                stored_statement_[i] = voter.recorded_statement();
            }
        }
        Message reply;
        reply.from = message.to;
        reply.to = message.from;
        reply.request_id = message.request_id;
        if (answer.verdict == fix_common::LeaseVoter::Verdict::Granted) {
            reply.kind = Kind::Grant;
            reply.epoch = message.epoch;
            reply.echoed = echoed;
        } else {
            reply.kind = Kind::Refusal;
            reply.epoch = answer.highest_epoch;
        }
        send(reply);
    }

    void ask_each_voter(int i, int32_t epoch) {
        fix_common::LeaseParticipant& instance = *instances_[i];
        const int64_t self = i + 1;
        const int64_t peer = self == 1 ? 2 : 1;
        for (const int64_t voter : {peer, arbiter_id}) {
            Message request;
            request.kind = Kind::Request;
            request.from = self;
            request.to = voter;
            request.epoch = epoch;
            request.request_id = instance.record_request(voter, now_);
            if (instance.state() == fix_common::LeaseHolder::State::Leading) {
                request.statement = instance.statement();
            }
            send(request);
        }
    }

    // The acting leader writes commands, copies its log to its peer, and has the engine act on them.
    void handle_commands(int i) {
        fix_common::LeaseParticipant& instance = *instances_[i];
        const int peer = 1 - i;
        Log& log = logs_[i];
        if (chance(0.05)) {
            log.add(++last_command_);
        }
        const bool peer_reachable = instances_[peer].has_value() && link_up(i + 1, peer + 1);
        if (peer_reachable && now_ >= copying_stalled_until_ && chance(0.5)) {
            for (; copied_through_[i] < log.entries.size(); ++copied_through_[i]) {
                logs_[peer].add(log.entries[copied_through_[i]]);
            }
        }
        while (next_to_act_[i] < log.entries.size()) {
            const int64_t command = log.entries[next_to_act_[i]];
            if (acted_.count(command) == 0) {
                const bool peer_holds_it = logs_[peer].holds(command);
                if (!peer_holds_it && options_.leader_waits_for_an_echo && !instance.may_act_without_peer()) {
                    if (instance.statement().peer_may_lead && (!peer_reachable || chance(0.1))) {
                        instance.begin_running_without_peer();
                        stored_statement_[i] = instance.recorded_statement();
                        ask_each_voter(i, instance.epoch());
                    }
                    break;
                }
                acted_.insert(command);
                if (!peer_holds_it) {
                    ++commands_acted_on_without_the_peer_;
                }
            }
            ++next_to_act_[i];
        }
        if (!instance.statement().peer_may_lead && peer_reachable && copied_through_[i] == log.entries.size()) {
            instance.peer_holds_everything();
            stored_statement_[i] = instance.recorded_statement();
            ask_each_voter(i, instance.epoch());
        }
    }

    // An instance that begins acting as leader must hold every command the engine has acted on. It can
    // only lack one when it is not the instance that was last seen acting.
    void check_the_leader_holds_every_acted_command(int step) {
        const int leader = acting_instance();
        if (leader == 0 || leader == last_seen_acting_) {
            return;
        }
        if (last_seen_acting_ != 0 && !acted_.empty()) {
            ++changes_of_leader_after_acting_;
        }
        last_seen_acting_ = leader;
        for (const int64_t command : acted_) {
            if (!logs_[leader - 1].holds(command)) {
                violation_ = fmt::format("step {}: instance {} acts as leader without command {}, which the matching engine acted on", step, leader, command);
                return;
            }
        }
    }

    void act(int i) {
        if (!instances_[i].has_value()) {
            return;
        }
        fix_common::LeaseParticipant& instance = *instances_[i];
        static_cast<void>(instance.stop_if_lease_ran_out(now_));
        if (instance.acting(now_) && now_ - last_renewal_[i] >= renewal_interval) {
            last_renewal_[i] = now_;
            ask_each_voter(i, instance.epoch());
        }
        if (instance.acting(now_)) {
            handle_commands(i);
        }
        if (instance.state() == fix_common::LeaseHolder::State::Candidate && now_ - asked_at_[i] > give_up_asking_after) {
            instance.stop(now_, wait_before_asking_again());
        }
        if (instance.may_ask_to_lead(now_) && chance(0.2)) {
            const int32_t epoch = instance.begin_asking_to_lead();
            asked_at_[i] = now_;
            last_renewal_[i] = now_;
            ask_each_voter(i, epoch);
        }
        stored_epoch_[i] = instance.highest_epoch();
    }

    std::mt19937 random_;
    Options options_;
    Clock::time_point now_{};
    std::optional<fix_common::LeaseParticipant> instances_[2];
    int32_t stored_epoch_[2]{0, 0};
    bool kept_down_[2]{false, false};
    Clock::time_point last_renewal_[2]{};
    Clock::time_point asked_at_[2]{};
    std::optional<fix_common::LeaseVoter> arbiter_;
    fix_common::LeaderStatement stored_statement_[2]{};
    fix_common::LeaderStatement arbiter_statement_on_disk_{};
    Log logs_[2];
    std::set<int64_t> acted_;
    int64_t last_command_{0};
    size_t copied_through_[2]{0, 0};
    size_t next_to_act_[2]{0, 0};
    Clock::time_point copying_stalled_until_{};
    int last_seen_acting_{0};
    int64_t commands_acted_on_without_the_peer_{0};
    int changes_of_leader_after_acting_{0};
    std::vector<Message> in_flight_;
    Clock::time_point link_down_until_[3]{};
    std::map<int32_t, int64_t> leader_of_epoch_;
    std::string violation_;
    int steps_with_a_leader_{0};
    int elections_{0};
    bool last_step_had_leader_{false};
};

constexpr int simulation_runs = 400;
constexpr int steps_per_run = 20000;

// With one part of rule 11 removed, some run must find an instance acting as leader without a command
// the engine acted on, and not some other violation.
void expect_a_leader_lacking_an_acted_command(const Options& options) {
    std::string violation;
    uint32_t seed = 1;
    for (; seed <= simulation_runs && violation.empty(); ++seed) {
        PairSimulation simulation(seed, options);
        violation = simulation.run(steps_per_run);
    }
    std::cout << "seed " << seed - 1 << ": " << violation << "\n";
    EXPECT_NE(violation.find("without command"), std::string::npos) << violation;
}

} // un-named namespace

TEST(LeaseSimulationTest, TwoInstancesNeverActAsLeaderAtOnce) {
    int64_t elections = 0;
    int64_t steps_with_a_leader = 0;
    for (uint32_t seed = 1; seed <= simulation_runs; ++seed) {
        PairSimulation simulation(seed, Options{});
        const std::string violation = simulation.run(steps_per_run);
        ASSERT_TRUE(violation.empty()) << "seed " << seed << ": " << violation;
        elections += simulation.elections();
        steps_with_a_leader += simulation.steps_with_a_leader();
    }
    // The property means something only if leaders exist for much of the time and leadership moves
    // often. Without these, a simulation in which nobody ever led would pass.
    const int64_t total_steps = static_cast<int64_t>(simulation_runs) * steps_per_run;
    std::cout << "elections: " << elections << " (" << elections / simulation_runs << " per run); a leader was acting for "
              << 100 * steps_with_a_leader / total_steps << "% of steps\n";
    EXPECT_GT(elections, 10 * simulation_runs);
    EXPECT_GT(steps_with_a_leader, total_steps / 2);
}

TEST(LeaseSimulationTest, AClockDriftWithinTheAllowanceIsAbsorbed) {
    Options options;
    options.arbiter_clock_gain_per_lease = milliseconds{200};
    for (uint32_t seed = 1; seed <= simulation_runs; ++seed) {
        PairSimulation simulation(seed, options);
        const std::string violation = simulation.run(steps_per_run);
        ASSERT_TRUE(violation.empty()) << "seed " << seed << ": " << violation;
    }
}

TEST(LeaseSimulationTest, AClockDriftBeyondTheAllowanceIsCaught) {
    // The check must be able to fail. With the arbiter's clock gaining more per lease period than the
    // drift allowance covers, it forgets promises before the instance that holds them stops relying on
    // them, and some run must find two instances acting as leader at once.
    Options options;
    options.arbiter_clock_gain_per_lease = milliseconds{1500};
    bool found = false;
    for (uint32_t seed = 1; seed <= simulation_runs && !found; ++seed) {
        PairSimulation simulation(seed, options);
        found = !simulation.run(steps_per_run).empty();
    }
    EXPECT_TRUE(found);
}

TEST(LeaseSimulationTest, WithNothingFailingALeaderEmergesAndStays) {
    Options options;
    options.failures = false;
    options.slow_and_lossy_messages = false;
    for (uint32_t seed = 1; seed <= 50; ++seed) {
        PairSimulation simulation(seed, options);
        ASSERT_TRUE(simulation.run(200).empty());
        ASSERT_TRUE(simulation.last_step_had_leader()) << "seed " << seed << ": no leader after ten seconds";
        const int leader = simulation.acting_instance();
        ASSERT_TRUE(simulation.run(2000).empty());
        EXPECT_EQ(simulation.acting_instance(), leader) << "seed " << seed << ": leadership moved with nothing failing";
    }
}

TEST(LeaseSimulationTest, WithTheArbiterDownForGoodThePairStillElectsALeader) {
    Options options;
    options.failures = false;
    options.slow_and_lossy_messages = false;
    options.arbiter_down_for_good = true;
    for (uint32_t seed = 1; seed <= 50; ++seed) {
        PairSimulation simulation(seed, options);
        ASSERT_TRUE(simulation.run(400).empty());
        EXPECT_TRUE(simulation.last_step_had_leader()) << "seed " << seed << ": the pair elected no leader without the arbiter";
    }
}

TEST(LeaseSimulationTest, WithTheArbiterAndTheLeaderBothGoneNobodyLeads) {
    // The one case the design halts in: the follower cannot tell a dead leader from being cut off, so
    // it must not take over without a majority.
    Options options;
    options.failures = false;
    options.slow_and_lossy_messages = false;
    options.arbiter_down_for_good = true;
    for (uint32_t seed = 1; seed <= 50; ++seed) {
        PairSimulation simulation(seed, options);
        ASSERT_TRUE(simulation.run(400).empty());
        const int leader = simulation.acting_instance();
        ASSERT_NE(leader, 0) << "seed " << seed;
        simulation.crash_for_good(leader - 1);
        ASSERT_TRUE(simulation.run(2000).empty());
        EXPECT_EQ(simulation.acting_instance(), 0) << "seed " << seed << ": the surviving instance took over with no majority";
    }
}

TEST(LeaseSimulationTest, AnInstanceActingAsLeaderHoldsEveryCommandTheEngineActedOn) {
    int64_t acted_without_the_peer = 0;
    int64_t changes_of_leader = 0;
    for (uint32_t seed = 1; seed <= simulation_runs; ++seed) {
        PairSimulation simulation(seed, Options{});
        const std::string violation = simulation.run(steps_per_run);
        ASSERT_TRUE(violation.empty()) << "seed " << seed << ": " << violation;
        acted_without_the_peer += simulation.commands_acted_on_without_the_peer();
        changes_of_leader += simulation.changes_of_leader_after_acting();
    }
    // The property is tested only if leaders do act on commands their peers lack, and leadership does
    // move after commands have been acted on.
    std::cout << "commands acted on without the peer: " << acted_without_the_peer << "; changes of leader after acting: " << changes_of_leader << "\n";
    EXPECT_GT(acted_without_the_peer, 10 * simulation_runs);
    EXPECT_GT(changes_of_leader, 10 * simulation_runs);
}

TEST(LeaseSimulationTest, ALeaderThatActsWithoutAnEchoIsCaught) {
    Options options;
    options.leader_waits_for_an_echo = false;
    expect_a_leader_lacking_an_acted_command(options);
}

TEST(LeaseSimulationTest, AnArbiterThatForgetsItsStatementIsCaught) {
    Options options;
    options.arbiter_keeps_its_statement = false;
    expect_a_leader_lacking_an_acted_command(options);
}

TEST(LeaseSimulationTest, AnInstanceThatForgetsItsStatementIsCaught) {
    Options options;
    options.instances_keep_their_statement = false;
    expect_a_leader_lacking_an_acted_command(options);
}
