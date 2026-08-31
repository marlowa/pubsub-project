#!/usr/bin/env python3
"""
check_doc_claims.py -- Check that what the documentation says about the code is still true.

Why this exists. On 2026-08-31 five statements in the documentation were found to be wrong,
none of them by anybody reading the documentation:

  * fix_order_gateway.md headed a section "Planned Migration to fix_codec (not yet done)" and
    said the gateway "currently carries its own hand-written FIX layer". The migration had
    landed five weeks earlier, in a commit whose message names the stage it completed.
  * fix_test_client.md described an "Advanced NOS Fields" form as "not yet built" and said the
    order form "exposes six fields". It exposes nine, in the collapsed block the section
    proposed, and the section's claim that repeating groups were deliberately left out was
    wrong too.
  * Four documents said the gateway must start before the sequencer, each contradicting itself
    in the same sentence by describing the retry that makes the ordering unnecessary.

Every one of those was a statement about a named thing in the source: a class, an include, an
element id. Nothing compared the statement to the source, so all of them read perfectly well
and were false. Link checking cannot catch this -- check_docs.py reported the tree consistent
throughout -- because the links were fine. It was the sentences that were wrong.

WHAT THIS CHECKS, AND WHAT IT CANNOT. It verifies claims an author has marked. It cannot find
an unmarked claim that has gone stale, and it is not meant to: a checker that tried to
understand prose would be wrong in both directions. What it gives is a way to bind a sentence
you expect to rot to the fact that would prove it, so the build fails instead of a reader
finding out months later.

Mark a claim by putting a comment before it:

    <!-- verify: present applications/fix_order_gateway/FixParser.cpp "fix_codec::FixMessageReader" -->
    The gateway frames inbound messages with a zero-copy reader from `fix_codec`.

    <!-- verify: absent applications/fix_order_gateway/FixMessage.hpp "namespace Tag {" -->
    `FixMessage.hpp` no longer carries tag tables of its own.

    <!-- verify: count java/.../messages.html "f-minqty" 1 -->
    The advanced fields include MinQty.

The forms, all of which take a repository-relative path:

    present  <path> "<text>"        the file contains that text at least once
    absent   <path> "<text>"        the file does not contain it
    count    <path> "<text>" <n>    it occurs exactly n times
    exists   <path>                 the path exists
    missing  <path>                 the path does not exist

The text is matched literally, not as a regular expression, because a claim in prose is about
a thing with a name and the name is what should be written down. A marker whose file has gone
is a failure in itself: it means the claim now describes nothing.
"""

from __future__ import annotations

import argparse
import re
import shlex
import sys
from pathlib import Path

# A marker is an HTML comment so that it renders as nothing, on GitHub and through Doxygen
# alike. The claim it guards is the prose that follows it, which this script never reads --
# only a person can say whether the prose and the fact agree, and the point of the marker is
# that they said so once and the build holds them to it.
MARKER = re.compile(r"<!--\s*verify:\s*(?P<body>.+?)\s*-->", re.DOTALL)

# A marker inside a fenced code block is an example of the notation, not a use of it. Without
# this, documenting the notation asserts it: the example in orientation/building.md became a
# thirteenth live claim the moment it was written, and an author illustrating the form with a
# made-up path would get a failure naming a file they never meant to talk about.
FENCE = re.compile(r"^\s*(```|~~~)", re.MULTILINE)

FORMS_TAKING_TEXT = ("present", "absent", "count")
FORMS_TAKING_PATH_ONLY = ("exists", "missing")


class Claim:
    """One marked claim: where it was written, and what it asserts."""

    def __init__(self, doc: Path, line: int, form: str, target: str, text: str | None, count: int | None):
        self.doc = doc
        self.line = line
        self.form = form
        self.target = target
        self.text = text
        self.count = count

    def where(self) -> str:
        return f"{self.doc}:{self.line}"

    def describe(self) -> str:
        if self.form in FORMS_TAKING_PATH_ONLY:
            return f"{self.form} {self.target}"
        if self.form == "count":
            return f'{self.form} {self.target} "{self.text}" {self.count}'
        return f'{self.form} {self.target} "{self.text}"'


