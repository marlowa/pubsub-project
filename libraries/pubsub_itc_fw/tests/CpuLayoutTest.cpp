// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file CpuLayoutTest.cpp
 * @brief Tests for CpuLayout::load(), which reads run/cpu_layout.toml.
 *
 * This class decides which cores every component in the venue runs on, and until these
 * tests it had no coverage at all -- not one line. That matters more than the figure: the
 * class exists so that a component which does not get its dedicated cores is, in its own
 * documentation's words, "diagnosable rather than showing up only as unexplained latency".
 * That promise rests entirely on load() parsing correctly and demotion_reason() reporting
 * honestly, and nothing was checking either. A silent mis-parse puts a component on the
 * wrong cores, which is precisely the failure the design exists to prevent and among the
 * most tedious to chase from the other end.
 *
 * load() is worth testing rather than exempting because it decides something, and it is
 * easy to test because it touches no hardware: std::filesystem plus TOML parsing, and
 * nine distinct ways to fail, every one reachable by writing a temporary file.
 *
 * verify_cores_present() and apply_background_affinity() are deliberately NOT tested here.
 * They call sched_setaffinity and interrogate the running machine, so what they do depends
 * on the host the suite happens to run on -- the kind of test that passes everywhere until
 * it matters. They remain honestly uncovered.
 */

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/CpuLayout.hpp>

using namespace pubsub_itc_fw;

namespace {

// Each test writes its own layout file into a directory named for it, so a failure leaves
// evidence that belongs to one test and parallel runs cannot collide.
class CpuLayoutTest : public ::testing::Test {
  protected:
    void SetUp() override {
        const ::testing::TestInfo* info = ::testing::UnitTest::GetInstance()->current_test_info();
        directory_ = std::filesystem::temp_directory_path() / (std::string("cpu_layout_test_") + info->name());
        std::filesystem::remove_all(directory_);
        std::filesystem::create_directories(directory_);
    }

    void TearDown() override {
        std::filesystem::remove_all(directory_);
    }

    // Write ``content`` as the layout file and return its path.
    std::string write_layout(const std::string& content) {
        const std::filesystem::path path = directory_ / "cpu_layout.toml";
        std::ofstream file(path);
        file << content;
        file.close();
        return path.string();
    }

    // A path inside the fixture's directory that deliberately has no file behind it.
    std::string absent_path() const {
        return (directory_ / "not_generated_yet.toml").string();
    }

    static std::vector<int> core_values(const std::vector<CpuId>& cores) {
        std::vector<int> values;
        values.reserve(cores.size());
        for (const CpuId core : cores) {
            values.push_back(core.get_value());
        }
        return values;
    }

    std::filesystem::path directory_;
};

// A layout with one admitted component and one demoted one, which is the shape deploy.py
// produces on a host that cannot give every component its own cores.
const char* const representative_layout = R"(
[machine]
name = "test-host"
background_cores = "6-8"

[components.fix_order_gateway_a]
admitted = true
hot_path_cores = "1-2"
quill_backend_core = 7

[components.matching_engine_primary]
admitted = false
demotion_reason = "ranked below the cores this machine had left"
)";

} // namespaces

TEST_F(CpuLayoutTest, EmptyFilePathIsRejected) {
    CpuLayout layout;
    const auto [loaded, error] = layout.load("", "fix_order_gateway_a");
    EXPECT_FALSE(loaded);
    EXPECT_NE(error.find("no CPU layout file configured"), std::string::npos) << error;
}

TEST_F(CpuLayoutTest, EmptyComponentNameIsRejected) {
    CpuLayout layout;
    const auto [loaded, error] = layout.load(write_layout(representative_layout), "");
    EXPECT_FALSE(loaded);
    EXPECT_NE(error.find("no CPU layout component name configured"), std::string::npos) << error;
}

// The message must name deploy.py: this is what a component prints when it starts before
// the layout has ever been generated, and the reader needs to be told the remedy.
TEST_F(CpuLayoutTest, MissingFileIsRejectedAndNamesTheRemedy) {
    CpuLayout layout;
    const auto [loaded, error] = layout.load(absent_path(), "fix_order_gateway_a");
    EXPECT_FALSE(loaded);
    EXPECT_NE(error.find("does not exist"), std::string::npos) << error;
    EXPECT_NE(error.find("deploy.py"), std::string::npos) << error;
}

TEST_F(CpuLayoutTest, UnparseableFileIsRejected) {
    const std::string path = write_layout("[machine\nthis is not valid TOML = = =\n");
    CpuLayout layout;
    const auto [loaded, error] = layout.load(path, "fix_order_gateway_a");
    EXPECT_FALSE(loaded);
    EXPECT_NE(error.find("could not be parsed"), std::string::npos) << error;
}

