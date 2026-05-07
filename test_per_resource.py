#!/usr/bin/env python3
"""
Test flux.job.JobspecV1.per_resource() for tasks-per-node functionality.

Usage:
  1. Start a Flux allocation: flux alloc --nodes=4 --exclusive
  2. Run this script: python3 test_per_resource.py
"""

import sys
import flux
import flux.job

def test_per_resource(nnodes, tasks_per_node):
    """Test per_resource() with specified nodes and tasks per node."""

    print(f"\n=== Testing per_resource() ===")
    print(f"Nodes: {nnodes}")
    print(f"Tasks per node: {tasks_per_node}")
    print(f"Total tasks: {nnodes * tasks_per_node}")

    # Query available resources
    h = flux.Flux()
    result = flux.Flux().rpc("resource.status").get()
    print(f"\nAvailable resources: {result}")

    try:
        # Create jobspec using per_resource()
        # ncores=nnodes creates minimal allocation (1 core per node)
        # per_resource tells shell to run tasks_per_node tasks per node
        jobspec = flux.job.JobspecV1.per_resource(
            command=["hostname"],
            nnodes=nnodes,
            ncores=nnodes,  # Minimal: 1 core per node
            per_resource_type="node",
            per_resource_count=tasks_per_node,
            duration=60.0,
        )

        print(f"\nJobspec created successfully")
        print(f"Jobspec attributes: {jobspec.attributes}")

        # Submit and wait
        print(f"Submitting job...")
        jobid = flux.job.submit(h, jobspec, waitable=True)
        print(f"Job submitted: {jobid}")
        print(f"Waiting for completion...")

        result = flux.job.wait(h, jobid)
        print(f"Result: {result}")

        if result.success:
            print(f"✓ SUCCESS: Job completed successfully")
            return 0
        else:
            print(f"✗ FAILED: {result.errstr}")
            return 1

    except Exception as e:
        print(f"✗ ERROR: {e}")
        import traceback
        traceback.print_exc()
        return 1


if __name__ == "__main__":
    # Test with 4 nodes, 3 tasks per node (12 total tasks)
    # This should work even with limited cores due to per-resource scheduling

    nnodes = 4
    tasks_per_node = 300

    if len(sys.argv) > 1:
        nnodes = int(sys.argv[1])
    if len(sys.argv) > 2:
        tasks_per_node = int(sys.argv[2])

    sys.exit(test_per_resource(nnodes, tasks_per_node))
