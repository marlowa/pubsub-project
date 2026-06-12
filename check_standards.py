#!/usr/bin/env python3
"""
check_standards.py -- Check C++ source files for coding-standard violations.

Checks implemented:
  1.  #define outside LoggingMacros.hpp
  2.  SCREAMING_SNAKE_CASE identifiers in non-macro contexts
  3.  camelCase identifiers (variables/functions must be snake_case)
  4.  NULL instead of nullptr
  5.  std::size_t instead of size_t
  6.  using namespace std
  7.  noexcept keyword
  8.  final keyword on classes or methods
  9.  #include with double-quoted path (library code only; tests/integration_tests/performance exempted)
  10. Lines exceeding 160 characters
  11. Non-ASCII characters
  12. End-of-brace comments (} // ...), except '} // namespaces' and '} // un-named namespace'
  13. Double underscore in user-defined identifiers
  14. Trailing return type syntax (auto f() ->)
  15. Tabs used for indentation
  16. East const style (T const* / T const&)
  17. static function at file scope in .cpp (prefer unnamed namespace)
  18. s_ prefix on class static members
  19. @throws in Doxygen comments (pubsub_itc_fw only)
  20. Missing #pragma once in .hpp files
  21. Template keyword on its own line before <
"""

import argparse
import re
import sys
from pathlib import Path

# ── Directories skipped during recursive discovery ────────────────────────────

_EXCLUDE_DIRS = frozenset({
    'build', 'installed', 'cmake-build-debug', 'cmake-build-release',
    '.git', 'thirdparty', '__pycache__',
})

# ── Violation ─────────────────────────────────────────────────────────────────

class Violation:
    def __init__(self, path: Path, line_number: int, message: str) -> None:
        self.path = path
        self.line_number = line_number
        self.message = message

    def __str__(self) -> str:
        return f"{self.path}:{self.line_number}: {self.message}"


# ── Comment and string stripping ──────────────────────────────────────────────

def strip_comments_and_strings(text: str) -> str:
    """
    Replace the interior of C++ comments and string/char literals with spaces,
    preserving newlines so that line numbers remain correct.
    """
    result = []
    i = 0
    n = len(text)
    while i < n:
        if text[i:i+2] == '/*':
            result.append('/*')
            i += 2
            while i < n and text[i:i+2] != '*/':
                result.append(' ' if text[i] != '\n' else '\n')
                i += 1
            if i < n:
                result.append('*/')
                i += 2
        elif text[i:i+2] == '//':
            result.append('//')
            i += 2
            while i < n and text[i] != '\n':
                result.append(' ')
                i += 1
        elif text[i] == '"':
            result.append('"')
            i += 1
            while i < n and text[i] not in ('"', '\n'):
                if text[i] == '\\' and i + 1 < n:
                    result.append('  ')
                    i += 2
                else:
                    result.append(' ')
                    i += 1
            if i < n and text[i] == '"':
                result.append('"')
                i += 1
        elif text[i] == "'":
            result.append("'")
            i += 1
            while i < n and text[i] not in ("'", '\n'):
                if text[i] == '\\' and i + 1 < n:
                    result.append('  ')
                    i += 2
                else:
                    result.append(' ')
                    i += 1
            if i < n and text[i] == "'":
                result.append("'")
                i += 1
        else:
            result.append(text[i])
            i += 1
    return ''.join(result)


# ── Helpers ───────────────────────────────────────────────────────────────────

_DEFINE_RE = re.compile(r'^\s*#\s*define\b')


def _is_define_line(line: str) -> bool:
    return bool(_DEFINE_RE.match(line))


# ── Check 1: #define outside LoggingMacros.hpp ───────────────────────────────
# Defines inside #ifdef CLANG_TIDY blocks are exempt: they are system-level
# feature-test macros or test-infrastructure workarounds that cannot be
# replaced with constexpr.

_IFDEF_ANY_RE = re.compile(r'^\s*#\s*if(?:def|ndef)?\b')
_IFDEF_CLANG_TIDY_RE = re.compile(r'^\s*#\s*ifdef\s+CLANG_TIDY\b')
_ENDIF_RE = re.compile(r'^\s*#\s*endif\b')

