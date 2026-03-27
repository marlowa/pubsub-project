#!/usr/bin/env python3
"""
easterly.py — Convert East const C++ style to West const style.

East const:  int const x         →  West const: const int x
             int const *p        →              const int *p
             int * const p       →              int * const p   (unchanged — ptr itself is const)
             char const * const  →              const char * const

Named "easterly" because, like a wind from the east, it blows east-const
code back westward. The inverse of "westerly".

Usage:
    python3 easterly.py input.cpp              # prints to stdout
    python3 easterly.py input.cpp -o out.cpp   # writes to file
    python3 easterly.py input.cpp --dry-run    # shows diff only
    python3 easterly.py -i input.cpp           # modify in place
    cat input.cpp | python3 easterly.py -      # reads from stdin

No regex used. Uses a hand-written C++ lexer that understands:
  - Line comments, block comments (preserved verbatim)
  - String literals, char literals, raw string literals  R"(...)"
  - All C++ tokens
"""

import sys
import argparse
from dataclasses import dataclass
from enum import Enum, auto


# ---------------------------------------------------------------------------
# Token kinds
# ---------------------------------------------------------------------------

class TK(Enum):
    WORD          = auto()   # identifier or keyword
    STAR          = auto()   # *
    AMP           = auto()   # &
    AMPAMP        = auto()   # &&
    LBRACKET      = auto()   # [
    RBRACKET      = auto()   # ]
    LPAREN        = auto()   # (
    RPAREN        = auto()   # )
    LBRACE        = auto()   # {
    RBRACE        = auto()   # }
    LANGLE        = auto()   # <
    RANGLE        = auto()   # >
    SEMICOLON     = auto()   # ;
    COMMA         = auto()   # ,
    EQUALS        = auto()   # =
    OTHER         = auto()   # any other character / operator
    STRING        = auto()   # "..." or R"tag(...)tag"
    CHARLIT       = auto()   # '...'
    LINE_COMMENT  = auto()   # // ...
    BLOCK_COMMENT = auto()   # /* ... */
    WHITESPACE    = auto()   # spaces / tabs (NOT newlines)
    NEWLINE       = auto()   # \n or \r\n
    EOF           = auto()


@dataclass
class Token:
    kind: TK
    text: str
    line: int
    col: int

    def is_const(self):    return self.kind == TK.WORD and self.text == "const"
    def is_volatile(self): return self.kind == TK.WORD and self.text == "volatile"
    def is_cv(self):       return self.is_const() or self.is_volatile()
    def is_trivial(self):
        return self.kind in (TK.WHITESPACE, TK.NEWLINE,
                             TK.LINE_COMMENT, TK.BLOCK_COMMENT)


# ---------------------------------------------------------------------------
# Lexer — hand-written, no regex
# ---------------------------------------------------------------------------

