#!/usr/bin/env python3
"""
check_doxyfile.py -- Check that Doxyfile is understood by the Doxygen on the target platform.

Why this exists. The development host runs Doxygen 1.9.8 and RHEL8 ships 1.8.14, which is the
newest release packaged for it. A tag the older version does not know is ignored with a warning,
and a value it does not accept falls back to the default -- both silently, so a Doxyfile can work
here and quietly do something else there.

That is not hypothetical. `WARN_AS_ERROR = FAIL_ON_WARNINGS` is a 1.9 value; 1.8.14 rejects it as
an invalid boolean and uses NO, so the gate meant to fail the documentation build on a warning was
off on the one platform where an unresolved \\ref renders as a bare directory link instead of
failing. That is BUG-0049, and this script is the second half of it.

Checks implemented:
  1. Every tag set in Doxyfile is one 1.8.14 knows
  2. Every tag that is a boolean in 1.8.14 is given YES or NO, not a newer keyword

The vocabulary lives in scripts/doxygen_1_8_14_tags.txt, generated from the RHEL8 container.
`--regenerate` rebuilds it, and `--container` checks the real thing rather than the snapshot.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

_PROJECT_ROOT = Path(__file__).resolve().parent.parent
_DOXYFILE = _PROJECT_ROOT / 'Doxyfile'
_VOCABULARY = _PROJECT_ROOT / 'scripts' / 'doxygen_1_8_14_tags.txt'
_IMAGE = 'pubsub-rhel8:latest'

# A tag written with no space before '=' is still a tag. Both this Doxyfile and doxygen's own
# generated template do that for the longest names, and missing them makes the comparison lie.
_SETTING_RE = re.compile(r'^([A-Z_0-9]+) *=(.*)$')


def settings(path: Path) -> dict[str, str]:
    found = {}
    for line in path.read_text(encoding='utf-8').split('\n'):
        if line.startswith('#'):
            continue
        match = _SETTING_RE.match(line)
        if match:
            found[match.group(1)] = match.group(2).strip().rstrip('\\').strip()
    return found


def vocabulary() -> tuple[set[str], set[str]]:
    """Return (every tag 1.8.14 knows, those of them that are booleans there)."""
    if not _VOCABULARY.exists():
        raise SystemExit(f'{_VOCABULARY} is missing. Regenerate it with --regenerate.')
    known, booleans = set(), set()
    for tag, value in settings(_VOCABULARY).items():
        known.add(tag)
        if value in ('YES', 'NO'):
            booleans.add(tag)
    return known, booleans


def regenerate() -> int:
    """Rebuild the vocabulary from the container, which is the authority rather than this file."""
    command = ['docker', 'run', '--rm', '--entrypoint', 'bash', _IMAGE, '-lc',
               'doxygen -g /tmp/t.cfg >/dev/null 2>&1 && grep -E "^[A-Z_0-9]+ *=" /tmp/t.cfg']
    result = subprocess.run(command, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        print(f'could not run the container: {result.stderr.strip()}', file=sys.stderr)
        return 1

    header = [line for line in _VOCABULARY.read_text(encoding='utf-8').split('\n') if line.startswith('#')]
    body = [re.sub(r' +', ' ', line).rstrip() for line in result.stdout.split('\n') if line.strip()]
    _VOCABULARY.write_text('\n'.join(header + body) + '\n', encoding='utf-8')
    print(f'{_VOCABULARY.name}: {len(body)} tags from {_IMAGE}')
    return 0


def check_against_container() -> list[str]:
    """Ask the real 1.8.14 what it makes of this Doxyfile, rather than trusting the snapshot."""
    command = ['docker', 'run', '--rm', '--entrypoint', 'bash',
               '-v', f'{_PROJECT_ROOT}:/workspace:ro', _IMAGE, '-lc',
               '( cat /workspace/Doxyfile; echo "INPUT=/etc/hostname"; echo "GENERATE_HTML=NO"; '
               'echo "GENERATE_LATEX=NO"; echo "QUIET=YES" ) | doxygen - 2>&1 | grep -E "unsupported tag|not a valid" || true']
    result = subprocess.run(command, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        return [f'could not run the container: {result.stderr.strip()}']
    return [f'1.8.14 says: {line.strip()}' for line in result.stdout.split('\n') if line.strip()]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--regenerate', action='store_true',
                        help='rebuild the tag vocabulary from the RHEL8 container and exit')
    parser.add_argument('--container', action='store_true',
                        help='additionally run the real 1.8.14 against Doxyfile, which needs docker and the image')
    args = parser.parse_args()

    if args.regenerate:
        return regenerate()

    known, booleans = vocabulary()
    faults = []
    for tag, value in settings(_DOXYFILE).items():
        if tag not in known:
            faults.append(f'Doxyfile: {tag} is not a tag Doxygen 1.8.14 knows; it will be ignored there')
        elif tag in booleans and value and value not in ('YES', 'NO'):
            faults.append(f'Doxyfile: {tag} = {value} -- a boolean in 1.8.14, which will reject the '
                          f'value and fall back to its default')

    if args.container:
        faults += check_against_container()

    for fault in faults:
        print(fault)

    if faults:
        print(f'\n{len(faults)} fault(s) found.')
        return 1

    print(f'Doxyfile is readable by Doxygen 1.8.14: {len(settings(_DOXYFILE))} tags checked '
          f'against {len(known)} it knows.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
