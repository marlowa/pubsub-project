#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <errno.h>   // NOLINT(modernize-deprecated-headers) -- IWYU pragma: keep
#include <pthread.h> // IWYU pragma: keep
#include <sched.h>   // IWYU pragma: keep -- cpu_set_t, sched_setaffinity
#include <signal.h>  // NOLINT(modernize-deprecated-headers) -- IWYU pragma: keep
#include <sys/types.h>
#include <unistd.h>

#include <pubsub_itc_fw/WrappedInteger.hpp>

namespace pubsub_itc_fw {

/// Type-safe CPU identifier. Signed int to align with Linux system call APIs.
using CpuId = WrappedInteger<struct CpuIdTag, int>;

/**
 * @brief Distinguishes Intel hybrid core types detected via sysfs cpu_capacity.
 *
 * P-cores (Performance) have higher clock speed, out-of-order execution, and
 * hyperthreading.  E-cores (Efficiency) have lower clock speed and simpler
 * in-order pipelines.  Unknown is used when the file is absent (non-hybrid
 * architecture or older kernel) and is treated as P-core for scheduling.
 *
 * Detection: /sys/devices/system/cpu/cpuN/cpu_capacity
 *   1024 => P-core (maximum normalised capacity, as used by Linux EAS)
 *   < 1024 => E-core (lower relative capacity, e.g. 355 on Alder Lake)
 *   absent => Unknown (treat as P-core; safe fallback for uniform architectures)
 */
enum class CoreType {
    P_core, ///< Performance core
    E_core, ///< Efficiency core -- latency-sensitive threads should avoid these
    Unknown ///< Not detectable; treated as P-core for scheduling purposes
};

/// Returns a short human-readable label for logging.
inline const char* core_type_name(CoreType t) {
    switch (t) {
        case CoreType::P_core:
            return "P-core";
        case CoreType::E_core:
            return "E-core";
        default:
            return "unknown-core-type";
    }
}

/// A CPU with its associated NUMA node and core type, as returned by discovery functions.
struct CpuAssignment {
    CpuId cpu_id;
    int numa_node_id{-1};
    CoreType core_type{CoreType::Unknown};
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

/**
 * @brief Detect whether a CPU is a P-core or E-core from sysfs.
 *
 * Two detection mechanisms are tried in order:
 *
 * 1. /sys/devices/system/cpu/cpuN/cpu_capacity (Linux EAS, value 1024 = P-core)
 *    Present on kernels with Energy Aware Scheduling support.
 *
 * 2. /sys/devices/system/cpu/cpuN/acpi_cppc/highest_perf (ACPI CPPC)
 *    Present on Intel hybrid machines even without EAS.  The value is not
 *    normalised, so a threshold is not fixed.  The caller must pass the
 *    system-wide maximum highest_perf observed across all CPUs; a CPU whose
 *    highest_perf is less than (max * cppc_p_core_threshold_pct / 100) is
 *    classified as an E-core.  Pass max_cppc_perf == 0 to skip this method.
 *
 * Returns Unknown when neither source is available (treated as P-core by callers).
 */
inline CoreType read_core_type(CpuId cpu_id, int max_cppc_perf = 0) {
    // Method 1: cpu_capacity (EAS)
    const std::filesystem::path cap_path{"/sys/devices/system/cpu/cpu" + std::to_string(cpu_id.get_value()) + "/cpu_capacity"};
    if (std::filesystem::exists(cap_path)) {
        std::ifstream f(cap_path);
        int capacity = 0;
        if (f >> capacity) {
            // 1024 is the maximum normalised capacity value used by the Linux EAS scheduler.
            return (capacity >= 1024) ? CoreType::P_core : CoreType::E_core;
        }
    }

    // Method 2: acpi_cppc/highest_perf
    if (max_cppc_perf > 0) {
        const std::filesystem::path cppc_path{"/sys/devices/system/cpu/cpu" + std::to_string(cpu_id.get_value()) + "/acpi_cppc/highest_perf"};
        if (std::filesystem::exists(cppc_path)) {
            std::ifstream f(cppc_path);
            int perf = 0;
            if (f >> perf) {
                // CPUs whose highest_perf is below 80% of the system maximum are E-cores.
                return (perf * 100 >= max_cppc_perf * 80) ? CoreType::P_core : CoreType::E_core;
            }
        }
    }

    return CoreType::Unknown;
}

/**
 * @brief Read the system-wide maximum acpi_cppc/highest_perf across all online CPUs.
 *
 * Returns 0 if the file is absent (no ACPI CPPC support or uniform architecture).
 * Used as the reference value for read_core_type() method 2.
 */
inline int read_max_cppc_perf() {
    int max_perf = 0;
    const std::filesystem::path cpu_base{"/sys/devices/system/cpu"};
    if (!std::filesystem::exists(cpu_base)) {
        return 0;
    }
    for (const auto& entry : std::filesystem::directory_iterator(cpu_base)) {
        const std::string name = entry.path().filename().string();
        // Match "cpuN" directories (digits only after "cpu").
        if (name.size() <= 3 || name.substr(0, 3) != "cpu") {
            continue;
        }
        bool all_digits = true;
        for (size_t i = 3; i < name.size(); ++i) {
            if (name[i] < '0' || name[i] > '9') {
                all_digits = false;
                break;
            }
        }
        if (!all_digits) {
            continue;
        }

        const std::filesystem::path cppc_path = entry.path() / "acpi_cppc" / "highest_perf";
        if (!std::filesystem::exists(cppc_path)) {
            continue;
        }
        std::ifstream f(cppc_path);
        int perf = 0;
        if (f >> perf && perf > max_perf) {
            max_perf = perf;
        }
    }
    return max_perf;
}

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

} // namespaces

/**
 * @brief Discover CPUs available for pinning from the kernel sysfs topology.
 *
 * Iterates NUMA nodes in order (Node 0, Node 1, ...) to discover available
 * CPUs, then sorts the result by preference:
 *
 *   1. P-cores (and Unknown, treated as P-core) on lower NUMA nodes first.
 *   2. E-cores on lower NUMA nodes first.
 *
 * This means callers that take the first N CPUs automatically prefer P-cores
 * and fall back to E-cores only when all P-cores are already claimed.
 *
 * @param reserve_cpu0  When true, CPU 0 is excluded (reserved for OS use).
 * @param registry      Cross-process shared registry of currently claimed cores.
 * @return Available CPUs in P-core-first, NUMA-near order.
 */
inline AvailableCpuVector get_available_cpu_ids(bool reserve_cpu0, const SharedCoreRegistryLayout& registry) {
    AvailableCpuVector candidates;

    // Read once; passed to read_core_type() for ACPI CPPC fallback detection.
    const int max_cppc_perf = detail::read_max_cppc_perf();

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
                    candidates.push_back({cpu_id, 0, detail::read_core_type(cpu_id, max_cppc_perf)});
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
    std::sort(node_dirs.begin(), node_dirs.end(), [&node_number](const auto& a, const auto& b) { return node_number(a) < node_number(b); });

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
                candidates.push_back({cpu_id, numa_node, detail::read_core_type(cpu_id, max_cppc_perf)});
            }
        }
    }

    // Sort: P-cores (and Unknown) before E-cores; within each tier, lower NUMA
    // node first.  stable_sort preserves the existing NUMA order among ties.
    std::stable_sort(candidates.begin(), candidates.end(), [](const CpuAssignment& a, const CpuAssignment& b) {
        const int a_rank = (a.core_type == CoreType::E_core) ? 1 : 0;
        const int b_rank = (b.core_type == CoreType::E_core) ? 1 : 0;
        if (a_rank != b_rank) {
            return a_rank < b_rank;
        }
        return a.numa_node_id < b.numa_node_id;
    });

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
 * @brief Render a core list as a kernel cpu-list string, so [1,2,3,7] reads "1-3,7".
 *
 * The same notation the layout file and taskset -c use, which keeps log output
 * comparable with the file it was derived from.
 */
