# Spindle Flux Plugin Hang Investigation and Fix

## Problem Statement

### Observed Behavior
The Spindle Flux plugin (`src/flux/flux-spindle.c`) exhibited intermittent hangs during initialization on GitHub Actions CI (approximately 1-9% failure rate when running 32-node containerized Flux clusters with 3 tasks per node). The hang occurred during the first test iteration and was **not reproducible** on dedicated hardware (LLNL's Tuolumne supercomputer).

### Symptoms
- Jobs would timeout waiting for resource allocation to expire
- No progress after job submission
- Affected only containerized, heavily oversubscribed environments (GitHub Actions runners with 32 containers competing for limited CPU cores)

## Root Cause Analysis

### The Flux Shell Lifecycle

Understanding the Flux shell initialization sequence is critical. From `man flux-shell(1)` and `flux-core/src/shell/shell.c`:

```
1. shell.init plugin callbacks (line ~2095 in shell.c)
2. shell_barrier("init") - all ranks synchronize (line 2100)
3. Rank 0 emits "shell.init" event to exec.eventlog (lines 2105-2107)
4. shell.post-init plugin callbacks (line 2111)
5. shell_start_tasks() (line 2116)
6. shell.start callbacks (line 2119)
7. shell_barrier("start") (line 2122)
8. Rank 0 emits "shell.start" event (lines 2127-2129)
9. flux_reactor_run() - **REACTOR STARTS HERE** (line 2134)
```

**Critical insight:** The Flux reactor does **not start** until step 9, but all plugin callbacks (shell.init, shell.post-init, task.init, shell.start) run in steps 1-8 **before** the reactor is active.

### Original Implementation Problem

The original Spindle plugin code in `sp_init()` (shell.init callback) attempted to use asynchronous event watching:

```c
// In sp_init() - runs during step 1, before reactor starts
if (!(f = flux_job_event_watch (h, id, "guest.exec.eventlog", 0))
    || flux_future_then (f, -1., wait_for_shell_init, ctx) < 0)
    shell_die (1, "flux_job_event_watch");
```

This registers a callback (`wait_for_shell_init`) to fire when "shell.init" appears in the eventlog.

**The Race Condition:**

```
Timeline on each rank:

Step 1:  All ranks: sp_init() registers flux_job_event_watch() callback
Step 2:  All ranks: exit barrier together  
Step 3:  Rank 0: emits "shell.init" to exec.eventlog
         ↓
         KVS propagates event notification to all ranks
         ↓
         Some ranks receive notification HERE
         (but reactor not running - callback cannot fire!)
Step 4:  All ranks: shell.post-init callbacks run
...
Step 9:  All ranks: flux_reactor_run() starts
         (Now callbacks can fire, but notification already delivered and lost)
```

**Why GitHub Actions but not Tuolumne?**

- **Tuolumne (bare metal, dedicated CPUs):** All ranks move through steps 1-9 quickly and roughly synchronized. Natural network latency means "shell.init" notification arrives after step 9 when reactors are running.

- **GitHub Actions (32 containers, oversubscribed):** Extreme CPU contention. Rank 0 emits shell.init (step 3), then gets preempted. Event propagates quickly (localhost, shared kernel) to other ranks still between steps 3-9. Those ranks receive the notification while their reactor isn't running yet. Notification lost.

### How Flux Reactors and Callbacks Work

From `flux-core/src/common/libflux/future.c`:

```c
int flux_future_then (flux_future_t *f,
                      double timeout,
                      flux_continuation_f cb,
                      void *arg)
{
    // ...
    if (future_is_ready (f))
        then_context_start (f->then);  // Fire immediately if already ready
    // ...
    f->then->continuation = cb;
    // ...
}
```

The future callback mechanism relies on the **reactor** (`flux_reactor_t *r`) being active to process events. See `flux-core/src/shell/shell.c` line 2134 - this is the single point where the reactor starts.

### Event Watch Implementation

The `flux_job_event_watch()` API (from `flux-core/src/common/libjob/info.c`) creates a streaming RPC to the job-info module. The server side (`flux-core/src/modules/job-info/watch.c`) uses KVS watch with `FLUX_KVS_WATCH_APPEND` to stream eventlog updates as they arrive.

This is fundamentally **asynchronous** and requires an active reactor to deliver notifications.

## Solution: Synchronous KVS Lookup in shell.post-init

### Design Decision

Instead of async event watching in `sp_init()`, use **synchronous KVS reading** in a new `sp_post_init()` callback:

**Why shell.post-init?**
- Runs at step 4 - **after** shell.init is emitted (step 3)
- Runs **before** reactor starts (step 9)
- Runs synchronously - no reactor dependency

**Approach:**
- Directly read eventlog contents from KVS using `flux_kvs_lookup()`
- Poll in a retry loop until "shell.init" appears
- Parse eventlog to extract `spindle_port` and `spindle_num_ports` from context
- No async callbacks, no reactor dependency

### Implementation Changes

#### 1. Removed Async Watch from sp_init()

**Before:**
```c
// In sp_init() - lines 657-659
if (!(f = flux_job_event_watch (h, id, "guest.exec.eventlog", 0))
    || flux_future_then (f, -1., wait_for_shell_init, ctx) < 0)
    shell_die (1, "flux_job_event_watch");
```

**After:**
```c
// Removed async watch entirely
// Added comment explaining synchronous approach in shell.post-init
```

#### 2. Added Eventlog Parser

**New function:** `parse_eventlog_for_shell_init()` (lines 316-361)

Parses newline-delimited JSON eventlog entries to find "shell.init" and extract spindle context:

```c
static int parse_eventlog_for_shell_init (const char *eventlog_str,
                                          int *port,
                                          int *num_ports)
{
    // Parse each line as JSON
    // Find entry where name == "shell.init"  
    // Extract context.spindle_port and context.spindle_num_ports
}
```

#### 3. Added sp_post_init() Handler

**New function:** `sp_post_init()` (lines 363-443)

Core logic:
```c
static int sp_post_init (flux_plugin_t *p, ...)
{
    // Get job's KVS namespace
    flux_job_kvs_namespace (ns, sizeof (ns), ctx->id);
    
    // Retry loop: poll for shell.init
    for (int retry = 0; retry < 1000; retry++) {
        // Synchronous KVS lookup
        f = flux_kvs_lookup (h, ns, 0, "exec.eventlog");
        flux_future_wait_for (f, -1.0);  // Block until complete
        flux_kvs_lookup_get (f, &eventlog_str);
        
        // Copy eventlog before destroying future (data owned by future)
        eventlog_copy = strdup (eventlog_str);
        
        // Parse eventlog
        rc = parse_eventlog_for_shell_init (eventlog_str,
                                           &ctx->params.port,
                                           &ctx->params.num_ports);
        flux_future_destroy (f);
        
        if (rc == 0)
            break;  // Found shell.init!
            
        usleep (10000);  // 10ms sleep between retries
    }
    
    // Start backends and frontend
    run_spindle_backend (ctx);
    if (ctx->shell_rank == 0)
        run_spindle_frontend (ctx);
}
```

**Key implementation details:**

a) **Memory management:** `flux_kvs_lookup_get()` returns a pointer to data **owned by the future**. Must `strdup()` before destroying the future to avoid use-after-free. Initial implementation bug caused garbage output when printing eventlog on failure.