def lex(src: str) -> list:
    tokens = []
    i = 0
    n = len(src)
    line = 1
    line_start = 0

    def col():
        return i - line_start + 1

    def emit(kind, text, ln, cl):
        tokens.append(Token(kind, text, ln, cl))

    while i < n:
        c = src[i]
        cl = col()
        ln = line

        # Newline
        if c == '\r':
            nxt = src[i+1] if i+1 < n else ''
            text = '\r\n' if nxt == '\n' else '\r'
            emit(TK.NEWLINE, text, ln, cl)
            i += len(text)
            line += 1
            line_start = i
            continue

        if c == '\n':
            emit(TK.NEWLINE, '\n', ln, cl)
            i += 1
            line += 1
            line_start = i
            continue

        # Non-newline whitespace
        if c in (' ', '\t', '\f', '\v'):
            j = i + 1
            while j < n and src[j] in (' ', '\t', '\f', '\v'):
                j += 1
            emit(TK.WHITESPACE, src[i:j], ln, cl)
            i = j
            continue

        # Line comment
        if c == '/' and i+1 < n and src[i+1] == '/':
            j = i + 2
            while j < n and src[j] not in ('\n', '\r'):
                j += 1
            emit(TK.LINE_COMMENT, src[i:j], ln, cl)
            i = j
            continue

        # Block comment
        if c == '/' and i+1 < n and src[i+1] == '*':
            j = i + 2
            while j < n and not (src[j] == '*' and j+1 < n and src[j+1] == '/'):
                if src[j] == '\n':
                    line += 1; line_start = j + 1
                elif src[j] == '\r':
                    line += 1
                    if j+1 < n and src[j+1] == '\n':
                        j += 1
                    line_start = j + 1
                j += 1
            j += 2  # consume */
            emit(TK.BLOCK_COMMENT, src[i:j], ln, cl)
            i = j
            continue

        # Raw string literal: optional-prefix R"delim(...)delim"
        # Detect encoding prefix
        raw_prefix_end = i
        if i < n and src[raw_prefix_end] == 'u' and raw_prefix_end+1 < n and src[raw_prefix_end+1] == '8':
            raw_prefix_end += 2
        elif i < n and src[raw_prefix_end] in ('L', 'u', 'U'):
            raw_prefix_end += 1
        if (raw_prefix_end < n and src[raw_prefix_end] == 'R'
                and raw_prefix_end+1 < n and src[raw_prefix_end+1] == '"'):
            j = raw_prefix_end + 2
            delim_start = j
            while j < n and src[j] != '(':
                j += 1
            delimiter = src[delim_start:j]
            closing = ')' + delimiter + '"'
            j += 1  # past (
            close_idx = src.find(closing, j)
            end = (close_idx + len(closing)) if close_idx != -1 else n
            raw = src[i:end]
            for ch in raw:
                if ch == '\n':
                    line += 1
            emit(TK.STRING, raw, ln, cl)
            i = end
            continue

        # Encoding-prefixed string:  u8"..." u"..." U"..." L"..."
        if (c in ('u', 'U', 'L') and i+1 < n and src[i+1] == '"'):
            j = i + 2
            while j < n and src[j] != '"':
                if src[j] == '\\': j += 1
                j += 1
            j += 1
            emit(TK.STRING, src[i:j], ln, cl)
            i = j
            continue

        # Regular string literal
        if c == '"':
            j = i + 1
            while j < n and src[j] != '"':
                if src[j] == '\\': j += 1
                j += 1
            j += 1
            emit(TK.STRING, src[i:j], ln, cl)
            i = j
            continue

        # Char literal (with optional L/u/U prefix)
        if c == '\'' or (c in ('L', 'u', 'U') and i+1 < n and src[i+1] == '\''):
            j = i
            if src[j] != '\'': j += 1
            j += 1  # opening '
            while j < n and src[j] != '\'':
                if src[j] == '\\': j += 1
                j += 1
            j += 1
            emit(TK.CHARLIT, src[i:j], ln, cl)
            i = j
            continue

        # Identifier / keyword
        if c == '_' or c.isalpha():
            j = i + 1
            while j < n and (src[j] == '_' or src[j].isalnum()):
                j += 1
            emit(TK.WORD, src[i:j], ln, cl)
            i = j
            continue

        # Number (treat as OTHER — we don't need numeric values)
        if c.isdigit() or (c == '.' and i+1 < n and src[i+1].isdigit()):
            j = i + 1
            while j < n and (src[j].isalnum() or src[j] in ".xXbBoO_'eEpP+-"):
                j += 1
            emit(TK.OTHER, src[i:j], ln, cl)
            i = j
            continue

        # &&
        if c == '&' and i+1 < n and src[i+1] == '&':
            emit(TK.AMPAMP, '&&', ln, cl)
            i += 2
            continue

        # Single-char tokens
        kind_map = {
            '*': TK.STAR,     '&': TK.AMP,
            '[': TK.LBRACKET, ']': TK.RBRACKET,
            '(': TK.LPAREN,   ')': TK.RPAREN,
            '{': TK.LBRACE,   '}': TK.RBRACE,
            '<': TK.LANGLE,   '>': TK.RANGLE,
            ';': TK.SEMICOLON, ',': TK.COMMA, '=': TK.EQUALS,
        }
        emit(kind_map.get(c, TK.OTHER), c, ln, cl)
        i += 1

    emit(TK.EOF, '', line, col())
    return tokens