def check_defines(path: Path, lines: list[str], stripped: list[str]) -> list[Violation]:
    if path.name == 'LoggingMacros.hpp':
        return []
    violations = []
    ifdef_stack: list[bool] = []  # True = this nesting level is a CLANG_TIDY block
    for i, (line, sline) in enumerate(zip(lines, stripped), 1):
        raw = line.strip()
        if _IFDEF_CLANG_TIDY_RE.match(raw):
            ifdef_stack.append(True)
        elif _IFDEF_ANY_RE.match(raw):
            ifdef_stack.append(False)
        elif _ENDIF_RE.match(raw) and ifdef_stack:
            ifdef_stack.pop()
        if any(ifdef_stack):
            continue
        if _DEFINE_RE.match(sline):
            violations.append(Violation(path, i,
                '#define used outside LoggingMacros.hpp; use constexpr or inline instead'))
    return violations


# ── Check 2: SCREAMING_SNAKE_CASE in non-macro contexts ──────────────────────
# Extracts only the *declared* identifier from a constexpr declaration so that
# SCREAMING names used on the right-hand side (e.g. system constants such as
# INET6_ADDRSTRLEN) are not falsely flagged.

_SCREAMING_DECL_EXTRACT_RE = re.compile(
    r'\b(?:static\s+)?constexpr\s+'   # constexpr keyword
    r'[\w:<>*&\s]+?\s+'               # type (non-greedy)
    r'([A-Z][A-Z0-9]*(?:_[A-Z0-9]+)+)'  # SCREAMING declared name
    r'\s*[=;{\[]'                     # followed by =, ;, {, or [
)

def check_screaming_snake_case(path: Path, lines: list[str], stripped: list[str]) -> list[Violation]:
    violations = []
    for i, line in enumerate(stripped, 1):
        if _is_define_line(line):
            continue
        match = _SCREAMING_DECL_EXTRACT_RE.search(line)
        if match:
            name = match.group(1)
            violations.append(Violation(path, i,
                f"SCREAMING_SNAKE_CASE constant '{name}' -- "
                f"only macros may use SCREAMING_SNAKE_CASE; use snake_case for constants"))
    return violations


# ── Check 3: camelCase identifiers ───────────────────────────────────────────
# Variables, parameters, and function names must be snake_case.
# PascalCase (class/struct/type names) is correct and is not flagged here.

_CAMEL_CASE_RE = re.compile(r'\b([a-z][a-z0-9]*[A-Z][a-zA-Z0-9]*)\b')

# Known external/standard identifiers that legitimately use camelCase.
_CAMEL_CASE_ALLOWED = frozenset({
    'nullptr', 'noexcept', 'constexpr', 'sizeof', 'alignof', 'decltype',
    'reinterpret_cast', 'static_cast', 'dynamic_cast', 'const_cast',
    'typeid', 'true', 'false',
    # QuickFIX/J internal calls visible in Java but not in C++; none expected.
    # Add external C++ API names here if false positives arise.
})

def check_camel_case(path: Path, lines: list[str], stripped: list[str]) -> list[Violation]:
    violations = []
    seen_on_line: set[int] = set()
    for i, line in enumerate(stripped, 1):
        if _is_define_line(line):
            continue
        if line.lstrip().startswith('#include'):
            continue
        for match in _CAMEL_CASE_RE.finditer(line):
            name = match.group(1)
            if name in _CAMEL_CASE_ALLOWED:
                continue
            if i not in seen_on_line:
                violations.append(Violation(path, i,
                    f"camelCase identifier '{name}' -- use snake_case for variables and functions"))
                seen_on_line.add(i)
    return violations


# ── Check 4: NULL instead of nullptr ─────────────────────────────────────────

_NULL_RE = re.compile(r'\bNULL\b')

def check_null(path: Path, lines: list[str], stripped: list[str]) -> list[Violation]:
    violations = []
    for i, line in enumerate(stripped, 1):
        if _NULL_RE.search(line):
            violations.append(Violation(path, i, "NULL used; use nullptr instead"))
    return violations


# ── Check 5: std::size_t instead of size_t ───────────────────────────────────

_STD_SIZE_T_RE = re.compile(r'\bstd::size_t\b')

def check_std_size_t(path: Path, lines: list[str], stripped: list[str]) -> list[Violation]:
    violations = []
    for i, line in enumerate(stripped, 1):
        if _STD_SIZE_T_RE.search(line):
            violations.append(Violation(path, i,
                "std::size_t used; write size_t instead (the typedef needs no qualification)"))
    return violations


# ── Check 6: using namespace std ─────────────────────────────────────────────

_USING_NS_STD_RE = re.compile(r'\busing\s+namespace\s+std\b')

def check_using_namespace_std(path: Path, lines: list[str], stripped: list[str]) -> list[Violation]:
    violations = []
    for i, line in enumerate(stripped, 1):
        if _USING_NS_STD_RE.search(line):
            violations.append(Violation(path, i, "'using namespace std' is banned"))
    return violations


