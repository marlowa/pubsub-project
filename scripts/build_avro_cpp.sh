#!/bin/bash
# Builds the C++ part of Apache Avro from an unpacked source release, and installs it under
# THIRDPARTY_DIR as installed/avro-cpp/<version>: the libraries, the headers, the CMake package
# files and the avrogencpp code generator in bin/.
#
# Usage:
#   scripts/build_avro_cpp.sh /path/to/avro-release-1.12.2
#
# The argument is the top of the unpacked release, the directory holding share/VERSION.txt and
# lang/. Nothing is downloaded, so this runs on a build host with no network. The source is only
# read: it is copied under /tmp and built there, so it may be mounted read-only in a container.
#
# Avro is built against, and linked to, the fmt the project uses, from THIRDPARTY_DIR. Upstream,
# Avro compiles its own copy of the fmt code it needs and does not tell a project that uses it
# that fmt is needed. One line of its CMakeLists.txt is changed in the copy to link fmt::fmt
# instead, which gives the process one fmt, the project's, and makes the installed package name
# fmt as a dependency.
#
# Runs on Linux Mint and inside the Rocky Linux 8 container. THIRDPARTY_DIR and FMT_VERSION are
# taken from the environment when set, and otherwise default to what scripts/devsetup.sh uses
# on each platform, so the library is built against the same fmt the project uses.

set -euo pipefail

usage() {
    sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'
}
if [ "$#" -eq 1 ] && { [ "$1" = "--help" ] || [ "$1" = "-h" ]; }; then
    usage
    exit 0
fi
if [ "$#" -ne 1 ]; then
    usage >&2
    exit 1
fi

RELEASE_DIR="$(cd "$1" && pwd)"
SOURCE_DIR="${RELEASE_DIR}/lang/c++"
if [ ! -f "${RELEASE_DIR}/share/VERSION.txt" ] || [ ! -f "${SOURCE_DIR}/CMakeLists.txt" ]; then
    echo "ERROR: ${RELEASE_DIR} is not the top of an Avro source release (no share/VERSION.txt or lang/c++)" >&2
    exit 1
fi
AVRO_VERSION="$(tr -d '[:space:]' < "${RELEASE_DIR}/share/VERSION.txt")"

. /etc/os-release
case "${ID}${VERSION_ID}" in
    linuxmint22*)
        : "${THIRDPARTY_DIR:=/home/marlowa/mystuff/thirdparty}"
        : "${FMT_VERSION:=12.1.0}"
        ;;
    rocky8*|rhel8*|centos8*)
        : "${THIRDPARTY_DIR:=/development/3rdparty}"
        : "${FMT_VERSION:=11.0.2}"
        ;;
    *)
        echo "ERROR: unrecognised platform ${ID}${VERSION_ID}; set THIRDPARTY_DIR and FMT_VERSION" >&2
        : "${THIRDPARTY_DIR:?}" "${FMT_VERSION:?}"
        ;;
esac

FMT_PREFIX="${THIRDPARTY_DIR}/installed/fmt/${FMT_VERSION}"
INSTALL_PREFIX="${THIRDPARTY_DIR}/installed/avro-cpp/${AVRO_VERSION}"
WORK_DIR="${TMPDIR:-/tmp}/avro-cpp-${AVRO_VERSION}"
COPY_DIR="${WORK_DIR}/source"
BUILD_DIR="${WORK_DIR}/build"

if [ ! -f "${FMT_PREFIX}/lib/cmake/fmt/fmt-config.cmake" ] && [ ! -f "${FMT_PREFIX}/lib64/cmake/fmt/fmt-config.cmake" ]; then
    # Without this check Avro's build does not fail: it quietly clones fmt from GitHub, which
    # on a host with no network fails later and less clearly, and on a host with one builds
    # against a different fmt from the project's.
    echo "ERROR: no fmt ${FMT_VERSION} at ${FMT_PREFIX}; build fmt first, or set FMT_VERSION" >&2
    exit 1
fi

echo "============================================================"
echo "Building Avro C++ ${AVRO_VERSION}"
echo "Source:         ${SOURCE_DIR}"
echo "Against fmt:    ${FMT_PREFIX}"
echo "Install prefix: ${INSTALL_PREFIX}"
echo "============================================================"

