#!/usr/bin/env python3
"""
deployment_freshness.py -- say when the deployed venue predates the source you are looking at.

Why this exists. In a real deployment the release goes to a central repository -- Artifactory or
similar -- and the target host carries no source, so "is what is running the same as what I am
editing?" is not a question anybody can ask. Here the release directory sits *inside* the
development tree, and that simplification invites a specific mistake: change a file, redeploy, and
expect the change to be running. It will not be. `deploy.py` deploys a release, and the release is
whatever was packaged the last time somebody built one.

That is not a careless mistake, it is one the layout produces, so the tools should notice rather
than let somebody draw conclusions from a venue built an hour ago. See docs/bug_list.md, BUG-0015
and BUG-0011.

## What is compared, and why it is a timestamp

**A release is built from the working tree, not from a commit.** The git hash in an artefact's name
records where the tree was when it was built; it does not describe what went into it. A build from
a dirty tree -- which is the normal case here, since changes are made and tested before they are
committed -- packages those uncommitted changes and stamps a hash that says nothing about them.

So the question that matters is not "which commit was this built at" but **"was this built after my
last edit"**, and only a time answers it. `release.json` records `built_at`; a tracked file with a
later modification time is a change the deployed tree cannot contain.

An earlier version of this module compared the artefact's hash against HEAD and treated a dirty
tree as proof that no release contained the changes. Both were wrong, and wrong in the direction
that matters: build dirty at a commit, edit again without committing, redeploy, and the hash still
matched -- so the check stayed silent in exactly the workflow it was written for.

The hash comparison is kept, demoted to context. It answers a different and lesser question: which
commit the tree was near when the build ran.

## What this deliberately does not do

**It does not decide whether the difference matters.** A change to a README warns exactly like a
change to the matching engine. Classifying which edits matter means a rule per language -- C++,
Java, Python, JavaScript, documentation, environment files are all in the artefact -- and the
asymmetry does not justify it: a false positive costs one line of output, and a false negative
costs a run whose result is about the wrong code.

**It reads tracked files only, and entirely new code is the case it cannot catch.** Adding a file
that no existing file references leaves nothing for this to see. That is not a gap to be closed by
guessing: whether a new file belongs to the project is a decision only the developer can make, and
`git add` is where they make it. A check that treated every unstaged file as project source would
warn about working notes on every run for ever, which is how a warning stops being read.

In practice the case is narrower than it sounds, because new code usually has to be referenced --
a CMakeLists, an import, a scenario table -- and those files are tracked. But a genuinely
self-contained addition is invisible here, and that is something to live with rather than defend
against.

**A `git checkout` or `git pull` rewrites modification times without changing content**, so either
can produce a warning when nothing is really stale. That is the accepted price of asking the right
question.
"""

from __future__ import annotations

import json
import subprocess
from datetime import datetime, timezone
from pathlib import Path


def _git(*args: str, cwd: Path) -> str | None:
    """Run a git command, or None when git fails or there is no repository."""
    try:
        result = subprocess.run(['git', *args], cwd=cwd, capture_output=True, text=True, check=False)
    except (OSError, ValueError):
        return None
    if result.returncode != 0:
        return None
    return result.stdout.strip()


def _built_at_epoch(release: dict) -> float | None:
    """`built_at` as a POSIX timestamp, or None when it is missing or unreadable."""
    raw = str(release.get('built_at') or '')
    if not raw:
        return None
    try:
        stamp = datetime.fromisoformat(raw)
    except ValueError:
        return None
    if stamp.tzinfo is None:
        stamp = stamp.replace(tzinfo=timezone.utc)
    return stamp.timestamp()


def staleness_warnings(install_dir: Path, source_dir: Path | None = None) -> list[str]:
    """How the deployed tree lags the working tree, as lines to print. Empty when it does not.

    Returns empty rather than raising for every "cannot tell" case -- no git repository, no
    release.json, no readable build time. A check that cannot answer must be silent, because a
    warning nobody can act on trains people to skip the ones they can.

    @param[in] install_dir The deployed tree, which carries release.json.
    @param[in] source_dir  Where to ask git. Defaults to this script's project root.
    """
    root = (source_dir or Path(__file__).resolve().parent.parent).resolve()

    # No repository means a real target host, where this question does not arise and there is
    # nothing to compare against. Self-disabling, so this stays a sandbox aid rather than
    # deployment machinery.
    if _git('rev-parse', '--is-inside-work-tree', cwd=root) != 'true':
        return []

    try:
        release = json.loads((install_dir / 'release.json').read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return []

    warnings: list[str] = []
    built_at = _built_at_epoch(release)

    if built_at is not None:
        listing = _git('ls-files', cwd=root)
        newer = []
        for name in (listing or '').splitlines():
            path = root / name
            try:
                if path.stat().st_mtime > built_at:
                    newer.append(name)
            except OSError:
                continue          # deleted between the listing and the stat
        if newer:
            when = str(release.get('built_at'))
            warnings.append(f'{len(newer)} tracked file(s) were edited after the deployed venue was built at {when}')
            for name in sorted(newer)[:5]:
                warnings.append(f'  {name}')
            if len(newer) > 5:
                warnings.append(f'  ... and {len(newer) - 5} more')

    # Context rather than evidence: which commit the tree was near when the build ran. A matching
    # hash proves nothing about content, because the build takes the working tree as it finds it.
    deployed = str(release.get('git_hash') or '')
    head = _git('rev-parse', 'HEAD', cwd=root)
    if deployed and head and not head.startswith(deployed):
        short_head = _git('rev-parse', '--short', 'HEAD', cwd=root) or head[:7]
        warnings.append(f'it was also built near commit {deployed}, and HEAD is now {short_head}')

    if warnings:
        warnings.append('run scripts/devsetup.sh to build, release and deploy this tree')
    return warnings


def report(install_dir: Path, source_dir: Path | None = None, label: str = 'WARNING') -> bool:
    """Print the warnings, if any. Returns True when something was printed.

    Prints rather than raises, and the callers do not stop. Running an older release on purpose is
    legitimate -- rolling back to reproduce an incident, or bisecting -- so this reports and gets
    out of the way.
    """
    lines = staleness_warnings(install_dir, source_dir)
    if not lines:
        return False
    print(f'{label}: the deployed venue predates changes in this working tree.')
    for line in lines:
        print(f'  {line}')
    return True


if __name__ == '__main__':
    import sys
    target = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parent.parent / 'installed'
    report(target)
    raise SystemExit(0)
