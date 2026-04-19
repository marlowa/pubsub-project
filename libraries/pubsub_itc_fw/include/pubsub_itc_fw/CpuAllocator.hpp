#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <map>
#include <set>
#include <string>
#include <vector>
#include <sys/types.h>

/**
 * @brief NUMA-aware, SMT-safe CPU allocation and thread pinning class.
 *
 * @section design_notes Design Notes
 *
 * This class provides a simple, robust mechanism for allocating CPUs across
 * multiple cooperating processes. It avoids the complexity of shared-memory
 * registries, leases, heartbeats, or daemons. Instead, it relies on:
 *
 * - A global lock file (`flock`) to serialize allocation across processes.
 * - A global state file that records which CPUs are allocated to which PIDs.
 * - A cleanup pass that removes stale PIDs (processes that no longer exist).
 * - NUMA-aware CPU selection.
 * - SMT-safe allocation (one logical CPU per physical core).
 *
 * ## Why this design?
 *
 * The goal is to avoid:
 * - Two programs selecting the same CPUs.
 * - Two programs selecting SMT siblings on the same physical core.
 * - Races during CPU selection.
 * - NUMA imbalance across processes.
 *
 * The system does **not** require:
 * - Reclaiming CPUs dynamically while programs are running.
 * - Handling PID reuse.
 * - Complex distributed coordination.
 *
 * The allocator is intended for systems where:
 * - A suite of cooperating programs starts and stops as a whole.
 * - Full restarts should not inherit stale CPU allocation state.
 *
 * ## High-level workflow
 *
 * 1. Acquire global lock (`FileLock`).
 * 2. Load CPU allocation state.
 * 3. Remove stale PIDs.
 * 4. Discover CPU topology (NUMA node, package, core, logical CPU).
 * 5. Select one logical CPU per physical core.
 * 6. Allocate free cores to the calling process.
 * 7. Save updated state.
 * 8. Release lock.
 * 9. Pin threads to assigned CPUs.
 *
 */

namespace cpualloc {

/**
 * @class FileLock
 * @brief RAII wrapper for `flock()`-based file locking.
 *
 * Acquires an exclusive lock in the constructor and releases it in the
 * destructor. Ensures safe cross-process mutual exclusion for CPU allocation.
 */
class FileLock {
public:
    /**
     * @brief Construct and acquire an exclusive lock on the given file.
     * @param path Path to the lock file.
     * @throws std::runtime_error on failure to open or lock the file.
     */
    explicit FileLock(const std::string& path);

    /// Non-copyable
    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;

    /// Movable
    FileLock(FileLock&& other) noexcept;
    FileLock& operator=(FileLock&& other) noexcept;

    /// Destructor releases the lock.
    ~FileLock();

private:
    void release();

    int fd_{-1};
    std::string file_path_;
};

/**
 * @class CpuAllocator
 * @brief NUMA-aware, SMT-safe CPU allocator with cross-process coordination.
 *
 * This class manages CPU allocation for a process and provides a method to pin
 * threads to the allocated CPUs. It ensures:
 *
 * - Only one logical CPU per physical core is used (avoids SMT siblings).
 * - CPUs are allocated in NUMA-aware order.
 * - No two processes using this library will collide.
 * - Stale state is cleaned automatically on each allocation.
 *
 * The allocator is safe for repeated start/stop cycles of the entire system.
 */
class CpuAllocator {
public:
    /**
     * @brief Construct a CPU allocator.
     *
     * @param lock_file  Path to the global lock file.
     * @param state_file Path to the global CPU allocation state file.
     */
    CpuAllocator(std::string lock_file = "/var/run/cpu_pinning.lock",
                 std::string state_file = "/var/run/cpu_pinning.state");

    /**
     * @brief Allocate a number of CPUs for the calling process.
     *
     * This function:
     * - Acquires the global lock.
     * - Loads and cleans the state file.
     * - Discovers CPU topology.
     * - Selects one logical CPU per physical core.
     * - Allocates free cores in NUMA-aware order.
     * - Updates the state file.
     *
     * @param count Number of CPUs to allocate.
     * @return Vector of allocated CPU IDs, or empty on failure.
     */
    std::vector<int> allocate_cpus(int count);

    /**
     * @brief Pin a collection of thread IDs to the allocated CPUs.
     *
     * Threads are pinned in round-robin fashion across the provided CPUs.
     *
     * @param tids Vector of thread IDs (TIDs) to pin.
     * @param cpus Vector of CPU IDs to pin to.
     * @return true on success, false on failure.
     */
    bool pin_threads(const std::vector<pid_t>& tids,
                     const std::vector<int>& cpus) const;

private:
    using CpuState = std::map<int, pid_t>;

    CpuState load_state() const;
    void save_state(const CpuState& state) const;
    void cleanup_dead_pids(CpuState& state) const;

    std::string lock_file_;
    std::string state_file_;
};

} // namespaces
