# Benchmarks for pubsub_itc_fw, written with Google Benchmark.
#
# Each program here measures one thing and is run by hand; none is run by the build or the test
# suites. They are installed into bin_benchmarks, not bin, so they are kept apart from the
# programs a deployment runs, and no release includes them.

if(ENABLE_COVERAGE OR ENABLE_ASAN OR ENABLE_TSAN)
    message(STATUS "Benchmarks disabled (coverage/sanitizer build)")
    return()
endif()

# Google Benchmark is looked for under ${THIRDPARTY_DIR}/installed/benchmark/${BENCHMARK_VERSION}
# (the prefix is added in the top-level CMakeLists.txt) and built by scripts/build_benchmark.sh.
# It is optional: these are measuring tools, not part of the venue, so a host without it builds
# everything else and says here that the benchmarks were left out.
find_package(benchmark CONFIG QUIET)
if(NOT benchmark_FOUND)
    message(STATUS "Benchmarks not built: Google Benchmark not found. Build it with scripts/build_benchmark.sh "
                   "and set BENCHMARK_VERSION (scripts/devsetup.sh does) to build them.")
    return()
endif()

message(STATUS "Building benchmarks for pubsub_itc_fw, with Google Benchmark ${benchmark_VERSION}")

# -----------------------------------------------------------------------------
# prometheus_observe_overhead: what recording one value in a histogram costs, through a virtual
# call and through the concrete handle the framework uses.
#
# HistogramsUnderTest.cpp is a separate source file on purpose: see the comment at the top of
# PrometheusObserveOverhead.cpp.
add_executable(prometheus_observe_overhead
    src/PrometheusObserveOverhead.cpp
    src/HistogramsUnderTest.cpp
)

target_link_libraries(prometheus_observe_overhead PRIVATE
    pubsub_itc_fw
    benchmark::benchmark
    pthread
)

target_include_directories(prometheus_observe_overhead PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

# No -O optimisation level here: the benchmark inherits the project's -O2 so that it measures the
# code that actually ships. See "Performance work" in coding-rules-for-ai-chatbots.txt.
target_compile_options(prometheus_observe_overhead PRIVATE
    -g
    -fno-omit-frame-pointer
)

if(CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "11.0")
    target_compile_options(prometheus_observe_overhead PRIVATE -mcx16 -march=x86-64-v2)
else()
    target_compile_options(prometheus_observe_overhead PRIVATE -mcx16 -msse4.2 -mpopcnt)
endif()

message(STATUS "  Added benchmark: prometheus_observe_overhead")

install(TARGETS prometheus_observe_overhead
    RUNTIME DESTINATION bin_benchmarks
)
