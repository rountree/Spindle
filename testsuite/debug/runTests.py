#!/usr/bin/env python3
"""
Spindle test runner with integrated debugging support.
"""

import argparse
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(
        description='Run Spindle tests with optional debugging features'
    )
    parser.add_argument(
        '--dry-run',
        action='store_true',
        help='Print commands that would be run without executing them'
    )

    args = parser.parse_args()

    # For minimal implementation: single test --dependency --push
    test_cmd = '../run_driver --dependency --push'

    if args.dry_run:
        print(f"Running: {test_cmd}")
    else:
        print(f"Running: {test_cmd}")
        result = subprocess.run(test_cmd, shell=True)

        if result.returncode == 0:
            print("PASSED.")
            print("ALL TESTS PASSED")
        else:
            print(f"FAILED with return code {result.returncode}")
            print("SOME TESTS FAILED")

        sys.exit(result.returncode)


if __name__ == '__main__':
    main()
