#!/bin/bash
# Builds and installs cpptrace into the thirdparty directory.
#
# Runs on the Mint development host and inside the RHEL8/Rocky Linux 8 Docker container. Unlike
# build-fmt.sh and build_prometheus_cpp.sh, which hardcode the container's third-party path, this
# script detects the platform the same way build.sh does, so the same recipe serves both. The
# options below are deliberately identical on the two platforms -- see the note on zstd.
#
# NOTHING HERE REACHES THE NETWORK when both source trees are already unpacked in the third-party
# directory, which is what a build host behind a corporate firewall needs. Left to itself cpptrace
# clones libdwarf with git, and a git fetch is exactly what such a host refuses; the two archives
# below can be downloaded by hand anywhere and copied across. See CPPTRACE_VERSION and
# LIBDWARF_VERSION for what to fetch, and the FetchContent note further down for how the unpacked
# copy is used in place of the clone.

set -euo pipefail

CPPTRACE_VERSION="1.0.4"

# The libdwarf-lite commit cpptrace 1.0.4 pins, 5dfb2cd2aacf2bf473e5bfea79e41289f88b3a5f, is the
# tag v2.1.0 -- verified against the repository rather than assumed, because a release tarball of
# the wrong revision would build and then behave differently from every other machine.
LIBDWARF_VERSION="2.1.0"

# An exported THIRDPARTY_DIR wins. The container has two third-party trees serving two workflows --
# the tree the README's docker run mounts, and the pubsub-rocky-deps volume release_check.py mounts
# at /opt/deps -- and the platform alone cannot say which of them this build is for. Where nothing
# is exported the platform decides, exactly as scripts/build.sh does.
if [ -n "${THIRDPARTY_DIR:-}" ]; then
    PLATFORM_ID="THIRDPARTY_DIR from the environment"
else
    if [ -f /etc/os-release ]; then
        . /etc/os-release
        PLATFORM_ID="${ID}${VERSION_ID}"
    else
        PLATFORM_ID="unknown"
    fi

    case "${PLATFORM_ID}" in
        linuxmint22*)
            THIRDPARTY_DIR=/home/marlowa/mystuff/thirdparty
            ;;
        rocky8*|rhel8*|centos8*)
            THIRDPARTY_DIR=/development/3rdparty
            ;;
        *)
            echo "ERROR: Unrecognised platform: ${PLATFORM_ID}" >&2
            exit 1
            ;;
    esac
fi

INSTALL_PREFIX="${THIRDPARTY_DIR}/installed/cpptrace/${CPPTRACE_VERSION}"
BUILD_DIR="/tmp/cpptrace-build"

echo "============================================================"
echo "Building cpptrace ${CPPTRACE_VERSION}"
echo "Platform:       ${PLATFORM_ID}"
echo "Install prefix: ${INSTALL_PREFIX}"
echo "============================================================"

# Use the source already unpacked in the third-party tree if it is there, and fetch it if it is
# not. Downloading goes to /tmp rather than the current directory: run from the project root, a
# bare wget leaves the tarball in the tree, and it will not overwrite an existing file -- it saves
# alongside as .1 and the build then silently uses whatever stale tarball was already there.
SOURCE_DIR="${THIRDPARTY_DIR}/cpptrace-${CPPTRACE_VERSION}"
if [ ! -d "${SOURCE_DIR}" ]; then
    ARCHIVE="/tmp/cpptrace-${CPPTRACE_VERSION}.tar.gz"
    if [ ! -f "${ARCHIVE}" ]; then
        if ! wget "https://github.com/jeremy-rifkin/cpptrace/archive/refs/tags/v${CPPTRACE_VERSION}.tar.gz" -O "${ARCHIVE}"; then
            rm -f "${ARCHIVE}"
            echo "ERROR: cpptrace ${CPPTRACE_VERSION} source not found and could not be downloaded." >&2
            echo "       On a host without internet access, fetch this archive elsewhere and unpack it" >&2
            echo "       into the third-party directory:" >&2
            echo "         https://github.com/jeremy-rifkin/cpptrace/archive/refs/tags/v${CPPTRACE_VERSION}.tar.gz" >&2
            echo "       so that ${SOURCE_DIR} exists." >&2
            exit 1
        fi
    fi
    rm -rf "/tmp/cpptrace-${CPPTRACE_VERSION}"
    tar xzf "${ARCHIVE}" -C /tmp
    SOURCE_DIR="/tmp/cpptrace-${CPPTRACE_VERSION}"
fi
echo "Source:         ${SOURCE_DIR}"

