# fix_codec is a header-only, application-tier FIX codec library. It sits above
# pubsub_itc_fw (never inside it) and is the FIX counterpart to scram_crypto:
# a zero-copy reader/writer plus a generated dictionary header. Consumers include
# <fix_codec/...> for the hand-written headers and <fix_codec/fix_dictionary.hpp>
# for the generated one (produced into ${FIX_DICT_INCLUDE_DIR} at build time).
add_library(fix_codec INTERFACE)

target_include_directories(fix_codec INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/..
        ${FIX_DICT_INCLUDE_DIR}
)

# fmt is the library's only third-party dependency, used by FixReject::describe for a
# bounded, non-allocating format into the caller's buffer. It replaced snprintf, which
# gcc 8.5 on RHEL8 rejected under -Werror=format-truncation at call sites with a small
# constant capacity -- truncation being the documented behaviour there, not a defect.
# Because this is an INTERFACE target the dependency propagates to every consumer; that
# is accepted, as fmt is already present project-wide via quill.
target_link_libraries(fix_codec INTERFACE fmt::fmt)

# ── Unit tests ────────────────────────────────────────────────────────────────
add_executable(fix_codec_tests
        tests/FixChecksumTest.cpp
        tests/FixFieldTest.cpp
        tests/FixMessageReaderTest.cpp
        tests/FixMessageReaderAdversarialTest.cpp
        tests/FixMessageValidatorTest.cpp
        tests/FixMessageWriterTest.cpp
        tests/FixRejectTest.cpp
)

target_link_libraries(fix_codec_tests PRIVATE
        fix_codec
        GTest::gtest_main
)

# The generated dictionary header must exist before the tests compile.
add_dependencies(fix_codec_tests fix_dictionary_generated)

include(GoogleTest)
if(ENABLE_TSAN)
    gtest_discover_tests(fix_codec_tests DISCOVERY_MODE PRE_TEST)
else()
    gtest_discover_tests(fix_codec_tests)
endif()

install(TARGETS fix_codec_tests
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

# ── Performance benchmark ─────────────────────────────────────────────────────
# Measures NewOrderSingle parse vs parse+full-validate. A tight, long-running loop
# so it can be profiled directly under perf; installs to bin.
add_executable(fix_codec_bench
        performance/FixCodecBenchMain.cpp
)

target_link_libraries(fix_codec_bench PRIVATE fix_codec)

add_dependencies(fix_codec_bench fix_dictionary_generated)

install(TARGETS fix_codec_bench
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

# ── Performance regression guard ──────────────────────────────────────────────
# A separate binary from fix_codec_tests: these assert on timing rather than on
# behaviour, so a failure means something different and should not be mixed into the
# unit test result. build.py runs it after the unit and integration suites, and
# --no-performance-tests skips just this one.
#
# It deliberately inherits the project's -O2 rather than setting -O3 as the
# pubsub_itc_fw harnesses do, because the point is to measure the code that ships.
# Not built under coverage or a sanitizer, where the instrumentation is what gets timed.
if(NOT (ENABLE_COVERAGE OR ENABLE_ASAN OR ENABLE_TSAN OR ENABLE_VALGRIND))
    add_executable(fix_codec_performance_tests
            performance/FixCodecPerformanceTest.cpp
    )

    target_link_libraries(fix_codec_performance_tests PRIVATE
            fix_codec
            GTest::gtest_main
    )

    add_dependencies(fix_codec_performance_tests fix_dictionary_generated)

    install(TARGETS fix_codec_performance_tests
            RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    )
endif()
