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
 * verify_cores_present() and apply_background_affinity() are tested in the ways that do not
 * depend on the machine the suite runs on: core 0, which Linux does not take offline, is always
 * present, and core 4000 is beyond any machine this runs on. The failures of
 * apply_background_affinity() are reached before it changes anything, and the tests check that
 * the process's mask is left as it was.
 */

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <sched.h>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/CpuLayout.hpp>
#include <pubsub_itc_fw/tests_common/ScratchDirectory.hpp>

using namespace pubsub_itc_fw;

namespace {

// Each test writes its own layout file into a directory named for it, so a failure leaves
// evidence that belongs to one test and parallel runs cannot collide.
class CpuLayoutTest : public ::testing::Test {
  protected:
    void SetUp() override {
        const ::testing::TestInfo* info = ::testing::UnitTest::GetInstance()->current_test_info();
        directory_ = tests_common::make_scratch_directory(std::string("cpu_layout_test_") + info->name());
    }

    void TearDown() override {
        tests_common::remove_scratch_directory(directory_.string());
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

namespace {

// A layout giving the named component the given cores, all of them in the background pool too.
std::string layout_with_cores(const std::string& cores) {
    return "[machine]\nbackground_cores = \"" + cores + "\"\n\n[components.sequencer_primary]\nadmitted = true\nhot_path_cores = \"" + cores + "\"\n";
}

bool masks_equal(const cpu_set_t& first, const cpu_set_t& second) {
    return CPU_EQUAL(&first, &second) != 0;
}

} // namespaces

TEST_F(CpuLayoutTest, CoresThatAreOnlineArePresent) {
    CpuLayout layout;
    const auto [loaded, error] = layout.load(write_layout(layout_with_cores("0")), "sequencer_primary");
    ASSERT_TRUE(loaded) << error;
    const auto [present, missing] = layout.verify_cores_present();
    EXPECT_TRUE(present) << missing;
}

TEST_F(CpuLayoutTest, ACoreThatIsNotOnlineIsNamed) {
    CpuLayout layout;
    const auto [loaded, error] = layout.load(write_layout(layout_with_cores("0,4000")), "sequencer_primary");
    ASSERT_TRUE(loaded) << error;
    const auto [present, missing] = layout.verify_cores_present();
    EXPECT_FALSE(present);
    EXPECT_NE(missing.find("4000"), std::string::npos) << missing;
    EXPECT_NE(missing.find("re-run deploy.py"), std::string::npos) << missing;
}

TEST_F(CpuLayoutTest, ApplyingTheBackgroundMaskFromAMissingLayoutFailsAndChangesNothing) {
    cpu_set_t before;
    CPU_ZERO(&before);
    ASSERT_EQ(::sched_getaffinity(0, sizeof(before), &before), 0);
    const auto [applied, error] = apply_background_affinity(absent_path(), "sequencer_primary");
    EXPECT_FALSE(applied);
    EXPECT_FALSE(error.empty());
    cpu_set_t after;
    CPU_ZERO(&after);
    ASSERT_EQ(::sched_getaffinity(0, sizeof(after), &after), 0);
    EXPECT_TRUE(masks_equal(before, after));
}

TEST_F(CpuLayoutTest, ApplyingTheBackgroundMaskWithACoreThatIsNotOnlineFailsAndChangesNothing) {
    cpu_set_t before;
    CPU_ZERO(&before);
    ASSERT_EQ(::sched_getaffinity(0, sizeof(before), &before), 0);
    const auto [applied, error] = apply_background_affinity(write_layout(layout_with_cores("0,4000")), "sequencer_primary");
    EXPECT_FALSE(applied);
    EXPECT_NE(error.find("4000"), std::string::npos) << error;
    cpu_set_t after;
    CPU_ZERO(&after);
    ASSERT_EQ(::sched_getaffinity(0, sizeof(after), &after), 0);
    EXPECT_TRUE(masks_equal(before, after));
}
