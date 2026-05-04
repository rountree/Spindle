#!/usr/bin/env python3
"""
Spindle test runner with integrated debugging support.
"""

import argparse
import glob
import os
import shlex
import shutil
import subprocess
import sys
from datetime import datetime
from io import StringIO

# Try to import flux - it's only available in flux containers
try:
    import flux
    import flux.job
    FLUX_AVAILABLE = True
except ImportError:
    FLUX_AVAILABLE = False

# Define all available tests from the original runTests
ALL_TESTS = [
    ('dependency', 'push'),
    ('dlopen', 'push'),
    ('dlreopen', 'push'),
    ('thrdopen', 'push'),
    ('reorder', 'push'),
    ('partial', 'push'),
    ('ldpreload', 'push'),
    ('spindleapi', 'push'),
    ('dependency', 'pull'),
    ('dlopen', 'pull'),
    ('dlreopen', 'pull'),
    ('thrdopen', 'pull'),
    ('reorder', 'pull'),
    ('partial', 'pull'),
    ('ldpreload', 'pull'),
    ('spindleapi', 'pull'),
    ('dependency', 'numa'),
    ('dlopen', 'numa'),
    ('dlreopen', 'numa'),
    ('thrdopen', 'numa'),
    ('reorder', 'numa'),
    ('partial', 'numa'),
    ('ldpreload', 'numa'),
    ('spindleapi', 'numa'),
    ('dependency', 'fork'),
    ('dlopen', 'fork'),
    ('dlreopen', 'fork'),
    ('thrdopen', 'fork'),
    ('reorder', 'fork'),
    ('partial', 'fork'),
    ('ldpreload', 'fork'),
    ('spindleapi', 'fork'),
    ('dependency', 'forkexec'),
    ('dlopen', 'forkexec'),
    ('dlreopen', 'forkexec'),
    ('thrdopen', 'forkexec'),
    ('reorder', 'forkexec'),
    ('partial', 'forkexec'),
    ('ldpreload', 'forkexec'),
    ('spindleapi', 'forkexec'),
    ('dependency', 'chdir'),
    ('dlopen', 'chdir'),
    ('dlreopen', 'chdir'),
    ('thrdopen', 'chdir'),
    ('reorder', 'chdir'),
    ('partial', 'chdir'),
    ('ldpreload', 'chdir'),
    ('spindleapi', 'chdir'),
    ('dependency', 'preload'),
    ('dlopen', 'preload'),
    ('dlreopen', 'preload'),
    ('thrdopen', 'preload'),
    ('reorder', 'preload'),
    ('partial', 'preload'),
    ('ldpreload', 'preload'),
    ('spindleapi', 'preload'),
]


class TeeOutput:
    """Capture output to both stdout and a file."""
    def __init__(self, filepath, verbose=True):
        self.filepath = filepath
        self.file = open(filepath, 'w')
        self.verbose = verbose
        self.original_stdout = sys.stdout
        self.original_stderr = sys.stderr

    def write(self, data):
        self.file.write(data)
        if self.verbose:
            self.original_stdout.write(data)
        self.file.flush()

    def flush(self):
        self.file.flush()
        if self.verbose:
            self.original_stdout.flush()

    def close(self):
        self.file.close()


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


