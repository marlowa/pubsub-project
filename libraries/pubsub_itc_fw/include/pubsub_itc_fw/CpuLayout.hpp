#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include <pubsub_itc_fw/CpuPinning.hpp>

namespace pubsub_itc_fw {

/**
 * @brief One component's view of the machine-wide CPU layout computed by deploy.py.
 *
 * The environment TOML declares intent -- which processes run on each machine and
 * the order in which they surrender a dedicated core. deploy.py runs on the
 * target host, reads its real topology, resolves that intent into core ids and
 * writes the answer to run/cpu_layout.toml. This class reads that answer.
 *
 * Allocation is therefore not negotiated at runtime. A restarted component gets
 * the same cores it had before, because the file did not change; the greedy
 * claiming this replaces could shift the layout on any mid-life restart, which
 * is designed-for behaviour here since ha_test.py kills components routinely.
 *
 * Two tiers are read from the file:
 *
 *  - background_cores(), machine-wide and identical for every component. Every
 *    process masks itself to these at start-up.
 *  - hot_path_cores(), this component's own dedicated cores, empty when it was
 *    not admitted. The Reactor promotes its reactor thread and registered
 *    ApplicationThreads onto these.
 *
 * A component that was not admitted is not an error: it runs entirely in the
 * background tier, which is what cpu_pinning_enabled = false gives today.
 * demotion_reason() says why, so a demotion is diagnosable rather than showing
 * up only as unexplained latency.
 *
 * See docs/design/cpu_pinning_anti_affinity.md.
 */
class CpuLayout {
  public:
    ~CpuLayout() = default;
    CpuLayout() = default;

    /**
     * @brief Read the layout file and extract this component's entry.
     *
     * @param[in] layout_file_path Path to the generated run/cpu_layout.toml.
     * @param[in] component_name This component's key in the environment TOML,
     *                           for example "sequencer_secondary".
     * @return true and an empty string on success; false and a description of
     *         what was wrong otherwise.
     */
    [[nodiscard]] std::tuple<bool, std::string> load(const std::string& layout_file_path, const std::string& component_name);

    /// Cores shared by every unpinned thread on this machine. Never empty after a successful load.
    [[nodiscard]] const std::vector<CpuId>& background_cores() const {
        return background_cores_;
    }

    /// This component's dedicated cores, empty when it was not admitted.
    [[nodiscard]] const std::vector<CpuId>& hot_path_cores() const {
        return hot_path_cores_;
    }

    /// True when this component was allocated dedicated cores.
    [[nodiscard]] bool is_admitted() const {
        return admitted_;
    }

    /// Why this component was left in the background tier. Empty when admitted.
    [[nodiscard]] const std::string& demotion_reason() const {
        return demotion_reason_;
    }

    /// The machine key the layout was computed for, for reporting.
    [[nodiscard]] const std::string& machine_name() const {
        return machine_name_;
    }

    /**
     * @brief The background core this component's Quill backend is pinned to.
     *
     * Backends are given a specific background core rather than left to drift
     * under the scheduler, and the core is allocated by deploy.py rather than
     * chosen at run time so that thirteen backends do not all land on the same
     * CPU and so the affinity audit has something to check against.
     *
     * @return The core, or an unset optional when the layout named none, in
     *         which case the backend keeps the shared background mask.
     */
    [[nodiscard]] const std::optional<CpuId>& quill_backend_core() const {
        return quill_backend_core_;
    }

    /**
     * @brief Check that every core named in the layout still exists and is online.
     *
     * The layout is computed once at deploy time, so a machine that changes
     * shape afterwards -- cores offlined, a VM resized, hardware replaced --
     * leaves the file describing CPUs that are no longer there. Pinning to one
     * returns EINVAL. This must be a start-up error rather than a warning: a
     * latency-critical component running under a layout computed for different
     * hardware is worse than one that does not run. The remedy is to re-run
     * deploy.py.
     *
     * @return true and an empty string when every core is present; false and a
     *         description naming the missing cores otherwise.
     */
    [[nodiscard]] std::tuple<bool, std::string> verify_cores_present() const;

  private:
    std::vector<CpuId> background_cores_;
    std::vector<CpuId> hot_path_cores_;
    std::optional<CpuId> quill_backend_core_;
    std::string demotion_reason_;
    std::string machine_name_;
    bool admitted_{false};
};

/**
 * @brief Place the whole process in the background tier, before it makes threads.
 *
 * Call this from main() once the configuration is loaded and before the
 * application is constructed. Every thread created afterwards inherits the mask,
 * so the process is background by default and the Reactor need only promote the
 * few threads that were allocated dedicated cores. Getting the direction this
 * way round matters: the alternative is enumerating every thread that should
 * *not* be hot-path, and the ones that get forgotten are the ones that land on a
 * pinned core and contaminate it.
 *
 * The generated wrapper applies the same mask before the process starts, which
 * covers the JVM components and the window before main(). This is the backstop
 * that keeps production working when the launcher did not cooperate, so it is
 * deliberately duplicated effort.
 *
 * Applying an affinity mask is not a ratchet, so a later pin_thread_to_core()
 * onto a hot-path core still succeeds.
 *
 * @param[in] layout_file_path Path to the generated run/cpu_layout.toml.
 * @param[in] component_name This component's key in the environment TOML.
 * @return true and an empty string on success; false and a description
 *         otherwise. A caller that cannot mask itself should say so loudly
 *         rather than run unmasked, because an unmasked process is free to be
 *         scheduled onto the cores other components rely on.
 */
[[nodiscard]] std::tuple<bool, std::string> apply_background_affinity(const std::string& layout_file_path, const std::string& component_name);

} // namespaces
