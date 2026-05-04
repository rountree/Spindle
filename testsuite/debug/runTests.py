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

# Try to import flux - it's only available in flux containers
try:
    import flux
    import flux.job
    FLUX_AVAILABLE = True
except ImportError:
    FLUX_AVAILABLE = False


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


def run_serial_test(args, env, testsuite_dir):
    """Run test using serial resource manager."""
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

    if args.verbose:
        print(f"Command: {cmd}")

    # Change to testsuite directory to run
    result = subprocess.run(cmd, shell=True, env=env, cwd=testsuite_dir)
    return result.returncode


def run_flux_test(args, env, testsuite_dir):
    """Run test using Flux resource manager."""
    if not FLUX_AVAILABLE:
        print("ERROR: Flux Python module not available", file=sys.stderr)
        return 1

    # Query available resources for debugging
    if args.verbose:
        print("Querying Flux resources...")
        try:
            result = subprocess.run("flux resource list", shell=True, capture_output=True, text=True)
            print("Available resources:")
            print(result.stdout)
        except Exception as e:
            print(f"Could not query resources: {e}")

    # Determine TEST_EXEC based on first argument (--dependency)
    test_exec = './test_driver_libs'
    test_args = ['--dependency', '--push']

    # Get SPINDLE executable path
    if 'SPINDLE' in env:
        spindle_exec = env['SPINDLE']
    else:
        spindle_exec = 'spindle'

    env['SPINDLE'] = spindle_exec
    env['TEST_EXEC'] = test_exec
    env['SPINDLE_OPTS'] = '--push'

    # Build full command with spindle wrapper
    spindle_flags = env['SPINDLE_FLAGS']
    full_command = [spindle_exec] + spindle_flags.split() + ['--push', '--launcher=serial', test_exec] + test_args

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

        # Collect logs from all nodes if no shared filesystem
        if args.no_shared_filesystem:
            if args.verbose:
                print("Collecting logs from all nodes to shared filesystem...")

            # Use shared filesystem for log aggregation
            shared_logs = '/shared-logs'
            timestamp = datetime.now().strftime('%Y%m%d_%H%M%S')
            target_base = os.path.join(shared_logs, f'{returncode}_{timestamp}')

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
    echo "Warning: Could not copy logs from $(hostname)" >&2
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

        return returncode

    except Exception as e:
        print(f"ERROR running Flux job: {e}", file=sys.stderr)
        import traceback
        traceback.print_exc()
        return 1


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
        '--no-shared-filesystem',
        action='store_true',
        help='Copy log files from all nodes to node-1 after test completes (flux). '
             'Use for parallel container jobs without a shared logging filesystem. Default: off.'
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

        # Create a temporary directory for this test run
        debug_dir = os.path.join(testsuite_dir, 'debug')
        timestamp = datetime.now().strftime('%Y%m%d_%H%M%S')
        temp_dir = os.path.join(debug_dir, f'temp_{timestamp}')
        os.makedirs(temp_dir, exist_ok=True)

        # Print the "Running:" message like run_driver does
        print(f"Running: ./run_driver --dependency --push")

        # Run test with appropriate resource manager
        if args.resource_manager == 'serial':
            returncode = run_serial_test(args, env, testsuite_dir)
        elif args.resource_manager == 'flux':
            returncode = run_flux_test(args, env, testsuite_dir)
        else:
            print(f"ERROR: Unknown resource manager: {args.resource_manager}", file=sys.stderr)
            returncode = 1

        # Rename directory to include return code
        final_dir = os.path.join(debug_dir, f'{returncode}_{timestamp}')
        os.rename(temp_dir, final_dir)

        # Move spindle_output files if they exist
        spindle_outputs = glob.glob(os.path.join(testsuite_dir, 'spindle_output*'))
        if spindle_outputs:
            for output_file in spindle_outputs:
                filename = os.path.basename(output_file)
                dest = os.path.join(final_dir, filename)
                os.rename(output_file, dest)

        # Handle log preservation based on success/failure
        if returncode == 0:
            print("ALL TESTS PASSED")
            # Delete logs for successful runs unless explicitly preserving
            if not args.preserve_logs_on_success:
                shutil.rmtree(final_dir)
        else:
            print("SOME TESTS FAILED")
            # Stop at first failure unless explicitly continuing
            if not args.continue_after_failure:
                sys.exit(returncode)

        sys.exit(returncode)


if __name__ == '__main__':
    main()
