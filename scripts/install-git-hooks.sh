#!/usr/bin/env bash
#
# Enable the repository's tracked git hooks (.githooks/) for this clone.
#
# Git does not version-control .git/hooks, so each clone must opt in once by
# pointing core.hooksPath at the tracked .githooks directory. Run this after
# cloning:
#
#   scripts/install-git-hooks.sh
#
set -euo pipefail

repo_root="$(git rev-parse --show-toplevel)"
cd "$repo_root"

chmod +x .githooks/* 2>/dev/null || true
git config core.hooksPath .githooks

echo "Enabled tracked git hooks (core.hooksPath -> .githooks):"
for hook in .githooks/*; do
    [ -f "$hook" ] && echo "  - $(basename "$hook")"
done
echo
echo "The pre-commit hook checks staged C/C++ against .clang-format."
echo "It needs the pinned clang-format:  pip install -e python[dev]"