[[nodiscard]] inline std::string format_cpu_list(const std::vector<CpuId>& core_ids) {
    if (core_ids.empty()) {
        return "(none)";
    }

    std::vector<int> ordered;
    ordered.reserve(core_ids.size());
    for (const CpuId core_id : core_ids) {
        ordered.push_back(core_id.get_value());
    }
    std::sort(ordered.begin(), ordered.end());

    std::string result;
    size_t index = 0;
    while (index < ordered.size()) {
        size_t last = index;
        while (last + 1 < ordered.size() && ordered[last + 1] == ordered[last] + 1) {
            ++last;
        }
        if (!result.empty()) {
            result += ",";
        }
        result += std::to_string(ordered[index]);
        if (last > index) {
            result += "-" + std::to_string(ordered[last]);
        }
        index = last + 1;
    }
    return result;
}

/**
 * @brief Restrict a thread to a set of cores rather than a single one.
 *
 * This is how the background tier is applied. An affinity mask is not a ratchet:
 * a thread masked to the background pool can later be moved to a hot-path core
 * by pin_thread_to_core(), which is what makes background-by-default with
 * promotion-by-exception possible without cgroup cpusets.
 *
 * @param[in] thread_handle The pthread handle of the thread to mask.
 * @param[in] core_ids The cores the thread may run on. Must not be empty: an
 *                     empty CPU set is EINVAL.
 * @return true on success, false on failure (check errno for details).
 */
