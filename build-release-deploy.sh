#!/usr/bin/env bash
# Sets platform-specific environment variables then delegates to build-release-deploy.py.

set -euo pipefail

if [ -f /etc/os-release ]; then
    . /etc/os-release
    PLATFORM_ID="${ID}${VERSION_ID}"
else
    PLATFORM_ID="unknown"
fi

case "${PLATFORM_ID}" in
    linuxmint22*)
        export THIRDPARTY_DIR=/home/marlowa/mystuff/thirdparty
        export FMT_VERSION=12.1.0
        export QUILL_VERSION=11.0.2
        export ARGPARSE_VERSION=3.2
        export GOOGLETEST_VERSION=1.17.0
        export TOMLPLUSPLUS_VERSION=3.4.0
        export ROBINMAP_VERSION=1.4.1
        ;;
    rocky8*|rhel8*|centos8*)
        export THIRDPARTY_DIR=/workspace/thirdparty
        export FMT_VERSION=11.0.2
        export QUILL_VERSION=11.0.2
        export ARGPARSE_VERSION=3.2
        export GOOGLETEST_VERSION=1.10.0
        export TOMLPLUSPLUS_VERSION=3.4.0
        export ROBINMAP_VERSION=1.4.1
        ;;
    *)
        echo "ERROR: Unrecognised platform: ${PLATFORM_ID}" >&2
        exit 1
        ;;
esac

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec python3 "${SCRIPT_DIR}/build-release-deploy.py" "$@"
