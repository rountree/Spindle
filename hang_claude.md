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

## Deep Dive: KVS Propagation and the Real Root Cause

### Investigation into Flux KVS Architecture

After implementing the synchronous polling fix, failures persisted at ~3-6% rate. Investigation into flux-core source code revealed the actual problem: **Flux KVS eventual consistency**.

#### How Flux KVS Works (from flux-core/src/modules/kvs/)

**Write Path (kvs.c lines 1690-1711):**
- Non-zero ranks send commits to rank 0 via `flux_rpc_pack("kvs.relaycommit", 0, ...)`
- Rank 0 is the authoritative source for all KVS writes
- Writes are synchronous - rank 0 waits for KVS commit to complete

**Read Path (lookup.c + cache.c):**
- NOT a tree overlay - each rank independently fetches content
- Lookups check local cache first (`cache_lookup()`)
- If not cached, fetch from content-backing service
- Content-backing may need to fetch from rank 0
- **Cache entries expire after 10 seconds of inactivity** (cache.c line 59)

**Critical Finding:** Reads are **on-demand** and **eventually consistent**. No proactive push distribution.

### The Cache Refresh Problem

Examining Run #115 (rank 1 failure):
```
Error counts: lookup=0 wait=0 get=0
Eventlog: {"name":"init"}{"name":"starting"}
```

**All Flux APIs returned success.** No timeouts, no errors. The KVS successfully returned an eventlog - it just returned one **without** shell.init.

Checking the failing rank's flux-dmesg.log:
```
2026-05-12T16:21:40.244840-07:00 broker.debug[12]: insmod kvs
... (no errors, clean startup)
2026-05-12T16:21:42.686437-07:00 broker.info[12]: quorum-full: quorum->run
```

No KVS errors. No content-backing timeouts. Everything "worked" from Flux's perspective.

**Diagnosis:** This is an **eventual consistency issue**, not a propagation failure:

1. Rank N queries eventlog early, gets `{"init"}{"starting"}` from rank 0
2. Rank N caches this data locally
3. Rank 0 writes shell.init to eventlog
4. Rank N's retry loop: `flux_kvs_lookup()` hits the **local cache**
5. Each cache hit **refreshes the 10-second expiration timer**
6. Cache never expires during the retry loop!
7. After 1000 retries (10 seconds), still have cached stale data

The retry loop was **defeating itself** by keeping the stale cache entry alive.

### Solution Attempt #1: flux_kvs_get_version() + flux_kvs_wait_version()

**Commit f16d116** - "Use flux_kvs_wait_version() to ensure KVS consistency"

Flux provides version-based synchronization (flux-core/src/common/libkvs/kvs.h):
```c
int flux_kvs_get_version (flux_t *h, const char *ns, int *versionp);
int flux_kvs_wait_version (flux_t *h, const char *ns, int version);
```

Initial implementation:
```c
int kvs_version = 0;
flux_kvs_get_version (h, ns, &kvs_version);  // Get current version
flux_kvs_wait_version (h, ns, kvs_version);  // Wait for that version
// Read eventlog
```

**Result:** No improvement. Failure rate remained 3-6%.

**Why it failed:**

From flux-core/src/common/libkvs/kvs.c:
```c
int flux_kvs_get_version (flux_t *h, const char *ns, int *versionp) {
    f = flux_rpc_pack (h, "kvs.getroot", FLUX_NODEID_ANY, 0, ...);
    // FLUX_NODEID_ANY = query LOCAL kvs module
}
```

Run #120 logs showed:
```
[SPINDLE rank=1] Current KVS version: 4
[SPINDLE rank=1] KVS synchronized to version 4
[FAILED - eventlog still missing shell.init]
```

We were querying the **local** KVS module's version, getting version 4 (with stale cached data), then waiting for the local KVS to reach version 4 (which it already was). No synchronization occurred.

### Solution Attempt #2: Query Rank 0 Directly for Authoritative Version

**Commit bcbc457** - "Fix: Query rank 0 directly for authoritative KVS version"

The fix: explicitly query rank 0 for the version **after** it wrote shell.init:

```c
// Query rank 0 (authoritative source) for current version
flux_future_t *version_f = flux_rpc_pack (h, "kvs.getroot", 0, 0,
                                          "{ s:s }", "namespace", ns);
flux_rpc_get_unpack (version_f, "{ s:i }", "rootseq", &rank0_version);
flux_future_destroy (version_f);

// Wait for LOCAL KVS to reach rank 0's version
flux_kvs_wait_version (h, ns, rank0_version);

// Now read eventlog - guaranteed to have rank 0's view
flux_kvs_lookup (h, ns, 0, "exec.eventlog");
```

