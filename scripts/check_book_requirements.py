#!/usr/bin/env python3
"""Check the venue book's requirement identifiers and their test coverage.

Reads ``book.req``, which the book emits as it is typeset, rather than parsing LaTeX.  See ``docs/book/reqmacros.sty``.

What it enforces:

* every identifier used in the book is listed in ``docs/book/requirement_ids.txt``, and is listed there once;
* every scenario a requirement claims to be verified by exists in ``scripts/ha_test.py``;
* every scenario in ``ha_test.py`` verifies at least one requirement.  Reported, and fatal only under ``--strict``.

It also counts the gaps the book records, and checks that every defect a gap cites exists in ``docs/bug_list.md``.  A gap is a difference between what the
book specifies and what the venue does; a book with none left describes a system that works.
"""

import argparse
import re
import sys
from pathlib import Path

_PROJECT_ROOT = Path(__file__).resolve().parent.parent
_LEDGER = _PROJECT_ROOT / "docs" / "book" / "requirement_ids.txt"
_HA_TEST = _PROJECT_ROOT / "scripts" / "ha_test.py"
_BUG_LIST = _PROJECT_ROOT / "docs" / "bug_list.md"

_ID_PATTERN = re.compile(r"^R-\d{4}$")


def read_requirements(req_path: Path) -> list[dict]:
    """The requirements the build emitted, in document order."""
    records: list[dict] = []
    current: dict = {}
    for raw in req_path.read_text().splitlines():
        line = raw.strip()
        if line == "BEGIN":
            current = {"covers": [], "uncovered": ""}
        elif line == "END":
            records.append(current)
        elif line.startswith("ID "):
            current["id"] = line[3:].strip()
        elif line.startswith("SEC "):
            current["section"] = line[4:].strip()
        elif line.startswith("TITLE "):
            current["title"] = line[6:].strip()
        elif line.startswith("COVERS "):
            current["covers"] = [c.strip() for c in line[7:].split(",") if c.strip()]
        elif line.startswith("UNCOVERED "):
            current["uncovered"] = line[10:].strip()
    return records


def read_gaps(req_path: Path) -> list[tuple[str, list[str]]]:
    """The gaps the book recorded, as (section, defects cited)."""
    gaps: list[tuple[str, list[str]]] = []
    for raw in req_path.read_text().splitlines():
        if not raw.startswith("GAP "):
            continue
        fields = raw[4:].split(maxsplit=1)
        section = fields[0] if fields else "?"
        cited = re.findall(r"BUG-\d{4}", fields[1]) if len(fields) > 1 else []
        gaps.append((section, cited))
    return gaps


def read_known_defects(path: Path) -> set[str]:
    """Every defect identifier the bug list mentions."""
    return set(re.findall(r"BUG-\d{4}", path.read_text())) if path.is_file() else set()


def read_ledger(path: Path) -> tuple[dict[str, str], list[str]]:
    """The allocated identifiers as {id: status}, and any complaints about the ledger itself."""
    allocated: dict[str, str] = {}
    problems: list[str] = []
    for number, raw in enumerate(path.read_text().splitlines(), start=1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        fields = line.split(maxsplit=2)
        if len(fields) < 2 or not _ID_PATTERN.match(fields[0]):
            problems.append(f"{path.name}:{number}: not an 'id status ...' line: {line}")
            continue
        if fields[0] in allocated:
            problems.append(f"{path.name}:{number}: {fields[0]} is listed more than once -- an identifier is allocated exactly once and never reused")
            continue
        allocated[fields[0]] = fields[1]
    return allocated, problems


def read_scenarios(path: Path) -> set[int]:
    """The scenario numbers ha_test.py defines."""
    return {int(n) for n in re.findall(r"^\s*number=(\d+),", path.read_text(), re.M)}


def check(req_path: Path) -> tuple[list[str], list[str]]:
    """Every complaint, as (errors, unlinked scenarios) in the order a reader would want to fix them."""
    problems: list[str] = []

    requirements = read_requirements(req_path)
    if not requirements:
        return [f"{req_path}: no requirements were emitted -- has the book been built?"], []

    allocated, ledger_problems = read_ledger(_LEDGER)
    problems.extend(ledger_problems)
    scenarios = read_scenarios(_HA_TEST)

    seen: dict[str, str] = {}
    claimed: set[int] = set()

    for req in requirements:
        req_id = req.get("id", "<missing>")
        where = f"{req_id} (section {req.get('section', '?')})"

        if not _ID_PATTERN.match(req_id):
            problems.append(f"{where}: identifier is not of the form R-0001")
        if req_id in seen:
            problems.append(f"{where}: stated twice, also in section {seen[req_id]}")
        seen[req_id] = req.get("section", "?")

        if req_id not in allocated:
            problems.append(f"{where}: not listed in {_LEDGER.name} -- allocate the identifier before using it")
        elif allocated[req_id] == "retired":
            problems.append(f"{where}: the identifier is retired and must not be reused")

        if not req.get("covers") and not req.get("uncovered"):
            problems.append(f"{where}: says nothing about what verifies it -- use \\covers or \\uncovered")

        for reference in req.get("covers", []):
            match = re.fullmatch(r"ha_test:(\d+)", reference)
            if match is None:
                problems.append(f"{where}: coverage '{reference}' is not of the form ha_test:N")
                continue
            number = int(match.group(1))
            claimed.add(number)
            if number not in scenarios:
                problems.append(f"{where}: claims ha_test scenario {number}, which does not exist")

    known_defects = read_known_defects(_BUG_LIST)
    for section, cited in read_gaps(req_path):
        for defect in cited:
            if defect not in known_defects:
                problems.append(f"gap in section {section}: cites {defect}, which the bug list does not mention")

    unlinked = [f"ha_test scenario {number}: verifies no requirement -- say what it proves, or record that the book does not yet state it"
                for number in sorted(scenarios - claimed)]

    return problems, unlinked


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("req_file", type=Path, help="the .req file the book's build emitted")
    parser.add_argument("--strict", action="store_true", help="fail when a scenario verifies no requirement, not merely report it")
    parser.add_argument("--verbose", action="store_true", help="list every scenario that verifies no requirement, rather than counting them")
    args = parser.parse_args()

    if not args.req_file.is_file():
        print(f"check_book_requirements: {args.req_file} does not exist -- build the book first", file=sys.stderr)
        return 2

    problems, unlinked = check(args.req_file)
    requirements = read_requirements(args.req_file)
    covered = sum(1 for r in requirements if r.get("covers"))
    scenarios = read_scenarios(_HA_TEST)

    print(f"requirements stated: {len(requirements)}, verified by at least one scenario: {covered}, awaiting coverage: {len(requirements) - covered}")
    print(f"ha_test scenarios: {len(scenarios)}, verifying at least one requirement: {len(scenarios) - len(unlinked)}, verifying none: {len(unlinked)}")
    gaps = read_gaps(args.req_file)
    print(f"gaps recorded: {len(gaps)}, of which citing a defect: {sum(1 for _, cited in gaps if cited)}")

    if unlinked and args.verbose:
        for line in unlinked:
            print(f"  {line}")

    if args.strict:
        problems = problems + unlinked

    if problems:
        print(f"problems: {len(problems)}")
        for problem in problems:
            print(f"  {problem}")
        return 1

    print("no problems")
    return 0


if __name__ == "__main__":
    sys.exit(main())
