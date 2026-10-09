#!/bin/bash
# Builds and installs Google Benchmark into the thirdparty directory.
#
# Builds from a tarball already copied into the thirdparty directory; nothing is downloaded,
# so this also works on a build host with no network. The tarball is the source archive of
# the release tag, benchmark-<version>.tar.gz, whose top directory is benchmark-<version>.
#
# THIRDPARTY_DIR says where the thirdparty directory is. Inside the RHEL8/Rocky Linux 8
# Docker container it is /development/3rdparty, the default; on the Mint workstation run
#   THIRDPARTY_DIR=/home/marlowa/mystuff/thirdparty scripts/build_benchmark.sh
#
# The source is unpacked and built in tmp/benchmark-build under the directory the script is run
# from, not in the shared /tmp. Besides being shared with every user of the machine, /tmp is
# often mounted noexec on RHEL8 hosts, and that breaks this build: while configuring,
# benchmark's CMake compiles small test programs in the build directory and runs them, to find
# out which regular expression library to use and whether the clock and thread functions it
# wants exist. Where programs may not be run, every check fails and CMake stops with "Failed to
# determine the source files for the regular expression backend".

set -euo pipefail

BENCHMARK_VERSION="1.9.5"
THIRDPARTY_DIR="${THIRDPARTY_DIR:-/development/3rdparty}"
ARCHIVE="${THIRDPARTY_DIR}/benchmark-${BENCHMARK_VERSION}.tar.gz"
INSTALL_PREFIX="${THIRDPARTY_DIR}/installed/benchmark/${BENCHMARK_VERSION}"
BUILD_DIR="${PWD}/tmp/benchmark-build"

echo "============================================================"
echo "Building Google Benchmark ${BENCHMARK_VERSION}"
echo "Source archive: ${ARCHIVE}"
echo "Install prefix: ${INSTALL_PREFIX}"
echo "Build directory: ${BUILD_DIR}"
echo "============================================================"

if [ ! -f "${ARCHIVE}" ]; then
    echo "ERROR: ${ARCHIVE} not found. Copy the release's source archive there first," >&2
    echo "       or set THIRDPARTY_DIR to the directory that holds it." >&2
    exit 1
fi

# Unpacked afresh into a scratch directory each time, so a build never picks up files left
# by an earlier one, and the copy already unpacked in the thirdparty directory is not touched.
rm -rf "${BUILD_DIR}"
mkdir -p "${BUILD_DIR}"
tar xzf "${ARCHIVE}" -C "${BUILD_DIR}"
cd "${BUILD_DIR}/benchmark-${BENCHMARK_VERSION}"

# The library only: benchmark's own tests are off, because they need GoogleTest, which it
# would otherwise try to download. -Werror is off because benchmark turns it on for release
# builds, and a warning that a newer or older compiler raises in benchmark's own code is not
# a reason to fail this build. Static, which is benchmark's default, so a benchmark program
# carries the library with it; position-independent so it can still be linked into a shared
# library. CXXFEATURECHECK_DEBUG makes each of the checks described at the top print the
# compiler's or the program's output when it fails, so a failure says why.
cmake -S . -B build \
    -DCXXFEATURECHECK_DEBUG=ON \
    -DCMAKE_INSTALL_PREFIX="${INSTALL_PREFIX}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DBENCHMARK_ENABLE_TESTING=OFF \
    -DBENCHMARK_ENABLE_GTEST_TESTS=OFF \
    -DBENCHMARK_ENABLE_WERROR=OFF \
    -DBENCHMARK_DOWNLOAD_DEPENDENCIES=OFF \
    -DBENCHMARK_ENABLE_INSTALL=ON

cmake --build build --parallel "$(nproc)"
cmake --install build

echo ""
echo "============================================================"
echo "Google Benchmark ${BENCHMARK_VERSION} installed to ${INSTALL_PREFIX}"
echo "============================================================"

# Verify
echo ""
echo "Installed cmake files:"
find "${INSTALL_PREFIX}" -name "*.cmake"

echo ""
echo "Installed libraries:"
find "${INSTALL_PREFIX}" -name "libbenchmark*"
