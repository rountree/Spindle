#!/usr/bin/env python3
"""
Spindle test runner with integrated debugging support.
"""

import argparse
import fnmatch
import glob
import os
import shlex
import shutil
import subprocess
import sys
import time
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

# Serial-exec tests (use --serial launcher with specific executables)
SERIAL_TESTS = [
    './spindle_exec_test',
    './symbind_test',
    './interpreter_test',
]

# Session tests - use same test types as ALL_TESTS but with --session flag
SESSION_TEST_TYPES = [
    'dependency',
    'dlopen',
    'dlreopen',
    'thrdopen',
    'reorder',
    'partial',
    'ldpreload',
    'spindleapi',
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
        self.file.flush()  # Flush file immediately for real-time logging
        if self.verbose:
            self.original_stdout.write(data)
            self.original_stdout.flush()  # Flush stdout immediately for real-time output

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

    # Determine which modes are Spindle options vs test_driver options
    # Spindle options: push, pull, numa, preload
    # Test-only modes: fork, forkexec, chdir
    spindle_modes = ['push', 'pull', 'numa', 'preload']
    if test_mode in spindle_modes:
        # Special case: preload mode needs --preload=preload_file_list
        if test_mode == 'preload':
            spindle_opts = '--preload=preload_file_list'
            env['SPINDLE_OPTS'] = spindle_opts
        else:
            spindle_opts = f'--{test_mode}'
            env['SPINDLE_OPTS'] = spindle_opts
    else:
        spindle_opts = ''
        env['SPINDLE_OPTS'] = ''

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

    # For ldpreload/preload tests, set LD_PRELOAD to LIBRARY_LIST
    if test_type == 'ldpreload' or test_mode == 'preload':
        if 'LIBRARY_LIST' in env:
            env['LD_PRELOAD'] = env['LIBRARY_LIST']

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


def run_serial_exec_test(args, env, testsuite_dir, test_executable, log_dir=None):
    """Run a serial-exec test (spindle_exec_test, symbind_test, interpreter_test).

    Args:
        args: Command line arguments
        env: Environment variables
        testsuite_dir: Path to testsuite directory
        test_executable: Path to test executable (e.g., './spindle_exec_test')
        log_dir: Directory to store logs (if None, use timestamp-based default)

    Returns:
        (returncode, log_directory)
    """
    # Get SPINDLE executable path
    if 'SPINDLE' in env:
        spindle_exec = env['SPINDLE']
    else:
        spindle_exec = 'spindle'

    env['SPINDLE'] = spindle_exec

    # Build command using --serial launcher
    spindle_flags = env['SPINDLE_FLAGS']
    cmd = f"{spindle_exec} {spindle_flags} --launcher=serial {test_executable}"

    if args.verbose:
        print(f"Command: {cmd}")

    # Change to testsuite directory to run
    result = subprocess.run(cmd, shell=True, env=env, cwd=testsuite_dir)

    # Create log directory
    if log_dir is None:
        timestamp = datetime.now().strftime('%Y%m%d_%H%M%S')
        test_name = os.path.basename(test_executable)
        log_dir = os.path.join(testsuite_dir, 'debug', f'{result.returncode}_{test_name}_{timestamp}')

    return (result.returncode, log_dir)


def run_flux_session_test(args, env, testsuite_dir, test_type, session_num, log_dir=None):
    """Run a session test using Flux resource manager.

    Sessions allow multiple jobs in a single allocation to share libraries.
    This implements the session lifecycle:
      1. Start session: spindle --start-session
      2. Run test: flux run with --env=SESSION_ID
      3. End session: spindle --end-session

    Args:
        args: Command line arguments
        env: Environment variables
        testsuite_dir: Path to testsuite directory
        test_type: Type of test (dependency, dlopen, etc.)
        session_num: Session number (for tracking, not used by spindle)
        log_dir: Directory to store logs (if None, use timestamp-based default)

    Returns:
        (returncode, log_directory)
    """
    if not FLUX_AVAILABLE:
        print("ERROR: Flux Python module not available", file=sys.stderr)
        return (1, log_dir)

    # Get SPINDLE executable path
    if 'SPINDLE' in env:
        spindle_exec = env['SPINDLE']
    else:
        spindle_exec = 'spindle'

    # Start the session
    if args.verbose:
        print(f"Starting Spindle session...")

    start_result = subprocess.run(
        [spindle_exec, '--start-session', '--level=high'],
        env=env,
        cwd=testsuite_dir,
        capture_output=True,
        text=True
    )

    if start_result.returncode != 0:
        print(f"ERROR: Failed to start session: {start_result.stderr}", file=sys.stderr)
        return (1, log_dir)

    session_id = start_result.stdout.strip()
    if not session_id:
        session_id = "ANONYMOUS_SESSION"

    if args.verbose:
        print(f"Session started: {session_id}")

    # Set session environment variable
    env['SESSION_ID'] = session_id

    try:
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
        test_exec = './test_driver_libs' if test_type in ['dependency', 'dlreopen'] else './test_driver'
        test_args = [f'--{test_type}', '--session']

        # For ldpreload tests, set LD_PRELOAD via Flux
        flux_ld_preload = []
        if test_type == 'ldpreload':
            if 'LIBRARY_LIST' in env:
                flux_ld_preload = [f'--env=LD_PRELOAD={env["LIBRARY_LIST"]}']

        # Build flux run command - session tests just need SESSION_ID in environment
        # The spindle.rc plugin will use the active session
        flux_cmd = ['flux', 'run']
        if flux_ld_preload:
            flux_cmd.extend(flux_ld_preload)
        flux_cmd.extend([
            f'--env=SESSION_ID={session_id}',
            '-o', 'userrc=spindle.rc',
            '-o', 'spindle.level=high',
            '-t', args.time_limit,  # Time limit per test
            f'-N', str(args.num_nodes),
            f'-n', str(args.num_tasks),
            test_exec
        ] + test_args)

        if args.verbose:
            print(f"Flux command: {' '.join(flux_cmd)}")
            print(f"Nodes: {args.num_nodes}, Tasks: {args.num_tasks}, Cores per task: {args.cores_per_task}, Time limit: {args.time_limit}")

        # Run the flux command
        result = subprocess.run(
            flux_cmd,
            env=env,
            cwd=testsuite_dir,
            capture_output=False,  # Let output go to stdout/stderr (captured by TeeOutput)
        )

        returncode = result.returncode

        # Collect logs from all nodes to shared filesystem
        if args.verbose:
            print("Collecting logs from all nodes to shared filesystem...")

        # Use shared filesystem for log aggregation (use log-dir if specified)
        if log_dir is None:
            timestamp = datetime.now().strftime('%Y%m%d_%H%M%S')
            if args.log_dir:
                target_base = os.path.join(args.log_dir, f'{returncode}_{timestamp}')
            else:
                # Fall back to testsuite/debug
                target_base = os.path.join(testsuite_dir, 'debug', f'{returncode}_{timestamp}')
        else:
            target_base = log_dir

        # Create directory structure on shared filesystem
        try:
            os.makedirs(target_base, exist_ok=True)
            for i in range(1, args.num_nodes + 1):
                os.makedirs(os.path.join(target_base, f'node-{i}'), exist_ok=True)
        except Exception as e:
            print(f"Warning: Could not create shared log directories: {e}", file=sys.stderr)
            return (returncode, log_dir)

        # Launch collection job: one task per node to move files
        collection_cmd = f"""hostname=$(hostname)
node_num=${{hostname##*-}}
target_dir="{target_base}/node-${{node_num}}"
mv {testsuite_dir}/spindle_output.${{hostname}}.* $target_dir/ 2>/dev/null || \\
    echo "Warning: Could not move Spindle logs from $(hostname)" >&2
flux dmesg > $target_dir/flux-dmesg.log 2>&1 || \\
    echo "Warning: Could not capture flux dmesg from $(hostname)" >&2
"""

        # Add dmesg collection if enabled
        if args.enable_dmesg_collection:
            collection_cmd += """dmesg > $target_dir/dmesg.log 2>&1 || \\
    echo "Warning: Could not capture dmesg from $(hostname)" >&2
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

    finally:
        # Always end the session
        if args.verbose:
            print(f"Ending Spindle session {session_id}...")

        if session_id == "ANONYMOUS_SESSION":
            end_result = subprocess.run(
                [spindle_exec, '--end-session'],
                env=env,
                cwd=testsuite_dir,
                capture_output=True,
                text=True
            )
        else:
            end_result = subprocess.run(
                [spindle_exec, f'--end-session={session_id}'],
                env=env,
                cwd=testsuite_dir,
                capture_output=True,
                text=True
            )

        if end_result.returncode != 0 and args.verbose:
            print(f"Warning: Failed to end session: {end_result.stderr}", file=sys.stderr)


def run_flux_test(args, env, testsuite_dir, test_type='dependency', test_mode='push', log_dir=None):
    """Run test using Flux resource manager with native Spindle integration.

    Uses flux run with -o spindle.* options instead of calling spindle directly.
    This matches the behavior of run_driver_flux.

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
    test_exec = './test_driver_libs' if test_type in ['dependency', 'dlreopen'] else './test_driver'
    test_args = [f'--{test_type}', f'--{test_mode}']

    # Translate spindle modes to Flux spindle plugin options
    # Spindle options: push, pull, numa, preload
    # Test-only modes: fork, forkexec, chdir (no spindle option needed)
    spindle_modes = ['push', 'pull', 'numa', 'preload']
    flux_spindle_opt = None
    if test_mode in spindle_modes:
        if test_mode == 'preload':
            flux_spindle_opt = '-o spindle.preload=preload_file_list'
        else:
            flux_spindle_opt = f'-o spindle.{test_mode}'

    # For ldpreload/preload tests, set LD_PRELOAD via Flux
    flux_ld_preload = []
    if test_type == 'ldpreload' or test_mode == 'preload':
        if 'LIBRARY_LIST' in env:
            flux_ld_preload = [f'--env=LD_PRELOAD={env["LIBRARY_LIST"]}']

    # Build flux run command using Flux's native Spindle integration
    flux_cmd = ['flux', 'run']
    if flux_ld_preload:
        flux_cmd.extend(flux_ld_preload)
    flux_cmd.extend([
        '-o', 'userrc=spindle.rc',
        '-o', 'spindle.level=high',
    ])
    if flux_spindle_opt:
        flux_cmd.extend(flux_spindle_opt.split())
    flux_cmd.extend([
        '-t', args.time_limit,  # Time limit per test
        f'-N', str(args.num_nodes),
        f'-n', str(args.num_tasks),
        test_exec
    ] + test_args)

    if args.verbose:
        print(f"Flux command: {' '.join(flux_cmd)}")
        print(f"Nodes: {args.num_nodes}, Tasks: {args.num_tasks}, Cores per task: {args.cores_per_task}, Time limit: {args.time_limit}")
        sys.stdout.flush()
        sys.stderr.flush()

    # Run the flux command with unbuffered output
    # Setting bufsize=0 and using line buffering helps with real-time output
    result = subprocess.run(
        flux_cmd,
        env=env,
        cwd=testsuite_dir,
        capture_output=False,  # Let output go to stdout/stderr (captured by TeeOutput)
        bufsize=0,  # Unbuffered
    )

    returncode = result.returncode

    # Collect logs from all nodes to shared filesystem
    if args.verbose:
        print("Collecting logs from all nodes to shared filesystem...")

    # Use shared filesystem for log aggregation (use log-dir if specified)
    if log_dir is None:
        timestamp = datetime.now().strftime('%Y%m%d_%H%M%S')
        if args.log_dir:
            target_base = os.path.join(args.log_dir, f'{returncode}_{timestamp}')
        else:
            # Fall back to testsuite/debug
            target_base = os.path.join(testsuite_dir, 'debug', f'{returncode}_{timestamp}')
    else:
        target_base = log_dir

    # Create directory structure on shared filesystem (only need to do this once)
    try:
        os.makedirs(target_base, exist_ok=True)
        for i in range(1, args.num_nodes + 1):
            os.makedirs(os.path.join(target_base, f'node-{i}'), exist_ok=True)
    except Exception as e:
        print(f"Warning: Could not create shared log directories: {e}", file=sys.stderr)
        return (returncode, log_dir)

    # Launch collection job: one task per node to move files
    collection_cmd = f"""hostname=$(hostname)
node_num=${{hostname##*-}}
target_dir="{target_base}/node-${{node_num}}"
mv {testsuite_dir}/spindle_output.${{hostname}}.* $target_dir/ 2>/dev/null || \\
    echo "Warning: Could not move Spindle logs from $(hostname)" >&2
flux dmesg > $target_dir/flux-dmesg.log 2>&1 || \\
    echo "Warning: Could not capture flux dmesg from $(hostname)" >&2
"""

    # Add dmesg collection if enabled
    if args.enable_dmesg_collection:
        collection_cmd += """dmesg > $target_dir/dmesg.log 2>&1 || \\
    echo "Warning: Could not capture dmesg from $(hostname)" >&2
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
        help='Time limit PER TEST, e.g., "5m", "30s" (flux). Accepts Flux duration format.'
    )
    parser.add_argument(
        '--run-typemode-tests',
        action='store_true',
        help='Run all type_mode tests (56 tests: 8 types × 7 modes)'
    )
    parser.add_argument(
        '--run-session-tests',
        action='store_true',
        help='Run all session tests (requires flux RM)'
    )
    parser.add_argument(
        '--run-serial-tests',
        action='store_true',
        help='Run all serial-exec tests (spindle_exec_test, symbind_test, interpreter_test) - requires serial RM'
    )
    parser.add_argument(
        '--run-all-tests',
        action='store_true',
        help='Run all tests appropriate for the selected resource manager'
    )
    parser.add_argument(
        '--single-test',
        metavar='SPEC',
        action='append',
        help='Run specific test(s). Formats:\n'
             '  - type_mode: "dependency_push", "dlopen_pull", etc.\n'
             '  - Wildcards: "*_push" (all push tests), "dependency_*" (all dependency modes)\n'
             '  - Serial test: "serial:path/to/test" (path can be relative, e.g., "serial:./spindle_exec_test")\n'
             '  - Session test: "dependency_session", "dlopen_session", etc.\n'
             'Can be specified multiple times.'
    )
    parser.add_argument(
        '--ignore-test',
        metavar='TYPE_MODE',
        action='append',
        help='Ignore specific test(s). Same wildcard format as --single-test. '
             'Can be specified multiple times.'
    )
    parser.add_argument(
        '--reps',
        type=int,
        default=1,
        metavar='N',
        help='Repeat all requested tests N times (must be positive integer)'
    )
    parser.add_argument(
        '--enable-dmesg-collection',
        action='store_true',
        help='Enable collection of dmesg output (requires sufficient permissions)'
    )
    parser.add_argument(
        '--list-available-tests',
        action='store_true',
        help='List all available tests and exit'
    )
    parser.add_argument(
        '--log-dir',
        type=str,
        default=None,
        help='Base directory for test logs (default: current directory when --preserve-logs-on-success is used)'
    )

    args = parser.parse_args()

    # Handle --list-available-tests
    if args.list_available_tests:
        print("Available tests:")
        print("\nType-mode tests (any resource manager):")
        for test_type, test_mode in ALL_TESTS:
            print(f"  {test_type}_{test_mode}")
        print(f"\nSerial-exec tests (serial only):")
        for test_exe in SERIAL_TESTS:
            test_name = os.path.basename(test_exe)
            print(f"  serial:{test_exe} ({test_name})")
        print(f"\nSession tests (flux only):")
        for test_type in SESSION_TEST_TYPES:
            print(f"  {test_type}_session")
        print(f"\nTotal: {len(ALL_TESTS)} type-mode + {len(SERIAL_TESTS)} serial-exec + {len(SESSION_TEST_TYPES)} session = {len(ALL_TESTS) + len(SERIAL_TESTS) + len(SESSION_TEST_TYPES)} tests")
        sys.exit(0)

    # Validate reps
    if args.reps < 1:
        parser.error(f"--reps must be a positive integer, got {args.reps}")

    # Validate and create log directory if specified
    if args.log_dir:
        try:
            os.makedirs(args.log_dir, exist_ok=True)
        except OSError as e:
            parser.error(f"Failed to create log directory '{args.log_dir}': {e}")

    # Validate single-test format if provided
    if args.single_test:
        for test_spec in args.single_test:
            if test_spec.startswith('serial:'):
                # Serial-exec test format: serial:path/to/test
                continue
            # Otherwise expect type_mode or type_session format
            parts = test_spec.split('_')
            if len(parts) != 2:
                parser.error(f"--single-test must be in format 'type_mode', 'type_session', 'serial:path', or use wildcards (e.g., '*_push'), got '{test_spec}'")

    # Validate resource manager availability
    if args.resource_manager == 'flux' and not FLUX_AVAILABLE:
        parser.error("--resource-manager=flux requires Flux Python module, which is not available")

    # Validate RM compatibility with test selections
    if args.resource_manager == 'flux' and args.run_serial_tests:
        parser.error("--run-serial-tests requires --resource-manager=serial")
    if args.resource_manager == 'serial' and args.run_session_tests:
        parser.error("--run-session-tests requires --resource-manager=flux")

    # Check that at least some tests are selected
    no_tests_selected = not any([
        args.run_typemode_tests,
        args.run_session_tests,
        args.run_serial_tests,
        args.run_all_tests,
        args.single_test,
    ])
    if no_tests_selected:
        # Default to typemode tests
        args.run_typemode_tests = True

    # Set up environment
    env, testsuite_dir = setup_environment()

    # Add SPINDLE_DEBUG if specified
    if args.spindle_debug is not None:
        env['SPINDLE_DEBUG'] = str(args.spindle_debug)

    # Check dmesg permissions if requested
    if args.enable_dmesg_collection:
        try:
            result = subprocess.run('dmesg', capture_output=True, timeout=5)
            if result.returncode != 0:
                parser.error("--enable-dmesg-collection requires permission to run 'dmesg'. "
                           "Error: " + result.stderr.decode())
        except subprocess.TimeoutExpired:
            parser.error("--enable-dmesg-collection: 'dmesg' command timed out")
        except FileNotFoundError:
            parser.error("--enable-dmesg-collection: 'dmesg' command not found")

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

        # Helper function to match test against pattern
        def matches_pattern(test_type, test_mode, pattern):
            """Check if test_type_mode matches pattern with wildcards."""
            pattern_parts = pattern.split('_')
            if len(pattern_parts) != 2:
                return False
            type_pattern, mode_pattern = pattern_parts
            return (fnmatch.fnmatch(test_type, type_pattern) and
                    fnmatch.fnmatch(test_mode, mode_pattern))

        # Build list of tests to run
        # Tests are stored as: ('typemode', test_type, test_mode), ('serial', test_executable), or ('session', test_type, session_num)
        tests_to_run = []

        # Session counter - generate unique session numbers for session tests
        session_counter = 1

        if args.single_test:
            # Parse single-test specifications
            for test_spec in args.single_test:
                if test_spec.startswith('serial:'):
                    # Serial-exec test
                    test_path = test_spec[7:]  # Strip 'serial:' prefix
                    tests_to_run.append(('serial', test_path))
                elif test_spec.endswith('_session'):
                    # Session test (e.g., dependency_session)
                    test_type = test_spec[:-8]  # Strip '_session' suffix
                    if test_type in SESSION_TEST_TYPES:
                        tests_to_run.append(('session', test_type, session_counter))
                        session_counter += 1
                    else:
                        print(f"Warning: --single-test '{test_spec}' - '{test_type}' is not a valid session test type", file=sys.stderr)
                else:
                    # Type_mode test with possible wildcards
                    matched = False
                    for test_type, test_mode in ALL_TESTS:
                        if matches_pattern(test_type, test_mode, test_spec):
                            if ('typemode', test_type, test_mode) not in tests_to_run:
                                tests_to_run.append(('typemode', test_type, test_mode))
                            matched = True
                    if not matched and '*' not in test_spec:
                        print(f"Warning: --single-test '{test_spec}' did not match any tests", file=sys.stderr)
        else:
            # Handle --run-* flags
            if args.run_all_tests:
                # Add appropriate tests based on RM
                if args.resource_manager == 'serial':
                    tests_to_run.extend([('typemode', t, m) for t, m in ALL_TESTS])
                    tests_to_run.extend([('serial', exe) for exe in SERIAL_TESTS])
                elif args.resource_manager == 'flux':
                    tests_to_run.extend([('typemode', t, m) for t, m in ALL_TESTS])
                    # Add session tests with generated session numbers
                    for test_type in SESSION_TEST_TYPES:
                        tests_to_run.append(('session', test_type, session_counter))
                        session_counter += 1
            else:
                # Individual flags
                if args.run_typemode_tests:
                    tests_to_run.extend([('typemode', t, m) for t, m in ALL_TESTS])
                if args.run_serial_tests:
                    tests_to_run.extend([('serial', exe) for exe in SERIAL_TESTS])
                if args.run_session_tests:
                    for test_type in SESSION_TEST_TYPES:
                        tests_to_run.append(('session', test_type, session_counter))
                        session_counter += 1

        # Apply ignore-test filters (only to typemode tests)
        if args.ignore_test:
            original_count = len(tests_to_run)
            filtered = []
            for test_item in tests_to_run:
                if test_item[0] == 'typemode':
                    test_type, test_mode = test_item[1], test_item[2]
                    if not any(matches_pattern(test_type, test_mode, ignore_spec)
                              for ignore_spec in args.ignore_test):
                        filtered.append(test_item)
                else:
                    # Keep serial tests (ignore doesn't apply)
                    filtered.append(test_item)
            tests_to_run = filtered
            ignored_count = original_count - len(tests_to_run)
            if ignored_count > 0 and args.verbose:
                print(f"Ignored {ignored_count} test(s) based on --ignore-test filters")

        # Repeat tests if reps > 1
        if args.reps > 1:
            tests_to_run = tests_to_run * args.reps

        # Run each test
        global_result = 0
        for test_item in tests_to_run:
            if test_item[0] == 'typemode':
                # Type-mode test (e.g., dependency_push)
                test_type, test_mode = test_item[1], test_item[2]
                test_name = f"{test_type}_{test_mode}"
                test_start_time = time.time()

                # Create directories
                debug_dir = os.path.join(testsuite_dir, 'debug')
                timestamp = datetime.now().strftime('%Y%m%d_%H%M%S')

                # For Flux, use log-dir if specified; for serial, use local debug dir
                if args.resource_manager == 'flux' and args.log_dir:
                    temp_dir = os.path.join(args.log_dir, f'temp_{test_name}_{timestamp}')
                else:
                    temp_dir = os.path.join(debug_dir, f'temp_{test_name}_{timestamp}')

                os.makedirs(temp_dir, exist_ok=True)

                # Print the "Running:" message BEFORE any output redirection for real-time visibility
                print(f"Running: ./run_driver --{test_type} --{test_mode}")
                sys.stdout.flush()

                # Set up log capture
                runtest_log_path = os.path.join(temp_dir, 'runtest.log')
                log_capture = TeeOutput(runtest_log_path, verbose=args.verbose)
                old_stdout = sys.stdout
                old_stderr = sys.stderr
                sys.stdout = log_capture
                sys.stderr = log_capture

                try:
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

                # Calculate test duration
                test_duration = time.time() - test_start_time

                # Handle log preservation based on success/failure
                if returncode == 0:
                    print(f"Test {test_name} PASSED ({test_duration:.1f}s)")
                    # Delete logs for successful runs unless explicitly preserving
                    if not args.preserve_logs_on_success and os.path.exists(final_dir):
                        shutil.rmtree(final_dir)
                else:
                    print(f"Test {test_name} FAILED ({test_duration:.1f}s)")
                    # Stop at first failure unless explicitly continuing
                    if not args.continue_after_failure:
                        sys.exit(returncode)

            elif test_item[0] == 'serial':
                # Serial-exec test (e.g., ./spindle_exec_test)
                test_executable = test_item[1]
                test_name = f"serial_{os.path.basename(test_executable)}"
                test_start_time = time.time()

                # Create directories (serial tests are always local)
                debug_dir = os.path.join(testsuite_dir, 'debug')
                timestamp = datetime.now().strftime('%Y%m%d_%H%M%S')
                temp_dir = os.path.join(debug_dir, f'temp_{test_name}_{timestamp}')
                os.makedirs(temp_dir, exist_ok=True)

                # Print the "Running:" message BEFORE any output redirection for real-time visibility
                print(f"Running: {test_executable}")
                sys.stdout.flush()

                # Set up log capture
                runtest_log_path = os.path.join(temp_dir, 'runtest.log')
                log_capture = TeeOutput(runtest_log_path, verbose=args.verbose)
                old_stdout = sys.stdout
                old_stderr = sys.stderr
                sys.stdout = log_capture
                sys.stderr = log_capture

                try:

                    # Run serial-exec test
                    returncode, log_dir = run_serial_exec_test(args, env, testsuite_dir, test_executable, temp_dir)

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
                    final_dir = log_dir

                # Move spindle_output files if they exist
                spindle_outputs = glob.glob(os.path.join(testsuite_dir, 'spindle_output*'))
                if spindle_outputs:
                    for output_file in spindle_outputs:
                        filename = os.path.basename(output_file)
                        dest = os.path.join(final_dir, filename)
                        if os.path.exists(output_file):
                            os.rename(output_file, dest)

                # Calculate test duration
                test_duration = time.time() - test_start_time

                # Handle log preservation based on success/failure
                if returncode == 0:
                    print(f"Test {test_name} PASSED ({test_duration:.1f}s)")
                    # Delete logs for successful runs unless explicitly preserving
                    if not args.preserve_logs_on_success and os.path.exists(final_dir):
                        shutil.rmtree(final_dir)
                else:
                    print(f"Test {test_name} FAILED ({test_duration:.1f}s)")
                    # Stop at first failure unless explicitly continuing
                    if not args.continue_after_failure:
                        sys.exit(returncode)

            elif test_item[0] == 'session':
                # Session test (e.g., dependency_session)
                test_type = test_item[1]
                session_num = test_item[2]
                test_name = f"{test_type}_session_{session_num}"
                test_start_time = time.time()

                # Create directories (use log-dir if specified, otherwise debug dir)
                debug_dir = os.path.join(testsuite_dir, 'debug')
                timestamp = datetime.now().strftime('%Y%m%d_%H%M%S')
                if args.log_dir:
                    temp_dir = os.path.join(args.log_dir, f'temp_{test_name}_{timestamp}')
                else:
                    temp_dir = os.path.join(debug_dir, f'temp_{test_name}_{timestamp}')
                os.makedirs(temp_dir, exist_ok=True)

                # Print the "Running:" message BEFORE any output redirection for real-time visibility
                print(f"Running: ./run_driver --{test_type} --session (session {session_num})")
                sys.stdout.flush()

                # Set up log capture
                runtest_log_path = os.path.join(temp_dir, 'runtest.log')
                log_capture = TeeOutput(runtest_log_path, verbose=args.verbose)
                old_stdout = sys.stdout
                old_stderr = sys.stderr
                sys.stdout = log_capture
                sys.stderr = log_capture

                try:

                    # Run session test (only Flux supported)
                    returncode, log_dir = run_flux_session_test(args, env, testsuite_dir, test_type, session_num, temp_dir)

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

                # Calculate test duration
                test_duration = time.time() - test_start_time

                # Handle log preservation based on success/failure
                if returncode == 0:
                    print(f"Test {test_name} PASSED ({test_duration:.1f}s)")
                    # Delete logs for successful runs unless explicitly preserving
                    if not args.preserve_logs_on_success and os.path.exists(final_dir):
                        shutil.rmtree(final_dir)
                else:
                    print(f"Test {test_name} FAILED ({test_duration:.1f}s)")
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
