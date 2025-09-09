#include <gtest/gtest.h>

#include <pubsub_itc_fw/tests_common/TestRunner.hpp>

namespace pubsub_itc_fw::tests_common {

int TestRunner::runTests(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

} // namespace pubsub_itc_fw::tests_common

int main(int argc, char** argv) {
    using namespace pubsub_itc_fw::tests_common;
    return TestRunner::runTests(argc, argv);
}
