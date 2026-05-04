# Spindle Serial Podman Container

Rootless Podman-compatible Spindle test container for serial resource manager.

Includes LLNL-specific workarounds for rootless Podman on LLNL systems.

## Features

- **Rootless compatible**: No capabilities, no sudo, no munge
- **Serial RM**: Uses `--with-rm=serial`
- **No security**: Uses `--enable-sec-none` (no munge authentication)
- **No NUMA**: Omits `--enable-numa` for simplicity
- **LLNL workarounds**: APT sandbox fix, CA certificates, UID mapping

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

Build (with LLNL-specific UID mapping):
```bash
enable-podman  # LLNL systems only
podman build \
  --userns-uid-map=0:0:1 \
  --userns-uid-map=1:1:1999 \
  --userns-uid-map=65534:2000:2 \
  -t spindle-serial-podman:latest \
  -f containers/debug/spindle-serial-podman/Dockerfile \
  .
```

Run (with LLNL-specific UID mapping):
```bash
podman run -d \
  --name spindle-test \
  --uidmap 0:0:2000 \
  --uidmap 65534:2000:2 \
  --rm \
  spindle-serial-podman:latest
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

## LLNL-Specific Workarounds

This container includes several workarounds for LLNL rootless Podman environments:

1. **APT Sandbox Fix**: Sets `APT::Sandbox::User root;` to avoid UID mapping issues during apt operations
2. **LLNL CA Certificates**: Installs ADPKI root certificates for HTTPS access within LLNL
3. **UID Mapping**: Uses specific `--userns-uid-map` and `--uidmap` parameters to work with LLNL user namespaces
4. **enable-podman**: Calls LLNL-specific command to set up Podman environment

These workarounds may not be needed on non-LLNL systems. For standard rootless Podman, remove the UID mapping parameters and the `enable-podman` call.
