#!/bin/bash
# Builds librdkafka, the Kafka client library, and its C++ wrapper librdkafka++, from an
# unpacked source release, and installs them under THIRDPARTY_DIR as
# installed/librdkafka/<version>: shared libraries, headers and CMake package files.
#
# Usage:
#   scripts/build_librdkafka.sh /path/to/librdkafka-2.15.1
#
# Nothing is downloaded, so this runs on a build host with no network. The source is only read:
# the build happens under /tmp, so the source may be mounted read-only in a container. The
# example programs are built but not installed; they stay in the build directory, which is
# printed at the end, for trying the library against a broker.
#
# Runs on Linux Mint and inside the Rocky Linux 8 container. THIRDPARTY_DIR is taken from the
# environment when set, and otherwise defaults to what scripts/devsetup.sh uses on each platform.

set -euo pipefail

usage() {
    sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'
}
if [ "$#" -eq 1 ] && { [ "$1" = "--help" ] || [ "$1" = "-h" ]; }; then
    usage
    exit 0
fi
if [ "$#" -ne 1 ]; then
    usage >&2
    exit 1
fi

SOURCE_DIR="$(cd "$1" && pwd)"
if [ ! -f "${SOURCE_DIR}/src/rdkafka.h" ] || [ ! -f "${SOURCE_DIR}/CMakeLists.txt" ]; then
    echo "ERROR: ${SOURCE_DIR} is not a librdkafka source release (no src/rdkafka.h)" >&2
    exit 1
fi

# The version is held in rdkafka.h as one hexadecimal number, 0xMMmmrrxx: major, minor,
# revision, and a pre-release byte that is ff for a release.
VERSION_HEX="$(sed -n 's/^#define RD_KAFKA_VERSION *0x\([0-9a-fA-F]\{8\}\).*/\1/p' "${SOURCE_DIR}/src/rdkafka.h")"
if [ -z "${VERSION_HEX}" ]; then
    echo "ERROR: could not read RD_KAFKA_VERSION from ${SOURCE_DIR}/src/rdkafka.h" >&2
    exit 1
fi
RDKAFKA_VERSION="$((16#${VERSION_HEX:0:2})).$((16#${VERSION_HEX:2:2})).$((16#${VERSION_HEX:4:2}))"

. /etc/os-release
case "${ID}${VERSION_ID}" in
    linuxmint22*)
        : "${THIRDPARTY_DIR:=/home/marlowa/mystuff/thirdparty}"
        ;;
    rocky8*|rhel8*|centos8*)
        : "${THIRDPARTY_DIR:=/development/3rdparty}"
        ;;
    *)
        echo "ERROR: unrecognised platform ${ID}${VERSION_ID}; set THIRDPARTY_DIR" >&2
        : "${THIRDPARTY_DIR:?}"
        ;;
esac

INSTALL_PREFIX="${THIRDPARTY_DIR}/installed/librdkafka/${RDKAFKA_VERSION}"
BUILD_DIR="${TMPDIR:-/tmp}/librdkafka-${RDKAFKA_VERSION}-build"

echo "============================================================"
echo "Building librdkafka ${RDKAFKA_VERSION}"
echo "Source:         ${SOURCE_DIR}"
echo "Install prefix: ${INSTALL_PREFIX}"
echo "============================================================"

rm -rf "${BUILD_DIR}"

# librdkafka switches most of its optional features on by itself whenever CMake happens to find
# the library each needs, so the same source built on two machines can produce two different
# libraries. Every feature is therefore set here, on or off:
#
# - Shared only: the project links its third-party libraries shared.
# - zlib on (compression), from the system package zlib-devel.
# - TLS on, from the system package openssl-devel: a production cluster will need it, and it is
#   cheaper to find out now whether it builds on RHEL8 than later.
# - SASL off. It brings in Cyrus SASL and, through it, Kerberos, and nothing here needs it.
# - curl off. It serves only OAuth sign-in to the cluster.
# - zstd off, and lz4 from the copy bundled with librdkafka rather than a system one, so that
#   neither depends on what a machine has installed.
# - Plugins off: they load code into the client at run time, which nothing here needs.
# - Tests off; the examples are built, for trying the library against a broker.
# - lib, not lib64, so the layout is the same on both platforms. librdkafka++ is given an RPATH
#   of its own directory so it finds librdkafka beside it without a library path.
cmake -S "${SOURCE_DIR}" -B "${BUILD_DIR}" \
    -DCMAKE_INSTALL_PREFIX="${INSTALL_PREFIX}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_LIBDIR=lib \
    -DCMAKE_INSTALL_RPATH='$ORIGIN' \
    -DRDKAFKA_BUILD_STATIC=OFF \
    -DRDKAFKA_BUILD_TESTS=OFF \
    -DRDKAFKA_BUILD_EXAMPLES=ON \
    -DWITH_ZLIB=ON \
    -DWITH_SSL=ON \
    -DWITH_SASL=OFF \
    -DWITH_CURL=OFF \
    -DWITH_ZSTD=OFF \
    -DENABLE_LZ4_EXT=OFF \
    -DWITH_PLUGINS=OFF

cmake --build "${BUILD_DIR}" --parallel "$(nproc)"
cmake --install "${BUILD_DIR}"

echo ""
echo "============================================================"
echo "librdkafka ${RDKAFKA_VERSION} installed to ${INSTALL_PREFIX}"
echo "============================================================"
echo ""
echo "Installed libraries:"
find "${INSTALL_PREFIX}" -name "librdkafka*.so*"
echo ""
echo "Built with: $(sed -n 's/^#define BUILT_WITH *"\(.*\)"/\1/p' "${BUILD_DIR}/generated/config.h" 2>/dev/null || true)"
echo "Example programs, not installed, in: ${BUILD_DIR}/examples"
