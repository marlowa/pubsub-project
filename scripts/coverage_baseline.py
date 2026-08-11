#!/usr/bin/env python3
"""Record a coverage baseline, and report how the current build has moved against it.

    ./build.sh --coverage --coverage-report          # produces the tracefile
    python3 coverage_baseline.py --update            # record where we are now
    python3 coverage_baseline.py                     # report what has changed since

This REPORTS. It does not gate, and it never fails a build: the exit status is 0 whether
coverage rose, fell or stayed put. A threshold that blocks a merge is satisfiable by
writing a test that executes the hard path and asserts nothing, which passes the check and
leaves behind a test that can never fail and must be maintained forever. The number is
worth watching; it is not worth obeying.

Three things follow from that, and they are the whole design:

  * Per file, never one number. A single percentage is a scalar summary of a
    distribution: it cannot tell 80% everywhere from 100% of the trivial code and 20% of
    the matching engine, and those are different risks.

  * Counts, not percentages. Delete fifty well-tested lines and a percentage falls though
    nothing got worse. With hit/total you can tell "five lines stopped being covered" from
    "fifty covered lines were deleted" -- different events, different responses.

  * Name what regressed. "SessionStore::evict_expired is no longer covered" is actionable
    in thirty seconds; "-0.4%" starts a discussion. The baseline therefore records the
    UNCOVERED function signatures, which are both the smaller set and the useful one.

The baseline is a committed text file, sorted, one record per line, so that a change in
coverage arrives as a reviewable diff rather than as a number in a database somewhere.
Update it in the same commit as the change that moved it, while the reason is still known.

Note on excluded code: the baseline does not record gcovr's --exclude patterns, it records
the set of files that survived them. That is the observable thing. If the excludes change,
files appear or disappear and this reports FILE NEW / FILE GONE, which is what you want to
see -- the patterns themselves could match nothing and look fine.
"""

import argparse
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

DEFAULT_TRACEFILE = Path("build-coverage/coverage.info")
DEFAULT_BASELINE = Path("coverage_baseline.txt")

# A tracefile records the absolute paths of the machine that built it. The prefix varies by
# checkout and must not reach the committed file, or the baseline only matches the machine
# that wrote it.
#
# Taken from this script's own location rather than named as a string. A named directory is
# only ever right until the project is moved: this held "pubsub-project-10-copilot/" from
# 2026-08-07 until the move out of that directory, after which it matched nothing, every path
# stayed absolute, and all 302 files read as new. The baseline was not stale and coverage had
# not moved -- but the release check reported a stale baseline and said not to tag.
PROJECT_ROOT = Path(__file__).resolve().parent.parent


# =========================================================================== #
# READING THE TRACEFILE
# =========================================================================== #

def read_tracefile(path):
    """Parse an lcov tracefile into {relative path: {"lines": {...}, "functions": {...}}}.

    Hit counts are kept rather than reduced to booleans on the way in. A line whose count
    moved from 900 to 901 is not a coverage change, and a comparison that treated it as one
    would report every run as different; the reduction to covered/not-covered happens where
    the comparison is made, deliberately and once.
    """
    files = {}
    current = {}
    with open(path, "r", encoding="utf-8") as handle:
        for raw_line in handle:
            line = raw_line.strip()
            if line.startswith("SF:"):
                current = files.setdefault(strip_project_root(line[3:]), {"lines": {}, "functions": {}})
            elif line.startswith("DA:") and current:
                number, _, remainder = line[3:].partition(",")
                current["lines"][int(number)] = int(remainder.split(",")[0])
            elif line.startswith("FNDA:") and current:
                hits, _, name = line[5:].partition(",")
                current["functions"][name] = int(hits)
            elif line == "end_of_record":
                current = {}
    return files


def summarise(entry):
    """Return (lines hit, lines total, functions hit, functions total, uncovered names)."""
    lines_hit = sum(1 for hits in entry["lines"].values() if hits > 0)
    functions_hit = sum(1 for hits in entry["functions"].values() if hits > 0)
    uncovered = sorted(name for name, hits in entry["functions"].items() if hits == 0)
    return lines_hit, len(entry["lines"]), functions_hit, len(entry["functions"]), uncovered


