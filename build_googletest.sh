#!/bin/bash
# Builds and installs GoogleTest 1.10.0 into the thirdparty directory.
# Run this inside the RHEL8/Rocky Linux 8 Docker container.

set -euo pipefail

GTEST_VERSION="1.10.0"
INSTALL_PREFIX="/workspace/thirdparty/installed/googletest/${GTEST_VERSION}"
BUILD_DIR="/tmp/googletest-build"

echo "============================================================"
echo "Building GoogleTest ${GTEST_VERSION}"
echo "Install prefix: ${INSTALL_PREFIX}"
echo "============================================================"

# Download
mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

if [ ! -f "v${GTEST_VERSION}.tar.gz" ]; then
    wget "https://github.com/google/googletest/archive/refs/tags/release-${GTEST_VERSION}.tar.gz" \
        -O "v${GTEST_VERSION}.tar.gz"
fi

tar xzf "v${GTEST_VERSION}.tar.gz"
cd "googletest-release-${GTEST_VERSION}"

# Build and install
cmake -S . -B build \
    -DCMAKE_INSTALL_PREFIX="${INSTALL_PREFIX}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_GMOCK=ON \
    -DINSTALL_GTEST=ON

cmake --build build --parallel "$(nproc)"
cmake --install build

echo ""
echo "============================================================"
echo "GoogleTest ${GTEST_VERSION} installed to ${INSTALL_PREFIX}"
echo "============================================================"

# Verify
echo ""
echo "Installed cmake files:"
find "${INSTALL_PREFIX}" -name "*.cmake"
