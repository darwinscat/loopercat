#!/usr/bin/env python3
# Copyright (c) 2026 Darwin's Cat. Part of LooperCat - see LICENSE.
# SPDX-License-Identifier: AGPL-3.0-or-later
r"""The UTF-8 literal lint, tried on planted sources.

The theory: a narrow string literal holding a byte above 0x7f reaches JUCE as
Latin-1 unless it is the direct argument of fromUTF8(...) or utf8(...). Every
way such a byte can be written must be caught; everything that is not such a
literal - comments, wide literals, characters, digit separators - must not
be. Sources here are written with escapes so this file stays ASCII.
"""

import os
import subprocess
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True  # ctest runs this in the source tree: leave nothing behind
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_utf8_literals as lint  # noqa: E402

DASH = "\u2014"  # what a C++ source holds when someone types an em dash


def flagged(source):
    return lint.violations(source)


class Caught(unittest.TestCase):
    def test_a_bare_dash(self):
        self.assertEqual(flagged('auto s = "a ' + DASH + ' b";\n'), [1])

    def test_the_mojibake_shape_left_of_a_juce_string(self):
        self.assertEqual(flagged('trace("connect: refused ' + DASH + ' " + utf8(reason));\n'), [1])

    def test_on_the_right_of_plus_too(self):
        # safe at run time for both string types, and still not allowed: one rule, no exceptions
        self.assertEqual(flagged('auto s = name + " ' + DASH + ' only mono";\n'), [1])

    def test_every_escape_that_makes_a_high_byte(self):
        for literal in (r'"\xe2\x80\x94"', r'"\342\200\224"', r'"\u2014"', r'"\U0001F431"', r'"\xff"',
                        r'"\x80"', r'"\200"', r'"\N{EM DASH}"'):
            with self.subTest(literal=literal):
                self.assertEqual(flagged("auto s = " + literal + ";\n"), [1])

    def test_a_raw_string(self):
        self.assertEqual(flagged('auto s = R"(a ' + DASH + ' b)";\n'), [1])
        self.assertEqual(flagged('auto s = R"x(a )" ' + DASH + ' b)x";\n'), [1])

    def test_a_literal_inside_fromutf8_but_not_its_direct_argument(self):
        self.assertEqual(flagged('auto s = juce::String::fromUTF8(on ? "a ' + DASH + '" : "b");\n'), [1])
        self.assertEqual(flagged('auto s = utf8(std::string("' + DASH + '"));\n'), [1])
        self.assertEqual(flagged('auto s = toUTF8("' + DASH + '");\n'), [1])
        self.assertEqual(flagged('auto s = myutf8("' + DASH + '");\n'), [1])

    def test_a_concatenation_counts_as_one_literal(self):
        self.assertEqual(flagged('auto s = "plain " "\\xe2\\x80\\x94";\n'), [1])
        self.assertEqual(flagged('auto s = "plain "\n    "' + DASH + '";\n'), [1])  # reported where it starts

    def test_the_line_is_the_literals_own(self):
        source = ('// a comment\n'
                  'auto a = R"(one\ntwo\nthree)";\n'
                  'auto b = "fine";\n'
                  'auto c = "' + DASH + '";\n')
        self.assertEqual(flagged(source), [6])  # after a raw string that took three lines

    def test_a_marker_needs_a_reason(self):
        self.assertEqual(flagged('auto s = "caf\\xc3\\xa9"; // utf8-data:\n'), [1])
        self.assertEqual(flagged('auto s = "caf\\xc3\\xa9"; // utf8-data:   \n'), [1])

    def test_a_marker_on_another_line_does_not_count(self):
        self.assertEqual(flagged('// utf8-data: a FAT name\nauto s = "caf\\xc3\\xa9";\n'), [2])


