#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <iostream>
#include <string>

namespace pubsub_itc_fw {

/*
 * Hot-path thread demand, and why the binary is asked rather than the config.
 *
 * deploy.py resolves each component's hot_path_rank into concrete core ids, so
 * it must know how many cores the component wants. That quantity is
 *
 *     reactor thread + the ApplicationThreads registered with the Reactor
 *
 * Threads registered through register_extra_thread() are background by default
 * and are deliberately excluded. That is not a simplification: OrderGatewayThread
 * registers FixCaptureWriter conditionally on fix_capture_enabled, so a count
 * that included extras would vary with a configuration flag, and any figure
 * written into the environment TOML would be correct for one setting of that
 * flag and silently wrong for the other.
 *
 * The count therefore lives in the code, never in the TOML. Each component
 * declares it next to where it registers its threads and answers
 * --hot-path-thread-count with it, which deploy.py invokes on the target host.
 * Answering is a bare print: the query must not construct the application, whose
 * constructor allocates arenas and registers services.
 *
 * That leaves the declared constant and the actual registrations as two things
 * that could drift apart. Reactor::verify_hot_path_thread_count() closes the
 * gap by checking the declaration against what was really registered, at
 * startup, where the truth is finally known. Adding an ApplicationThread without
 * updating the constant is then a loud startup failure rather than a component
 * quietly sharing a core it was never allocated.
 */

/**
 * @brief Answer a --hot-path-thread-count query if that is what was asked.
 *
 * Call this as the first statement in main(), before logging or configuration
 * are set up, so the answer is not buried in start-up output.
 *
 * @param[in] argument_count Value of argc as received by main().
 * @param[in] argument_values Value of argv as received by main().
 * @param[in] hot_path_thread_count The component's declared demand.
 * @return true when the query was answered and main() should return 0.
 */
[[nodiscard]] inline bool answer_hot_path_thread_count_query(int argument_count, char* argument_values[], size_t hot_path_thread_count) {
    if (argument_count != 2) {
        return false;
    }
    if (std::string{argument_values[1]} != "--hot-path-thread-count") {
        return false;
    }
    std::cout << hot_path_thread_count << "\n";
    return true;
}

} // namespaces
