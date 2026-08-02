#!/bin/bash
# Builds and installs fmt into the thirdparty directory.
# Run this inside the RHEL8/Rocky Linux 8 Docker container.

set -euo pipefail

FMT_VERSION="11.0.2"
INSTALL_PREFIX="/development/3rdparty/installed/fmt/${FMT_VERSION}"
BUILD_DIR="/tmp/fmt-build"

echo "============================================================"
echo "Building fmt ${FMT_VERSION}"
echo "Install prefix: ${INSTALL_PREFIX}"
echo "============================================================"

# Download into /tmp, not the current directory. Run from the project root this used to leave
# the tarball and the unpacked source behind in the tree, and a bare wget will not overwrite an
# existing file -- it saves alongside as .1 and the build then silently uses whatever stale
# tarball was already there.
mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

ARCHIVE="fmt-${FMT_VERSION}.tar.gz"
if [ ! -f "${ARCHIVE}" ]; then
    wget "https://github.com/fmtlib/fmt/archive/refs/tags/${FMT_VERSION}.tar.gz" -O "${ARCHIVE}"
fi

rm -rf "fmt-${FMT_VERSION}"
tar xzf "${ARCHIVE}"
cd "fmt-${FMT_VERSION}"

# Shared, to match the Mint build and the work RHEL8 environment, where every third-party
# library is shared. Installed binaries find it through the RPATH the project records; nothing
# is copied out of the thirdparty tree.
cmake -S . -B build \
    -DCMAKE_INSTALL_PREFIX="${INSTALL_PREFIX}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=ON \
    -DFMT_TEST=OFF \
    -DFMT_DOC=OFF

cmake --build build --parallel "$(nproc)"
cmake --install build

echo ""
echo "============================================================"
echo "fmt ${FMT_VERSION} installed to ${INSTALL_PREFIX}"
echo "============================================================"

# Verify
echo ""
echo "Installed libraries:"
find "${INSTALL_PREFIX}" -name "libfmt.*"