def current_commit():
    """Short commit the working tree is at, or "unknown" outside a repository."""
    try:
        result = subprocess.run(["git", "rev-parse", "--short", "HEAD"],
                                capture_output=True, text=True, check=True)
        return result.stdout.strip()
    except (subprocess.CalledProcessError, FileNotFoundError):
        return "unknown"


def platform_identifier():
    """Distribution and compiler version this coverage data was produced by.

    Recorded because a baseline is TOOLCHAIN-SPECIFIC and comparing across toolchains is
    meaningless. gcc 8.5 in the Rocky container and gcc 13 on the development host emit
    different function lists for identical sources -- different template instantiations,
    different lambda naming, different [abi:cxx11] decoration -- and the container also
    builds against different third-party versions.

    Without this, a coverage build run in the container lands in build-coverage-rocky8/
    (the flavour suffix takes care of that) but `--update` from it would overwrite the
    host's baseline with a list that legitimately differs in hundreds of places. Nothing
    would complain, and the next release check would report a wall of phantom regressions.
    """
    system = "unknown"
    try:
        text = Path("/etc/os-release").read_text(encoding="utf-8")
        lines = dict(entry.split("=", 1) for entry in text.splitlines() if "=" in entry)
        name = lines.get("ID", "unknown").strip('"')
        version = lines.get("VERSION_ID", "").strip('"')
        system = f"{name}{version}"
    except OSError:
        pass
    compiler = "unknown"
    try:
        result = subprocess.run(["gcc", "-dumpfullversion"],
                                capture_output=True, text=True, check=True)
        compiler = result.stdout.strip()
    except (subprocess.CalledProcessError, FileNotFoundError, OSError):
        pass
    return f"{system}-gcc{compiler}"


# =========================================================================== #
# THE BASELINE FILE
# =========================================================================== #

def strip_project_root(name):
    """A tracefile path with the checkout's own prefix removed, so it names the same file
    whoever built it. A path from outside the project -- a system or third-party header --
    has no such prefix and is left as it stands, which is what keeps it recognisable.

    resolve() is applied to both sides so that a checkout reached through a symlink still
    matches: /home/marlowa/mystuff is a link to /mnt/sda1/marlowa-extra/mystuff, and a
    textual comparison of the two would agree on nothing.
    """
    try:
        return str(Path(name).resolve().relative_to(PROJECT_ROOT))
    except ValueError:
        return name


def relative_if_possible(path):
    """Path relative to the working directory, or its bare name if it lies outside."""
    try:
        return Path(path).resolve().relative_to(Path.cwd())
    except ValueError:
        return Path(path).name


def write_baseline(files, path, tracefile):
    """Write the baseline, sorted, so that a later diff is readable."""
    total_lines_hit = total_lines = total_functions_hit = total_functions = 0
    records = []
    for name in sorted(files):
        lines_hit, lines_total, functions_hit, functions_total, uncovered = summarise(files[name])
        total_lines_hit += lines_hit
        total_lines += lines_total
        total_functions_hit += functions_hit
        total_functions += functions_total
        records.append((name, lines_hit, lines_total, functions_hit, functions_total, uncovered))

    lines = [
        "# Coverage baseline -- see coverage_baseline.py.",
        "#",
        "# Counts, not percentages: a percentage moves when the denominator moves, so it",
        "# cannot distinguish lines that stopped being covered from covered lines that were",
        "# deleted. UNCOVERED records name the functions with no observations, which is the",
        "# smaller set and the one worth acting on.",
        "#",
        "# Regenerate with: ./build.sh --coverage --coverage-report",
        "#                 python3 coverage_baseline.py --update",
        "# Commit the result alongside the change that moved it.",
        "",
        f"COMMIT {current_commit()}",
        f"PLATFORM {platform_identifier()}",
        f"GENERATED {datetime.now(timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')}",
        # Relative where possible: an absolute path names the machine that happened to
        # write the baseline, and would show up as a spurious diff on the next one.
        f"TRACEFILE {relative_if_possible(tracefile)}",
        f"TOTALS lines {total_lines_hit}/{total_lines} functions "
        f"{total_functions_hit}/{total_functions}",
        "",
    ]
    for name, lines_hit, lines_total, functions_hit, functions_total, uncovered in records:
        lines.append(f"FILE {name} lines {lines_hit}/{lines_total} "
                     f"functions {functions_hit}/{functions_total}")
        for signature in uncovered:
            lines.append(f"  UNCOVERED {signature}")

    Path(path).write_text("\n".join(lines) + "\n", encoding="utf-8")
    return total_lines_hit, total_lines, total_functions_hit, total_functions