# ── Check 7: noexcept ─────────────────────────────────────────────────────────

_NOEXCEPT_RE = re.compile(r'\bnoexcept\b')

def check_noexcept(path: Path, lines: list[str], stripped: list[str]) -> list[Violation]:
    violations = []
    for i, line in enumerate(stripped, 1):
        if _NOEXCEPT_RE.search(line):
            violations.append(Violation(path, i,
                "'noexcept' is banned (coding rules: do not use noexcept, not even on dtors)"))
    return violations


# ── Check 8: final keyword ────────────────────────────────────────────────────

_FINAL_CLASS_RE = re.compile(r'\b(?:class|struct)\s+\w+\s+final\b')
_FINAL_METHOD_RE = re.compile(r'\)\s*(?:const\s+)?final\b')

def check_final(path: Path, lines: list[str], stripped: list[str]) -> list[Violation]:
    violations = []
    for i, line in enumerate(stripped, 1):
        if _FINAL_CLASS_RE.search(line) or _FINAL_METHOD_RE.search(line):
            violations.append(Violation(path, i,
                "'final' keyword is banned (coding rules: do not use final)"))
    return violations


# ── Check 9: #include with double quotes ─────────────────────────────────────
# Exempted: test, integration-test, and performance directories — they are not
# publishing an API and may use quoted includes for local convenience.

_INCLUDE_QUOTE_RE = re.compile(r'^\s*#\s*include\s+"')
_INCLUDE_QUOTE_EXEMPT_DIRS = frozenset({'tests', 'integration_tests', 'performance'})

def check_include_quotes(path: Path, lines: list[str], stripped: list[str]) -> list[Violation]:
    if any(part in _INCLUDE_QUOTE_EXEMPT_DIRS for part in path.parts):
        return []
    violations = []
    for i, line in enumerate(lines, 1):
        if _INCLUDE_QUOTE_RE.match(line):
            violations.append(Violation(path, i,
                '#include uses double quotes; all includes must use <angle brackets>'))
    return violations


# ── Check 10: lines exceeding 160 characters ─────────────────────────────────

def check_line_length(path: Path, lines: list[str], stripped: list[str]) -> list[Violation]:
    violations = []
    for i, line in enumerate(lines, 1):
        length = len(line.rstrip('\n\r'))
        if length > 160:
            violations.append(Violation(path, i,
                f"line length {length} exceeds 160-character limit"))
    return violations


# ── Check 11: non-ASCII characters ───────────────────────────────────────────

def check_non_ascii(path: Path, lines: list[str], stripped: list[str]) -> list[Violation]:
    violations = []
    for i, line in enumerate(lines, 1):
        for col, ch in enumerate(line, 1):
            if ord(ch) > 127:
                violations.append(Violation(path, i,
                    f"non-ASCII character U+{ord(ch):04X} at column {col} "
                    f"(source files must contain only ASCII)"))
                break
    return violations


# ── Check 12: end-of-brace comments ─────────────────────────────────────────
# Two permitted forms only:
#   '} // namespaces'         — closes one or more named namespaces (no name
#                               repeated, so nothing rots when namespaces are renamed)
#   '} // un-named namespace' — closes an unnamed namespace

_BRACE_COMMENT_RE = re.compile(r'\}\s*//')
_BRACE_COMMENT_ALLOWED_RE = re.compile(
    r'\}\s*//\s*(?:namespaces|un-named namespace)\s*$'
)
_BRACE_COMMENT_NAMED_NS_RE = re.compile(r'\}\s*//\s*namespace\b')
_BRACE_COMMENT_UNNAMED_NS_RE = re.compile(r'\}\s*//\s*(?:unnamed|anonymous)\s*namespace\b')

def check_brace_comments(path: Path, lines: list[str], stripped: list[str]) -> list[Violation]:
    violations = []
    for i, line in enumerate(lines, 1):
        if _BRACE_COMMENT_RE.search(line) and not _BRACE_COMMENT_ALLOWED_RE.search(line):
            if _BRACE_COMMENT_UNNAMED_NS_RE.search(line):
                msg = "unnamed namespace closing brace must use '} // un-named namespace'"
            elif _BRACE_COMMENT_NAMED_NS_RE.search(line):
                msg = ("named namespace closing brace must use '} // namespaces' "
                       "(do not repeat the namespace name)")
            else:
                msg = "end-of-brace comment is banned"
            violations.append(Violation(path, i, msg))
    return violations


# ── Check 13: double underscore in user-defined identifiers ──────────────────
# The C++ standard reserves all identifiers containing __.
# Predefined compiler macros starting with __ are permitted for use but not creation.

