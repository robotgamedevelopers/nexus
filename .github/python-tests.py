#!/usr/bin/env python3
"""Run one start directory's Python unittests for one tier, and fail on anything that is not a pass.

    python .github/python-tests.py --tier pr|native --start <dir> [--list .github/python-native-tests.txt]

The same rules as .github/ctest.sh: a skipped test fails the run, and so does an empty run, an expected failure, an
unexpected success, a module that does not import, and a row in the list that names a test which does not exist.
The list (.github/python-native-tests.txt) names the tests of the native tier; the pr tier is every other test of
the start directory, so a test lives in exactly one tier.
"""

import argparse
import os
import sys
import unittest


def flatten(suite):
    for item in suite:
        if isinstance(item, unittest.TestSuite):
            yield from flatten(item)
        else:
            yield item


def read_list(path, start):
    rows = set()
    with open(path, encoding="utf-8") as f:
        for number, line in enumerate(f, 1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            fields = line.split()
            if len(fields) != 2:
                sys.exit(f"python-tests.py: {path}:{number}: expected '<start directory> <unittest id>', got: {line}")
            if os.path.normpath(fields[0]) == start:
                rows.add(fields[1])
    return rows


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--tier", choices=["pr", "native"], required=True)
    parser.add_argument("--start", required=True)
    parser.add_argument("--list", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "python-native-tests.txt"))
    args = parser.parse_args()
    start = os.path.normpath(args.start)

    native = read_list(args.list, start)
    tests = list(flatten(unittest.TestLoader().discover(start)))
    ids = {t.id() for t in tests}
    unknown = sorted(native - ids)
    if unknown:
        for name in unknown:
            print(f"python-tests.py: {args.list} names {start} {name}, which is not a test there", file=sys.stderr)
        return 1
    picked = [t for t in tests if (t.id() in native) == (args.tier == "native")]
    if not picked:
        print(f"python-tests.py: no {args.tier} tests in {start}", file=sys.stderr)
        return 1

    result = unittest.TextTestRunner(verbosity=2).run(unittest.TestSuite(picked))
    bad = len(result.failures) + len(result.errors) + len(result.skipped)
    bad += len(result.expectedFailures) + len(result.unexpectedSuccesses)
    for test, reason in result.skipped:
        print(f"python-tests.py: SKIPPED {test.id()}: {reason}", file=sys.stderr)
    if result.testsRun != len(picked) or bad:
        print(f"python-tests.py: {start} {args.tier}: ran {result.testsRun} of {len(picked)}, "
              f"{len(result.failures)} failed, {len(result.errors)} errors, {len(result.skipped)} skipped, "
              f"{len(result.expectedFailures)} expected failures, {len(result.unexpectedSuccesses)} unexpected successes",
              file=sys.stderr)
        return 1
    print(f"python-tests.py: {start} {args.tier}: {result.testsRun} passed, none skipped")
    return 0


if __name__ == "__main__":
    sys.exit(main())