def read_baseline_at_ref(reference, path):
    """Return the baseline as it was committed at ``reference``, via a temporary file.

    The previous release's coverage needs no storage of its own: the baseline is a
    committed file, so `git show v0.2.0:coverage_baseline.txt` IS what coverage looked
    like when v0.2.0 was tagged. That is the comparison a release wants -- movement since
    the last release, not since the last commit.
    """
    import tempfile

    result = subprocess.run(["git", "show", f"{reference}:{path}"],
                            capture_output=True, text=True, check=False)
    if result.returncode != 0:
        raise FileNotFoundError(
            f"no {path} committed at '{reference}': {result.stderr.strip()}\n"
            f"A release before the baseline existed will not have one; compare against a "
            f"later tag, or review this release's coverage without a comparison.")
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False, encoding="utf-8") as handle:
        handle.write(result.stdout)
        temporary_path = handle.name
    try:
        return read_baseline(temporary_path)
    finally:
        Path(temporary_path).unlink(missing_ok=True)


def read_baseline(path):
    """Parse a baseline file back into {file: (counts, uncovered set)} plus its header."""
    header = {}
    files = {}
    current = None
    with open(path, "r", encoding="utf-8") as handle:
        for raw_line in handle:
            line = raw_line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            if line.startswith("  UNCOVERED "):
                if current is not None:
                    files[current]["uncovered"].add(line[len("  UNCOVERED "):])
                continue
            parts = line.split()
            if parts[0] == "FILE":
                # FILE <path> lines <hit>/<total> functions <hit>/<total>
                current = parts[1]
                lines_hit, lines_total = (int(value) for value in parts[3].split("/"))
                functions_hit, functions_total = (int(value) for value in parts[5].split("/"))
                files[current] = {
                    "lines": (lines_hit, lines_total),
                    "functions": (functions_hit, functions_total),
                    "uncovered": set(),
                }
            elif parts[0] in ("COMMIT", "PLATFORM", "GENERATED", "TRACEFILE"):
                header[parts[0]] = " ".join(parts[1:])
            elif parts[0] == "TOTALS":
                header["TOTALS"] = line
                current = None
    return header, files


# =========================================================================== #
# COMPARISON
# =========================================================================== #

def warn_on_platform_mismatch(header):
    """Print a warning when the baseline was produced by a different toolchain.

    A warning and not a refusal: reading someone else's baseline is a reasonable thing to
    do deliberately. Writing over it is not, which is why --update refuses instead.
    """
    recorded = header.get("PLATFORM")
    running = platform_identifier()
    if recorded is None:
        print(f"NOTE: this baseline predates the PLATFORM field; it is assumed to be {running}.\n",
              file=sys.stderr)
    elif recorded != running:
        print(f"WARNING: baseline was produced on {recorded}, this is {running}.\n"
              f"         Function lists differ between toolchains for identical sources, so\n"
              f"         most of what follows is likely to be noise rather than a change.\n",
              file=sys.stderr)


