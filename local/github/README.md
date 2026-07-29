# GitHub Actions CI Scripts

These scripts allow you to trigger and monitor Spindle's GitHub Actions CI workflows from the command line.

## Prerequisites

1. **Authenticate with GitHub:**
   ```bash
   /g/g24/rountree/v/rzadams/sandbox/workspace-Spindle/install/gh-2.96.0/bin/gh auth login
   ```

2. **Add gh to your PATH** (optional but recommended):
   ```bash
   export PATH="/g/g24/rountree/v/rzadams/sandbox/workspace-Spindle/install/gh-2.96.0/bin:$PATH"
   ```

## Scripts

### trigger-ci.sh

Trigger a CI workflow run on GitHub Actions.

**Basic usage:**
```bash
./trigger-ci.sh                    # Trigger CI on current branch
./trigger-ci.sh --watch            # Trigger and watch in real-time
./trigger-ci.sh --wait --download  # Trigger, wait for completion, download artifacts
```

**Options:**
- `--branch BRANCH` - Run CI on a specific branch
- `--ref REF` - Run CI on a specific git ref
- `--watch` - Watch the run output in real-time
- `--wait` - Wait for the run to complete
- `--download` - Download artifacts after completion

**Examples:**
```bash
# Trigger CI on current branch and watch it
./trigger-ci.sh --watch

# Trigger CI on a specific branch and download results
./trigger-ci.sh --branch devel --wait --download

# Just trigger CI (don't wait)
./trigger-ci.sh
```

### download-ci-logs.sh

Download logs and artifacts from completed (or running) CI runs.

**Basic usage:**
```bash
./download-ci-logs.sh              # Download from latest run
./download-ci-logs.sh --run-id 123 # Download from specific run ID
```

**Options:**
- `--run-id ID` - Download from specific run ID
- `--latest` - Download from most recent run (default)
- `--branch BRANCH` - Download from latest run on specific branch
- `--output-dir DIR` - Custom output directory
- `--artifacts-only` - Only download artifacts, skip logs
- `--logs-only` - Only download logs, skip artifacts

**Examples:**
```bash
# Download everything from the latest run
./download-ci-logs.sh

# Download from a specific run ID
./download-ci-logs.sh --run-id 12345678

# Download latest run from devel branch
./download-ci-logs.sh --branch devel

# Only download artifacts (useful for large log files)
./download-ci-logs.sh --artifacts-only

# Custom output location
./download-ci-logs.sh --output-dir ~/spindle-ci-results
```

## Typical Workflows

### Quick test on current branch:
```bash
./trigger-ci.sh --watch
```

### Full test with artifact collection:
```bash
# Trigger and wait
./trigger-ci.sh --wait --download

# Or trigger, do other work, then download later
./trigger-ci.sh
# ... do other work ...
./download-ci-logs.sh
```

### Debug a failed run:
```bash
# Find the run ID from GitHub Actions UI or:
gh run list --workflow=ci.yml --limit 5

# Download that specific run
./download-ci-logs.sh --run-id <run-id>
```

## Using gh CLI directly

These scripts wrap `gh` CLI commands. You can also use `gh` directly:

```bash
# List recent runs
gh run list --workflow=ci.yml

# View a specific run
gh run view <run-id>

# Watch a run in progress
gh run watch <run-id>

# Download artifacts
gh run download <run-id>

# View logs
gh run view <run-id> --log
```

## Notes

- The CI workflow is defined in `.github/workflows/ci.yml`
- Workflow runs must be enabled in the repository settings
- You need appropriate repository permissions to trigger workflows
- Logs and artifacts are saved with timestamps for easy organization