# ---------------------------------------------------------------------------
# Token-stream navigation helpers
# ---------------------------------------------------------------------------

def prev_nontrivial(tokens: list, pos: int):
    i = pos - 1
    while i >= 0:
        if not tokens[i].is_trivial():
            return i
        i -= 1
    return None


def next_nontrivial(tokens: list, pos: int):
    i = pos + 1
    while i < len(tokens):
        if not tokens[i].is_trivial():
            return i
        i += 1
    return None


def _is_type_token(t: Token, tokens: list, idx: int) -> bool:
    """Could this token be the rightmost token of a type expression?"""
    if t.kind == TK.WORD:
        return True
    if t.kind == TK.RANGLE:
        return True
    if t.kind == TK.RBRACKET:
        # A ']' that closes a C++ attribute '[[...]]' must not be treated as
        # a type token — it is followed by 'const' that qualifies the
        # declaration, not a type. Detect this by checking if the previous
        # non-trivial token is also ']'.
        prev = idx - 1
        while prev >= 0 and tokens[prev].is_trivial():
            prev -= 1
        if prev >= 0 and tokens[prev].kind == TK.RBRACKET:
            return False  # closing ']]' of an attribute
        return True
    return False


def _is_ptr_or_ref(t: Token) -> bool:
    return t.kind in (TK.STAR, TK.AMP, TK.AMPAMP)


def _find_type_start(tokens: list, rightmost_idx: int) -> int:
    """
    Given the index of the rightmost token of a type expression (e.g. the
    'int' in 'unsigned int', or '>' in 'vector<int>'), walk leftward to find
    the index of the FIRST token that is part of this type expression.

    Walks past: WORD, '::', '<...>' template args, cv-qualifiers, whitespace.
    Stops at: *, &, (, {, ;, =, commas, and other non-type tokens.
    """
    i = rightmost_idx
    angle_depth = 0

    while i >= 0:
        t = tokens[i]

        if t.is_trivial():
            i -= 1
            continue

        if t.kind == TK.RANGLE:
            angle_depth += 1
            i -= 1
            continue

        if t.kind == TK.LANGLE:
            if angle_depth > 0:
                angle_depth -= 1
                i -= 1
                continue
            else:
                break  # bare < — not a template bracket

        if angle_depth > 0:
            i -= 1  # inside template args — consume anything
            continue

        if t.kind == TK.WORD:
            i -= 1
            continue

        # :: (two consecutive OTHER ':' tokens)
        if t.kind == TK.OTHER and t.text == ':':
            prev = i - 1
            while prev >= 0 and tokens[prev].is_trivial():
                prev -= 1
            if prev >= 0 and tokens[prev].kind == TK.OTHER and tokens[prev].text == ':':
                i = prev - 1
                continue
            else:
                break

        break  # anything else stops the walk

    # i is now one step to the LEFT of the type start. Advance to first non-trivial.
    j = i + 1
    while j <= rightmost_idx and tokens[j].is_trivial():
        j += 1
    return j


# ---------------------------------------------------------------------------
# Core transformation: East const → West const
# ---------------------------------------------------------------------------
#
# For each `const` / `volatile` token:
#   1. Look at the previous non-trivial token.
#   2. If it is a type-like token (WORD, >, ]), this is East-const.
#      (If it's * or &, the qualifier applies to the pointer — leave it.)
#   3. Find the start of the type expression to the left.
#   4. Remove the cv token (+adjacent whitespace) and re-insert before type.
#
# Mutations are collected first, then applied right-to-left to keep indices stable.

