# Testsuite Random Filename False Positive Fix

## Problem Detection

During extensive testing on GitHub Actions CI (32-node containerized Flux clusters, 50 repetitions per run, 6 runs = 300 total iterations), a single test failure occurred with the error:

```
Error: Read shared object from outside cache: /dev/shm/mpich_shar_tmptUBbin
```

The test reported:
```
Job completed: JobWaitResult(jobid=364938002432, success=False, errstr=b'task(s) Unknown signal 127')
Test dependency_push FAILED (38.1s)
```

This occurred in iteration 50, with a failure rate of approximately 1 in 300 (0.3%).

## Investigation

### Initial Observations

Examining the spindle output logs revealed:
1. All 32 ranks successfully initialized Spindle
2. No ERROR messages in standard error output
3. Four instances of the same error message, all on node-3:
   ```
   Error: Read shared object from outside cache: /dev/shm/mpich_shar_tmptUBbin
   ```

### Key Discovery

Comparing the **failing** node (node-3) logs with **working** nodes (e.g., node-4) revealed:

**Working case (node-4):**
```
36668 open("/dev/shm/mpich_shar_tmpdrF572", O_RDONLY) = 14
36680 open("/dev/shm/mpich_shar_tmpQia9ge", O_RDONLY) = 14  
36692 open("/dev/shm/mpich_shar_tmp9qU99V", O_RDONLY) = 14
```
None of these filenames triggered errors.

**Failing case (node-3):**
```
37035 open("/dev/shm/mpich_shar_tmptUBbin", O_RDONLY) = 14
Error: Read shared object from outside cache: /dev/shm/mpich_shar_tmptUBbin
```

The critical difference: **The failing filename contains the substring "bin"** (`tmptUBbin`), while the working filenames do not.

### Understanding the Test Verifier

The test verifier (`src/logging/spindle_logd.cc`) validates that shared objects are loaded from Spindle's cache. The relevant code (lines 267-271):

```cpp
if (strstr(filename, ".so") == NULL &&
    strstr(filename, "retzero") == NULL &&
    strstr(filename, "bin") == NULL &&          // Line 269
    strstr(filename, ".py") == NULL)
    return true;
```

This check attempts to identify files that should be verified (shared libraries, binaries, Python files). Files matching these patterns proceed to validation (lines 281-285):

```cpp
if (!is_from_temp && !is_local_test && ret_code != -1 && strstr(filename, "libc.so") == NULL) {
    std::string msg = std::string("Error: Read shared object from outside cache: ") + std::string(filename) + "\n";
    logerror(msg);
    return false;
}
```

### Root Cause

**The bug:** The test uses `strstr(filename, "bin")` to identify binary files, intending to match paths like `/bin/ls`, `/usr/bin/cat`, or `*/bin/*`. However, this also matches any filename that **happens to contain "bin" as a substring**, regardless of whether it's a path component.

**MPICH behavior:** MPICH creates temporary shared memory segments in `/dev/shm/` with randomly generated filenames following the pattern `mpich_shar_tmp<random_suffix>`. These are used for inter-process communication and are legitimately local to each node - they should not be distributed via Spindle.

**The race:** When MPICH's random suffix happens to contain "bin" (e.g., `tmptUBbin`, `XYZbinary`, `binFoo`), the verifier incorrectly flags it as a binary that should have been loaded from cache.

**Probability:** The random suffix is 6 characters from a set that includes lowercase letters. The probability of "bin" appearing as a substring is low but non-zero, explaining the ~0.3% (1 in 300) failure rate.

### Why `/dev/shm/` Files Are Correctly Handled by Spindle

Spindle's client code (`src/client/client/should_intercept.c`) correctly identifies `/dev/shm/` files as "local files" and does not intercept them:

```
[Client.2.56@should_intercept.c:156] open_filter - Not intercepting open of /dev/shm/mpich_shar_tmptUBbin, because local file
```

The runtime behavior is correct - only the test verifier has a bug.

## The Fix

Change the substring check from `"bin"` to `"/bin"` on line 269 of `src/logging/spindle_logd.cc`:

**Before:**
```cpp
if (strstr(filename, ".so") == NULL &&
    strstr(filename, "retzero") == NULL &&
    strstr(filename, "bin") == NULL &&
    strstr(filename, ".py") == NULL)
    return true;
```

**After:**
```cpp
if (strstr(filename, ".so") == NULL &&
    strstr(filename, "retzero") == NULL &&
    strstr(filename, "/bin") == NULL &&
    strstr(filename, ".py") == NULL)
    return true;
```

## Why This Fix Works

The verifier processes `open()` calls logged with **full paths** (as passed to the dynamic linker or open system call). Examples from actual logs:
- `/dev/shm/mpich_shar_tmpdrF572` (absolute path)
- `/usr/lib64/libmpi.so` (absolute path)
- `/bin/ls` (absolute path)

By checking for `"/bin"` instead of `"bin"`, we match actual binary directories:
- `/bin/` - system binaries
- `/usr/bin/` - user binaries  
- `/usr/local/bin/` - local binaries
- Paths ending in `/bin` (e.g., `/opt/software/bin`)

While avoiding false positives on filenames that merely contain "bin":
- `mpich_shar_tmptUBbin` - MPICH shared memory
- `libcombine.so` - hypothetical library
- Random filenames with "bin" substring

## Edge Cases

The only potential edge case would be a bare filename `bin` (no path separators), but:
1. Such files would be extremely unusual as shared libraries or executables
2. The dynamic linker typically provides full paths to `la_objsearch()`
3. Even if this occurred, it would be a legitimate binary to verify

## Verification

This fix was identified through analysis of logs from 300 test iterations. The single failure showed the exact pattern predicted by this root cause analysis. The fix is minimal, targeted, and preserves the original intent of catching binaries that should have been cached while eliminating false positives from random filename generation.

---
*Document created 2026-05-13*