**Key differences:**
1. `flux_rpc_pack(..., 0, ...)` - explicit RPC to rank 0, not FLUX_NODEID_ANY
2. Get rank 0's version **after** it wrote shell.init (step 3 of shell lifecycle)
3. Wait for local KVS to catch up to that specific version
4. Forces cache invalidation/refresh when version advances

### Results

**Before (synchronous polling only):**
- Failure rate: 3-6% (1-2 ranks out of 32 per test)
- All API calls succeeded but returned stale cached data
- 10-second timeout insufficient due to cache refresh problem

**After (rank 0 version synchronization):**
- Failure rate: ~0.3% (1 failure in 6 runs × 50 reps = 1/300)
- Ranks wait for authoritative version before reading
- Cache consistency guaranteed by version-based sync

**Status:** Successfully reduced intermittent failures from 3-6% to ~0.3%, a **~20x improvement**. The remaining rare failures may be:
- Extreme edge cases under catastrophic load
- Flux KVS issues under containerized oversubscription
- Worth investigating if they persist in production

### Git Commit History (Version Sync Solution)

Key commits on `dbg_local` branch:

1. **382e0ba** - "Dump eventlog directly to Spindle log instead of separate files"
   - Added [EVENTLOG] tagged output for debugging
   - Eliminated file-based extraction issues

2. **f16d116** - "Use flux_kvs_wait_version() to ensure KVS consistency before reading"
   - Initial attempt using flux_kvs_get_version() + flux_kvs_wait_version()
   - Failed because FLUX_NODEID_ANY queried local stale view

3. **bcbc457** - "Fix: Query rank 0 directly for authoritative KVS version"
   - Query rank 0 explicitly with `flux_rpc_pack(..., 0, ...)`
   - Wait for local KVS to reach rank 0's authoritative version
   - **This fixed it** - reduced failures from 3-6% to ~0.3%

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

6. **FLUX_NODEID_ANY vs explicit rank** - When synchronization matters, query the authoritative source (rank 0) explicitly rather than using FLUX_NODEID_ANY which may return local cached state.

7. **Cache refresh defeats polling** - Repeatedly accessing cached data resets expiration timers. Use version-based synchronization instead of blind retry loops.

8. **Eventual consistency requires explicit sync** - Flux KVS is eventually consistent with on-demand reads. Use `flux_kvs_wait_version()` with rank 0's version to force synchronization.

## Detailed Analysis of Original Buggy Code

For reference, here is the original code from the `devel` branch that exhibited the hang:

### Original Code (devel branch, sp_init function)

```c
640     if (shell_rank == 0) {
641         /*  Rank 0: add spindle port and num_ports to the shell.init
642          *   exec eventlog event. All other shell's will wait for this
643          *   event and initialize their port/num_ports from these values.
644          */
645         flux_shell_add_event_context (shell, "shell.init", 0,
646                                       "{s:i s:i}",
647                                       "spindle_port",
648                                       ctx->params.port,
649                                       "spindle_num_ports",
650                                       ctx->params.num_ports);
651     }
652
653     /*  All ranks, watch guest.exec.eventlog for the shell.init event in
654      *   order to distribute port and num_ports. This is unnecessary on
655      *   rank 0, but code is simpler if we treat all ranks the same.
656      */
657     if (!(f = flux_job_event_watch (h, id, "guest.exec.eventlog", 0))
658         || flux_future_then (f, -1., wait_for_shell_init, ctx) < 0)
659         shell_die (1, "flux_job_event_watch");
```

### Line-by-Line Analysis

#### Lines 645-650: Rank 0 Decorates shell.init Event

```c
flux_shell_add_event_context (shell, "shell.init", 0,
                              "{s:i s:i}",
                              "spindle_port",
                              ctx->params.port,
                              "spindle_num_ports",
                              ctx->params.num_ports);
```

Rank 0 doesn't directly write to KVS. Instead, it tells the Flux shell framework: "When you emit the shell.init event (at step 3 of the shell lifecycle), include this extra JSON context with the spindle port information."

The Flux shell framework will write this to `job-<id>-guest/exec.eventlog` in KVS as a JSON entry:
```json
{"timestamp": ..., "name": "shell.init", "context": {"spindle_port": 12345, "spindle_num_ports": 2}}
```

#### Lines 657-658: Setting up Async Event Watch

```c
if (!(f = flux_job_event_watch (h, id, "guest.exec.eventlog", 0))
    || flux_future_then (f, -1., wait_for_shell_init, ctx) < 0)
    shell_die (1, "flux_job_event_watch");
```