def compare(header, baseline, files, description):
    """Report movement against a baseline. Always returns 0 -- this reports, never gates."""
    warn_on_platform_mismatch(header)
    print(f"{description} taken at {header.get('COMMIT', '?')} on {header.get('GENERATED', '?')}")
    print(f"working tree is at {current_commit()}\n")

    current = {}
    for name, entry in files.items():
        lines_hit, lines_total, functions_hit, functions_total, uncovered = summarise(entry)
        current[name] = {
            "lines": (lines_hit, lines_total),
            "functions": (functions_hit, functions_total),
            "uncovered": set(uncovered),
        }

    def totals(source):
        return (sum(v["lines"][0] for v in source.values()),
                sum(v["lines"][1] for v in source.values()),
                sum(v["functions"][0] for v in source.values()),
                sum(v["functions"][1] for v in source.values()))

    was = totals(baseline)
    now = totals(current)
    print(f"{'':10s} {'lines':>16s}   {'functions':>16s}")
    print(f"{'baseline':10s} {was[0]:6d}/{was[1]:<9d} {was[2]:6d}/{was[3]:<9d}")
    print(f"{'now':10s} {now[0]:6d}/{now[1]:<9d} {now[2]:6d}/{now[3]:<9d}")
    print(f"{'change':10s} {now[0] - was[0]:+6d}/{now[1] - was[1]:<+9d} "
          f"{now[2] - was[2]:+6d}/{now[3] - was[3]:<+9d}\n")

    gone = sorted(set(baseline) - set(current))
    added = sorted(set(current) - set(baseline))
    for name in gone:
        print(f"FILE GONE  {name}")
    for name in added:
        entry = current[name]
        print(f"FILE NEW   {name} lines {entry['lines'][0]}/{entry['lines'][1]} "
              f"functions {entry['functions'][0]}/{entry['functions'][1]}")
    if gone or added:
        print()

    # Function movement and line movement are reported separately because they are not
    # equally trustworthy, and the difference was measured rather than assumed. Across four
    # clean runs of the whole suite at one commit, function coverage was identical every
    # time -- same count, same set -- while the line count came out 5634, 5644, 5643 and
    # 5649. The variance is a handful of shutdown-race lines (an event arriving while a
    # thread winds down, and the as_string() call in the log statement that reports it):
    # real code, reachable only when the timing falls a certain way.
    #
    # So a function that stops being covered is a finding, and a line count that shifts by
    # a few is weather. Presenting them alike would train the reader to skim both.
    function_moves = []
    line_moves = []
    for name in sorted(set(baseline) & set(current)):
        before, after = baseline[name], current[name]
        # Computed from the UNCOVERED sets rather than the counts, so a file that loses
        # coverage of one function and gains it on another -- unchanged counts, changed
        # reality -- still reports.
        regressed = sorted(after["uncovered"] - before["uncovered"])
        recovered = sorted(before["uncovered"] - after["uncovered"])
        if regressed or recovered:
            function_moves.append((name, regressed, recovered))
        if before["lines"] != after["lines"]:
            line_moves.append((name, before["lines"], after["lines"]))

    if function_moves:
        print("FUNCTION COVERAGE CHANGED -- this is the signal worth acting on:\n")
        for name, regressed, recovered in function_moves:
            # The file's function TOTAL is printed beside the name when it moved, because
            # the baseline records only uncovered functions and so cannot by itself
            # distinguish "this function was added without tests" from "this function was
            # covered and something broke it". A file that grew makes the first reading
            # likely, and both readings are findings -- the reviewer needs to know which.
            before_total = baseline[name]["functions"][1]
            after_total = current[name]["functions"][1]
            grew = ""
            if before_total != after_total:
                grew = f"   (file's functions {before_total} -> {after_total})"
            print(f"  {name}{grew}")
            for signature in regressed:
                print(f"      NOT COVERED  {signature}")
            for signature in recovered:
                print(f"      now covered  {signature}")
        print()

    if line_moves:
        print("Line counts also moved (informational -- a few lines vary between identical")
        print("runs; see the note in this script):\n")
        for name, before_lines, after_lines in line_moves:
            print(f"  {name}: {before_lines[0]}/{before_lines[1]}"
                  f"  ->  {after_lines[0]}/{after_lines[1]}")
        print()

    if not function_moves and not line_moves and not gone and not added:
        print("No change against the baseline.")
    elif not function_moves:
        print("No function-level change against the baseline.")
    else:
        print("If this is intended, re-run with --update and commit the baseline "
              "alongside\nthe change that caused it.")
    return 0


