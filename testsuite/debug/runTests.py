#!/usr/bin/env python3
"""
Spindle test runner with integrated debugging support.
"""

import argparse
import os
import subprocess
import sys


def setup_environment():
    """Set up the environment variables needed for Spindle tests."""
    env = os.environ.copy()

    # Change to parent directory (testsuite)
    testsuite_dir = os.path.dirname(os.path.abspath(__file__))
    testsuite_dir = os.path.dirname(testsuite_dir)  # Go up from debug/ to testsuite/

    # Set up LD_LIBRARY_PATH
    if 'LD_LIBRARY_PATH' in env:
        env['LD_LIBRARY_PATH'] = f"{env['LD_LIBRARY_PATH']}:{testsuite_dir}"
    else:
        env['LD_LIBRARY_PATH'] = testsuite_dir

    # Set SPINDLE_TEST flag
    env['SPINDLE_TEST'] = '1'

    # Set PATH
    env['PATH'] = f"{env['PATH']}:."

    # Set SPINDLE_FLAGS
    env['SPINDLE_FLAGS'] = '--level=high'

    # Set SPINDLE_BLUEGENE and related
    env['SPINDLE_BLUEGENE'] = 'false'
    env['SPINDLE_BGQ_LD_PRELOAD'] = 'false'

    # Build LIBRARY_LIST
    libs = [
        'libcxxexceptA.so', 'libtest100.so', 'libtest2000.so', 'libtest500.so',
        'libtest1000.so', 'libtest4000.so', 'libtest6000.so', 'libdepA.so',
        'libtest10.so', 'libtest11.so', 'libtest12.so', 'libtest13.so',
        'libtest14.so', 'libtest15.so', 'libtest16.so', 'libtest17.so',
        'libtest18.so', 'libtest19.so', 'libtest20.so', 'libtest10000.so',
        'libtest50.so', 'libtest8000.so', 'origin_dir/liboriginlib.so'
    ]
    env['LIBRARY_LIST'] = ':'.join(f"{testsuite_dir}/{lib}" for lib in libs)

    # Determine SPINDLE path if not already set
    if 'SPINDLE' not in env:
        # Read SPINDLE_PREFIX from config.h
        # testsuite_dir is typically .../build/Spindle-XXX/testsuite
        build_dir = os.path.dirname(testsuite_dir)
        config_h_path = os.path.join(build_dir, 'config.h')

        if os.path.exists(config_h_path):
            prefix = None
            with open(config_h_path, 'r') as f:
                for line in f:
                    if line.startswith('#define SPINDLE_PREFIX '):
                        # Extract the quoted string
                        parts = line.split('"')
                        if len(parts) >= 2:
                            prefix = parts[1]
                            break

            if prefix:
                env['SPINDLE'] = f"{prefix}/bin/spindle"

    return env, testsuite_dir


def build_spindle_command(args, env, testsuite_dir):
    """Build the Spindle command based on arguments."""
    # Determine TEST_EXEC based on first argument (--dependency)
    test_exec = './test_driver_libs'

    # Set SPINDLE_OPTS based on second argument (--push)
    spindle_opts = '--push'
    env['SPINDLE_OPTS'] = spindle_opts

    # For serial launcher, we need to construct the full command
    # Get SPINDLE executable path from environment or construct it
    if 'SPINDLE' in env:
        spindle_exec = env['SPINDLE']
    else:
        # Try to find spindle in the install directory
        # This is a fallback, normally SPINDLE should be set
        spindle_exec = 'spindle'

    env['SPINDLE'] = spindle_exec
    env['TEST_EXEC'] = test_exec

    # Build the command based on run_driver_serial logic
    # Important: pass through the test arguments (--dependency --push) to test_exec
    test_args = '--dependency --push'
    spindle_flags = env['SPINDLE_FLAGS']

    if 'SPINDLE_LD_PRELOAD' in env and env['SPINDLE_LD_PRELOAD']:
        ld_preload = env['SPINDLE_LD_PRELOAD']
        cmd = f"{spindle_exec} {spindle_flags} {spindle_opts} --launcher=serial bash -c 'LD_PRELOAD={ld_preload} {test_exec} {test_args}'"
    else:
        cmd = f"{spindle_exec} {spindle_flags} {spindle_opts} --launcher=serial {test_exec} {test_args}"

    return cmd


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

    # Set up environment
    env, testsuite_dir = setup_environment()

    # Add SPINDLE_DEBUG if specified
    if args.spindle_debug is not None:
        env['SPINDLE_DEBUG'] = str(args.spindle_debug)

    # Build the Spindle command
    test_cmd = build_spindle_command(args, env, testsuite_dir)

    if args.dry_run:
        print(f"Running: {test_cmd}")
    else:
        if args.verbose:
            print(f"Command: {test_cmd}")
            print("Spindle environment variables:")
            interesting_vars = [
                'SPINDLE_DEBUG', 'SPINDLE_TEST', 'LD_LIBRARY_PATH', 'PATH',
                'SPINDLE', 'SPINDLE_LAUNCH_MODE', 'TEST_RM', 'SPINDLE_OPTS',
                'SPINDLE_FLAGS', 'TEST_EXEC', 'LIBRARY_LIST', 'SPINDLE_LD_PRELOAD',
                'SPINDLE_BGQ_LD_PRELOAD', 'SPINDLE_BLUEGENE', 'SESSION_ID',
                'STARTED_SPINDLE_SESSION', 'SPINDLEID'
            ]
            for key in sorted(interesting_vars):
                if key in env:
                    print(f"  {key}={env[key]}")
            print()

        # Print the "Running:" message like run_driver does
        print(f"Running: ./run_driver --dependency --push")

        # Change to testsuite directory to run
        result = subprocess.run(test_cmd, shell=True, env=env, cwd=testsuite_dir)

        if result.returncode == 0:
            print("ALL TESTS PASSED")
        else:
            print("SOME TESTS FAILED")

        sys.exit(result.returncode)


if __name__ == '__main__':
    main()