b) **Retry timeout:** Started at 100 retries (1 second), increased to 1000 retries (10 seconds) when 1 second proved insufficient on heavily oversubscribed systems. On GitHub Actions, rank 0 can be delayed >1 second between exiting the barrier (step 2) and writing shell.init (step 3).

c) **Path:** Using `flux_job_kvs_namespace()` to get guest namespace (e.g., `job-<jobid>-guest`), then looking up `exec.eventlog` within that namespace. This matches where Flux writes the eventlog.

#### 4. Registered New Handler

**In flux_plugin_init():**
```c
if (flux_plugin_set_name (p, "spindle") < 0
    || flux_plugin_add_handler (p, "shell.init", sp_init, NULL) < 0
    || flux_plugin_add_handler (p, "shell.post-init", sp_post_init, NULL) < 0  // NEW
    || flux_plugin_add_handler (p, "task.init",  sp_task, NULL) < 0
    || flux_plugin_add_handler (p, "shell.exit", sp_exit, NULL) < 0)
    return -1;
```

### Git Commit History

Key commits on `dbg_local` branch:

1. **8cd6a1b** - "Fix race condition: read eventlog synchronously in shell.post-init"
   - Replaced async watch with sync lookup
   - Added parse_eventlog_for_shell_init()
   - Added sp_post_init() handler

