#!/usr/bin/env python3
"""Run the pool free-list TLC checks and verify the expected ABA counterexample."""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Optional

_PROJECT_ROOT = Path(__file__).resolve().parent.parent
_TLA_DIR = _PROJECT_ROOT / 'docs' / 'framework' / 'tla'
_TLA_TOOLS_VERSION = '1.7.4'
_TLC_TIMEOUT_SECONDS = 300
_EXPECTED_ABA_ERRORS = [
    'Error: Invariant FreeListSound is violated.',
    'Error: The behavior up to this point is:',
]


class TlaCheckError(Exception):
    """A TLC run did not produce the result required by its configuration."""


def find_jar(explicit: Optional[str]) -> Path:
    """Locate tla2tools.jar from the command line or environment."""
    candidates = []
    if explicit:
        candidates.append(Path(explicit))
    if os.environ.get('TLA2TOOLS_JAR'):
        candidates.append(Path(os.environ['TLA2TOOLS_JAR']))
    if os.environ.get('THIRDPARTY_DIR'):
        candidates.append(Path(os.environ['THIRDPARTY_DIR']) / f'tla2tools-{_TLA_TOOLS_VERSION}' / 'tla2tools.jar')
    for candidate in candidates:
        if candidate.is_file():
            return candidate.resolve()
    searched = ', '.join(str(candidate) for candidate in candidates)
    if not searched:
        searched = 'no --jar, TLA2TOOLS_JAR or THIRDPARTY_DIR was provided'
    raise TlaCheckError(f'tla2tools.jar not found. Looked in: {searched}')


def run_tlc(java: str, jar: Path, config: str) -> tuple[int, str]:
    """Run one pool model in a temporary directory under the repository."""
    scratch_root = _PROJECT_ROOT / 'tmp'
    scratch_root.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='pool_free_list_tla_', dir=scratch_root) as scratch:
        work_dir = Path(scratch)
        shutil.copy(_TLA_DIR / 'PoolFreeList.tla', work_dir)
        shutil.copy(_TLA_DIR / config, work_dir)
        command = [
            java, '-XX:+UseParallelGC', '-cp', str(jar), 'tlc2.TLC',
            '-deadlock', '-workers', 'auto', '-metadir', str(work_dir / 'states'),
            '-config', config, 'PoolFreeList.tla',
        ]
        try:
            result = subprocess.run(
                command,
                cwd=work_dir,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                timeout=_TLC_TIMEOUT_SECONDS,
                check=False,
            )
        except FileNotFoundError as error:
            raise TlaCheckError(f'Could not run {java!r}: {error}') from error
        except subprocess.TimeoutExpired as error:
            raise TlaCheckError(f'{config}: TLC exceeded the {_TLC_TIMEOUT_SECONDS}-second time limit') from error
    return result.returncode, result.stdout


def check_run(config: str, jar: Path, java: str, expected_aba_failure: bool = False) -> None:
    """Require a clean invariant run or the exact expected ABA invariant failure."""
    print(f'Checking {config}...')
    return_code, output = run_tlc(java, jar, config)
    error_lines = [line for line in output.splitlines() if line.startswith('Error:')]
    if expected_aba_failure:
        expected_failure = return_code != 0 and error_lines == _EXPECTED_ABA_ERRORS
        if not expected_failure:
            print(output, end='', file=sys.stderr)
            raise TlaCheckError(
                f'{config}: expected TLC to exit nonzero with only '
                f'{_EXPECTED_ABA_ERRORS!r}, got exit code {return_code} and errors {error_lines!r}'
            )
        print('Confirmed: TLC found the expected FreeListSound ABA violation.')
        return

    completed_cleanly = (
        return_code == 0
        and not error_lines
        and 'Model checking completed. No error has been found.' in output
    )
    if not completed_cleanly:
        print(output, end='', file=sys.stderr)
        raise TlaCheckError(f'{config}: TLC did not complete without errors (exit code {return_code})')
    print(output, end='')


def main() -> int:
    parser = argparse.ArgumentParser(description='Check all pool free-list TLC configurations.')
    parser.add_argument('--jar', help='Path to tla2tools.jar')
    parser.add_argument('--java', default='java', help='Java executable (default: java)')
    arguments = parser.parse_args()

    try:
        jar = find_jar(arguments.jar)
        check_run('PoolFreeList.cfg', jar, arguments.java)
        check_run('PoolFreeValidation.cfg', jar, arguments.java)
        check_run('PoolFreeListNoCounter.cfg', jar, arguments.java, expected_aba_failure=True)
    except TlaCheckError as error:
        print(f'TLA check failed: {error}', file=sys.stderr)
        return 1

    print('All pool free-list TLC checks passed.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
