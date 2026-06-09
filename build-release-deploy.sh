#!/usr/bin/env bash
# Convenience wrapper: build (no tests) → release → deploy.
# Aborts on the first failure; each stage is announced so it is clear where a
# failure occurred.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INSTALL_DIR="${SCRIPT_DIR}/installed"
RELEASE_DIR="${SCRIPT_DIR}/build/release"

SKIP_DB=false
NO_PYLINT=false
NO_CPP=false
for arg in "$@"; do
    case "${arg}" in
        --skip-db)    SKIP_DB=true ;;
        --no-pylint)  NO_PYLINT=true ;;
        --no-cpp)     NO_CPP=true ;;
        *) echo "error: unknown argument: ${arg}" >&2; exit 1 ;;
    esac
done

step() { echo; echo "=== $* ==="; }

if ${NO_CPP}; then
    echo "NOTE: --no-cpp set; skipping clean of ${INSTALL_DIR}"
else
    echo "cleaning out ${INSTALL_DIR}"
    rm -fr "${INSTALL_DIR}"
fi

# ── Build ──────────────────────────────────────────────────────────────────────
step "BUILD"
BUILD_ARGS=(--no-tests)
${NO_PYLINT} && BUILD_ARGS+=(--no-pylint)
${NO_CPP}    && BUILD_ARGS+=(--no-cpp)
"${SCRIPT_DIR}/build.sh" "${BUILD_ARGS[@]}"

# ── Release ────────────────────────────────────────────────────────────────────
step "RELEASE"
python3 "${SCRIPT_DIR}/release.py"

# Locate the tarball just produced (newest match in build/release/).
TARBALL=$(ls -t "${RELEASE_DIR}"/pubsub-*.tar.gz 2>/dev/null | head -n1 || true)
if [[ -z "${TARBALL}" ]]; then
    echo "error: no release tarball found in ${RELEASE_DIR}" >&2
    exit 1
fi
echo "  tarball: ${TARBALL}"

# ── Deploy ─────────────────────────────────────────────────────────────────────
step "DEPLOY"
DEPLOY_ARGS=(--artefact "${TARBALL}")
${SKIP_DB} && DEPLOY_ARGS+=(--skip-db)
python3 "${SCRIPT_DIR}/deploy.py" "${DEPLOY_ARGS[@]}"

step "DONE"
