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

/// A CPU with its associated NUMA node, as returned by discovery and claim functions.
struct CpuAssignment {
    CpuId cpu_id;
    int numa_node_id{-1};
};

/// Flat vector of CPU assignments returned by discovery and claim functions.
using AvailableCpuVector = std::vector<CpuAssignment>;

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
 * struct. max_system_cores must never change between processes on the same
 * machine; it determines the on-disk / in-memory file size.
 */
struct SharedCoreRegistryLayout {
    static constexpr size_t max_system_cores = 256;
    uint32_t active_entry_count{0};
    CoreAllocationEntry entries[max_system_cores]; // NOLINT(modernize-avoid-c-arrays)
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
            const int end = std::stoi(token.substr(dash_pos + 1));
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
 * @param reserve_cpu0  When true, CPU 0 is excluded (reserved for OS use).
 * @param registry     Cross-process shared registry of currently claimed cores.
 * @return Flat list of available CPU IDs in NUMA-near order.
 */
inline AvailableCpuVector get_available_cpu_ids(bool reserve_cpu0, const SharedCoreRegistryLayout& registry) {
    AvailableCpuVector candidates;

    const std::filesystem::path node_base{"/sys/devices/system/node"};

    // Fallback for containers / restricted environments without sysfs NUMA info.
    // Try /sys/devices/system/cpu/present to enumerate all CPUs rather than
    // returning at most one, which was the old behaviour.
    if (!std::filesystem::exists(node_base)) {
        const std::filesystem::path present_path{"/sys/devices/system/cpu/present"};
        if (std::filesystem::exists(present_path)) {
            std::ifstream f(present_path);
            std::string line;
            if (std::getline(f, line)) {
                for (const CpuId cpu_id : detail::parse_cpu_list(line)) {
                    if (reserve_cpu0 && cpu_id.get_value() == 0) {
                        continue;
                    }
                    candidates.push_back({cpu_id, 0});
                }
            }
        } else if (!reserve_cpu0) {
            candidates.push_back({CpuId{0}, 0});
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

    // Extract the numeric index from a "nodeN" directory name.
    auto node_number = [](const std::filesystem::path& p) -> int {
        try {
            return std::stoi(p.filename().string().substr(4));
        } catch (...) {
            return -1;
        }
    };

    // Sort numerically (node0, node1, ..., node10, ...) not lexicographically,
    // which would wrongly order node10 before node2.
    std::sort(node_dirs.begin(), node_dirs.end(),
              [&node_number](const auto& a, const auto& b) {
                  return node_number(a) < node_number(b);
              });

    for (const auto& node_dir : node_dirs) {
        const int numa_node = node_number(node_dir);
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
            if (reserve_cpu0 && cpu_id.get_value() == 0) {
                continue;
            }

            // Skip cores claimed by live processes in the cross-process registry.
            bool busy = false;
            for (uint32_t i = 0; i < registry.active_entry_count; ++i) {
                const auto& e = registry.entries[i];
                // Entries with pid <= 0 are corrupt or zero-initialised and can
                // never correspond to a real process; skip them.  kill(0, 0) sends
                // to the calling process group (always returns 0) and kill(-1, 0)
                // sends to all processes (also always returns 0), so without this
                // guard every such entry would spuriously mark a CPU as busy.
                if (e.process_id <= 0) {
                    continue;
                }
                if (e.core_id == cpu_id.get_value()) {
                    // kill(pid, 0): returns 0 if process exists; ESRCH if not.
                    if (kill(e.process_id, 0) == 0 || errno != ESRCH) {
                        busy = true;
                        break;
                    }
                }
            }

            if (!busy) {
                candidates.push_back({cpu_id, numa_node});
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