rm -rf "${WORK_DIR}"
mkdir -p "${WORK_DIR}"
cp -a "${SOURCE_DIR}" "${COPY_DIR}"
# Avro reads its version from ../../share/VERSION.txt, or from VERSION.txt beside its
# CMakeLists.txt when there is one, which is what lets the C++ part be built from a copy.
cp "${RELEASE_DIR}/share/VERSION.txt" "${COPY_DIR}/VERSION.txt"

# The one change. Upstream links fmt::fmt-header-only, and only while Avro itself is being built
# (BUILD_INTERFACE), so the installed library carries fmt code of its own and its package does
# not pass fmt on. Linking fmt::fmt in both the build and the installed interface uses the
# project's fmt library and passes it on. Stopped rather than skipped if the line is not there
# exactly once, because a later Avro release that changed it would otherwise be built unchanged
# without anyone noticing.
FMT_LINE='$<BUILD_INTERFACE:fmt::fmt-header-only>'
if [ "$(grep -c -F "${FMT_LINE}" "${COPY_DIR}/CMakeLists.txt")" -ne 1 ]; then
    echo "ERROR: expected exactly one '${FMT_LINE}' in Avro's CMakeLists.txt; this release links fmt differently" >&2
    exit 1
fi
sed -i "s/\$<BUILD_INTERFACE:fmt::fmt-header-only>/fmt::fmt/" "${COPY_DIR}/CMakeLists.txt"

# What each choice is for:
#
# - Tests off, and AVRO_USE_BOOST off. Boost is needed only for those, and the Boost that ships
#   with RHEL8 is too old for them anyway. With both off the library needs no Boost at all.
# - fmt is found through CMAKE_PREFIX_PATH, and FETCHCONTENT_FULLY_DISCONNECTED stops any
#   attempt to download it should that fail.
# - Snappy and zstd are switched off rather than used when present. They serve only Avro's own
#   container files, which the project does not use, and leaving them to be found would make the
#   library depend on whatever a particular machine had installed.
# - Both the shared and the static library are built. The installed avro-cpp-config.cmake names
#   both, so a project that finds the package fails to configure if either is missing.
#   avrogencpp links the static one.
# - -Wno-error: Avro builds itself with -Werror, so a new warning from a different compiler --
#   gcc 8.5 on RHEL8 -- would stop the build of a library we do not maintain. It is added to the
#   Release flags because those come after Avro's own on the compiler's command line.
# - lib, not lib64, so the layout is the same on both platforms.
# - CMAKE_INSTALL_RPATH_USE_LINK_PATH records where the project's fmt library is, so the
#   installed libavrocpp and avrogencpp find it without a library path. The project's own
#   binaries find third-party libraries the same way.
cmake -S "${COPY_DIR}" -B "${BUILD_DIR}" \
    -DCMAKE_INSTALL_PREFIX="${INSTALL_PREFIX}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG -Wno-error" \
    -DCMAKE_INSTALL_LIBDIR=lib \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DCMAKE_INSTALL_RPATH_USE_LINK_PATH=ON \
    -DCMAKE_PREFIX_PATH="${FMT_PREFIX}" \
    -DFETCHCONTENT_FULLY_DISCONNECTED=ON \
    -DAVRO_BUILD_TESTS=OFF \
    -DAVRO_USE_BOOST=OFF \
    -DAVRO_BUILD_EXECUTABLES=ON \
    -DAVRO_BUILD_SHARED=ON \
    -DAVRO_BUILD_STATIC=ON \
    -DCMAKE_DISABLE_FIND_PACKAGE_Snappy=ON \
    -DCMAKE_DISABLE_FIND_PACKAGE_zstd=ON

# With tests and AVRO_USE_BOOST off, Avro does not look for Boost at all. Checked rather than
# assumed, because a later Avro release could start to, and on RHEL8 it would find a Boost too
# old to use.
if grep -q '^Boost_DIR:PATH=/' "${BUILD_DIR}/CMakeCache.txt"; then
    echo "ERROR: configuring Avro found Boost, which this build is meant not to need" >&2
    exit 1
fi

cmake --build "${BUILD_DIR}" --parallel "$(nproc)"
cmake --install "${BUILD_DIR}"

echo ""
echo "============================================================"
echo "Avro C++ ${AVRO_VERSION} installed to ${INSTALL_PREFIX}"
echo "============================================================"
echo ""
echo "Installed libraries:"
find "${INSTALL_PREFIX}" -name "libavrocpp*"
echo ""
echo "avrogencpp:"
"${INSTALL_PREFIX}/bin/avrogencpp" --version
