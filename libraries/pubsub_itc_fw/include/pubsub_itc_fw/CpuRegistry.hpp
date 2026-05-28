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
 * Multiple cooperating processes use a single shared file (typically in
 * /dev/shm/) to coordinate CPU allocation. CpuRegistry uses an flock-based
 * lock file to serialise access during claim and release operations, preventing
 * two processes from claiming the same core simultaneously.
 *
 * Stale entries from dead processes are cleaned up automatically the next time
 * any live process calls claim_cpus(). No daemon or heartbeat is required.
 *
 * ### Typical lifecycle
 *
 *  1. Construct — opens (or creates) the shared file and maps it read/write.
 *  2. claim_cpus() — acquires flock, discovers free CPUs via get_available_cpu_ids(),
 *     writes ownership entries, releases flock, returns the claimed CPU IDs.
 *  3. Caller pins its threads to the returned IDs.
 *  4. Destructor / release_cpus() — acquires flock, removes this PID's entries.
 *
 * ### File lifecycle
 *
 * The shared file lives in /dev/shm/ (tmpfs) by default and is therefore
 * cleared on reboot, which is desirable: stale entries from a previous boot
 * are never inherited. The lock file in /tmp/ is similarly ephemeral.
 */
class CpuRegistry {
  public:
    /**
     * @param shm_path       Path to the shared registry file.
     *                       Default: /dev/shm/pubsub_cpu_registry
     * @param lock_file_path Path to the flock serialisation file.
     *                       Default: /tmp/pubsub_cpu_registry.lock
     * @throws PubSubItcException if the shared file cannot be opened, sized, or mapped.
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
     * @param is_dev_mode When true, CPU 0 is excluded from candidates.
     * @return The claimed CPU IDs. May be fewer than `count` if not enough
     *         cores are available; never exceeds SharedCoreRegistryLayout::MAX_SYSTEM_CORES.
     */
    AvailableCpuVector claim_cpus(size_t count, bool is_dev_mode);

    /**
     * @brief Remove all registry entries owned by this process.
     *
     * Idempotent: safe to call multiple times or after a failed claim.
     */
    void release_cpus();

  private:
    void close_mapping();

    std::string shm_path_;
    std::string lock_file_path_;
    pid_t my_pid_;
    SharedCoreRegistryLayout* layout_{nullptr};
    int shm_fd_{-1};
};

} // namespace pubsub_itc_fw
