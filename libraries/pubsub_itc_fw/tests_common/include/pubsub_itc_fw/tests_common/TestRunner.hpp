#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <memory>

/*
Note: This central unit test runner was going to ensure that each unit test program adjusts its argument list
so that by default the program runs all the tests twice, doing a random shuffle between.
The idea is to shake out idempotency issues in the tests and possibly in the code being tested as well.
However, this doesn't work with quill.

TODO I am not sure what to do about this for the moment because one of our unit tests
performs tests on the quill wrapper.
These cannot be repeated within a single invocation of the executable.

 */

namespace pubsub_itc_fw::tests_common {

class TestRunner {
  public:
    static int runTests(int argc, char** argv);
};

} // namespace pubsub_itc_fw::tests_common