def run_serial_test(args, env, testsuite_dir, test_type='dependency', test_mode='push', log_dir=None):
    """Run test using serial resource manager.

    Args:
        args: Command line arguments
        env: Environment variables
        testsuite_dir: Path to testsuite directory
        test_type: Type of test (dependency, dlopen, etc.)
        test_mode: Mode of test (push, pull, numa, fork, etc.)
        log_dir: Directory to store logs (if None, use timestamp-based default)

    Returns:
        (returncode, log_directory)
    """
    # Determine TEST_EXEC based on test type
    test_exec = './test_driver_libs'

    # Set SPINDLE_OPTS based on mode
    spindle_opts = f'--{test_mode}'
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
    test_args = f'--{test_type} --{test_mode}'
    spindle_flags = env['SPINDLE_FLAGS']

    if 'SPINDLE_LD_PRELOAD' in env and env['SPINDLE_LD_PRELOAD']:
        ld_preload = env['SPINDLE_LD_PRELOAD']
        cmd = f"{spindle_exec} {spindle_flags} {spindle_opts} --launcher=serial bash -c 'LD_PRELOAD={ld_preload} {test_exec} {test_args}'"
    else:
        cmd = f"{spindle_exec} {spindle_flags} {spindle_opts} --launcher=serial {test_exec} {test_args}"

    if args.verbose:
        print(f"Command: {cmd}")

    # Change to testsuite directory to run
    result = subprocess.run(cmd, shell=True, env=env, cwd=testsuite_dir)

    # For serial tests, log handling is simpler - just note the directory
    if log_dir is None:
        timestamp = datetime.now().strftime('%Y%m%d_%H%M%S')
        log_dir = os.path.join(testsuite_dir, 'debug', f'{result.returncode}_{timestamp}')

    return (result.returncode, log_dir)