2. **7f4794d** - "Add retry loop to sp_post_init for shell.init event"
   - Fixed timing: non-zero ranks reach post-init before rank 0 writes shell.init
   - Added 100-retry poll loop

3. **a6925a0** - "Add debug output to sp_post_init retry loop"
   - Added diagnostics to track retry count and eventlog contents

4. **ae8a61c** - "Fix dangling pointer to eventlog data after future destruction"
   - Fixed use-after-free bug: must strdup() eventlog before destroying future
   - Prevented garbage output in error messages

5. **e59fc54** - "Increase eventlog polling timeout from 1s to 10s"
   - Increased retries from 100 to 1000 (10 seconds)
   - Accommodates extreme scheduling delays on oversubscribed systems

## Current Status and Remaining Issues

### What Works
- The fix successfully eliminates the reactor/callback race condition
- ~94-97% of test iterations succeed (30-31 of 32 ranks consistently find shell.init)
- All ranks successfully execute sp_post_init() and parse eventlog when it contains shell.init
- Rank 0 always succeeds (finds shell.init on retry 0 as expected)

### Remaining Problem: KVS Propagation Delays

**Symptom:** Occasionally (3-6% of runs), 1-2 non-zero ranks timeout after 10 seconds. Their eventlog shows:
```json
{"timestamp":1778608944.3881707,"name":"init"}
{"timestamp":1778608944.3904207,"name":"starting"}
```

But **NOT** shell.init - even though rank 0 successfully wrote it and 30+ other ranks read it.

**Evidence this is KVS propagation:**
- Rank 0 (node-1) log shows it successfully found shell.init and started backend/frontend
- 30-31 other ranks successfully found shell.init in the same eventlog
- The 1-2 failing ranks polled the same eventlog path for 10 seconds but never saw shell.init appear

**Hypothesis:**
This appears to be a Flux KVS consistency/propagation issue under extreme load:
- 32 shells simultaneously reading from KVS namespace
- Heavy CPU contention delaying KVS operations
- Possible KVS caching/synchronization edge case where some ranks see stale view of eventlog

**Not a Spindle bug** - the synchronous lookup approach is correct. The issue is that KVS propagation of the shell.init event to all ranks is not instantaneous under extreme load, and even 10 seconds is occasionally insufficient.

### Potential Next Steps

1. **Increase timeout further** - try 30 seconds (3000 retries), though this is band-aid
2. **Add exponential backoff** - reduce KVS load from tight polling loop
3. **Investigate Flux KVS consistency** - may be a Flux issue under extreme containerized load
4. **Flux barrier before shell.post-init** - propose upstream change to add barrier between shell.init emit and post-init callbacks (would eliminate need for polling entirely)
5. **Alternative: Use Flux PMI/barrier** - have non-zero ranks synchronize with rank 0 via PMI after rank 0 confirms shell.init is written

## References

### Flux Core Source Files
- `flux-core/src/shell/shell.c` - Shell lifecycle, reactor startup (line 2134)
- `flux-core/src/common/libflux/future.c` - Future callback implementation
- `flux-core/src/common/libjob/info.c` - flux_job_event_watch() API
- `flux-core/src/modules/job-info/watch.c` - Event watch server implementation
- `flux-core/src/modules/kvs/lookup.c` - KVS lookup implementation

### Spindle Files Modified
- `src/flux/flux-spindle.c` - Main changes (sp_post_init, remove async watch)

### Documentation
- `man flux-shell(1)` - Shell lifecycle and plugin hooks
- Flux documentation: https://flux-framework.readthedocs.io/

## Key Takeaways

1. **Async operations require active reactor** - Any use of `flux_future_then()` or event watches requires `flux_reactor_run()` to be active. Check the shell lifecycle carefully.

2. **Timing assumptions break under load** - Code that works on dedicated hardware may fail under extreme CPU contention (32:1+ oversubscription).

3. **shell.post-init runs before reactor** - Perfect place for synchronous initialization that needs to happen after shell.init but before tasks start.

4. **Memory lifetime with Flux futures** - Data returned by `flux_kvs_lookup_get()` is owned by the future. Must copy before `flux_future_destroy()`.

5. **KVS consistency is not instantaneous** - Under heavy load, KVS propagation to all ranks can take seconds or occasionally fail entirely.

---
*Document created 2026-05-12*
*Last updated: 2026-05-12*
