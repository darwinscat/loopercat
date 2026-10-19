#!/usr/bin/env python3
# Copyright (c) 2026 Darwin's Cat. Part of LooperCat - see LICENSE.
# SPDX-License-Identifier: AGPL-3.0-or-later
r"""Every non-ASCII string literal says it is UTF-8.

JUCE reads a bare `const char*` as ASCII: juce::String("a \xe2\x80\x94 b"),
or such a literal on the left of `+`, turns the dash into three Latin-1
characters - the mojibake operations.log carried in 0.9.6 previews. Whether a
literal ends up there depends on types this script cannot see, so the rule is
one any reader can check by eye: a narrow string literal that holds a byte
above 0x7f - written raw, or as a \x, octal or \u escape - is the direct
argument of fromUTF8(...) or utf8(...). Text that never crosses into JUCE is
written in ASCII instead. Bytes that are data, not words (a FAT name with an
accent, a malformed sequence a parser must refuse), are allowed by a comment
on the literal's first line: `// utf8-data: <why>`.

Wide (L), UTF-16/32 (u, U) and u8 literals are not narrow bytes and are not
checked. Comments are free text.

Usage: check_utf8_literals.py <dir-or-file>...   Prints file:line for each
literal that breaks the rule and exits 1; exits 0 when there is none.
"""

import os
import sys

SOURCE_SUFFIXES = (".h", ".hh", ".hpp", ".hxx", ".c", ".cc", ".cpp", ".cxx", ".m", ".mm", ".inl", ".ipp")
UTF8_SINKS = ("fromUTF8", "utf8")
DATA_MARKER = "utf8-data:"
HEX = set("0123456789abcdefABCDEF")
OCTAL = set("01234567")
NARROW_PREFIXES = ("", "R")
ALL_PREFIXES = ("u8", "u", "U", "L")


class Token:
    __slots__ = ("kind", "value", "line", "narrow", "wide_bytes")

    def __init__(self, kind, value, line, narrow=False, wide_bytes=False):
        self.kind = kind  # "id", "str", "punct", "other"
        self.value = value
        self.line = line
        self.narrow = narrow
        self.wide_bytes = wide_bytes  # holds a byte above 0x7f


def _escape_is_high(text, k):
    """`text[k]` is the character after a backslash. Returns (high, next)."""
    e = text[k]
    if e == "x":
        m = k + 1
        while m < len(text) and text[m] in HEX:
            m += 1
        digits = text[k + 1:m]
        return (bool(digits) and int(digits, 16) > 0x7F), m
    if e in OCTAL:
        m = k
        while m < len(text) and m < k + 3 and text[m] in OCTAL:
            m += 1
        return int(text[k:m], 8) > 0x7F, m
    if e in "uU":
        width = 4 if e == "u" else 8
        digits = text[k + 1:k + 1 + width]
        high = len(digits) == width and all(c in HEX for c in digits) and int(digits, 16) > 0x7F
        return high, k + 1 + width
    if e == "N" and k + 1 < len(text) and text[k + 1] == "{":
        end = text.find("}", k)
        return True, (len(text) if end < 0 else end + 1)  # a named character is never ASCII here
    return False, k + 1


def splice(text):
    """Translation phases 1 and 2, as the compiler runs them before any token
    is read: CRLF and CR end a line like LF, and a backslash at the end of a
    line joins it to the next. Returns the joined text and, for each of its
    characters, the source line it came from."""
    text = text.replace("\r\n", "\n").replace("\r", "\n")
    out, lines = [], []
    line, i, n = 1, 0, len(text)
    while i < n:
        if text[i] == "\\" and i + 1 < n and text[i + 1] == "\n":
            line += 1
            i += 2
            continue
        out.append(text[i])
        lines.append(line)
        if text[i] == "\n":
            line += 1
        i += 1
    lines.append(line)  # the end of the text has a line too
    return "".join(out), lines


