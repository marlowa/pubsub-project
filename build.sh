#!/bin/bash
# Wrapper script that sets environment variables and invokes the Python build script

set -euo pipefail

# Set third-party library versions
export THIRDPARTY_DIR=/home/marlowa/mystuff/thirdparty
export PROTOBUF_VERSION=25.1
export ABSEIL_VERSION=abseil-cpp-20230802.1
export FMT_VERSION=12.1.0
export QUILL_VERSION=11.0.2
export ARGPARSE_VERSION=3.2
export GOOGLETEST_VERSION=1.17.0

# Get the directory where this script is located
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

# Invoke Python build script with all arguments passed through
# Note that by default it runs the test. Use --no_tests to suppress that.
exec python3 "${SCRIPT_DIR}/build.py" "$@"
