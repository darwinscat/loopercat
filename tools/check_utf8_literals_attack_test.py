#!/usr/bin/env python3
# Copyright (c) 2026 Darwin's Cat. Part of LooperCat - see LICENSE.
# SPDX-License-Identifier: AGPL-3.0-or-later
r"""The UTF-8 literal lint, attacked (review of feat/history-row-menu).

The theory under test is the lint's own, as its docstring states it: every
way a narrow string literal can come to hold a byte above 0x7f is caught, and
every source file a compiler accepts can be checked - a lint that cannot read
a valid file fails the build for nothing. The sources are planted with escapes
so this file stays ASCII.
"""

import os
import subprocess
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True  # ctest runs this in the source tree: leave nothing behind
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_utf8_literals as lint  # noqa: E402

DASH = "\u2014"


def flagged(source):
    """The lines the lint flags, or a failure naming the reason it could not read the source."""
    try:
        return lint.violations(source)
    except SyntaxError as error:
        raise AssertionError(f"a valid source cannot be checked: {error}") from error


class Evasions(unittest.TestCase):
    """High bytes written in a way the compiler reads and the lint does not."""

    def test_an_escape_split_by_a_line_continuation(self):
        # Phase 2 of translation splices the lines before the escape is read:
        # the compiler sees U+2014, \xe2 and \342 and writes high bytes.
        for source in ('auto s = "\\u20\\\n14";\n',
                       'auto s = "\\x\\\ne2";\n',
                       'auto s = "\\3\\\n42";\n'):
            with self.subTest(source=source):
                self.assertEqual(flagged(source), [1])

    def test_a_continued_comment_hides_the_next_line_under_crlf_too(self):
        # A // comment ending in a backslash runs on to the next line, with
        # CRLF line endings exactly as with LF: the literal below is a comment.
        self.assertEqual(flagged('// a comment that goes on \\\n"' + DASH + '"\nint x;\n'), [])
        self.assertEqual(flagged('// a comment that goes on \\\r\n"' + DASH + '"\r\nint x;\r\n'), [])


class Checkable(unittest.TestCase):
    """Sources a compiler accepts that the lint must be able to read."""

    def test_crlf_with_a_continuation_inside_a_literal(self):
        self.assertEqual(flagged('auto s = "a \\\r\n b";\r\n'), [])
        self.assertEqual(flagged('auto s = "a \\\r\n ' + DASH + '";\r\n'), [1])

    def test_an_apostrophe_in_preprocessor_text(self):
        for source in ("#error Don't build this\nint x;\n",
                       "#warning the player's card\nint x;\n",
                       "#if 0\nthe card's folder\n#endif\n"):
            with self.subTest(source=source):
                self.assertEqual(flagged(source), [])

    def test_every_cxx_source_suffix_is_read(self):
        # The rule is about every narrow literal in app/, core/ and tests/,
        # whatever the file's suffix: a .cxx or .hh file with a bare dash
        # must be reported, not passed over.
        names = ("a.cxx", "b.hh", "c.hxx", "d.m")
        with tempfile.TemporaryDirectory() as root:
            for name in names:
                with open(os.path.join(root, name), "w", encoding="utf-8") as handle:
                    handle.write('auto s = "' + DASH + '";\n')
            result = subprocess.run([sys.executable, lint.__file__, root], capture_output=True, text=True)
        self.assertEqual(result.returncode, 1, result.stdout)
        for name in names:
            with self.subTest(name=name):
                self.assertIn(name + ":1:", result.stdout)


class StillCaught(unittest.TestCase):
    """Shapes that must stay caught, or stay allowed, under the same theory."""

    def test_raw_strings_with_odd_delimiters(self):
        self.assertEqual(flagged('auto s = R"a)b(x ' + DASH + ' y)a)b";\n'), [1])
        self.assertEqual(flagged('auto s = R"_x_(a )" b)" c ' + DASH + ')_x_";\n'), [1])
        self.assertEqual(flagged('auto s = R"(\\u2014 is text here, not an escape)";\n'), [])

    def test_wide_raw_strings_are_not_narrow(self):
        for prefix in ("u8R", "uR", "UR", "LR"):
            with self.subTest(prefix=prefix):
                self.assertEqual(flagged('auto s = ' + prefix + '"(' + DASH + ')";\n'), [])

    def test_a_user_defined_literal_suffix_keeps_the_literal_narrow(self):
        self.assertEqual(flagged('auto s = "' + DASH + '"sv;\n'), [1])
        self.assertEqual(flagged('auto s = "' + DASH + '"_s;\n'), [1])
        self.assertEqual(flagged('auto s = u8"' + DASH + '"sv;\n'), [])

    def test_the_sink_with_a_comment_or_a_continuation_before_the_literal(self):
        self.assertEqual(flagged('auto s = juce::String::fromUTF8(/* words */ "' + DASH + '");\n'), [])
        self.assertEqual(flagged('auto s = utf8( \\\n    "' + DASH + '" \\\n);\n'), [])

    def test_a_sink_that_is_only_named_like_one_is_not_trusted_by_a_cast(self):
        self.assertEqual(flagged('auto s = juce::String::fromUTF8((const char*) "' + DASH + '");\n'), [1])

    def test_quoted_include_lines_and_character_quotes(self):
        self.assertEqual(flagged('#include "Strings.h"\n#include <map>\nchar q = \'"\';\n'), [])
        self.assertEqual(flagged('#include "caf\\xc3\\xa9.h"\n'), [1])

    def test_a_digit_separator_before_a_literal_on_the_same_line(self):
        self.assertEqual(flagged('int n = 1\'000; auto s = "' + DASH + '";\n'), [1])
        self.assertEqual(flagged('int n = 0x1\'F; auto s = "fine";\n'), [])

    def test_the_marker_in_a_block_comment_that_starts_on_an_earlier_line(self):
        # The rule says the marker sits on the literal's first line.
        self.assertEqual(flagged('/* utf8-data: a FAT name\n */ auto s = "caf\\xc3\\xa9";\n'), [2])


if __name__ == "__main__":
    unittest.main()