TEST_F(CpuLayoutTest, MissingBackgroundCoresIsRejected) {
    const std::string path = write_layout(R"(
[machine]
name = "test-host"

[components.fix_order_gateway_a]
admitted = true
hot_path_cores = "1-2"
)");
    CpuLayout layout;
    const auto [loaded, error] = layout.load(path, "fix_order_gateway_a");
    EXPECT_FALSE(loaded);
    EXPECT_NE(error.find("no machine.background_cores"), std::string::npos) << error;
}

// An empty CPU set is EINVAL to sched_setaffinity, and every process has at least a Quill
// backend that must run somewhere, so this can never be a legitimate layout.
TEST_F(CpuLayoutTest, EmptyBackgroundPoolIsRejected) {
    const std::string path = write_layout(R"(
[machine]
background_cores = ""

[components.fix_order_gateway_a]
admitted = true
hot_path_cores = "1-2"
)");
    CpuLayout layout;
    const auto [loaded, error] = layout.load(path, "fix_order_gateway_a");
    EXPECT_FALSE(loaded);
    EXPECT_NE(error.find("empty background pool"), std::string::npos) << error;
}

// A component absent from the layout is a deployment mistake rather than a demotion:
// nothing decided where it should run, so it must not start as though it had been demoted.
TEST_F(CpuLayoutTest, ComponentAbsentFromLayoutIsRejected) {
    CpuLayout layout;
    const auto [loaded, error] = layout.load(write_layout(representative_layout), "sequencer_primary");
    EXPECT_FALSE(loaded);
    EXPECT_NE(error.find("no entry for component 'sequencer_primary'"), std::string::npos) << error;
    EXPECT_NE(error.find("re-run deploy.py"), std::string::npos) << error;
}

TEST_F(CpuLayoutTest, AdmittedWithoutHotPathCoresIsRejected) {
    const std::string path = write_layout(R"(
[machine]
background_cores = "6-8"

[components.fix_order_gateway_a]
admitted = true
)");
    CpuLayout layout;
    const auto [loaded, error] = layout.load(path, "fix_order_gateway_a");
    EXPECT_FALSE(loaded);
    EXPECT_NE(error.find("has no hot_path_cores"), std::string::npos) << error;
}

TEST_F(CpuLayoutTest, AdmittedWithEmptyHotPathCoresIsRejected) {
    const std::string path = write_layout(R"(
[machine]
background_cores = "6-8"

[components.fix_order_gateway_a]
admitted = true
hot_path_cores = ""
)");
    CpuLayout layout;
    const auto [loaded, error] = layout.load(path, "fix_order_gateway_a");
    EXPECT_FALSE(loaded);
    EXPECT_NE(error.find("hot_path_cores list is empty"), std::string::npos) << error;
}

TEST_F(CpuLayoutTest, AdmittedComponentReadsItsCoresAndExpandsRanges) {
    CpuLayout layout;
    const auto [loaded, error] = layout.load(write_layout(representative_layout), "fix_order_gateway_a");
    ASSERT_TRUE(loaded) << error;

    EXPECT_TRUE(layout.is_admitted());
    EXPECT_EQ(layout.machine_name(), "test-host");
    // "6-8" and "1-2" are ranges in the file: the point of checking the expansion is that
    // a component silently masked to core 6 alone would look entirely plausible.
    EXPECT_EQ(core_values(layout.background_cores()), (std::vector<int>{6, 7, 8}));
    EXPECT_EQ(core_values(layout.hot_path_cores()), (std::vector<int>{1, 2}));
    ASSERT_TRUE(layout.quill_backend_core().has_value());
    EXPECT_EQ(layout.quill_backend_core().value().get_value(), 7);
}

// Not being admitted is a normal outcome, not a failure: the component runs entirely in
// the background tier. load() must succeed, and must say why it was demoted.
TEST_F(CpuLayoutTest, DemotedComponentLoadsAndReportsWhy) {
    CpuLayout layout;
    const auto [loaded, error] = layout.load(write_layout(representative_layout), "matching_engine_primary");
    ASSERT_TRUE(loaded) << error;

    EXPECT_FALSE(layout.is_admitted());
    EXPECT_TRUE(layout.hot_path_cores().empty());
    EXPECT_EQ(layout.background_cores().size(), 3u);
    EXPECT_EQ(layout.demotion_reason(), "ranked below the cores this machine had left");
}

// machine.name is reporting only, and quill_backend_core is absent for the JVM components
// which have no Quill backend at all. Neither may turn a usable layout into a failure.
TEST_F(CpuLayoutTest, MachineNameAndQuillBackendCoreAreOptional) {
    const std::string path = write_layout(R"(
[machine]
background_cores = "6-8"

[components.admin_service]
admitted = false
)");
    CpuLayout layout;
    const auto [loaded, error] = layout.load(path, "admin_service");
    ASSERT_TRUE(loaded) << error;

    EXPECT_TRUE(layout.machine_name().empty());
    EXPECT_FALSE(layout.quill_backend_core().has_value());
    EXPECT_FALSE(layout.is_admitted());
}
