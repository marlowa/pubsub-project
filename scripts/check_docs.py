#!/usr/bin/env python3
"""
check_docs.py -- Check that the documentation forms a connected book, and that every path
pointing into it still resolves.

Why this exists. Three documents were moved into docs/design/history/ and the references to them
from pubsub.dsl, topics.dsl, TopicPublisher.hpp and generator_topics.py were never updated, so
four doc paths cited from source resolve to nothing. Twelve markdown files are reachable from no
other document at all. Both faults are silent: nothing builds the documentation as a whole, so a
path that has stopped working looks exactly like one nobody has followed yet.

Checks implemented:
  1. Every markdown link between documents resolves to a file that exists
  2. Every documentation path cited from a non-markdown file exists
  3. Every tracked markdown file under docs/ is reachable by following links from the contents
     page, so a document cannot be added and then be readable by nobody
  4. Every clickable target in the architecture diagram resolves to a declared Doxygen anchor
  5. No reference uses a path-derived Doxygen id (md_*), which a file move would silently break
  6. Every anchor cited as file.md#anchor, or linked as (#anchor), resolves -- either to an
     explicit {#anchor} declaration or to a heading whose GitHub slug matches

Reachability starts at the repository README and the documentation contents page, which is
docs/README.md where that exists and docs/README.md otherwise. GitHub renders README.md when a
directory is browsed, which is why the contents page wants that name.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

_PROJECT_ROOT = Path(__file__).resolve().parent.parent

# Deliberately outside the book: instructions to an AI assistant, the changelog, which GitHub
# renders on its own, and scratch directories that are not documentation at all.
_NOT_IN_THE_BOOK = {'claude.md', 'CHANGELOG.md', 'README.md', 'DESIGN.md'}
_IGNORED_PREFIXES = ('.pytest_cache/', 'python/.pytest_cache/', '.claude/')

# Paths that are deliberately illustrative. architecture_map_howto.dox teaches how to add a
# component by working through an invented one, so its example path must not exist.
_DOCUMENTED_EXAMPLES = {'docs/venue/foo_service.md'}

_MARKDOWN_LINK_RE = re.compile(r'\[[^\]]*\]\(([^)]+)\)')
# A raw HTML link is how a document links to one Doxygen deliberately excludes: Doxygen passes
# it through untouched instead of trying to resolve it as a page reference.
_HTML_LINK_RE = re.compile(r'<a\s+href="([^"]+)"')


def tracked_markdown() -> list[str]:
    result = subprocess.run(['git', 'ls-files', '*.md'], cwd=_PROJECT_ROOT, capture_output=True, text=True, check=True)
    return [line for line in result.stdout.split() if not line.startswith(_IGNORED_PREFIXES)]


def without_code_blocks(text: str) -> str:
    """Drop fenced blocks and inline code, so C++ such as vector<const char*> is not read as a link."""
    text = re.sub(r'^```.*?^```', '', text, flags=re.MULTILINE | re.DOTALL)
    return re.sub(r'`[^`\n]*`', '', text)


def link_targets(path: Path) -> list[str]:
    """Return the link targets in one markdown file, anchors and external URLs removed."""
    targets = []
    text = without_code_blocks(path.read_text(encoding='utf-8'))
    for raw in _MARKDOWN_LINK_RE.findall(text) + _HTML_LINK_RE.findall(text):
        target = raw.split()[0].split('#')[0].strip()
        if not target or '://' in target or target.startswith('mailto:'):
            continue
        targets.append(target)
    return targets


def check_links(files: list[str]) -> tuple[list[str], dict[str, set[str]]]:
    """Check every markdown link resolves, and return the graph of what links to what."""
    faults: list[str] = []
    graph: dict[str, set[str]] = {}
    for name in files:
        path = _PROJECT_ROOT / name
        graph[name] = set()
        for target in link_targets(path):
            resolved = (path.parent / target).resolve()
            try:
                relative = str(resolved.relative_to(_PROJECT_ROOT))
            except ValueError:
                faults.append(f'{name}: link escapes the repository: {target!r}')
                continue
            if not resolved.exists():
                faults.append(f'{name}: link to {target!r} resolves to {relative}, which does not exist')
            elif relative.endswith('.md'):
                graph[name].add(relative)
    return faults, graph


def check_citations_from_source() -> list[str]:
    """Documentation paths named in code, configuration and scripts must exist."""
    result = subprocess.run(['git', 'grep', '-hoE', r'docs/[A-Za-z0-9_/.-]+\.md', '--', ':!*.md',
                             ':!scripts/check_docs.py'],
                            cwd=_PROJECT_ROOT, capture_output=True, text=True, check=False)
    if result.returncode not in (0, 1):
        return [f'git grep failed: {result.stderr.strip()}']

    faults = []
    for cited in sorted(set(result.stdout.split())):
        if cited in _DOCUMENTED_EXAMPLES:
            continue
        if not (_PROJECT_ROOT / cited).exists():
            where = subprocess.run(['git', 'grep', '-ln', cited, '--', ':!*.md', ':!scripts/check_docs.py'],
                                   cwd=_PROJECT_ROOT, capture_output=True, text=True, check=False)
            citers = ', '.join(where.stdout.split()) or 'unknown'
            faults.append(f'{cited} is cited by {citers} but does not exist')
    return faults


def declared_anchors() -> dict[str, str]:
    """Doxygen ids declared anywhere: {#id} in markdown, and \\page / \\section in .dox."""
    result = subprocess.run(['git', 'ls-files', '*.md', '*.dox'], cwd=_PROJECT_ROOT,
                            capture_output=True, text=True, check=True)
    anchors = {}
    for name in result.stdout.split():
        text = (_PROJECT_ROOT / name).read_text(encoding='utf-8')
        for anchor in re.findall(r'\{#([A-Za-z0-9_]+)\}', text):
            anchors[anchor] = name
        for anchor in re.findall(r'[\\@](?:page|section|subsection|anchor|defgroup)\s+([A-Za-z0-9_]+)', text):
            anchors[anchor] = name
    return anchors


def check_doxygen() -> list[str]:
    """The architecture diagram is clickable. Its boxes link by Doxygen anchor, so an anchor that
    stops being declared turns a box into a dead link, and Doxygen 1.8.14 does not fail on it."""
    anchors = declared_anchors()
    faults = []

    for dot in subprocess.run(['git', 'ls-files', '*.dot'], cwd=_PROJECT_ROOT,
                              capture_output=True, text=True, check=True).stdout.split():
        text = (_PROJECT_ROOT / dot).read_text(encoding='utf-8')
        for target in re.findall(r'URL\s*=\s*"\\ref\s+([A-Za-z0-9_]+)"', text):
            if target not in anchors:
                faults.append(f'{dot}: the diagram links to \\ref {target}, which no document declares')

    grep = subprocess.run(['git', 'grep', '-nE', r'[\\@]ref[[:space:]]+md_'], cwd=_PROJECT_ROOT,
                          capture_output=True, text=True, check=False)
    for line in grep.stdout.splitlines():
        faults.append(f'{line.strip()}: references a path-derived Doxygen id, which a file move breaks. '
                      f'Give the target an explicit {{#anchor}} and reference that instead')
    return faults


def heading_slugs() -> set[str]:
    """GitHub derives an anchor from every heading: lower-cased, punctuation dropped, spaces to
    hyphens. Those are legitimate link targets even though nothing declares them."""
    slugs = set()
    result = subprocess.run(['git', 'ls-files', '*.md'], cwd=_PROJECT_ROOT, capture_output=True, text=True, check=True)
    for name in result.stdout.split():
        if name.startswith('.claude/'):
            continue
        for heading in re.findall(r'^#{1,6} +(.*)$', (_PROJECT_ROOT / name).read_text(encoding='utf-8'), re.MULTILINE):
            heading = re.sub(r'\{#[A-Za-z0-9_]+\}', '', heading)
            heading = re.sub(r'[`*\[\]()]', '', heading).strip().lower()
            slugs.add(re.sub(r'[^a-z0-9 _-]', '', heading).replace(' ', '-'))
    return slugs


def check_anchor_citations() -> list[str]:
    """A section cited by anchor must exist. Section numbers used to be the identifier in the HA
    decision record, and 11a to 11e exist because inserting a section would have renumbered every
    citation of it. Anchors do not have that problem, which is why the citations now use them."""
    known = set(declared_anchors()) | heading_slugs()
    faults = []
    grep = subprocess.run(['git', 'grep', '-nE', r'(\.md#[A-Za-z0-9_-]+|\]\(#[A-Za-z0-9_-]+\))', '--',
                           ':!scripts/check_docs.py'],
                          cwd=_PROJECT_ROOT, capture_output=True, text=True, check=False)
    if grep.returncode not in (0, 1):
        return [f'git grep failed: {grep.stderr.strip()}']

    for line in grep.stdout.splitlines():
        location, _, text = line.partition(':')
        number, _, body = text.partition(':')
        if location.startswith(('.claude/', 'docs/history/')):
            continue
        body = re.sub(r'`[^`]*`', '', body)
        cited = set(re.findall(r'\.md#([A-Za-z0-9_-]+)', body)) | set(re.findall(r'\]\(#([A-Za-z0-9_-]+)\)', body))
        for anchor in sorted(cited - known):
            faults.append(f'{location}:{number}: cites anchor #{anchor}, which is neither declared nor a heading')
    return faults


def contents_page() -> str | None:
    for candidate in ('docs/README.md', 'docs/README.md'):
        if (_PROJECT_ROOT / candidate).exists():
            return candidate
    return None


def check_reachable(files: list[str], graph: dict[str, set[str]]) -> list[str]:
    roots = [name for name in ('README.md', contents_page()) if name]
    seen: set[str] = set()
    queue = list(roots)
    while queue:
        name = queue.pop()
        if name in seen:
            continue
        seen.add(name)
        queue.extend(graph.get(name, ()))

    faults = []
    for name in sorted(files):
        if name in seen or Path(name).name in _NOT_IN_THE_BOOK:
            continue
        faults.append(f'{name} is reachable from no other document')
    return faults


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--links-only', action='store_true', help='check that links resolve, without the reachability check')
    args = parser.parse_args()

    files = tracked_markdown()
    faults, graph = check_links(files)
    faults += check_citations_from_source()
    faults += check_doxygen()
    faults += check_anchor_citations()
    if not args.links_only:
        faults += check_reachable(files, graph)

    for fault in faults:
        print(fault)

    if faults:
        print(f'\n{len(faults)} fault(s) found across {len(files)} documents.')
        return 1

    print(f'Documentation is consistent: {len(files)} documents, every link resolves, every one reachable.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
