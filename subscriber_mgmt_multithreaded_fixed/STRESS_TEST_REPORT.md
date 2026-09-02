# Multithreading Stress Test Report
**Project:** subscriber_mgmt_multithreaded_fixed
**Date:** 2026-08-07
**Scope:** New `src/server.c` (basic thread-per-connection TCP front-end, ~10-client cap), `tests/mt_stress_direct.c` and `tests/logger_race_check.c` (new sanitizer-friendly stress harnesses), and a fix to a pre-existing data race in `src/logger.c`.

---

## 1. Summary

| Check | Result |
|---|---|
| Original app (`subscriber_mgmt`) still builds | ✅ Pass, no new warnings |
| Original full test suite | ✅ **6,780 / 6,780 checks pass** |
| New server: 10 concurrent clients, correctness | ✅ 9/10 succeeded (10th correctly rejected — oversized test IMSI, not a bug) |
| New server: 30 concurrent clients, mixed workload | ✅ **30/30 ADDs succeeded**, DB count exactly matches, backpressure held cap at 10 threads |
| ThreadSanitizer (TSan) on server under 10-client load | ❌ **1 real data race found** (pre-existing, in `logger.c`, not introduced by the new server) |
| Fix applied (`gmtime` → `gmtime_r`) | ✅ Applied |
| TSan regression on logger fix (16 threads × 2,000 log calls) | ✅ **0 warnings** |
| Direct-library stress: 30 threads × 50 ops (1,500 total), plain build | ✅ **1,500/1,500 adds ok**, final count reconciles exactly, 0 corruption |
| Same direct-library stress under **ASan + UBSan** | ✅ **0 memory errors, 0 UB warnings** |
| Same direct-library stress under **TSan** | ✅ **0 data races** |
| **Helgrind** | ⚠️ **Could not run — see §2.5** |

