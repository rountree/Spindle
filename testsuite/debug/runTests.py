#!/usr/bin/env python3
"""
Spindle test runner with integrated debugging support.
"""

import argparse
import os
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
    parser.add_argument(
        '--spindle-debug',
        type=int,
        choices=[0, 1, 2, 3],
        help='Set SPINDLE_DEBUG level'
    )
    parser.add_argument(
        '--verbose',
        action='store_true',
        help='Print command and environment variables'
    )

    args = parser.parse_args()

    # For minimal implementation: single test --dependency --push
    test_cmd = '../run_driver --dependency --push'

    if args.dry_run:
        print(f"Running: {test_cmd}")
    else:
        env = os.environ.copy()
        if args.spindle_debug is not None:
            env['SPINDLE_DEBUG'] = str(args.spindle_debug)

        if args.verbose:
            print(f"Command: {test_cmd}")
            print("Environment variables:")
            for key in sorted(env.keys()):
                print(f"  {key}={env[key]}")
            print()

        result = subprocess.run(test_cmd, shell=True, env=env)

        if result.returncode == 0:
            print("ALL TESTS PASSED")
        else:
            print("SOME TESTS FAILED")

        sys.exit(result.returncode)


if __name__ == '__main__':
    main()
