#!/bin/bash
# Builds and installs prometheus-cpp into the thirdparty directory.
# Run this inside the RHEL8/Rocky Linux 8 Docker container.

set -euo pipefail

PROMETHEUS_VERSION="1.3.0"
INSTALL_PREFIX="/development/3rdparty/installed/prometheus-cpp/${PROMETHEUS_VERSION}"
BUILD_DIR="/tmp/prometheus-cpp-build"

echo "============================================================"
echo "Building prometheus-cpp ${PROMETHEUS_VERSION}"
echo "Install prefix: ${INSTALL_PREFIX}"
echo "============================================================"

# Download. The plain source tarball for the tag is not enough: civetweb is a
# git submodule, and a tag archive has the submodule directory but not its
# contents. The release carries a separate archive with the submodules folded
# in, which is what USE_THIRDPARTY_LIBRARIES=ON needs.
mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

ARCHIVE="prometheus-cpp-with-submodules.tar.gz"
if [ ! -f "${ARCHIVE}" ]; then
    wget "https://github.com/jupp0r/prometheus-cpp/releases/download/v${PROMETHEUS_VERSION}/${ARCHIVE}" \
        -O "${ARCHIVE}"
fi

rm -rf "prometheus-cpp-with-submodules"
tar xzf "${ARCHIVE}"
cd "prometheus-cpp-with-submodules"

# The options match the Mint build exactly -- pull, push and compression all on,
# civetweb bundled rather than external, no SSL in civetweb. They are recorded in
# the installed prometheus-cpp-config.cmake, which decides what find_dependency()
# the consuming project is made to satisfy, so a platform that diverges here
# fails to configure on that platform alone. Shared, because the project links
# prometheus-cpp shared.
cmake -S . -B build \
    -DCMAKE_INSTALL_PREFIX="${INSTALL_PREFIX}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DBUILD_SHARED_LIBS=ON \
    -DENABLE_PULL=ON \
    -DENABLE_PUSH=ON \
    -DENABLE_COMPRESSION=ON \
    -DUSE_THIRDPARTY_LIBRARIES=ON \
    -DENABLE_TESTING=OFF

cmake --build build --parallel "$(nproc)"
cmake --install build

echo ""
echo "============================================================"
echo "prometheus-cpp ${PROMETHEUS_VERSION} installed to ${INSTALL_PREFIX}"
echo "============================================================"

# Verify
echo ""
echo "Installed cmake files:"
find "${INSTALL_PREFIX}" -name "*.cmake"

echo ""
echo "Installed libraries:"
find "${INSTALL_PREFIX}" -name "libprometheus-cpp-*"
