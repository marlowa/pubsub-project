// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <string>
#include <vector>

#include <pthread.h>
#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/CpuPinning.hpp>

using namespace pubsub_itc_fw;

namespace {

std::vector<int> cpu_values(const std::vector<CpuId>& cpus) {
    std::vector<int> values;
    for (const CpuId cpu : cpus) {
        values.push_back(cpu.get_value());
    }
    return values;
}

int first_allowed_cpu() {
    cpu_set_t mask;
    CPU_ZERO(&mask);
    if (::sched_getaffinity(0, sizeof(mask), &mask) != 0) {
        return -1;
    }
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (CPU_ISSET(cpu, &mask)) {
            return cpu;
        }
    }
    return -1;
}

// A CPU index the process is NOT permitted to run on, derived from the live
// affinity mask so the test makes no assumption about the machine's core count.
// Returns -1 only in the impossible case where every cpu_set_t slot is permitted.
int first_disallowed_cpu() {
    cpu_set_t mask;
    CPU_ZERO(&mask);
    if (::sched_getaffinity(0, sizeof(mask), &mask) != 0) {
        return -1;
    }
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (!CPU_ISSET(cpu, &mask)) {
            return cpu;
        }
    }
    return -1;
}

} // namespaces

TEST(CpuPinningParseTest, EmptyInputYieldsNoCpus) {
    EXPECT_TRUE(detail::parse_cpu_list("").empty());
    EXPECT_TRUE(detail::parse_cpu_list("\n").empty());
}

TEST(CpuPinningParseTest, SingleCpu) {
    EXPECT_EQ(cpu_values(detail::parse_cpu_list("5")), (std::vector<int>{5}));
}

TEST(CpuPinningParseTest, SingleRange) {
    EXPECT_EQ(cpu_values(detail::parse_cpu_list("0-3")), (std::vector<int>{0, 1, 2, 3}));
}

TEST(CpuPinningParseTest, MixedRangesAndSingles) {
    EXPECT_EQ(cpu_values(detail::parse_cpu_list("0-3,8-11")), (std::vector<int>{0, 1, 2, 3, 8, 9, 10, 11}));
    EXPECT_EQ(cpu_values(detail::parse_cpu_list("1,3,5")), (std::vector<int>{1, 3, 5}));
}

TEST(CpuPinningParseTest, TrailingNewlineAndEmptyTokensAreIgnored) {
    EXPECT_EQ(cpu_values(detail::parse_cpu_list("0-2\n")), (std::vector<int>{0, 1, 2}));
    EXPECT_EQ(cpu_values(detail::parse_cpu_list(",,2,,")), (std::vector<int>{2}));
}

TEST(CpuPinningTest, CoreTypeNameLabels) {
    EXPECT_STREQ(core_type_name(CoreType::P_core), "P-core");
    EXPECT_STREQ(core_type_name(CoreType::E_core), "E-core");
    EXPECT_STREQ(core_type_name(CoreType::Unknown), "unknown-core-type");
}

TEST(CpuPinningTest, ReadMaxCppcPerfIsNonNegative) {
    EXPECT_GE(detail::read_max_cppc_perf(), 0);
}

TEST(CpuPinningTest, ReadCoreTypeReturnsAValidCategory) {
    const CoreType type = detail::read_core_type(CpuId{0}, detail::read_max_cppc_perf());

    EXPECT_TRUE(type == CoreType::P_core || type == CoreType::E_core || type == CoreType::Unknown);
}

TEST(CpuPinningTest, GetAvailableCpuIdsRunsAndSkipsBusyCores) {
    SharedCoreRegistryLayout registry{};
    const AvailableCpuVector all = get_available_cpu_ids(false, registry);
    // Exercised the sysfs discovery path; the result depends on the machine but
    // reserving CPU 0 must never widen the set.
    const AvailableCpuVector reserved = get_available_cpu_ids(true, registry);
    EXPECT_LE(reserved.size(), all.size());

    if (!all.empty()) {
        const int busy_core = all.front().cpu_id.get_value();
        registry.active_entry_count = 1;
        registry.entries[0].process_id = ::getpid(); // a live process -> that core is busy
        registry.entries[0].core_id = busy_core;

        const AvailableCpuVector filtered = get_available_cpu_ids(false, registry);
        for (const CpuAssignment& assignment : filtered) {
            EXPECT_NE(assignment.cpu_id.get_value(), busy_core);
        }
    }
}

TEST(CpuPinningTest, PinThreadToAllowedCoreSucceeds) {
    cpu_set_t original;
    CPU_ZERO(&original);
    ASSERT_EQ(::sched_getaffinity(0, sizeof(original), &original), 0);

    const int chosen = first_allowed_cpu();
    ASSERT_GE(chosen, 0);

    EXPECT_TRUE(pin_thread_to_core(::pthread_self(), CpuId{chosen}));

    ASSERT_EQ(::sched_setaffinity(0, sizeof(original), &original), 0); // restore
}

TEST(CpuPinningTest, PinTidToAllowedCoreSucceeds) {
    cpu_set_t original;
    CPU_ZERO(&original);
    ASSERT_EQ(::sched_getaffinity(0, sizeof(original), &original), 0);

    const int chosen = first_allowed_cpu();
    ASSERT_GE(chosen, 0);

    const pid_t tid = static_cast<pid_t>(::syscall(SYS_gettid));
    EXPECT_TRUE(pin_tid_to_core(tid, CpuId{chosen}));

    ASSERT_EQ(::sched_setaffinity(0, sizeof(original), &original), 0); // restore
}

TEST(CpuPinningTest, PinToDisallowedCoreFails) {
    const int disallowed = first_disallowed_cpu();
    if (disallowed < 0) {
        GTEST_SKIP() << "every cpu_set_t slot is permitted; no disallowed CPU to test with";
    }

    // Pinning to a mask whose only CPU is not permitted fails with EINVAL, leaving
    // affinity unchanged.
    EXPECT_FALSE(pin_thread_to_core(::pthread_self(), CpuId{disallowed}));
}