def transform(tokens: list) -> list:
    moves = []  # list of (cv_index, type_start_index)

    for idx, tok in enumerate(tokens):
        if not tok.is_cv():
            continue

        prev_idx = prev_nontrivial(tokens, idx)
        if prev_idx is None:
            continue

        prev = tokens[prev_idx]

        # After * or & → qualifies the pointer itself → don't move
        if _is_ptr_or_ref(prev):
            continue

        # After a type-like token → East const → move it
        if not _is_type_token(prev, tokens, prev_idx):
            continue

        type_start = _find_type_start(tokens, prev_idx)

        # Guard: if the type expression starts with an asm keyword, this
        # `volatile` is an asm-statement modifier, not a type qualifier.
        # Moving it would produce a syntax error.
        if tokens[type_start].kind == TK.WORD and \
                tokens[type_start].text in ("asm", "__asm__", "__asm"):
            continue

        moves.append((idx, type_start))

    if not moves:
        return tokens

    # Apply right-to-left so earlier indices remain valid
    result = list(tokens)
    moves.sort(key=lambda m: m[0], reverse=True)

    for cv_idx, type_start in moves:
        cv_text = result[cv_idx].text
        cv_ln   = result[cv_idx].line
        cv_cl   = result[cv_idx].col

        # --- Remove cv token and the whitespace immediately to its LEFT ---
        # The whitespace between the type and const (e.g. "int [space] const")
        # becomes redundant once const moves to the front. We identify it by
        # walking left from cv_idx past any trivial tokens to find the type's
        # rightmost non-trivial token, then removing any whitespace that sits
        # strictly between that token and cv_idx.
        # We must NOT touch whitespace to the right of const (e.g. before *
        # or &) — that belongs to the surrounding expression.
        remove = {cv_idx}
        # Walk left from cv_idx to find the whitespace between type and const
        left = cv_idx - 1
        if left >= 0 and result[left].kind == TK.WHITESPACE:
            # Only remove it if the token before the whitespace is a non-trivial
            # type token (not a newline, not another whitespace run)
            before_ws = left - 1
            if before_ws >= 0 and not result[before_ws].is_trivial():
                remove.add(left)

        result = [t for i, t in enumerate(result) if i not in remove]

        # Adjust type_start for tokens removed before it
        removed_before = sum(1 for i in remove if i < type_start)
        type_start -= removed_before

        # --- Insert "cv " before the type start ---
        # Always insert [const, space]. Any resulting runs of whitespace are
        # squeezed to a single space by normalise_whitespace() afterwards.
        new_cv = Token(TK.WORD, cv_text, cv_ln, cv_cl)
        new_sp = Token(TK.WHITESPACE, ' ', cv_ln, cv_cl)
        result = result[:type_start] + [new_cv, new_sp] + result[type_start:]

    return result


# ---------------------------------------------------------------------------
# Render tokens back to source
# ---------------------------------------------------------------------------

def render(tokens: list) -> str:
    parts = []
    for t in tokens:
        if t.kind == TK.EOF:
            break
        parts.append(t.text)
    return ''.join(parts)


def normalise_whitespace(tokens: list) -> list:
    """
    Squeeze any run of whitespace tokens that falls between two non-whitespace,
    non-newline tokens down to a single space token.  Newlines are left alone so
    that indentation and blank lines are preserved.
    """
    result = []
    i = 0
    while i < len(tokens):
        t = tokens[i]
        if t.kind == TK.WHITESPACE:
            # Collect the whole run
            j = i
            while j < len(tokens) and tokens[j].kind == TK.WHITESPACE:
                j += 1
            # If this run is longer than one token, squeeze it
            if j - i > 1:
                result.append(Token(TK.WHITESPACE, ' ', t.line, t.col))
            else:
                result.append(t)
            i = j
        else:
            result.append(t)
            i += 1
    return result


def process_source(src: str) -> str:
    return render(normalise_whitespace(transform(lex(src))))


# ---------------------------------------------------------------------------
# Self-test
# ---------------------------------------------------------------------------