def tokenize(source):
    """Returns (tokens, marker_lines): the tokens that matter, and the lines
    whose comments carry the data marker with a reason."""
    text, line_of = splice(source)
    tokens = []
    markers = set()
    i, n = 0, len(text)

    def comment(body, at):
        found = body.find(DATA_MARKER)
        if found >= 0 and body[found + len(DATA_MARKER):].strip():
            markers.add(line_of[at])

    while i < n:
        c = text[i]
        if c in " \t\n\f\v":
            i += 1
            continue
        if text.startswith("//", i):
            end = text.find("\n", i)
            end = n if end < 0 else end
            comment(text[i:end], i)
            i = end
            continue
        if text.startswith("/*", i):
            end = text.find("*/", i + 2)
            end = n if end < 0 else end + 2
            comment(text[i:end], i)
            i = end
            continue
        prefix = ""
        if c.isalpha() or c == "_":
            j = i
            while j < n and (text[j].isalnum() or text[j] == "_"):
                j += 1
            word = text[i:j]
            raw_prefixes = ("R", "u8R", "uR", "UR", "LR")
            if j < n and text[j] == '"' and word in raw_prefixes:
                close_paren = text.find("(", j)
                if close_paren < 0:
                    raise SyntaxError(f"line {line_of[i]}: a raw string with no opening parenthesis")
                delimiter = text[j + 1:close_paren]
                end = text.find(")" + delimiter + '"', close_paren)
                if end < 0:
                    raise SyntaxError(f"line {line_of[i]}: a raw string that never ends")
                body = text[close_paren + 1:end]
                tokens.append(Token("str", body, line_of[i], narrow=(word == "R"),
                                    wide_bytes=any(ord(ch) > 0x7F for ch in body)))
                i = end + len(delimiter) + 2
                continue
            if j < n and text[j] in "\"'" and word in ALL_PREFIXES:
                prefix = word
                i = j
            else:
                tokens.append(Token("id", word, line_of[i]))
                i = j
                continue
        c = text[i]
        if c.isdigit() or (c == "." and i + 1 < n and text[i + 1].isdigit()):
            j = i + 1
            while j < n:
                if text[j] in "eEpP" and j + 1 < n and text[j + 1] in "+-":
                    j += 2
                elif text[j].isalnum() or text[j] in "._":
                    j += 1
                elif text[j] == "'" and j + 1 < n and text[j + 1].isalnum():
                    j += 2  # a digit separator, not a character literal
                else:
                    break
            tokens.append(Token("other", text[i:j], line_of[i]))
            i = j
            continue
        if c in "\"'":
            quote, j, high = c, i + 1, False
            while j < n and text[j] != quote and text[j] != "\n":
                if text[j] == "\\" and j + 1 < n:
                    escaped_high, j = _escape_is_high(text, j + 1)
                    high = high or escaped_high
                    continue
                if ord(text[j]) > 0x7F:
                    high = True
                j += 1
            if j >= n or text[j] != quote:
                # No closing quote on its line: not a literal the compiler
                # reads - an apostrophe in #error text, prose under #if 0.
                tokens.append(Token("punct", c, line_of[i]))
                i += 1
                continue
            if quote == '"':
                tokens.append(Token("str", text[i + 1:j], line_of[i],
                                    narrow=prefix in NARROW_PREFIXES, wide_bytes=high))
            else:
                tokens.append(Token("other", text[i:j + 1], line_of[i]))
            i = j + 1
            continue
        tokens.append(Token("punct", c, line_of[i]))
        i += 1
    return tokens, markers


def violations(text):
    """Lines of the literals that break the rule, in order."""
    tokens, markers = tokenize(text)
    out = []
    k = 0
    while k < len(tokens):
        if tokens[k].kind != "str":
            k += 1
            continue
        end = k
        while end < len(tokens) and tokens[end].kind == "str":
            end += 1
        run = tokens[k:end]
        if any(t.narrow and t.wide_bytes for t in run):
            direct = (k >= 2 and tokens[k - 1].kind == "punct" and tokens[k - 1].value == "("
                      and tokens[k - 2].kind == "id" and tokens[k - 2].value in UTF8_SINKS
                      and end < len(tokens) and tokens[end].kind == "punct"
                      and tokens[end].value in ("),"))
            if not direct and run[0].line not in markers:
                out.append(run[0].line)
        k = end
    return out


def sources(paths):
    for path in paths:
        if os.path.isfile(path):
            yield path
            continue
        if not os.path.isdir(path):
            raise FileNotFoundError(f"{path}: no such file or directory")
        for directory, subdirs, files in os.walk(path):
            subdirs.sort()
            for name in sorted(files):
                if name.endswith(SOURCE_SUFFIXES):
                    yield os.path.join(directory, name)


def main(argv):
    if len(argv) < 2:
        print("usage: check_utf8_literals.py <dir-or-file>...", file=sys.stderr)
        return 2
    found = 0
    for path in sources(argv[1:]):
        with open(path, "rb") as handle:
            raw = handle.read()
        try:
            text = raw.decode("utf-8")
            lines = violations(text)
        except (UnicodeDecodeError, SyntaxError) as error:
            print(f"{path}: cannot be checked: {error}")
            found += 1
            continue
        for line in lines:
            print(f"{path}:{line}: a non-ASCII string literal outside fromUTF8(...) or utf8(...)")
            found += 1
    if found:
        print(f"{found} literal(s) would reach JUCE as Latin-1; write them through fromUTF8/utf8, "
              f"in ASCII, or mark data with '// {DATA_MARKER} <why>'")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