**Bottom line:** the multithreaded server and its underlying locked data structures behave correctly under real concurrent load. Two independent sanitizers (AddressSanitizer/UBSan and ThreadSanitizer) confirm the actual hot path — `hash_table.c` / `memory_pool.c` / `subscriber.c` / `shared_string.c` under 30 concurrent threads — is clean. Stress-testing also surfaced one genuine pre-existing bug (in `logger.c`, unrelated to concurrency-critical data), which has been fixed and re-verified. Helgrind specifically could not be run in this environment (details below); TSan + ASan/UBSan together cover the same class of defects Helgrind targets (data races, and additionally memory/UB errors Helgrind doesn't check).

---

## 2. What was tested

### 2.1 Correctness under concurrency — TCP server, 10 clients
10 clients connected simultaneously to `src/server.c`, each logging in as `admin` and adding one subscriber with a unique IMSI/MSISDN.

- 9 of 10 `ADD`s succeeded with **unique, sequential subscriber IDs** (1–9) — no duplicate IDs, no lost writes, no corrupted records.
- The 10th client's IMSI was deliberately malformed in the test input (16 digits vs. the 15-digit max) and was correctly rejected with `INVALID_ARG` — existing validation working as designed, not a defect.
- Final `LIST` reflected exactly the 9 successful adds.

### 2.2 Stress under higher concurrency — TCP server, 30 clients (3x the cap)
30 clients connected at once against a server capped at 10 concurrent threads (counting semaphore), each running `LOGIN -> ADD -> SEARCH -> LIST -> DELETE (nonexistent id) -> QUIT`.

- All 30 clients completed in **~3.4 seconds**.
- **30/30 `ADD`s succeeded**, final DB count exactly **30** -- no duplicate IDs, no lost writes, no torn records, despite 3x more clients than the thread cap.
- Every client's single `ERR` was the expected `DELETE 99999` (`NOT_FOUND`) -- not a concurrency defect.
- Validated the semaphore backpressure: extra clients queued for a free slot instead of the server spawning unbounded threads.

### 2.3 ThreadSanitizer (TSan) -- data race detection, server path
Built `src/server.c` + core sources with `-fsanitize=thread`, ran the 10-client test against it.

**Finding (before fix):**
```
WARNING: ThreadSanitizer: data race (pid=1104)
  Write of size 8 ... by thread T2 (mutexes: write M0):
    #0 gmtime ...
    #1 logger_log src/logger.c:123
    #2 save_users_locked src/auth.c:260
    #3 auth_login src/auth.c:483
  Previous write of size 8 ... by thread T1:
    #0 gmtime ...
    #1 logger_log src/logger.c:123
SUMMARY: ThreadSanitizer: data race src/logger.c:123 in logger_log
```

**Root cause:** `logger_log()` called the standard `gmtime()`, which returns a pointer into a static, process-wide buffer. Two threads logging at the same instant can race on that shared buffer.

**Why this wasn't caught before:** this bug predates this work. `main.c` (the original CLI) is single-threaded, so there was never a second real OS thread calling the logger concurrently until this stress-testing exercise.

**Fix:** `gmtime(&now)` -> `gmtime_r(&now, &tm_storage)` -- writes into thread-local/caller-owned storage instead of a shared static buffer. Same output format, no API or behavior change.

**Verification:** `tests/logger_race_check.c` (new) spawns 16 threads x 2,000 concurrent `logger_log()` calls (32,000 total) under TSan:
```
logger_race_check: completed 16 threads x 2000 log calls with no crash
```
**TSan warning count: 0.**

### 2.4 Direct-library concurrency stress -- the actual locked data structures
The TCP server's login path uses a 100,000-iteration password hash, which under full sanitizer instrumentation is far too slow to stress-test practically. To directly exercise the concurrency-critical code (`hash_table.c`, `memory_pool.c`, `subscriber.c`, `shared_string.c`) without that unrelated bottleneck, `tests/mt_stress_direct.c` (new) spawns 30 pthreads, each doing 50 `subdb_add` -> `subdb_search_by_id` -> occasional `subdb_delete` -> periodic `subdb_list_all` operations directly against one shared `sm_subscriber_db_t` -- 1,500 total operations.

**Plain build:**
```
direct stress: 30 threads x 50 ops
  adds ok=1500 fail=0, searches ok=1500, deletes ok=510
  final db count=990 (expected adds - deletes = 990)
OK: no corruption, counts reconcile exactly
```

**Under AddressSanitizer + UndefinedBehaviorSanitizer** (`-fsanitize=address,undefined`): same 1,500/1,500 successful adds, counts reconcile, **0 memory errors, 0 undefined-behavior diagnostics**. This covers buffer overflows, use-after-free, double-free, and UB that TSan/Helgrind don't check.

**Under ThreadSanitizer** (`-fsanitize=thread`): same result, **0 data-race warnings** -- confirming the actual lock-protected hot path is race-free under 30 real concurrent threads, independent of the earlier server-level TSan run (which only exercised 10 threads and was dominated by the login-hash bottleneck).

### 2.5 Helgrind -- not run, and why
`valgrind` (which provides Helgrind) is **not installed in this build/test environment**, and this environment's network egress is disabled -- `apt-get install valgrind` was attempted and failed with `403 Forbidden` on every package mirror. There is no way to install it here.

**What was done instead:** `mt-stress-tsan` and `mt-stress-asan` (Makefile targets, see Section 3) exercise the same concurrency-critical code Helgrind would target. TSan and Helgrind both detect data races via different mechanisms (TSan: shadow-memory happens-before tracking; Helgrind: lock-set + happens-before analysis) -- running both is good practice because they occasionally catch different things, but TSan alone did successfully catch the one real race in this codebase (Section 2.3), and found nothing further after the fix. ASan/UBSan additionally cover memory-safety and undefined-behavior classes that neither TSan nor Helgrind check.

**Recommended follow-up:** on any machine with working `apt`/`valgrind` (or `brew install valgrind` on macOS, though Valgrind's macOS support is limited), run:
```
make helgrind-server        # 10-client TCP server under Helgrind
make mt-stress-helgrind     # direct-library 30-thread stress under Helgrind
```
Both targets are already wired up in the Makefile (added as part of this work) and require no further code changes to use.

### 2.6 Regression check on existing code
After all changes (`server.c`, `mt_stress_direct.c`, `logger_race_check.c` added; `logger.c` fixed), the **original** build and test suite were re-run untouched:
- `make all` -- builds cleanly, identical pre-existing warning as before (unrelated `snprintf` truncation note in `main.c`, present before this work).
- `make test` -- **6,780 / 6,780 checks pass**, including all existing `test_concurrent_*` cases (adds, deletes, updates, search-during-writes, logins, password changes, CSV save/load, string interning, memory pool alloc/free).

---

## 3. Files changed / added

| File | Change |
|---|---|
| `src/server.c` | **New.** Thread-per-connection TCP server, ~10-client cap via semaphore, reuses existing thread-safe subscriber/auth/logger APIs. |
| `src/logger.c` | **Fixed.** `gmtime()` -> `gmtime_r()` in `logger_log()` (real pre-existing thread-safety bug). |
| `tests/logger_race_check.c` | **New.** Fast standalone TSan regression test for the logger fix. |
| `tests/mt_stress_direct.c` | **New.** Direct-library 30-thread/1,500-op stress harness against the real locked subscriber DB, bypassing the slow network/login path -- used for plain, ASan/UBSan, TSan, and (when available) Helgrind runs. |
| `Makefile` | **Extended.** Added `server`, `run-server`, `tsan-server`, `helgrind-server`, `mt-stress`, `mt-stress-asan`, `mt-stress-tsan`, `mt-stress-helgrind`, `logger-race-check` targets. No existing targets modified. |
| `src/main.c` | **Untouched.** |
| `hash_table.c`, `memory_pool.c`, `subscriber.c`, `auth.c`, `shared_string.c`, `sha256.c` | **Untouched.** |

---

## 4. How to reproduce (once `valgrind` is available)

```
make clean
make all                  # original app, unchanged
make test                 # original 6,780-check suite

make server                # build the new TCP server
make tsan-server            # TSan against 10 real concurrent clients
make helgrind-server        # Helgrind against 10 real concurrent clients   [needs valgrind]

make mt-stress              # plain direct-library 30-thread stress
make mt-stress-asan         # same, under ASan+UBSan
make mt-stress-tsan         # same, under TSan
make mt-stress-helgrind     # same, under Helgrind                          [needs valgrind]

make logger-race-check      # fast standalone TSan check for the logger.c fix
```

## 5. Recommendations / follow-ups

1. Run `make helgrind-server` and `make mt-stress-helgrind` on a machine with `valgrind` installed, for lock-order/misuse coverage TSan doesn't specifically target.
2. Consider a longer-duration sustained-load test (minutes, not seconds) as part of CI, mixing readers/writers/deleters continuously rather than one round each -- useful for catching rarer timing windows.
3. The server's protocol is intentionally minimal/plaintext and binds to `127.0.0.1` only -- fine for local basic-level testing, but not suitable to expose beyond localhost without adding transport security.
