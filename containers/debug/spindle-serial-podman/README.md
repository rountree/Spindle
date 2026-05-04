# Spindle Serial Podman Container

Rootless Podman-compatible Spindle test container for serial resource manager.

## Features

- **Rootless compatible**: No capabilities, no sudo, no munge
- **Serial RM**: Uses `--with-rm=serial`
- **No security**: Uses `--enable-sec-none` (no munge authentication)
- **No NUMA**: Omits `--enable-numa` for simplicity

## Quick Start

```bash
cd containers/debug/spindle-serial-podman/scripts
./run_me.sh
```

This will:
1. Build the container image
2. Start a container
3. Run the Spindle test suite
4. Copy logs to `./podman-logs/`
5. Stop the container

## Manual Usage

Build:
```bash
podman build -t spindle-serial-podman:latest -f containers/debug/spindle-serial-podman/Dockerfile .
```

Run:
```bash
podman run -d --name spindle-test --rm spindle-serial-podman:latest
```

Execute tests:
```bash
podman exec spindle-test bash -c 'cd Spindle-build/testsuite/debug && ./runTests.py --resource-manager=serial --verbose'
```

Stop:
```bash
podman stop spindle-test
```

## Differences from Docker/Flux Versions

- No `CAP_SYS_NICE` capability
- No munge daemon
- No Flux broker
- Single container (not multi-node)
- Runs as non-root user throughout
