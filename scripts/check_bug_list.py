#!/usr/bin/env python3
"""
check_bug_list.py -- Check docs/bug_list.md for structural faults, and check the repository for
citations of bug ids that do not exist.

The second half is the reason this script exists. Bug entries are cited from C++ comments, from
ha_test.py, from dev.toml and from the design notes. Before ids those citations named an entry by
its title sentence, and two of them had rotted unnoticed: one entry had been deleted, and one had
never been written at all. A citation that resolves to nothing is indistinguishable from a
citation to something the reader has not found yet, so it has to be a build-time failure rather
than something discovered months later.

Checks implemented:
  1.  Every entry heading is '### BUG-nnnn: <title>'
  2.  Ids are unique
  3.  Every id is below the 'Next id' high-water mark, so a deleted entry's id is never reused
  4.  Required rows present: Severity, Found, Recorded, How
  5.  Severity is one of high, medium, low
  6.  Kind, where present, is 'task'
  7.  Closed entries state how they closed (Fixed or Dismissed); open entries do not
  8.  The summary index lists exactly the open entries, with matching severity, kind and title
  9.  The index is ordered by severity then id
  10. The counts table matches the entries
  11. Headings inside an entry are level four or deeper, so '###' always means a new entry
  12. Every BUG-nnnn cited anywhere in the repository resolves to an entry
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

_PROJECT_ROOT = Path(__file__).resolve().parent.parent
_BUG_LIST = _PROJECT_ROOT / 'docs' / 'bug_list.md'

_ENTRY_RE = re.compile(r'^### (BUG-\d{4}): (.+)$')
_INDEX_RE = re.compile(r'^\| (BUG-\d{4}) \| (high|medium|low) \| (defect|task) \| (.+) \|$')
_CITATION_RE = re.compile(r'BUG-\d{4}')
_SEVERITY_RANK = {'high': 0, 'medium': 1, 'low': 2}
_REQUIRED_ROWS = ('Severity', 'Found', 'Recorded', 'How')
_CLOSING_ROWS = ('Fixed', 'Dismissed')


class Entry:
    """One bug entry: its id, title, section and the rows of its metadata table."""

    def __init__(self, bug_id: str, title: str, section: str, line: int):
        self.bug_id = bug_id
        self.title = title
        self.section = section
        self.line = line
        self.rows: dict[str, str] = {}

    @property
    def kind(self) -> str:
        return 'task' if 'Kind' in self.rows else 'defect'

    @property
    def severity(self) -> str:
        return self.rows.get('Severity', '')


def parse(lines: list[str]) -> tuple[list[Entry], list[tuple[str, str, str, str]], dict[str, str], list[str]]:
    """Return (entries, index rows, counts table, faults found while parsing)."""
    entries: list[Entry] = []
    index: list[tuple[str, str, str, str]] = []
    counts: dict[str, str] = {}
    faults: list[str] = []
    section = ''
    current: Entry | None = None
    in_counts = False

    for number, line in enumerate(lines, start=1):
        if line.startswith('## '):
            section = line[3:].strip()
            current = None
            in_counts = section == ''
        if line.startswith('# Bug List'):
            in_counts = True

        match = _ENTRY_RE.match(line)
        if match:
            current = Entry(match.group(1), match.group(2), section, number)
            entries.append(current)
            continue

        if line.startswith('### ') and section in ('Open', 'Closed'):
            faults.append(f"bug_list.md:{number}: '###' heading is not an entry: {line[4:]!r}. "
                          f"Sub-headings inside an entry must be '####' or deeper")
            continue

        if line.startswith('| ') and '---' not in line:
            parts = [p.strip() for p in line.split('|')]
            if len(parts) >= 3 and parts[1]:
                if current is not None and not current.rows.get('_closed'):
                    current.rows[parts[1]] = parts[2]
                if in_counts:
                    counts[parts[1]] = parts[2]

        index_match = _INDEX_RE.match(line)
        if index_match:
            index.append(index_match.groups())
            in_counts = False

    return entries, index, counts, faults


def check_entries(entries: list[Entry], counts: dict[str, str]) -> list[str]:
    faults: list[str] = []
    seen: dict[str, int] = {}

    for entry in entries:
        where = f'bug_list.md:{entry.line}: {entry.bug_id}'
        if entry.bug_id in seen:
            faults.append(f'{where}: duplicate id, already used at line {seen[entry.bug_id]}')
        seen[entry.bug_id] = entry.line

        for row in _REQUIRED_ROWS:
            if row not in entry.rows:
                faults.append(f'{where}: missing required row {row!r}')

        if entry.severity not in _SEVERITY_RANK:
            faults.append(f'{where}: severity {entry.severity!r} is not one of high, medium, low')

        if 'Kind' in entry.rows and not entry.rows['Kind'].startswith('task'):
            faults.append(f"{where}: Kind row must begin with 'task'; an entry with no Kind row is a defect")

        closed = [row for row in _CLOSING_ROWS if row in entry.rows]
        if entry.section == 'Closed' and not closed:
            faults.append(f'{where}: is in Closed but has no Fixed or Dismissed row saying when it closed')
        if entry.section == 'Open' and closed:
            faults.append(f'{where}: is in Open but carries a {closed[0]!r} row')

    next_id = counts.get('Next id', '')
    if not re.fullmatch(r'BUG-\d{4}', next_id):
        faults.append(f'bug_list.md: counts table has no valid "Next id" row (found {next_id!r})')
    else:
        limit = int(next_id[4:])
        for entry in entries:
            if int(entry.bug_id[4:]) >= limit:
                faults.append(f'bug_list.md:{entry.line}: {entry.bug_id} is not below the high-water mark {next_id}')

    return faults


def check_index(entries: list[Entry], index: list[tuple[str, str, str, str]], counts: dict[str, str]) -> list[str]:
    faults: list[str] = []
    open_entries = {e.bug_id: e for e in entries if e.section == 'Open'}
    listed = {row[0]: row for row in index}

    for bug_id in sorted(set(open_entries) - set(listed)):
        faults.append(f'bug_list.md: {bug_id} is open but missing from the summary index')
    for bug_id in sorted(set(listed) - set(open_entries)):
        faults.append(f'bug_list.md: {bug_id} is in the summary index but is not an open entry')

    for bug_id, row in sorted(listed.items()):
        entry = open_entries.get(bug_id)
        if entry is None:
            continue
        if (row[1], row[2], row[3]) != (entry.severity, entry.kind, entry.title):
            faults.append(f'bug_list.md: index row for {bug_id} does not match its entry '
                          f'(index: {row[1]}/{row[2]}/{row[3]!r}; entry: {entry.severity}/{entry.kind}/{entry.title!r})')

    ordered = sorted(index, key=lambda row: (_SEVERITY_RANK[row[1]], row[0]))
    if index != ordered:
        faults.append('bug_list.md: the summary index is not ordered by severity then id')

    closed_entries = [e for e in entries if e.section == 'Closed']
    expected = {
        'Bugs recorded': str(len(entries)),
        'Closed': str(len(closed_entries)),
    }
    for row, value in expected.items():
        if counts.get(row) != value:
            faults.append(f'bug_list.md: counts row {row!r} says {counts.get(row)!r}, entries say {value!r}')

    tasks = sum(1 for e in open_entries.values() if e.kind == 'task')
    defects = len(open_entries) - tasks
    wanted = f'{len(open_entries)} ({defects} defects, {tasks} task{"" if tasks == 1 else "s"})'
    if counts.get('Open') != wanted:
        faults.append(f'bug_list.md: counts row \'Open\' says {counts.get("Open")!r}, entries say {wanted!r}')

    return faults


def check_citations(entries: list[Entry]) -> list[str]:
    """Every BUG-nnnn cited anywhere in the repository must resolve to an entry."""
    known = {e.bug_id for e in entries}
    result = subprocess.run(['git', 'grep', '-n', '-E', r'BUG-[0-9]{4}'], cwd=_PROJECT_ROOT,
                            capture_output=True, text=True, check=False)
    if result.returncode not in (0, 1):
        return [f'git grep failed: {result.stderr.strip()}']

    faults: list[str] = []
    for line in result.stdout.splitlines():
        path, _, rest = line.partition(':')
        if path == 'docs/bug_list.md':
            continue
        for cited in sorted(set(_CITATION_RE.findall(rest))):
            if cited not in known:
                faults.append(f'{line.split(":", 2)[0]}:{line.split(":", 2)[1]}: cites {cited}, which has no entry')
    return faults


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--skip-citations', action='store_true',
                        help='check the bug list only, without searching the repository for citations')
    args = parser.parse_args()

    lines = _BUG_LIST.read_text(encoding='utf-8').split('\n')
    entries, index, counts, faults = parse(lines)
    faults += check_entries(entries, counts)
    faults += check_index(entries, index, counts)
    if not args.skip_citations:
        faults += check_citations(entries)

    for fault in faults:
        print(fault)

    if faults:
        print(f'\n{len(faults)} fault(s) found.')
        return 1

    open_count = sum(1 for e in entries if e.section == 'Open')
    print(f'bug_list.md is consistent: {len(entries)} entries, {open_count} open, {len(entries) - open_count} closed.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