_DUNDER_IN_WORD_RE = re.compile(r'\b(\w+__\w+)\b')  # __ not at start of token
_DUNDER_PREDEFINED = frozenset({
    '__FILE__', '__LINE__', '__func__', '__FUNCTION__', '__PRETTY_FUNCTION__',
    '__cplusplus', '__GNUC__', '__GNUC_MINOR__', '__GNUC_PATCHLEVEL__',
    '__clang__', '__clang_major__', '__clang_minor__', '__clang_patchlevel__',
    '__DATE__', '__TIME__', '__STDC__', '__STDC_VERSION__',
    '__has_include', '__has_attribute', '__has_builtin', '__has_cpp_attribute',
    '__attribute__', '__builtin_expect', '__builtin_unreachable',
    '__asm__', '__volatile__', '__extension__', '__restrict',
    '__int128',
})

def check_double_underscore(path: Path, lines: list[str], stripped: list[str]) -> list[Violation]:
    violations = []
    for i, line in enumerate(stripped, 1):
        for match in _DUNDER_IN_WORD_RE.finditer(line):
            name = match.group(1)
            if name not in _DUNDER_PREDEFINED:
                violations.append(Violation(path, i,
                    f"double underscore in identifier '{name}' "
                    f"(reserved by the C++ standard; do not create such identifiers)"))
                break
    return violations


# ── Check 14: trailing return type syntax ────────────────────────────────────

_TRAILING_RETURN_RE = re.compile(
    r'\bauto\b[^;{(=\n]*\)\s*(?:const\s*)?(?:override\s*)?->'
)

def check_trailing_return_type(path: Path, lines: list[str], stripped: list[str]) -> list[Violation]:
    violations = []
    for i, line in enumerate(stripped, 1):
        if _TRAILING_RETURN_RE.search(line):
            violations.append(Violation(path, i,
                "trailing return type (auto f() ->) is banned; use traditional T f() form"))
    return violations


# ── Check 15: tabs ───────────────────────────────────────────────────────────

def check_tabs(path: Path, lines: list[str], stripped: list[str]) -> list[Violation]:
    violations = []
    for i, line in enumerate(lines, 1):
        if '\t' in line:
            violations.append(Violation(path, i,
                "tab character used; use spaces for indentation"))
    return violations


# ── Check 16: east const (T const* / T const&) ───────────────────────────────
# West const is preferred: const T* / const T&.
# Trailing method const (void f() const) is not a violation.

_TRAILING_METHOD_CONST_RE = re.compile(r'\)\s*const\s*(?:override\s*)?(?:=\s*0\s*)?[;{]')
_EAST_CONST_RE = re.compile(r'\b(\w[\w<>:]*)\s+const\s*([*&])')

def check_east_const(path: Path, lines: list[str], stripped: list[str]) -> list[Violation]:
    violations = []
    for i, line in enumerate(stripped, 1):
        cleaned = _TRAILING_METHOD_CONST_RE.sub(') ', line)
        for match in _EAST_CONST_RE.finditer(cleaned):
            type_name = match.group(1)
            ptr_or_ref = match.group(2)
            if type_name == 'const':
                continue
            violations.append(Violation(path, i,
                f"east const: write 'const {type_name}{ptr_or_ref}', "
                f"not '{type_name} const{ptr_or_ref}'"))
            break
    return violations


# ── Check 17: static function at file scope in .cpp ──────────────────────────
# Prefer an unnamed namespace. Only checked in .cpp files.

_STATIC_FUNC_RE = re.compile(
    r'^static\s+(?!(?:constexpr|const|inline|thread_local|assert)\b)'
    r'(?:const\s+)?(\w[\w<>:]*)\s+\w+\s*\('
)

def check_static_file_scope(path: Path, lines: list[str], stripped: list[str]) -> list[Violation]:
    if path.suffix != '.cpp':
        return []
    violations = []
    for i, line in enumerate(stripped, 1):
        if _STATIC_FUNC_RE.match(line):
            violations.append(Violation(path, i,
                "static file-scope function; "
                "prefer an unnamed namespace (namespace { ... })"))
    return violations


# ── Check 18: s_ prefix on class static members ──────────────────────────────

_S_PREFIX_RE = re.compile(r'\bstatic\s+[\w:<>]+\s+s_\w+')

def check_s_prefix_static(path: Path, lines: list[str], stripped: list[str]) -> list[Violation]:
    violations = []
    for i, line in enumerate(stripped, 1):
        if _S_PREFIX_RE.search(line):
            violations.append(Violation(path, i,
                "s_ prefix on static member is banned "
                "(coding rules: do not use s_ prefix for class static members)"))
    return violations


