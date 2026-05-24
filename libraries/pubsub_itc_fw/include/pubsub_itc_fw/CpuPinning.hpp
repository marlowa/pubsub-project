#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <errno.h>   // NOLINT(modernize-deprecated-headers) — IWYU pragma: keep
#include <pthread.h> // IWYU pragma: keep
#include <sched.h>   // IWYU pragma: keep — cpu_set_t, sched_setaffinity
#include <signal.h>  // NOLINT(modernize-deprecated-headers) — IWYU pragma: keep
#include <sys/types.h>
#include <unistd.h>

#include <pubsub_itc_fw/WrappedInteger.hpp>

namespace pubsub_itc_fw {

/// Type-safe CPU identifier. Signed int to align with Linux system call APIs.
using CpuId = WrappedInteger<struct CpuIdTag, int>;

/// Flat vector of CPU IDs returned by discovery and claim functions.
using AvailableCpuVector = std::vector<CpuId>;

/// One entry in the cross-process shared registry: one CPU claimed by one PID.
struct CoreAllocationEntry {
    int core_id{-1};
    int numa_node_id{-1};
    pid_t process_id{0};
    uint64_t thread_tag{0};
    uint64_t timestamp_ns{0};
};

/**
 * @brief Fixed-size POD layout for the cross-process shared registry file.
 *
 * All cooperating processes memory-map the same file and read/write this
 * struct. MAX_SYSTEM_CORES must never change between processes on the same
 * machine; it determines the on-disk / in-memory file size.
 */
struct SharedCoreRegistryLayout {
    static constexpr size_t MAX_SYSTEM_CORES = 256;
    uint32_t active_entry_count{0};
    CoreAllocationEntry entries[MAX_SYSTEM_CORES]; // NOLINT(modernize-avoid-c-arrays)
};

namespace detail {

/// Parses a kernel CPU-list string such as "0-3,8-11" into a flat CpuId vector.
inline std::vector<CpuId> parse_cpu_list(const std::string& input) {
    std::vector<CpuId> cpus;
    if (input.empty() || input == "\n") {
        return cpus;
    }

    std::stringstream ss(input);
    std::string token;

    while (std::getline(ss, token, ',')) {
        if (token.empty()) {
            continue;
        }
        // Strip any trailing newline from the last token.
        while (!token.empty() && (token.back() == '\n' || token.back() == '\r')) {
            token.pop_back();
        }
        if (token.empty()) {
            continue;
        }

        const size_t dash_pos = token.find('-');
        if (dash_pos == std::string::npos) {
            cpus.emplace_back(std::stoi(token));
        } else {
            const int start = std::stoi(token.substr(0, dash_pos));
            const int end   = std::stoi(token.substr(dash_pos + 1));
            for (int i = start; i <= end; ++i) {
                cpus.emplace_back(i);
            }
        }
    }
    return cpus;
}

} // namespace detail

/**
 * @brief Discover CPUs available for pinning from the kernel sysfs topology.
 *
 * Iterates NUMA nodes in order (Node 0, Node 1, ...) to produce a list that
 * favours NUMA-local allocation. For each candidate CPU, the cross-process
 * registry is consulted: cores owned by live processes are excluded.
 *
 * @param is_dev_mode  When true, CPU 0 is excluded (reserved for OS use).
 * @param registry     Cross-process shared registry of currently claimed cores.
 * @return Flat list of available CPU IDs in NUMA-near order.
 */
inline AvailableCpuVector get_available_cpu_ids(bool is_dev_mode, const SharedCoreRegistryLayout& registry) {
    AvailableCpuVector candidates;

    const std::filesystem::path node_base{"/sys/devices/system/node"};

    // Fallback for containers / restricted environments without sysfs NUMA info.
    if (!std::filesystem::exists(node_base)) {
        if (!is_dev_mode) {
            candidates.emplace_back(0);
        }
        return candidates;
    }

    // Collect NUMA node directories.
    std::vector<std::filesystem::path> node_dirs;
    for (const auto& entry : std::filesystem::directory_iterator(node_base)) {
        if (entry.is_directory() && entry.path().filename().string().rfind("node", 0) == 0) {
            node_dirs.push_back(entry.path());
        }
    }

    // Sort so we traverse Node 0, Node 1, ... in order.
    std::sort(node_dirs.begin(), node_dirs.end(),
              [](const auto& a, const auto& b) { return a.filename().string() < b.filename().string(); });

    for (const auto& node_dir : node_dirs) {
        const auto cpulist_path = node_dir / "cpulist";
        if (!std::filesystem::exists(cpulist_path)) {
            continue;
        }

        std::ifstream cpulist_file(cpulist_path);
        std::string cpulist_str;
        if (!std::getline(cpulist_file, cpulist_str)) {
            continue;
        }

        for (const CpuId cpu_id : detail::parse_cpu_list(cpulist_str)) {
            if (is_dev_mode && cpu_id.get_value() == 0) {
                continue;
            }

            // Skip cores claimed by live processes in the cross-process registry.
            bool busy = false;
            for (uint32_t i = 0; i < registry.active_entry_count; ++i) {
                const auto& e = registry.entries[i];
                if (e.core_id == cpu_id.get_value()) {
                    // kill(pid, 0): returns 0 if process exists; ESRCH if not.
                    if (kill(e.process_id, 0) == 0 || errno != ESRCH) {
                        busy = true;
                        break;
                    }
                }
            }

            if (!busy) {
                candidates.push_back(cpu_id);
            }
        }
    }

    return candidates;
}

/**
 * @brief Pin a thread to a single CPU core using Linux POSIX thread affinity.
 *
 * @param thread_handle  The pthread handle of the thread to pin.
 * @param core_id        The target CPU.
 * @return true on success, false on failure (check errno for details).
 */
inline bool pin_thread_to_core(pthread_t thread_handle, CpuId core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id.get_value(), &cpuset);
    return pthread_setaffinity_np(thread_handle, sizeof(cpu_set_t), &cpuset) == 0;
}

/**
 * @brief Pin a kernel thread to a single CPU core using sched_setaffinity.
 *
 * For threads not addressable by a pthread_t — e.g. the Quill logger backend
 * thread, which is identified only by its Linux kernel TID as returned by
 * quill::Backend::get_thread_id().
 *
 * @param tid      Linux kernel thread ID (LWP).
 * @param core_id  The target CPU.
 * @return true on success, false on failure (check errno for details).
 */
inline bool pin_tid_to_core(pid_t tid, CpuId core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id.get_value(), &cpuset);
    return ::sched_setaffinity(tid, sizeof(cpu_set_t), &cpuset) == 0;
}

} // namespace pubsub_itc_fw