def main(argv=None):
    """Parse the command line and either record the baseline or report against it."""
    parser = argparse.ArgumentParser(
        description="Record and compare a code-coverage baseline. Reports; never gates.")
    parser.add_argument("--update", action="store_true",
                        help="overwrite the baseline with the current tracefile")
    parser.add_argument("--tracefile", type=Path, default=DEFAULT_TRACEFILE,
                        help=f"lcov tracefile to read (default: {DEFAULT_TRACEFILE})")
    parser.add_argument("--baseline", type=Path, default=DEFAULT_BASELINE,
                        help=f"baseline file (default: {DEFAULT_BASELINE})")
    parser.add_argument("--since", metavar="REF",
                        help="compare against the baseline as COMMITTED at a git ref -- a release "
                             "tag, normally. This is the release question: what moved since we "
                             "last shipped, rather than since the last commit. Needs no stored "
                             "history because the baseline is a committed file")
    parser.add_argument("--force-platform", action="store_true",
                        help="allow --update to overwrite a baseline produced by a different "
                             "toolchain. Almost always wrong: gcc 8.5 in the Rocky container and "
                             "gcc 13 on this host emit different function lists for identical "
                             "sources, so the result would describe neither")
    arguments = parser.parse_args(argv)

    if arguments.since and arguments.update:
        parser.error("--since compares against a released baseline; "
                     "it cannot be combined with --update")

    if not arguments.tracefile.exists():
        print(f"error: no tracefile at {arguments.tracefile}\n"
              f"Generate one with: ./build.sh --coverage --coverage-report", file=sys.stderr)
        return 2

    files = read_tracefile(arguments.tracefile)
    if not files:
        print(f"error: {arguments.tracefile} holds no file records", file=sys.stderr)
        return 2

    if arguments.update:
        # Refuse rather than warn: overwriting a baseline with one from another toolchain
        # silently replaces a correct file with a wrong one, and the damage only surfaces
        # later as a wall of phantom regressions that looks like a real problem.
        if arguments.baseline.exists() and not arguments.force_platform:
            existing_header, _ = read_baseline(arguments.baseline)
            recorded = existing_header.get("PLATFORM")
            running = platform_identifier()
            if recorded is not None and recorded != running:
                print(f"error: {arguments.baseline} was produced on {recorded}, "
                      f"this is {running}.\n"
                      f"A baseline is toolchain-specific -- different compilers emit different\n"
                      f"function lists for identical sources -- so overwriting it here would\n"
                      f"produce a file that describes neither. Update it on {recorded}, or pass\n"
                      f"--force-platform if you really mean to move the baseline over.",
                      file=sys.stderr)
                return 2
        lines_hit, lines_total, functions_hit, functions_total = write_baseline(
            files, arguments.baseline, arguments.tracefile)
        print(f"wrote {arguments.baseline}: {len(files)} files, "
              f"lines {lines_hit}/{lines_total}, functions {functions_hit}/{functions_total}")
        return 0

    if arguments.since:
        try:
            header, baseline = read_baseline_at_ref(arguments.since, str(arguments.baseline))
        except FileNotFoundError as error:
            print(f"error: {error}", file=sys.stderr)
            return 2
        print(f"Coverage review: what has moved since {arguments.since}\n")
        return compare(header, baseline, files, f"{arguments.since} baseline")

    if not arguments.baseline.exists():
        print(f"error: no baseline at {arguments.baseline}; create one with --update",
              file=sys.stderr)
        return 2
    header, baseline = read_baseline(arguments.baseline)
    return compare(header, baseline, files, "baseline")


if __name__ == "__main__":
    sys.exit(main())