# cpptrace fetches libdwarf itself, with git, from a commit it pins. FetchContent takes an already
# unpacked source tree in place of the clone when FETCHCONTENT_SOURCE_DIR_<NAME> names one, and
# FETCHCONTENT_FULLY_DISCONNECTED then forbids it from reaching the network at all -- so a build
# host that refuses git fetches, submodules and clones builds from the release tarball instead,
# and says so rather than hanging on a clone that cannot succeed. Where the tree is absent the
# clone is left to happen, which is what the development host does.
LIBDWARF_SOURCE_DIR="${THIRDPARTY_DIR}/libdwarf-lite-${LIBDWARF_VERSION}"
LIBDWARF_ARGS=()
if [ -d "${LIBDWARF_SOURCE_DIR}" ]; then
    LIBDWARF_ARGS+=(
        "-DFETCHCONTENT_SOURCE_DIR_LIBDWARF=${LIBDWARF_SOURCE_DIR}"
        "-DFETCHCONTENT_FULLY_DISCONNECTED=ON"
    )
    echo "libdwarf:       ${LIBDWARF_SOURCE_DIR} (no network access needed)"
else
    echo "libdwarf:       will be cloned by cpptrace -- this needs git and network access."
    echo "                To build without either, unpack this archive as ${LIBDWARF_SOURCE_DIR}:"
    echo "                  https://github.com/jeremy-rifkin/libdwarf-lite/archive/refs/tags/v${LIBDWARF_VERSION}.tar.gz"
fi

# Configure from scratch. A CMakeCache left by an earlier run holds the install prefix and every
# option below, so a rebuild after changing platform or version would install where the previous
# run was told to.
rm -rf "${BUILD_DIR}"

# Shared, to match the rest of the third-party tree: installed binaries find it through the RPATH
# the project records, and nothing is copied out of the tree.
#
# cpptrace reads DWARF through libdwarf, which it fetches itself at configure time -- so this needs
# git and network access, in the container as well as on the host. It pins a libdwarf-lite commit
# it has been tested against, builds it static with position-independent code, and links it
# privately into libcpptrace.so. That leaves one shared object to deploy rather than two. PIC is
# stated here as well as by cpptrace's own PIC_ALWAYS, because a non-relocatable archive cannot go
# into a shared library at all, and the flag costs nothing.
#
# libdwarf's headers, its archive and its cmake package config install into this same prefix.
# They are not surplus: the generated cpptrace-config.cmake calls find_dependency(libdwarf
# REQUIRED) even for the bundled build, and that resolves against <prefix>/lib/cmake/libdwarf. The
# two directories must therefore stay together, and a consumer needs only the cpptrace prefix on
# CMAKE_PREFIX_PATH.
#
# CPPTRACE_USE_EXTERNAL_ZSTD=ON takes the platform's zstd rather than downloading and building one.
# Rocky 8 has libzstd but not libzstd-devel by default, which is why the Dockerfile installs it
# (it is in baseos; no powertools needed). The options must not diverge between the platforms:
# they are recorded in the installed cpptrace-config.cmake, which decides what find_dependency()
# the consuming project is made to satisfy, so a platform that diverges here fails to configure on
# that platform alone.
cmake -S "${SOURCE_DIR}" -B "${BUILD_DIR}" \
    "${LIBDWARF_ARGS[@]+"${LIBDWARF_ARGS[@]}"}" \
    -DCMAKE_INSTALL_PREFIX="${INSTALL_PREFIX}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_STANDARD=17 \
    -DBUILD_SHARED_LIBS=ON \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DCPPTRACE_BUILD_TESTING=OFF \
    -DCPPTRACE_BUILD_TOOLS=OFF \
    -DCPPTRACE_BUILD_BENCHMARKING=OFF \
    -DCPPTRACE_USE_EXTERNAL_ZSTD=ON

cmake --build "${BUILD_DIR}" --parallel "$(nproc)"
cmake --install "${BUILD_DIR}"

echo ""
echo "============================================================"
echo "cpptrace ${CPPTRACE_VERSION} installed to ${INSTALL_PREFIX}"
echo "============================================================"

# Verify
echo ""
echo "Installed libraries:"
find "${INSTALL_PREFIX}" -name "libcpptrace.*" -o -name "libdwarf.a"

echo ""
echo "Installed cmake files:"
find "${INSTALL_PREFIX}" -name "*.cmake"

# Name the reason the static libdwarf can live inside a shared library, rather than leave it to be
# rediscovered. The link above is the real check -- an archive built without -fPIC fails it with a
# relocation error -- so this reports what that link relied on. R_X86_64_32 and R_X86_64_32S are
# the absolute relocations that a shared object cannot carry; PC-relative and GOT ones are fine.
# lib or lib64 depending on the platform, so look for it rather than name the directory.
DWARF_ARCHIVE=$(find "${INSTALL_PREFIX}" -name "libdwarf.a" | head -1)
if [ -n "${DWARF_ARCHIVE}" ]; then
    echo ""
    absolute_relocations=$(readelf -r "${DWARF_ARCHIVE}" | grep -cE "R_X86_64_(32|32S)\b" || true)
    if [ "${absolute_relocations}" -eq 0 ]; then
        echo "libdwarf.a is position-independent: no absolute 32-bit relocations."
    else
        echo "ERROR: libdwarf.a carries ${absolute_relocations} absolute 32-bit relocations, so it was not" >&2
        echo "       built with -fPIC and cannot be linked into a shared library." >&2
        exit 1
    fi
fi