def run_tests():
    cases = [
        # (input, expected_output)

        # Basic East → West
        ("int const x = 0;",              "const int x = 0;"),
        ("char const c = 'a';",           "const char c = 'a';"),
        ("double const pi = 3.14;",       "const double pi = 3.14;"),

        # Multi-word types
        ("unsigned int const n;",         "const unsigned int n;"),
        ("unsigned long long const big;", "const unsigned long long big;"),
        ("long double const ld;",         "const long double ld;"),

        # Pointer-to-const (pointee is const → move)
        ("int const * p;",                "const int * p;"),
        ("char const * s;",               "const char * s;"),

        # Const pointer (pointer itself is const → do NOT move)
        ("int * const p;",                "int * const p;"),

        # Const pointer to const (both)
        ("int const * const p;",          "const int * const p;"),
        ("char const * const s;",         "const char * const s;"),

        # Namespace-qualified type
        ("std::string const s;",          "const std::string s;"),

        # Template type
        ("std::vector<int> const v;",     "const std::vector<int> v;"),

        # Already West const — should be unchanged
        ("const int x = 0;",             "const int x = 0;"),
        ("const int * p;",               "const int * p;"),
        ("int * const p;",               "int * const p;"),

        # volatile
        ("int volatile x;",              "volatile int x;"),
        ("int volatile * p;",            "volatile int * p;"),

        # Strings and comments must not be touched
        ('const char * msg = "int const x";',  'const char * msg = "int const x";'),
        ("int const x; // int const y",  "const int x; // int const y"),
        ("int const x; /* int const */", "const int x; /* int const */"),

        # Function parameter
        ("void f(int const n);",         "void f(const int n);"),
        ("void f(int const * p);",       "void f(const int * p);"),

        # No double space should appear after const
        ("int const x;",                 "const int x;"),
        ("unsigned long const n;",       "const unsigned long n;"),

        # Attributes: must not be treated as type tokens; space after ]] preserved
        ("void f([[maybe_unused]] std::string const& s) {}",
         "void f([[maybe_unused]] const std::string& s) {}"),
        ("[[nodiscard]] std::string const& get() const;",
         "[[nodiscard]] const std::string& get() const;"),
        # Already West const with attribute — must be left completely alone
        ("virtual void on_timer_event([[maybe_unused]] const std::string& name) {}",
         "virtual void on_timer_event([[maybe_unused]] const std::string& name) {}"),

        # asm volatile — must NOT be touched (volatile is not a type qualifier here)
        ("asm volatile (\"nop\");",       "asm volatile (\"nop\");"),
        ("__asm__ volatile (\"nop\");",   "__asm__ volatile (\"nop\");"),
        ("__asm volatile (\"nop\");",     "__asm volatile (\"nop\");"),
    ]

    passed = failed = 0
    for src, expected in cases:
        result = process_source(src)
        if result == expected:
            passed += 1
        else:
            failed += 1
            print(f"FAIL\n  in:  {src!r}\n  got: {result!r}\n  exp: {expected!r}")

    print(f"\n{passed} passed, {failed} failed out of {passed+failed} tests.")
    return failed == 0


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        prog="easterly",
        description=(
            "easterly — Convert East const C++ style to West const.\n"
            "Like an easterly wind, it blows code from east back to west.\n"
            "No regex used."
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("input", metavar="FILE",
                        help="Input C++ file (use - for stdin)")
    parser.add_argument("-o", "--output", metavar="FILE",
                        help="Output file (default: stdout)")
    parser.add_argument("-i", "--in-place", action="store_true",
                        help="Modify the file in place")
    parser.add_argument("--test", action="store_true",
                        help="Run built-in self-tests and exit")
    args = parser.parse_args()

    if args.test:
        ok = run_tests()
        sys.exit(0 if ok else 1)

    if args.input == "-":
        src = sys.stdin.read()
        filename = "<stdin>"
    else:
        with open(args.input, encoding="utf-8") as f:
            src = f.read()
        filename = args.input

    result = process_source(src)

    if args.in_place:
        if args.input == "-":
            parser.error("--in-place cannot be used with stdin")
        with open(args.input, "w", encoding="utf-8") as f:
            f.write(result)
        print(f"easterly: {filename} converted in place.", file=sys.stderr)
        return

    if args.output:
        with open(args.output, "w", encoding="utf-8") as f:
            f.write(result)
    else:
        sys.stdout.write(result)


if __name__ == "__main__":
    main()