def parse_marker(doc: Path, line_number: int, body: str) -> tuple[Claim | None, str | None]:
    """Turn one marker's text into a Claim, or into a reason it is not one."""
    try:
        parts = shlex.split(body)
    except ValueError as error:
        return None, f"cannot be read ({error})"

    if not parts:
        return None, "is empty"

    form = parts[0]
    if form in FORMS_TAKING_PATH_ONLY:
        if len(parts) != 2:
            return None, f"'{form}' takes a path and nothing else"
        return Claim(doc, line_number, form, parts[1], None, None), None

    if form not in FORMS_TAKING_TEXT:
        known = ", ".join(sorted(FORMS_TAKING_TEXT + FORMS_TAKING_PATH_ONLY))
        return None, f"'{form}' is not a form this understands (try one of: {known})"

    if form == "count":
        if len(parts) != 4:
            return None, "'count' takes a path, some text, and how many times it should occur"
        try:
            wanted = int(parts[3])
        except ValueError:
            return None, f"'{parts[3]}' is not a number of occurrences"
        if wanted < 0:
            return None, "a count cannot be negative"
        return Claim(doc, line_number, form, parts[1], parts[2], wanted), None

    if len(parts) != 3:
        return None, f"'{form}' takes a path and some text"
    if not parts[2]:
        return None, f"'{form}' was given no text to look for, which every file satisfies"
    return Claim(doc, line_number, form, parts[1], parts[2], None), None


def fenced_spans(text: str) -> list[tuple[int, int]]:
    """The character ranges covered by fenced code blocks, so markers inside them can be left alone.

    Fences are paired in the order they appear. An unclosed fence is taken to run to the end of
    the document, which is what a renderer does with one.
    """
    spans: list[tuple[int, int]] = []
    opened: int | None = None
    for fence in FENCE.finditer(text):
        if opened is None:
            opened = fence.start()
        else:
            spans.append((opened, fence.end()))
            opened = None
    if opened is not None:
        spans.append((opened, len(text)))
    return spans


def collect(source_dir: Path, docs_dir: Path) -> tuple[list[Claim], list[str]]:
    """Every marked claim in the documentation, and every marker that could not be read."""
    claims: list[Claim] = []
    malformed: list[str] = []
    for doc in sorted(docs_dir.rglob("*.md")):
        text = doc.read_text(errors="replace")
        skip = fenced_spans(text)
        for match in MARKER.finditer(text):
            if any(start <= match.start() < end for start, end in skip):
                continue
            line_number = text.count("\n", 0, match.start()) + 1
            claim, why = parse_marker(doc.relative_to(source_dir), line_number, match.group("body"))
            if claim is None:
                malformed.append(f"{doc.relative_to(source_dir)}:{line_number}: marker {why}")
            else:
                claims.append(claim)
    return claims, malformed


def check(claim: Claim, source_dir: Path) -> str | None:
    """Test one claim. Returns the reason it failed, or None if it holds."""
    path = source_dir / claim.target

    if claim.form == "exists":
        return None if path.exists() else f"{claim.target} does not exist"
    if claim.form == "missing":
        return None if not path.exists() else f"{claim.target} still exists"

    if not path.is_file():
        # Not the same failure as the text being absent, and worth saying differently: the
        # claim has stopped describing anything at all, so nobody can tell what it meant.
        return f"{claim.target} is not a file, so the claim describes nothing"

    body = path.read_text(errors="replace")
    occurrences = body.count(claim.text)

    if claim.form == "present":
        if occurrences == 0:
            return f'{claim.target} no longer contains "{claim.text}"'
        return None
    if claim.form == "absent":
        if occurrences > 0:
            return f'{claim.target} contains "{claim.text}", {occurrences} time(s)'
        return None
    if occurrences != claim.count:
        return f'{claim.target} contains "{claim.text}" {occurrences} time(s), not {claim.count}'
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--source-dir", default=None,
                        help="Repository root. Defaults to the parent of this script's directory.")
    parser.add_argument("--verbose", action="store_true",
                        help="Print every claim that holds, not only the ones that do not.")
    args = parser.parse_args()

    source_dir = Path(args.source_dir).resolve() if args.source_dir else Path(__file__).resolve().parent.parent
    docs_dir = source_dir / "docs"
    if not docs_dir.is_dir():
        print(f"error: no docs directory under {source_dir}", file=sys.stderr)
        return 2

    claims, malformed = collect(source_dir, docs_dir)

    failures = []
    for claim in claims:
        why = check(claim, source_dir)
        if why is not None:
            failures.append(f"{claim.where()}: {why}\n    the claim was: {claim.describe()}")
        elif args.verbose:
            print(f"  ok  {claim.where()}: {claim.describe()}")

    for problem in malformed:
        print(f"error: {problem}", file=sys.stderr)
    for failure in failures:
        print(f"error: {failure}", file=sys.stderr)

    if malformed or failures:
        print(f"\n{len(failures)} claim(s) no longer hold and {len(malformed)} marker(s) could not be read.",
              file=sys.stderr)
        print("A claim that has stopped being true is not a failing test -- it is documentation that "
              "will mislead the next person to read it. Correct the prose, then the marker.", file=sys.stderr)
        return 1

    if not claims:
        print("No verifiable claims are marked. Nothing about the code is being held to account.")
        return 0

    print(f"Documentation claims hold: {len(claims)} marked claim(s) across "
          f"{len({claim.doc for claim in claims})} document(s).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
