// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.

#ifndef CLANG_TIDY
// TODO this whole file has to be neutralised for clang-tidy
// The quill backend sleep parameters are part of the problem
// but there is aslso a weird include cycle reported by clang-tidy from the gtest headers.
#include <chrono>

#include <gtest/gtest.h>

#include <quill/Backend.h>

#include <pubsub_itc_fw/tests_common/TestRunner.hpp>

namespace pubsub_itc_fw::tests_common {

int TestRunner::runTests(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

} // namespace pubsub_itc_fw::tests_common

int main(int argc, char** argv) {
#ifndef USING_VALGRIND
    // Start Quill backend with synchronous options for tests
    // The quill backend is started using std::call_once, hence it has to be started here.
    quill::BackendOptions backend_options{};
    backend_options.sleep_duration = std::chrono::nanoseconds{0};
    backend_options.sink_min_flush_interval = std::chrono::milliseconds{0};
    quill::Backend::start(backend_options);
#endif

    using namespace pubsub_itc_fw::tests_common;
    return TestRunner::runTests(argc, argv);
}
#endif
