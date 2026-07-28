#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <string>
#include <tuple>
#include <vector>

#include <sys/types.h>

#include <pubsub_itc_fw/CpuPinning.hpp>

namespace pubsub_itc_fw {

/**
 * @brief Cross-process CPU core registry backed by a memory-mapped shared file.
 *
 * The registry is a record and a collision detector, not an allocator. Cores are
 * allocated at deploy time by deploy.py, which resolves the declared layout
 * against the machine's real topology and writes run/cpu_layout.toml; every
 * process then simply uses what it was given. Negotiating at run time was
 * abandoned because the outcome depended on start order, so a component
 * restarted mid-life -- which ha_test.py does routinely -- could come back and
 * shift the layout under everything else.
 *
 * What a shared record is still needed for is the case one layout file cannot
 * see: two installations on the same machine, each with its own layout, each
 * self-consistent, both handing out core 5. Processes record what they have
 * pinned, under an flock-based lock file, and a process finding a core already
 * held by a live owner reports it.
 *
 * Stale entries from dead processes are cleaned up automatically the next time
 * any live process calls record_assignment(). No daemon or heartbeat is required.
 *
 * ### Typical lifecycle
 *
 *  1. Construct -- opens (or creates) the shared file and maps it read/write.
 *  2. Caller pins its threads to the cores the layout file allocated it.
 *  3. record_assignment() -- acquires flock, evicts dead owners, reports any
 *     live process already holding those cores, records this process, releases.
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
 * independently of file removal: record_assignment() drops entries whose owning PID is
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
     * @brief Record the cores this process was allocated, and report collisions.
     *
     * The registry does not allocate. deploy.py resolved the layout on this
     * machine and wrote it to the layout file; by the time this is called the
     * decision has been made and the threads are already pinned. What the
     * registry still earns its place doing is catching the case the declared
     * layout cannot prevent by itself: **two installations on one machine**,
     * each with its own layout file, each correct in isolation, both handing out
     * the same core.
     *
     * Under the flock:
     *  - Evicts entries owned by processes that are no longer alive.
     *  - Looks for any of `core_ids` already recorded by a live process.
     *  - Records this process as an owner of each of `core_ids`, whether or not
     *    a collision was found, so the runtime record stays a complete picture
     *    of what is actually pinned where.
     *
     * @param[in] core_ids The cores this process has pinned threads to.
     * @return true and an empty string when no other live process holds any of
     *         these cores; false and a description naming the cores and the
     *         processes holding them otherwise.
     */
    [[nodiscard]] std::tuple<bool, std::string> record_assignment(const std::vector<CpuId>& core_ids) const;

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