# ── Check 19: @throws in Doxygen comments (pubsub_itc_fw only) ───────────────
# Other libraries (e.g. scram_crypto) may use std::runtime_error and document
# it with @throws. The convention only applies to pubsub_itc_fw.

_DOXYGEN_THROWS_RE = re.compile(r'@throws?\b')

def check_doxygen_throws(path: Path, lines: list[str], stripped: list[str]) -> list[Violation]:
    if 'pubsub_itc_fw' not in path.parts:
        return []
    violations = []
    for i, line in enumerate(lines, 1):
        if _DOXYGEN_THROWS_RE.search(line):
            violations.append(Violation(path, i,
                "@throws/@throw in Doxygen comment is banned (coding rules)"))
    return violations


# ── Check 20: missing #pragma once in .hpp files ─────────────────────────────

_PRAGMA_ONCE_RE = re.compile(r'^\s*#\s*pragma\s+once\b', re.MULTILINE)

def check_pragma_once(path: Path, lines: list[str], stripped: list[str]) -> list[Violation]:
    if path.suffix != '.hpp':
        return []
    full_text = ''.join(lines)
    if not _PRAGMA_ONCE_RE.search(full_text):
        return [Violation(path, 1, "#pragma once missing from header file")]
    return []


# ── Check 21: template keyword on its own line before < ─────────────────────

_TEMPLATE_SPLIT_RE = re.compile(r'\btemplate\s*$')

def check_template_on_own_line(path: Path, lines: list[str], stripped: list[str]) -> list[Violation]:
    violations = []
    for i, line in enumerate(stripped, 1):
        if _TEMPLATE_SPLIT_RE.search(line.rstrip()):
            violations.append(Violation(path, i,
                "template keyword on its own line; "
                "the parameter list must be on the same line: template <...>"))
    return violations


# ── Registry ─────────────────────────────────────────────────────────────────

_CHECKS = [
    check_defines,
    check_screaming_snake_case,
    check_camel_case,
    check_null,
    check_std_size_t,
    check_using_namespace_std,
    check_noexcept,
    check_final,
    check_include_quotes,
    check_line_length,
    check_non_ascii,
    check_brace_comments,
    check_double_underscore,
    check_trailing_return_type,
    check_tabs,
    check_east_const,
    check_static_file_scope,
    check_s_prefix_static,
    check_doxygen_throws,
    check_pragma_once,
    check_template_on_own_line,
]


# ── File discovery ────────────────────────────────────────────────────────────

def find_cpp_files(root: Path) -> list[Path]:
    files = []
    for path in sorted(root.rglob('*')):
        if any(part in _EXCLUDE_DIRS for part in path.parts):
            continue
        if path.suffix in ('.hpp', '.cpp'):
            files.append(path)
    return files


# ── Entry point ───────────────────────────────────────────────────────────────

def main() -> int:
    parser = argparse.ArgumentParser(
        description='Check C++ source files for coding-standard violations.',
    )
    parser.add_argument(
        'paths', nargs='*', type=Path,
        help='Files or directories to check. '
             'Default: scan from the project root (directory of this script).',
    )
    parser.add_argument(
        '--root', type=Path, default=Path(__file__).resolve().parent,
        help='Project root used when no explicit paths are given.',
    )
    args = parser.parse_args()

    if args.paths:
        files: list[Path] = []
        for path in args.paths:
            if path.is_dir():
                files.extend(find_cpp_files(path))
            elif path.suffix in ('.hpp', '.cpp'):
                files.append(path)
            else:
                print(f"warning: skipping {path} (not a .hpp or .cpp file)",
                      file=sys.stderr)
    else:
        files = find_cpp_files(args.root)

    if not files:
        print("No C++ files found.", file=sys.stderr)
        return 1

    total = 0
    for path in files:
        try:
            text = path.read_text(encoding='utf-8', errors='replace')
        except OSError as exc:
            print(f"error: cannot read {path}: {exc}", file=sys.stderr)
            continue

        lines = text.splitlines(keepends=True)
        stripped_text = strip_comments_and_strings(text)
        stripped = stripped_text.splitlines(keepends=True)

        while len(stripped) < len(lines):
            stripped.append('\n')

        for check in _CHECKS:
            for violation in check(path, lines, stripped):
                print(violation)
                total += 1

    if total > 0:
        print(f"\n{total} violation(s) found.")
        return 1

    print("No violations found.")
    return 0


if __name__ == '__main__':
    sys.exit(main())