[[nodiscard]] inline bool apply_affinity_mask(pthread_t thread_handle, const std::vector<CpuId>& core_ids) {
    if (core_ids.empty()) {
        return false;
    }
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    for (const CpuId core_id : core_ids) {
        CPU_SET(core_id.get_value(), &cpuset);
    }
    return pthread_setaffinity_np(thread_handle, sizeof(cpu_set_t), &cpuset) == 0;
}

/**
 * @brief Restrict a kernel thread to a set of cores, by TID.
 *
 * For threads not addressable by a pthread_t -- the Quill backend, which
 * quill::Backend::get_thread_id() identifies only by its Linux TID.
 *
 * @param[in] thread_id Linux kernel thread ID (LWP).
 * @param[in] core_ids The cores the thread may run on. Must not be empty.
 * @return true on success, false on failure (check errno for details).
 */
[[nodiscard]] inline bool apply_thread_affinity_mask(pid_t thread_id, const std::vector<CpuId>& core_ids) {
    if (core_ids.empty()) {
        return false;
    }
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    for (const CpuId core_id : core_ids) {
        CPU_SET(core_id.get_value(), &cpuset);
    }
    return ::sched_setaffinity(thread_id, sizeof(cpu_set_t), &cpuset) == 0;
}

/**
 * @brief Restrict the whole calling process to a set of cores.
 *
 * sched_setaffinity() with pid 0 addresses the calling thread, and any thread
 * created afterwards inherits the mask. Called early in main(), before any
 * thread is spawned, this places the entire process in the background tier --
 * the "background by default" half of the design. The Reactor then promotes the
 * few threads that have been allocated dedicated cores.
 *
 * The generated wrapper script applies the same mask before the process starts,
 * which additionally covers the JVM components and the window before main() is
 * entered. This call is the backstop that makes production independent of
 * whether the launcher cooperated.
 *
 * @param[in] core_ids The cores the process may run on. Must not be empty.
 * @return true on success, false on failure (check errno for details).
 */
[[nodiscard]] inline bool apply_process_affinity_mask(const std::vector<CpuId>& core_ids) {
    if (core_ids.empty()) {
        return false;
    }
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    for (const CpuId core_id : core_ids) {
        CPU_SET(core_id.get_value(), &cpuset);
    }
    return ::sched_setaffinity(0, sizeof(cpu_set_t), &cpuset) == 0;
}

/**
 * @brief Pin a kernel thread to a single CPU core using sched_setaffinity.
 *
 * For threads not addressable by a pthread_t -- e.g. the Quill logger backend
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

} // namespaces
