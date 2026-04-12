#!/bin/bash
# Wrapper script that sets environment variables and invokes the Python build script

set -euo pipefail

# Set third-party library versions

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
        ;;
    rocky8*|rhel8*|centos8*)
        export THIRDPARTY_DIR=/workspace/thirdparty
        export FMT_VERSION="11.0.2"
        export QUILL_VERSION="11.0.2"
        export ARGPARSE_VERSION="3.2"
        export GOOGLETEST_VERSION="1.10.0"
        ;;
    *)
        echo "ERROR: Unrecognised platform: ${PLATFORM_ID}" >&2
        exit 1
        ;;
esac

# Get the directory where this script is located
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

# Invoke Python build script with all arguments passed through
# Note that by default it runs the test. Use --no_tests to suppress that.
exec python3 "${SCRIPT_DIR}/build.py" "$@"