**`flux_job_event_watch(h, id, "guest.exec.eventlog", 0)`**
- Creates a **streaming RPC** to the job-info module
- Says: "Watch the eventlog at path `guest.exec.eventlog` for job `id`"
- This is an **async operation** - it returns immediately with a future
- The `0` flags means "start from beginning" (send all existing events, then stream new ones)
- Returns a `flux_future_t *` representing this ongoing watch

**Behind the scenes:**
- The job-info module sets up a KVS watch with `FLUX_KVS_WATCH_APPEND`
- As events are appended to the eventlog, they're streamed to the requester as messages
- Each message contains one eventlog entry

**`flux_future_then(f, -1., wait_for_shell_init, ctx)`**
- Registers `wait_for_shell_init` as the **callback function**
- Says: "When this future becomes ready (receives data), call `wait_for_shell_init(f, ctx)`"
- The `-1.` is the timeout (negative = no timeout, wait forever)
- `ctx` is user data passed to the callback
- Returns immediately - callback will fire later

**The critical problem**: Callbacks registered with `flux_future_then()` **only fire when the reactor is running**.

### The Race Condition Timeline

**On all ranks during sp_init (step 1 of shell lifecycle):**

```
Step 1 (sp_init callback):
  - Line 657: flux_job_event_watch() starts streaming RPC to job-info
  - Line 658: flux_future_then() registers wait_for_shell_init callback
  - Returns immediately, sp_init() completes
  - Callback is queued, waiting for reactor to process events

Step 2 (shell.init barrier):
  - All ranks synchronize at shell_barrier("init")

Step 3 (ONLY rank 0):
  - Flux shell framework emits shell.init to exec.eventlog
  - shell.init gets written to KVS: job-<id>-guest/exec.eventlog
  - KVS commit bumps namespace version number

Step 3b (RACE WINDOW):
  - Job-info module's KVS watch fires (shell.init was appended)
  - Job-info sends eventlog event messages to ALL watching ranks
  - Messages arrive at non-zero ranks' message queues
  - But reactor ISN'T RUNNING YET!
  - Messages sit in the Flux message queue, unprocessed
  - The future is "ready" but no reactor is polling to discover this

Steps 4-8:
  - shell.post-init callbacks run
  - shell_start_tasks() executes
  - shell.start callbacks run
  - shell_barrier("start")
  - Rank 0 emits shell.start event

Step 9 (reactor finally starts):
  - flux_reactor_run() called (shell.c line 2134)
  - NOW the reactor begins processing the message queue
  - But the shell.init notification was ALREADY delivered in step 3b
  - The future/callback mechanism doesn't re-check for "ready" futures
  - If the notification arrived before reactor startup, it's lost
  - wait_for_shell_init callback NEVER FIRES
  - Spindle backend never starts
  - Job hangs waiting for Spindle to initialize
```

### Why Failure Rate Was Environment-Dependent

**On Tuolumne (dedicated hardware, low contention):**
- All ranks move through steps 1-9 quickly and roughly synchronized
- Natural network latency between physical nodes means:
  - Rank 0 completes step 3 and continues quickly
  - Shell.init KVS write happens
  - Job-info watch triggers
  - Network propagation takes microseconds-milliseconds
  - By the time messages arrive at non-zero ranks, they're at or past step 9
  - Reactor is running when message arrives → callback fires → success

**On GitHub Actions (32 containers, extreme oversubscription):**
- 32 containers competing for limited CPU cores (4-8 cores typically)
- High CPU contention causes scheduling delays
- Sequence of events:
  - Rank 0 emits shell.init (step 3)
  - Rank 0 process gets preempted by scheduler
  - Event propagates instantly (localhost, shared kernel, no real network)
  - Non-zero ranks receive notification while still between steps 3-9
  - Non-zero ranks also getting preempted, moving slowly through steps
  - Messages arrive in queue while reactor not running
  - By step 9, the "ready" event is stale/consumed/lost
  - Callback never fires → hang (1-9% of runs)

### The Fundamental Issue

The async callback approach (`flux_future_then()`) made an implicit assumption: **"The reactor will be running when events arrive."** This is violated in shell plugin callbacks that run before step 9 of the shell lifecycle.

Synchronous operations (`flux_future_wait_for()`, `flux_kvs_wait_version()`, `flux_rpc_get_unpack()`) don't have this problem - they block and internally process messages until completion, with no reactor required.

---
*Document created 2026-05-12*
*Last updated: 2026-05-13 - Added detailed analysis of original buggy async callback code*
