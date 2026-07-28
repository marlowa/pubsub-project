// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include <quill/Backend.h>

#include <pubsub_itc_fw/CpuLayout.hpp>

#include <pubsub_itc_fw/CpuPinning.hpp>
#include <pubsub_itc_fw/TomlConfiguration.hpp>

namespace pubsub_itc_fw {

namespace {

std::string join_core_ids(const std::vector<CpuId>& core_ids) {
    std::string result;
    for (const CpuId core_id : core_ids) {
        if (!result.empty()) {
            result += ",";
        }
        result += std::to_string(core_id.get_value());
    }
    return result;
}

} // namespaces

std::tuple<bool, std::string> CpuLayout::load(const std::string& layout_file_path, const std::string& component_name) {
    if (layout_file_path.empty()) {
        return {false, "no CPU layout file configured"};
    }
    if (component_name.empty()) {
        return {false, "no CPU layout component name configured -- the component cannot find its own entry"};
    }
    if (!std::filesystem::exists(layout_file_path)) {
        return {false, "CPU layout file '" + layout_file_path + "' does not exist -- run deploy.py to generate it"};
    }

    TomlConfiguration layout_file;
    const auto [parsed, parse_error] = layout_file.load_file(layout_file_path);
    if (!parsed) {
        return {false, "CPU layout file '" + layout_file_path + "' could not be parsed: " + parse_error};
    }

    std::string background_list;
    const auto [has_background, background_error] = layout_file.get_required("machine.background_cores", background_list);
    if (!has_background) {
        return {false, "CPU layout file '" + layout_file_path + "' has no machine.background_cores: " + background_error};
    }
    background_cores_ = detail::parse_cpu_list(background_list);
    if (background_cores_.empty()) {
        // An empty CPU set is EINVAL, and every process has at least a Quill
        // backend that needs somewhere to run, so this can never be legitimate.
        return {false, "CPU layout file '" + layout_file_path + "' declares an empty background pool"};
    }

    // Reporting only; a layout file without it is still usable.
    static_cast<void>(layout_file.get_required("machine.name", machine_name_));

    const std::string component_key = "components." + component_name;

    const auto [has_admitted, admitted_error] = layout_file.get_required(component_key + ".admitted", admitted_);
    if (!has_admitted) {
        // A component absent from the layout is a deployment mistake, not a
        // demotion: it means the environment TOML's [machines.*] entry does not
        // list it, so nothing decided where it should run.
        return {false, "CPU layout file '" + layout_file_path + "' has no entry for component '" + component_name +
                           "' -- add it to the [machines.*] list for this host and re-run deploy.py"};
    }

    if (admitted_) {
        std::string hot_path_list;
        const auto [has_cores, cores_error] = layout_file.get_required(component_key + ".hot_path_cores", hot_path_list);
        if (!has_cores) {
            return {false, "component '" + component_name + "' is admitted in '" + layout_file_path + "' but has no hot_path_cores: " + cores_error};
        }
        hot_path_cores_ = detail::parse_cpu_list(hot_path_list);
        if (hot_path_cores_.empty()) {
            return {false, "component '" + component_name + "' is admitted in '" + layout_file_path + "' but its hot_path_cores list is empty"};
        }
    } else {
        static_cast<void>(layout_file.get_required(component_key + ".demotion_reason", demotion_reason_));
    }

    // Absent for the JVM components, which have no Quill backend at all.
    int32_t backend_core = 0;
    const auto [has_backend_core, backend_core_error] = layout_file.get_required(component_key + ".quill_backend_core", backend_core);
    static_cast<void>(backend_core_error);
    if (has_backend_core) {
        quill_backend_core_ = CpuId{backend_core};
    }

    return {true, ""};
}

std::tuple<bool, std::string> CpuLayout::verify_cores_present() const {
    const SharedCoreRegistryLayout empty_registry{};
    const AvailableCpuVector online = get_available_cpu_ids(false, empty_registry); // bool-arg-ok

    std::vector<CpuId> missing;
    const auto is_online = [&online](CpuId core_id) {
        return std::any_of(online.begin(), online.end(), [core_id](const CpuAssignment& candidate) { return candidate.cpu_id == core_id; });
    };

    for (const CpuId core_id : background_cores_) {
        if (!is_online(core_id)) {
            missing.push_back(core_id);
        }
    }
    for (const CpuId core_id : hot_path_cores_) {
        if (!is_online(core_id)) {
            missing.push_back(core_id);
        }
    }
    if (quill_backend_core_.has_value() && !is_online(quill_backend_core_.value())) {
        missing.push_back(quill_backend_core_.value());
    }

    if (!missing.empty()) {
        return {false, "the CPU layout names core(s) " + join_core_ids(missing) +
                           " which are not online -- the machine has changed shape since deploy.py computed the layout; re-run deploy.py"};
    }
    return {true, ""};
}

std::tuple<bool, std::string> apply_background_affinity(const std::string& layout_file_path, const std::string& component_name) {
    CpuLayout layout;
    const auto [loaded, load_error] = layout.load(layout_file_path, component_name);
    if (!loaded) {
        return {false, load_error};
    }

    const auto [cores_present, missing_error] = layout.verify_cores_present();
    if (!cores_present) {
        return {false, missing_error};
    }

    if (!apply_process_affinity_mask(layout.background_cores())) {
        return {false, "failed to mask this process to background cores " + format_cpu_list(layout.background_cores())};
    }

    // The Quill backend must be dealt with here rather than left to inheritance.
    // sched_setaffinity() changes the calling thread and threads created after
    // it; the backend is neither, because it starts on first logger
    // construction, which happens before the configuration naming this file has
    // even been read. Left alone it keeps an unrestricted mask and is free to be
    // scheduled onto the very cores this design reserves.
    //
    // It is pinned to one allocated background core rather than given the whole
    // background mask, because a backend that stays put is more deterministic
    // than one drifting under the scheduler. deploy.py does the allocating, so
    // the backends spread across the pool instead of piling onto one core.
    const auto quill_thread_id = static_cast<pid_t>(quill::Backend::get_thread_id());
    if (quill_thread_id == 0) {
        return {false, "the Quill backend thread has not started, so it cannot be moved out of the hot-path cores"};
    }

    if (layout.quill_backend_core().has_value()) {
        if (!pin_tid_to_core(quill_thread_id, layout.quill_backend_core().value())) {
            return {false, "failed to pin the Quill backend thread to background CPU " + std::to_string(layout.quill_backend_core().value().get_value())};
        }
    } else if (!apply_thread_affinity_mask(quill_thread_id, layout.background_cores())) {
        return {false, "failed to mask the Quill backend thread to background cores " + format_cpu_list(layout.background_cores())};
    }

    return {true, ""};
}

} // namespaces
