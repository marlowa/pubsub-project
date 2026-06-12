#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <memory>

/*
 * Note: The original plan was to run all tests twice with a random shuffle in between,
 * to expose idempotency issues. This is not done because tests that exercise the Quill
 * logger cannot be safely repeated within a single process invocation (Quill initialises
 * global state on first use). The double-run idea is therefore shelved; each test
 * executable runs its suite once.
 */

namespace pubsub_itc_fw::tests_common {

class TestRunner {
  public:
    static int run_tests(int argc, char** argv);
};

} // namespace pubsub_itc_fw::tests_common
