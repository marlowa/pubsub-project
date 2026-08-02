#!/bin/bash
# Wrapper script that sets environment variables and invokes devsetup.py.
# devsetup.py runs build, release, and deploy in sequence for the developer sandbox.

set -euo pipefail

# Detect platform
if [ -f /etc/os-release ]; then
    . /etc/os-release
    PLATFORM_ID="${ID}${VERSION_ID}"
else
    PLATFORM_ID="unknown"
fi

# Set versions based on platform
case "${PLATFORM_ID}" in
    linuxmint22*)
        export THIRDPARTY_DIR=/home/marlowa/mystuff/thirdparty
        export FMT_VERSION=12.1.0
        export QUILL_VERSION=11.0.2
        export ARGPARSE_VERSION=3.2
        export GOOGLETEST_VERSION=1.17.0
        export TOMLPLUSPLUS_VERSION=3.4.0
        export ROBINMAP_VERSION=1.4.1
        export PROMETHEUS_VERSION=1.3.0
        ;;
    rocky8*|rhel8*|centos8*)
        # Third-party libraries live at different paths on the real RHEL8 build
        # hosts vs. the Rocky 8 build container (which mounts them at
        # /workspace/thirdparty -- see README). Only RHEL8 uses /development/3rdparty;
        # Rocky and CentOS stay on /workspace/thirdparty.
        if [ "${ID:-}" = "rhel" ]; then
            export THIRDPARTY_DIR=/development/3rdparty
        else
            export THIRDPARTY_DIR=/workspace/thirdparty
        fi
        export FMT_VERSION="11.0.2"
        export QUILL_VERSION="11.0.2"
        export ARGPARSE_VERSION="3.2"
        export GOOGLETEST_VERSION="1.10.0"
        export TOMLPLUSPLUS_VERSION=3.4.0
        export ROBINMAP_VERSION=1.4.1
        export PROMETHEUS_VERSION=1.3.0
        ;;
    *)
        echo "ERROR: Unrecognised platform: ${PLATFORM_ID}" >&2
        exit 1
        ;;
esac

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

exec python3 "${SCRIPT_DIR}/devsetup.py" "$@"