class Allowed(unittest.TestCase):
    def test_the_two_sinks(self):
        for source in ('auto s = juce::String::fromUTF8("a ' + DASH + ' b");\n',
                       'auto s = juce::String::fromUTF8 ( "a \\xe2\\x80\\x94 b" );\n',
                       'auto s = juce::String::fromUTF8("\\xe2\\x80\\x94", 3);\n',
                       'auto s = utf8("' + DASH + '");\n',
                       'auto s = juce::String::fromUTF8("a "\n    "' + DASH + '");\n'):
            with self.subTest(source=source):
                self.assertEqual(flagged(source), [])

    def test_ascii_anywhere(self):
        self.assertEqual(flagged('auto s = "a - b -> c ... \\"q\\" \\x7f \\177 \\u007F";\n'), [])

    def test_comments_are_free_text(self):
        self.assertEqual(flagged('// a ' + DASH + ' b "' + DASH + '"\n/* "' + DASH + '" */ int x;\n'), [])

    def test_wide_and_character_literals(self):
        for source in ('auto s = L"' + DASH + '";\n', 'auto s = u"' + DASH + '";\n',
                       'auto s = U"' + DASH + '";\n', 'auto s = u8"' + DASH + '";\n',
                       "char c = '\\xe2';\n", 'auto s = LR"(' + DASH + ')";\n'):
            with self.subTest(source=source):
                self.assertEqual(flagged(source), [])

    def test_quotes_that_are_not_strings(self):
        source = ("int big = 1'000'000; char q = '\"'; char s = '\\'';\n"
                  'auto t = "it\'s \\"fine\\"";\n')
        self.assertEqual(flagged(source), [])

    def test_data_with_its_reason(self):
        self.assertEqual(flagged('auto s = "caf\\xc3\\xa9"; // utf8-data: a FAT name with an accent\n'), [])
        self.assertEqual(flagged('check("caf\\xc3\\xa9", /* utf8-data: bytes the parser refuses */ 4);\n'), [])


class Command(unittest.TestCase):
    """The script as ctest runs it: a planted file fails, a clean one passes."""

    def run_on(self, files):
        with tempfile.TemporaryDirectory() as root:
            for name, text in files.items():
                path = os.path.join(root, name)
                os.makedirs(os.path.dirname(path), exist_ok=True)
                with open(path, "w", encoding="utf-8") as handle:
                    handle.write(text)
            return subprocess.run([sys.executable, lint.__file__, root], capture_output=True, text=True)

    def test_a_planted_dash_fails_with_file_and_line(self):
        result = self.run_on({"app/clean.cpp": 'auto a = "ascii";\n',
                              "app/planted.cpp": '\nauto s = "a ' + DASH + ' b";\n'})
        self.assertEqual(result.returncode, 1)
        self.assertIn(os.path.join("app", "planted.cpp") + ":2:", result.stdout)
        self.assertNotIn("clean.cpp", result.stdout)

    def test_a_clean_tree_passes(self):
        result = self.run_on({"core/a.hpp": 'auto a = "a - b";\n',
                              "app/b.cpp": 'auto b = juce::String::fromUTF8("' + DASH + '");\n',
                              "notes.txt": '"' + DASH + '" is not a source file\n'})
        self.assertEqual(result.returncode, 0, result.stdout)

    def test_a_file_that_is_not_utf8_cannot_pass(self):
        with tempfile.TemporaryDirectory() as root:
            with open(os.path.join(root, "latin1.cpp"), "wb") as handle:
                handle.write(b'auto s = "caf\xe9";\n')
            result = subprocess.run([sys.executable, lint.__file__, root], capture_output=True, text=True)
        self.assertEqual(result.returncode, 1)
        self.assertIn("cannot be checked", result.stdout)

    def test_a_missing_path_is_an_error_not_a_pass(self):
        result = subprocess.run([sys.executable, lint.__file__, "/no/such/loopercat/dir"],
                                capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)

    def test_no_path_is_an_error_not_a_pass(self):
        result = subprocess.run([sys.executable, lint.__file__], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main()
