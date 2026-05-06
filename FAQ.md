# Spindle FAQ

## Flux

### When using `testsuite/debug/runTests.py`, how do I set the number of Docker nodes to bring up?

The number of Docker nodes is controlled by the workflow configuration, not by `runTests.py` directly.

**For GitHub Actions workflows** (e.g., `.github/workflows/debug-flux.yml`):

Set the `NUM_NODES` environment variable in the workflow file:

```yaml
env:
  NUM_NODES: 32
  TASKS_PER_NODE: 3
```

Then pass it to the compose generation script:
```yaml
- name: Generate docker-compose.yml
  run: |
    cd containers/debug/spindle-flux-ubuntu
    ./generate-compose.py ${{ env.NUM_NODES }} > docker-compose.yml
```

**For local testing**:

Generate the docker-compose.yml manually with your desired node count:
```bash
cd containers/debug/spindle-flux-ubuntu
./generate-compose.py 4 > docker-compose.yml  # Creates a 4-node cluster
docker compose up -d
```

### How do I set the number of tasks per Docker node?

Tasks per node is specified when invoking `runTests.py` using the `--tasks-per-node` argument.

**For GitHub Actions workflows**:

Set the `TASKS_PER_NODE` environment variable and pass it to `runTests.py`:

```yaml
env:
  NUM_NODES: 32
  TASKS_PER_NODE: 3

# In the test step:
- name: Run testsuite
  run: |
    docker exec node-1 bash -c 'cd Spindle-build/testsuite/debug && \
    ./runTests.py \
      --resource-manager=flux \
      --num-nodes=${{ env.NUM_NODES }} \
      --tasks-per-node=${{ env.TASKS_PER_NODE }} \
      --run-all-tests'
```

**For local/manual testing**:

```bash
docker exec node-1 bash -c 'cd Spindle-build/testsuite/debug && \
  ./runTests.py \
    --resource-manager=flux \
    --num-nodes=4 \
    --tasks-per-node=2 \
    --single-test=dependency_push'
```

**Note**: `runTests.py` calculates the total number of tasks as `num_nodes × tasks_per_node` and passes this to Flux's `-n` flag.

### How do I change the number of available cores on each Docker node?

The number of cores per node is configured during the Docker image build via the `flux R encode` command in the Dockerfile.

**Location**: `containers/debug/spindle-flux-ubuntu/Dockerfile`

Look for the line:
```dockerfile
flux R encode --hosts="node-[1-${workers}]" --cores=0-7 > /etc/flux/system/R
```

The `--cores` flag takes a **range** of core IDs (e.g., `0-7` for 8 cores, `0-15` for 16 cores).

To change from 8 cores per node to a different value (e.g., 16 cores):
```dockerfile
flux R encode --hosts="node-[1-${workers}]" --cores=0-15 > /etc/flux/system/R
```

**Important**: Use a core ID range (`0-N`) rather than a count. Using `--cores=8` (without the range) causes Flux to look for a single core with ID 8, resulting in "missing resources: core8" errors.

**Important considerations**:

- **Total cores = nodes × cores-per-node**. With 32 nodes and 8 cores each, you have 256 total cores.
- **Task allocation**: Flux must have enough cores to satisfy your task request. If you request 32 nodes with 3 tasks per node (96 total tasks), you need at least 96 cores total.
- **Rebuild required**: After changing the `--cores` value, you must rebuild the Docker image for the change to take effect:
  ```bash
  cd containers/debug/spindle-flux-ubuntu
  docker compose build
  ```

**Example scenarios**:

- **32 nodes, 8 cores each** (`--cores=0-7`): 256 total cores → can run up to 8 tasks per node
- **32 nodes, 4 cores each** (`--cores=0-3`): 128 total cores → can run up to 4 tasks per node  
- **4 nodes, 16 cores each** (`--cores=0-15`): 64 total cores → can run up to 16 tasks per node

If you request more tasks than available cores, Flux will reject the job with an "unsatisfiable" error.