def run_flux_test(args, env, testsuite_dir, test_type='dependency', test_mode='push', log_dir=None):
    """Run test using Flux resource manager.

    Args:
        args: Command line arguments
        env: Environment variables
        testsuite_dir: Path to testsuite directory
        test_type: Type of test (dependency, dlopen, etc.)
        test_mode: Mode of test (push, pull, numa, fork, etc.)
        log_dir: Directory to store logs (if None, use timestamp-based default)

    Returns:
        (returncode, log_directory)
    """
    if not FLUX_AVAILABLE:
        print("ERROR: Flux Python module not available", file=sys.stderr)
        return (1, log_dir)

    # Query available resources for debugging
    if args.verbose:
        print("Querying Flux resources...")
        try:
            result = subprocess.run("flux resource list", shell=True, capture_output=True, text=True)
            print("Available resources:")
            print(result.stdout)
        except Exception as e:
            print(f"Could not query resources: {e}")

    # Determine TEST_EXEC based on test type
    test_exec = './test_driver_libs'
    test_args = [f'--{test_type}', f'--{test_mode}']

    # Get SPINDLE executable path
    if 'SPINDLE' in env:
        spindle_exec = env['SPINDLE']
    else:
        spindle_exec = 'spindle'

    env['SPINDLE'] = spindle_exec
    env['TEST_EXEC'] = test_exec
    env['SPINDLE_OPTS'] = f'--{test_mode}'

    # Build full command with spindle wrapper
    spindle_flags = env['SPINDLE_FLAGS']
    full_command = [spindle_exec] + spindle_flags.split() + [f'--{test_mode}', '--launcher=serial', test_exec] + test_args

    if args.verbose:
        print(f"Flux command: {' '.join(full_command)}")
        print(f"Nodes: {args.num_nodes}, Tasks: {args.num_tasks}, Cores per task: {args.cores_per_task}, Time limit: {args.time_limit}")

    try:
        handle = flux.Flux()

        # Use from_command() for a regular job, not from_nest_command()
        jobspec = flux.job.JobspecV1.from_command(
            command=full_command,
            num_tasks=args.num_tasks,
            num_nodes=args.num_nodes,
            cores_per_task=args.cores_per_task,
            duration=args.time_limit,
            cwd=testsuite_dir,
        )

        # Set environment variables
        jobspec.environment = dict(env)

        if args.verbose:
            print("Submitting job to Flux...")

        # Submit job with waitable=True so we can wait for it
        jobid = flux.job.submit(handle, jobspec, waitable=True)

        if args.verbose:
            print(f"Job submitted: {jobid}")
            print("Waiting for job to complete...")

        # wait() returns JobWaitResult(jobid, success, errstr)
        result = flux.job.wait(handle, jobid)

        if args.verbose:
            print(f"Job result: {result}")

        returncode = 0 if result.success else 1

        # Collect logs from all nodes to shared filesystem
        if args.verbose:
            print("Collecting logs from all nodes to shared filesystem...")

        # Use shared filesystem for log aggregation
        shared_logs = '/shared-logs'
        if log_dir is None:
            timestamp = datetime.now().strftime('%Y%m%d_%H%M%S')
            target_base = os.path.join(shared_logs, f'{returncode}_{timestamp}')
        else:
            target_base = log_dir

        # Create directory structure on shared filesystem (only need to do this once)
        try:
            os.makedirs(target_base, exist_ok=True)
            for i in range(1, args.num_nodes + 1):
                os.makedirs(os.path.join(target_base, f'node-{i}'), exist_ok=True)
        except Exception as e:
            print(f"Warning: Could not create shared log directories: {e}", file=sys.stderr)
            return returncode

        # Launch collection job: one task per node to copy files
        collection_cmd = f"""
hostname=$(hostname)
node_num=${{hostname##*-}}
target_dir="{target_base}/node-${{node_num}}"
cp {testsuite_dir}/spindle_output.* $target_dir/ 2>/dev/null || \\
    echo "Warning: Could not copy Spindle logs from $(hostname)" >&2
flux dmesg > $target_dir/flux-dmesg.log 2>&1 || \\
    echo "Warning: Could not capture flux dmesg from $(hostname)" >&2
"""

        try:
            # Run collection on all nodes (1 task per node)
            collect_result = subprocess.run(
                f"flux run -N {args.num_nodes} -n {args.num_nodes} bash -c {shlex.quote(collection_cmd)}",
                shell=True,
                env=env,
                cwd=testsuite_dir,
                capture_output=True,
                text=True
            )

            if args.verbose:
                if collect_result.returncode == 0:
                    print("Log collection completed successfully")
                else:
                    print(f"Log collection warnings/errors: {collect_result.stderr}")
        except Exception as e:
            print(f"Warning: Log collection failed: {e}", file=sys.stderr)
            # Don't fail the whole test just because collection failed

        return (returncode, target_base)

    except Exception as e:
        print(f"ERROR running Flux job: {e}", file=sys.stderr)
        import traceback
        traceback.print_exc()
        return (1, log_dir)


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
    parser.add_argument(
        '--preserve-logs-on-success',
        action='store_true',
        help='Keep log directories from successful test runs (default: delete them)'
    )
    parser.add_argument(
        '--continue-after-failure',
        action='store_true',
        help='Continue running tests after a failure (default: stop at first failure)'
    )
    parser.add_argument(
        '--resource-manager',
        choices=['serial', 'flux'],
        default='serial',
        help='Resource manager to use (serial, flux)'
    )
    parser.add_argument(
        '--num-nodes',
        type=int,
        default=1,
        help='Number of nodes to allocate (flux)'
    )
    parser.add_argument(
        '--num-tasks',
        type=int,
        default=1,
        help='Total number of tasks to run (flux)'
    )
    parser.add_argument(
        '--cores-per-task',
        type=int,
        default=1,
        help='Cores per task (flux)'
    )
    parser.add_argument(
        '--time-limit',
        default='20s',
        help='Time limit for job, e.g., "5m", "30s" (flux). Accepts Flux duration format.'
    )
    parser.add_argument(
        '--run-all-tests',
        action='store_true',
        help='Run all tests from the original runTests script'
    )

    args = parser.parse_args()

    # Validate resource manager availability
    if args.resource_manager == 'flux' and not FLUX_AVAILABLE:
        parser.error("--resource-manager=flux requires Flux Python module, which is not available")

    # Set up environment
    env, testsuite_dir = setup_environment()

    # Add SPINDLE_DEBUG if specified
    if args.spindle_debug is not None:
        env['SPINDLE_DEBUG'] = str(args.spindle_debug)

    if args.dry_run:
        print(f"Resource manager: {args.resource_manager}")
        if args.resource_manager == 'flux':
            print(f"Nodes: {args.num_nodes}, Tasks: {args.num_tasks}, Cores per task: {args.cores_per_task}, Time limit: {args.time_limit}")
        print(f"Running: ./run_driver --dependency --push")
    else:
        if args.verbose:
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

        # Determine which tests to run
        if args.run_all_tests:
            tests_to_run = ALL_TESTS
        else:
            # Default: just run dependency/push
            tests_to_run = [('dependency', 'push')]

        # Run each test
        global_result = 0
        for test_type, test_mode in tests_to_run:
            test_name = f"{test_type}_{test_mode}"

            # Create directories
            debug_dir = os.path.join(testsuite_dir, 'debug')
            timestamp = datetime.now().strftime('%Y%m%d_%H%M%S')

            # For Flux, use shared-logs; for serial, use local debug dir
            if args.resource_manager == 'flux':
                shared_logs = '/shared-logs'
                temp_dir = os.path.join(shared_logs, f'temp_{test_name}_{timestamp}')
            else:
                temp_dir = os.path.join(debug_dir, f'temp_{test_name}_{timestamp}')

            os.makedirs(temp_dir, exist_ok=True)

            # Set up log capture
            runtest_log_path = os.path.join(temp_dir, 'runtest.log')
            log_capture = TeeOutput(runtest_log_path, verbose=args.verbose)
            old_stdout = sys.stdout
            old_stderr = sys.stderr
            sys.stdout = log_capture
            sys.stderr = log_capture

            try:
                # Print the "Running:" message like run_driver does
                print(f"Running: ./run_driver --{test_type} --{test_mode}")

                # Run test with appropriate resource manager
                if args.resource_manager == 'serial':
                    returncode, log_dir = run_serial_test(args, env, testsuite_dir, test_type, test_mode, temp_dir)
                elif args.resource_manager == 'flux':
                    returncode, log_dir = run_flux_test(args, env, testsuite_dir, test_type, test_mode, temp_dir)
                else:
                    print(f"ERROR: Unknown resource manager: {args.resource_manager}")
                    returncode = 1
                    log_dir = temp_dir

                if returncode != 0:
                    global_result = -1

            finally:
                # Restore stdout/stderr
                sys.stdout = old_stdout
                sys.stderr = old_stderr
                log_capture.close()

            # Rename directory to include return code
            final_dir = os.path.join(os.path.dirname(temp_dir), f'{returncode}_{test_name}_{timestamp}')
            if os.path.exists(temp_dir):
                os.rename(temp_dir, final_dir)
            elif log_dir != temp_dir:
                # Flux may have created the dir directly
                final_dir = log_dir

            # Move spindle_output files if they exist (serial only)
            if args.resource_manager == 'serial':
                spindle_outputs = glob.glob(os.path.join(testsuite_dir, 'spindle_output*'))
                if spindle_outputs:
                    for output_file in spindle_outputs:
                        filename = os.path.basename(output_file)
                        dest = os.path.join(final_dir, filename)
                        if os.path.exists(output_file):
                            os.rename(output_file, dest)

            # Handle log preservation based on success/failure
            if returncode == 0:
                if args.verbose:
                    print(f"Test {test_name} PASSED")
                # Delete logs for successful runs unless explicitly preserving
                if not args.preserve_logs_on_success and os.path.exists(final_dir):
                    shutil.rmtree(final_dir)
            else:
                print(f"Test {test_name} FAILED")
                # Stop at first failure unless explicitly continuing
                if not args.continue_after_failure:
                    sys.exit(returncode)

        # Print final status
        if global_result == 0:
            print("ALL TESTS PASSED")
        else:
            print("SOME TESTS FAILED")

        sys.exit(global_result)


if __name__ == '__main__':
    main()
