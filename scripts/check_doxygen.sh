#!/usr/bin/env bash
# Checks the documentation comments the way the build does, but quickly: Doxygen is run on a copy of the project's
# Doxyfile with every diagram it draws by itself switched off (the class, collaboration, include and directory graphs),
# which is where most of its time goes, and with its own output directory, so that the documentation the build makes in
# build/docs is left alone. Dot stays available, so that a diagram a document asks for by name, such as the architecture
# map, is still drawn and checked. The copy keeps WARN_AS_ERROR, so any warning that would stop the build stops this
# too. Run it before the developer loop on any change to documented code or to the documents.
#
# Usage: scripts/check_doxygen.sh        (from the repository root, or anywhere: it changes to the root itself)
# Exit status: Doxygen's, 0 when there is no warning.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build
config=build/Doxyfile.check
# A later setting in a Doxyfile replaces an earlier one, so the lines added at the end take effect.
{
    cat Doxyfile
    echo "OUTPUT_DIRECTORY = build/docs_check"
    for graph in CLASS_GRAPH COLLABORATION_GRAPH INCLUDE_GRAPH INCLUDED_BY_GRAPH GROUP_GRAPHS GRAPHICAL_HIERARCHY DIRECTORY_GRAPH \
        CALL_GRAPH CALLER_GRAPH; do
        echo "$graph = NO"
    done
} > "$config"
doxygen "$config" > build/doxygen_check.log 2>&1 && status=0 || status=$?
if [ "$status" -ne 0 ]; then
    grep -E "error|warning" build/doxygen_check.log | head -20
fi
echo "doxygen check: exit $status (full output in build/doxygen_check.log)"
exit "$status"
