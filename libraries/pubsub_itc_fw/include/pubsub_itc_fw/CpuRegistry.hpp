#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <string>
#include <sys/types.h>

#include <pubsub_itc_fw/CpuPinning.hpp>

namespace pubsub_itc_fw {

/**
 * @brief Cross-process CPU core registry backed by a memory-mapped shared file.
 *
 * Multiple cooperating processes use a single shared file, held in the
 * deployment's own run directory, to coordinate CPU allocation. CpuRegistry uses
 * an flock-based lock file to serialise access during claim and release
 * operations, preventing two processes from claiming the same core
 * simultaneously.
 *
 * Stale entries from dead processes are cleaned up automatically the next time
 * any live process calls claim_cpus(). No daemon or heartbeat is required.
 *
 * ### Typical lifecycle
 *
 *  1. Construct -- opens (or creates) the shared file and maps it read/write.
 *  2. claim_cpus() -- acquires flock, discovers free CPUs via get_available_cpu_ids(),
 *     writes ownership entries, releases flock, returns the claimed CPU IDs.
 *  3. Caller pins its threads to the returned IDs.
 *  4. Destructor / release_cpus() -- acquires flock, removes this PID's entries.
 *
 * ### File lifecycle
 *
 * Both files live under the deployment's run directory and neither is cleared by
 * the operating system, so removing them is the deployment tooling's job:
 * deploy.py clears them when it lays down an install and devenv.py clears them on
 * every start. A machine-wide tmpfs location such as /dev/shm would be cleared on
 * reboot, but Linux hosts reboot rarely enough that this is no real protection,
 * and it would make two installations on one machine share a single registry.
 *
 * Stale entries left by a process that died without releasing are handled
 * independently of file removal: claim_cpus() drops entries whose owning PID is
 * no longer alive.
 */
class CpuRegistry {
  public:
    /**
     * @param[in] shm_path       Path to the shared registry file. Must not be empty.
     * @param[in] lock_file_path Path to the flock serialisation file. Must not be empty.
     */
    CpuRegistry(std::string shm_path, std::string lock_file_path);

    /// Calls release_cpus() then unmaps and closes the shared file.
    ~CpuRegistry();

    CpuRegistry(const CpuRegistry&) = delete;
    CpuRegistry& operator=(const CpuRegistry&) = delete;

    CpuRegistry(CpuRegistry&&);
    CpuRegistry& operator=(CpuRegistry&&);

    /**
     * @brief Atomically discover and claim up to `count` free CPU cores.
     *
     * Under the flock:
     *  - Calls get_available_cpu_ids() to find CPUs not owned by any live process.
     *  - Selects up to `count` of those CPUs.
     *  - Writes registry entries recording this process as the owner.
     *
     * @param count       Maximum number of CPUs to claim.
     * @param reserve_cpu0 When true, CPU 0 is excluded from candidates.
     * @return The claimed CPU IDs. May be fewer than `count` if not enough
     *         cores are available; never exceeds SharedCoreRegistryLayout::max_system_cores.
     */
    [[nodiscard]] AvailableCpuVector claim_cpus(size_t count, bool reserve_cpu0) const;

    /**
     * @brief Remove all registry entries owned by this process.
     *
     * Idempotent: safe to call multiple times or after a failed claim.
     */
    void release_cpus() const;

  private:
    void close_mapping();

    std::string shm_path_;
    std::string lock_file_path_;
    pid_t my_pid_;
    SharedCoreRegistryLayout* layout_{nullptr};
    int shm_fd_{-1};
};

} // namespaces
