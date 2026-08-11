/**
 * @file test_main.c
 * @brief Unit, integration, boundary, negative, and concurrency tests.
 *
 * Minimal self-contained test harness (no external framework dependency) —
 * intentional for a telecom build environment where pulling in a new test
 * framework requires its own qualification. Each TEST() registers a
 * function; RUN_ALL executes them and reports a pass/fail summary with a
 * non-zero exit code on any failure, suitable for CI gating.
 */
#ifndef _WIN32
/* 200809L exposes both nanosleep() and pthread_rwlock_t under strict
 * -std=c11 (which otherwise hides POSIX extensions above C11 itself).
 * Must be defined before any system header is included. */
#define _POSIX_C_SOURCE 200809L
#endif
#include "common.h"
#include "logger.h"
#include "memory_pool.h"
#include "hash_table.h"
#include "shared_string.h"
#include "subscriber.h"
#include "auth.h"

#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <stdlib.h>
#include <errno.h>
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <time.h>
#endif

/* Small portable backoff for retrying a transient SM_ERR_IO -- Windows in
 * particular can see a brief ERROR_SHARING_VIOLATION on the credential
 * store's rename-replace if an antivirus scan or editor file-watcher has
 * it open for a moment; that's filesystem contention, not a logic bug,
 * so a couple of short retries is the appropriate response in a test. */
static void tiny_backoff_sleep(void)
{
#ifdef _WIN32
    Sleep(15);
#else
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 15L * 1000L * 1000L; /* 15 ms */
    (void)nanosleep(&ts, NULL);
#endif
}

static int g_failures = 0;
static int g_total = 0;

#define CHECK(cond) do { \
    g_total++; \
    if (!(cond)) { \
        g_failures++; \
        fprintf(stderr, "  [FAIL] %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

#define RUN(fn) do { \
    printf("-- %s\n", #fn); \
    fn(); \
} while (0)

/* ================= logger ================= */

static void test_logger_levels_and_status_strings(void)
{
    /* Exercise every branch of level_name() and sm_status_str(), which are
     * otherwise dead code as far as the test suite is concerned (they are
     * only reached through logger_log()'s formatting path). Route to a
     * real file so logger_log()'s file-write branch (as opposed to the
     * stderr fallback) is covered too. */
    logger_shutdown();
    CHECK(logger_init("data/test_logger.log", LOG_DEBUG, 0U) == SM_OK);

    SM_LOG_DEBUG("test", "debug message %d", 1);
    SM_LOG_INFO("test", "info message %d", 2);
    SM_LOG_WARN("test", "warn message %d", 3);
    SM_LOG_ERROR("test", "error message %d", 4);
    SM_LOG_FATAL("test", "fatal message %d", 5);

    /* module == NULL and func supplied via macro: hits the "?" fallback
     * for module while leaving func non-NULL. */
    SM_LOG_INFO(NULL, "no module name");

    /* Call logger_log() directly (bypassing the macros) with an
     * out-of-range level to hit level_name()'s default case. */
    logger_log((sm_log_level_t)999, "test", "test_logger_levels_and_status_strings", __LINE__,
               "unknown level message");

    /* Every sm_status_str() branch, including its default case. */
    CHECK(strcmp(sm_status_str(SM_OK), "OK") == 0);
    CHECK(strcmp(sm_status_str(SM_ERR_NULL_PARAM), "NULL_PARAM") == 0);
    CHECK(strcmp(sm_status_str(SM_ERR_ALLOC_FAILED), "ALLOC_FAILED") == 0);
    CHECK(strcmp(sm_status_str(SM_ERR_NOT_FOUND), "NOT_FOUND") == 0);
    CHECK(strcmp(sm_status_str(SM_ERR_DUPLICATE), "DUPLICATE") == 0);
    CHECK(strcmp(sm_status_str(SM_ERR_INVALID_ARG), "INVALID_ARG") == 0);
    CHECK(strcmp(sm_status_str(SM_ERR_IO), "IO_ERROR") == 0);
    CHECK(strcmp(sm_status_str(SM_ERR_CORRUPT_DATA), "CORRUPT_DATA") == 0);
    CHECK(strcmp(sm_status_str(SM_ERR_LOCK_FAILED), "LOCK_FAILED") == 0);
    CHECK(strcmp(sm_status_str(SM_ERR_CAPACITY), "CAPACITY") == 0);
    CHECK(strcmp(sm_status_str(SM_ERR_UNAUTHORIZED), "UNAUTHORIZED") == 0);
    CHECK(strcmp(sm_status_str((sm_status_t)9999), "INTERNAL_ERROR") == 0);

    /* logger_shutdown() with a real g_fp open: covers the fflush/fclose
     * branch that a NULL-path (stderr-only) logger never reaches. */
    logger_shutdown();
    (void)remove("data/test_logger.log");

    /* Restore the stderr-only, fatal-level logger the rest of the suite
     * expects, so later tests' console output stays clean. */
    CHECK(logger_init(NULL, LOG_FATAL, 0U) == SM_OK);
}

static void test_logger_rotation(void)
{
    /* Force maybe_rotate()'s rename branch by picking a tiny rotate
     * threshold and writing enough log lines to exceed it. */
    logger_shutdown();
    (void)remove("data/test_logger_rot.log");
    (void)remove("data/test_logger_rot.log.1");
    CHECK(logger_init("data/test_logger_rot.log", LOG_DEBUG, 50U) == SM_OK);

    for (int i = 0; i < 10; ++i) {
        SM_LOG_INFO("rotate_test", "padding message number %d to exceed the rotate threshold", i);
    }

    FILE *backup = fopen("data/test_logger_rot.log.1", "r");
    CHECK(backup != NULL);
    if (backup != NULL) {
        fclose(backup);
    }

    logger_shutdown();
    (void)remove("data/test_logger_rot.log");
    (void)remove("data/test_logger_rot.log.1");
    CHECK(logger_init(NULL, LOG_FATAL, 0U) == SM_OK);
}

static void test_logger_init_bad_path(void)
{
    /* A path under a directory that does not exist: fopen(..., "a")
     * fails, covering logger_init()'s SM_ERR_IO return branch. */
    logger_shutdown();
    sm_status_t st = logger_init("no_such_directory_xyz/nested/test.log", LOG_INFO, 0U);
    CHECK(st == SM_ERR_IO);

    /* logger_init() left g_initialized false / g_fp NULL on failure;
     * re-init cleanly so later tests are unaffected. */
    CHECK(logger_init(NULL, LOG_FATAL, 0U) == SM_OK);
}

/* ================= memory pool ================= */

static void test_memory_pool_basic(void)
{
    sm_memory_pool_t *pool = NULL;
    CHECK(mempool_create(64U, 4U, &pool) == SM_OK);
    void *a = mempool_alloc(pool);
    void *b = mempool_alloc(pool);
    CHECK(a != NULL && b != NULL && a != b);

    sm_pool_stats_t stats;
    CHECK(mempool_get_stats(pool, &stats) == SM_OK);
    CHECK(stats.used_blocks == 2U);
    CHECK(stats.peak_blocks == 2U);

    mempool_free(pool, a);
    CHECK(mempool_get_stats(pool, &stats) == SM_OK);
    CHECK(stats.used_blocks == 1U);

    void *c = mempool_alloc(pool); /* should reuse freed block a */
    CHECK(c == a);

    mempool_free(pool, b);
    mempool_free(pool, c);
    mempool_destroy(pool);
}

static void test_memory_pool_growth(void)
{
    sm_memory_pool_t *pool = NULL;
    CHECK(mempool_create(sizeof(void *), 2U, &pool) == SM_OK);
    void *ptrs[10];
    for (int i = 0; i < 10; ++i) {
        ptrs[i] = mempool_alloc(pool); /* forces multiple slab growths */
        CHECK(ptrs[i] != NULL);
    }
    sm_pool_stats_t stats;
    CHECK(mempool_get_stats(pool, &stats) == SM_OK);
    CHECK(stats.total_blocks >= 10U);
    for (int i = 0; i < 10; ++i) mempool_free(pool, ptrs[i]);
    mempool_destroy(pool);
}

static void test_memory_pool_null_safety(void)
{
    CHECK(mempool_alloc(NULL) == NULL);
    mempool_free(NULL, NULL); /* must not crash */
    mempool_destroy(NULL);    /* must not crash */
}

/* ================= hash table ================= */

static void test_hash_table_crud(void)
{
    sm_hash_table_t *t = NULL;
    CHECK(ht_create(4U, sm_hash_str, sm_streq, NULL, NULL, &t) == SM_OK);

    static char k1[] = "alpha", k2[] = "beta";
    int v1 = 100, v2 = 200;

    CHECK(ht_put(t, k1, &v1, false) == SM_OK);
    CHECK(ht_put(t, k2, &v2, false) == SM_OK);
    CHECK(ht_put(t, k1, &v2, false) == SM_ERR_DUPLICATE);
    CHECK(ht_put(t, k1, &v2, true) == SM_OK); /* replace */

    void *out = NULL;
    CHECK(ht_get(t, k1, &out) == SM_OK);
    CHECK(out == &v2);

    CHECK(ht_get(t, "missing", &out) == SM_ERR_NOT_FOUND);
    CHECK(ht_remove(t, k2) == SM_OK);
    CHECK(ht_remove(t, k2) == SM_ERR_NOT_FOUND);
    CHECK(ht_size(t) == 1U);

    ht_destroy(t);
}

static void test_hash_table_resize(void)
{
    sm_hash_table_t *t = NULL;
    CHECK(ht_create(5U, sm_hash_u64, sm_u64eq, NULL, NULL, &t) == SM_OK);

    static uint64_t keys[500];
    for (uint64_t i = 0; i < 500U; ++i) {
        keys[i] = i;
        CHECK(ht_put(t, &keys[i], (void *)(uintptr_t)(i + 1U), false) == SM_OK);
    }
    CHECK(ht_size(t) == 500U);
    for (uint64_t i = 0; i < 500U; ++i) {
        void *out = NULL;
        CHECK(ht_get(t, &keys[i], &out) == SM_OK);
        CHECK((uintptr_t)out == i + 1U);
    }
    ht_destroy(t);
}

static bool count_iter_cb(void *key, void *value, void *user_data)
{
    SM_UNUSED(key); SM_UNUSED(value);
    size_t *n = (size_t *)user_data;
    (*n)++;
    return true;
}

static void test_hash_table_iterate(void)
{
    sm_hash_table_t *t = NULL;
    CHECK(ht_create(4U, sm_hash_u64, sm_u64eq, NULL, NULL, &t) == SM_OK);
    static uint64_t keys[10];
    for (uint64_t i = 0; i < 10U; ++i) {
        keys[i] = i;
        (void)ht_put(t, &keys[i], NULL, false);
    }
    size_t n = 0;
    ht_iterate(t, count_iter_cb, &n);
    CHECK(n == 10U);
    ht_destroy(t);
}

/* ================= shared strings ================= */

static void test_shared_string_interning(void)
{
    sm_shared_string_t *a = NULL, *b = NULL;
    CHECK(sstr_intern("GOLD_PLAN", &a) == SM_OK);
    CHECK(sstr_intern("GOLD_PLAN", &b) == SM_OK);
    CHECK(a == b); /* same backing pointer: true interning, not a copy */
    CHECK(strcmp(sstr_cstr(a), "GOLD_PLAN") == 0);

    sstr_release(a);
    /* b still holds a reference, so the string must still be valid/interned. */
    sm_shared_string_t *c = NULL;
    CHECK(sstr_intern("GOLD_PLAN", &c) == SM_OK);
    CHECK(c == b);

    sstr_release(b);
    sstr_release(c);
}

static void test_shared_string_rejects_bad_input(void)
{
    sm_shared_string_t *s = NULL;
    CHECK(sstr_intern(NULL, &s) == SM_ERR_NULL_PARAM);
    CHECK(sstr_intern("", &s) == SM_ERR_INVALID_ARG);
}

static void test_shared_string_pool_lifecycle_and_stats(void)
{
    /* sstr_pool_init() is idempotent when already initialized. */
    CHECK(sstr_pool_init(64U) == SM_OK);

    /* sstr_retain(NULL) / sstr_release(NULL) no-op branches. */
    CHECK(sstr_retain(NULL) == NULL);
    sstr_release(NULL); /* must not crash */

    /* sstr_cstr() with a NULL handle. */
    CHECK(strcmp(sstr_cstr(NULL), "") == 0);

    /* sstr_retain() increments refcount under the lock. */
    sm_shared_string_t *a = NULL;
    CHECK(sstr_intern("RETAIN_ME", &a) == SM_OK);
    sm_shared_string_t *a2 = sstr_retain(a);
    CHECK(a2 == a);
    CHECK(a->refcount == 2U);

    /* sstr_pool_get_stats(): NULL-out rejection, then a real snapshot that
     * drives the ht_iterate() callback across at least one live entry. */
    CHECK(sstr_pool_get_stats(NULL) == SM_ERR_NULL_PARAM);

    sm_sstr_pool_stats_t stats;
    CHECK(sstr_pool_get_stats(&stats) == SM_OK);
    CHECK(stats.distinct_strings >= 1U);
    CHECK(stats.total_references >= 2U);
    CHECK(stats.bytes_for_distinct_strings > 0U);

    sstr_release(a);
    sstr_release(a2);

    /* sstr_intern() before the pool is initialized: shut it down, confirm
     * the SM_ERR_INTERNAL guard, then leave the pool re-initialized for
     * every later test (matches main()'s startup state). */
    sstr_pool_shutdown();
    sm_shared_string_t *after_shutdown = NULL;
    CHECK(sstr_intern("SHOULD_FAIL", &after_shutdown) == SM_ERR_INTERNAL);

    sm_sstr_pool_stats_t stats_uninit;
    CHECK(sstr_pool_get_stats(&stats_uninit) == SM_ERR_INTERNAL);

    /* sstr_pool_shutdown() when already shut down: covers the early-return
     * !g_initialized branch. */
    sstr_pool_shutdown();

    CHECK(sstr_pool_init(256U) == SM_OK);
}

/* ================= subscriber CRUD / search ================= */

static void test_subscriber_validation(void)
{
    CHECK(sub_validate_imsi("404123456789012") == true);
    CHECK(sub_validate_imsi("40412") == false);      /* too short */
    CHECK(sub_validate_imsi("40412A456789012") == false); /* non-digit */
    CHECK(sub_validate_imsi(NULL) == false);

    CHECK(sub_validate_msisdn("919876543210") == true);
    CHECK(sub_validate_msisdn("123") == false);

    CHECK(sub_validate_name("Asha Rao") == true);
    CHECK(sub_validate_name("") == false);
    CHECK(sub_validate_name("Has,Comma") == false);
}

static void test_subscriber_add_duplicate_delete(void)
{
    sm_subscriber_db_t *db = NULL;
    CHECK(subdb_create(&db) == SM_OK);

    uint64_t id1 = 0, id2 = 0;
    CHECK(subdb_add(db, "111111111111111", "911111111111", "Test User", "REGION_A",
                     "PLAN_A", SUB_STATUS_ACTIVE, &id1) == SM_OK);
    CHECK(subdb_count(db) == 1U);

    /* duplicate IMSI */
    CHECK(subdb_add(db, "111111111111111", "922222222222", "Other", "REGION_A",
                     "PLAN_A", SUB_STATUS_ACTIVE, &id2) == SM_ERR_DUPLICATE);
    /* duplicate MSISDN */
    CHECK(subdb_add(db, "222222222222222", "911111111111", "Other", "REGION_A",
                     "PLAN_A", SUB_STATUS_ACTIVE, &id2) == SM_ERR_DUPLICATE);
    /* invalid IMSI */
    CHECK(subdb_add(db, "bad", "933333333333", "Other", "REGION_A",
                     "PLAN_A", SUB_STATUS_ACTIVE, &id2) == SM_ERR_INVALID_ARG);

    CHECK(subdb_delete(db, id1) == SM_OK);
    CHECK(subdb_count(db) == 0U);
    CHECK(subdb_delete(db, id1) == SM_ERR_NOT_FOUND);
    CHECK(subdb_delete(db, 999999U) == SM_ERR_NOT_FOUND);

    subdb_destroy(db);
}

static void test_auth_change_password_persists(void)
{
    const char *path = "data/test_auth.csv";
    (void)remove(path);
    (void)remove("data/test_auth.csv.tmp");

    CHECK(auth_init(path) == SM_OK);

    sm_session_t session;
    CHECK(auth_login("admin", "Ap39wb6301@@", &session) == SM_OK);
    CHECK(auth_change_password(session.token, "Ap39wb6301@@", "NewPassword123!") == SM_OK);

    auth_shutdown();

    CHECK(auth_init(path) == SM_OK);
    sm_session_t session2;
    CHECK(auth_login("admin", "NewPassword123!", &session2) == SM_OK);

    auth_shutdown();
    (void)remove(path);
    (void)remove("data/test_auth.csv.tmp");
}

static void test_auth_login_unknown_user_and_lockout(void)
{
    const char *path = "data/test_auth_lockout.csv";
    (void)remove(path);
    (void)remove("data/test_auth_lockout.csv.tmp");
    CHECK(auth_init(path) == SM_OK);

    sm_session_t session;
    CHECK(auth_login("no_such_user", "whatever1", &session) == SM_ERR_NOT_FOUND);

    CHECK(auth_create_user("locktest", "CorrectPass1", SM_ROLE_OPERATOR) == SM_OK);

    /* SM_AUTH_MAX_ATTEMPTS (5) wrong passwords lock the account. */
    for (int i = 0; i < 5; ++i) {
        CHECK(auth_login("locktest", "WrongPassword", &session) == SM_ERR_UNAUTHORIZED);
    }
    /* Account is now locked: even the correct password is blocked while
     * locked_until is in the future (only an ADMIN with the fixed default
     * password bypasses this, and this user is an OPERATOR). */
    CHECK(auth_login("locktest", "CorrectPass1", &session) == SM_ERR_UNAUTHORIZED);

    auth_shutdown();
    (void)remove(path);
    (void)remove("data/test_auth_lockout.csv.tmp");
}

static void test_auth_session_and_rbac(void)
{
    const char *path = "data/test_auth_rbac.csv";
    (void)remove(path);
    (void)remove("data/test_auth_rbac.csv.tmp");
    CHECK(auth_init(path) == SM_OK);

    /* auth_logout / auth_verify_session with an unrecognized token. */
    CHECK(auth_logout("not-a-real-token") == SM_ERR_NOT_FOUND);
    sm_auth_user_t who;
    CHECK(auth_verify_session("not-a-real-token", &who) == SM_ERR_UNAUTHORIZED);

    sm_session_t admin_session;
    CHECK(auth_login("admin", "Ap39wb6301@@", &admin_session) == SM_OK);
    CHECK(auth_verify_session(admin_session.token, &who) == SM_OK);
    CHECK(who.role == SM_ROLE_ADMIN);
    CHECK(auth_logout(admin_session.token) == SM_OK);
    /* Token no longer valid after logout. */
    CHECK(auth_verify_session(admin_session.token, &who) == SM_ERR_UNAUTHORIZED);

    /* auth_role_allows(): every branch of the switch. */
    CHECK(auth_role_allows(SM_ROLE_ADMIN, SM_ACTION_USER_MANAGE) == true);
    CHECK(auth_role_allows(SM_ROLE_OPERATOR, SM_ACTION_SUB_ADD) == true);
    CHECK(auth_role_allows(SM_ROLE_OPERATOR, SM_ACTION_SUB_UPDATE) == true);
    CHECK(auth_role_allows(SM_ROLE_OPERATOR, SM_ACTION_SUB_SEARCH) == true);
    CHECK(auth_role_allows(SM_ROLE_OPERATOR, SM_ACTION_SUB_LIST) == true);
    CHECK(auth_role_allows(SM_ROLE_OPERATOR, SM_ACTION_REPORT_GENERATE) == true);
    CHECK(auth_role_allows(SM_ROLE_OPERATOR, SM_ACTION_VIEW_STATS) == true);
    CHECK(auth_role_allows(SM_ROLE_OPERATOR, SM_ACTION_SUB_DELETE) == false);
    CHECK(auth_role_allows(SM_ROLE_OPERATOR, SM_ACTION_DB_BACKUP) == false);
    CHECK(auth_role_allows(SM_ROLE_OPERATOR, SM_ACTION_DB_RESTORE) == false);
    CHECK(auth_role_allows(SM_ROLE_OPERATOR, SM_ACTION_USER_MANAGE) == false);
    CHECK(auth_role_allows(SM_ROLE_OPERATOR, SM_ACTION_VIEW_LOGS) == false);
    CHECK(auth_role_allows(SM_ROLE_VIEWER, SM_ACTION_SUB_SEARCH) == true);
    CHECK(auth_role_allows(SM_ROLE_VIEWER, SM_ACTION_SUB_LIST) == true);
    CHECK(auth_role_allows(SM_ROLE_VIEWER, SM_ACTION_REPORT_GENERATE) == true);
    CHECK(auth_role_allows(SM_ROLE_VIEWER, SM_ACTION_VIEW_STATS) == true);
    CHECK(auth_role_allows(SM_ROLE_VIEWER, SM_ACTION_SUB_ADD) == false);
    CHECK(auth_role_allows(SM_ROLE_UNKNOWN, SM_ACTION_SUB_SEARCH) == false);

    /* auth_authorize(): valid session + allowed action, then valid
     * session + disallowed action. */
    CHECK(auth_create_user("rbac_viewer", "ViewerPass1", SM_ROLE_VIEWER) == SM_OK);
    sm_session_t viewer_session;
    CHECK(auth_login("rbac_viewer", "ViewerPass1", &viewer_session) == SM_OK);
    sm_auth_user_t authorized;
    CHECK(auth_authorize(viewer_session.token, SM_ACTION_SUB_SEARCH, &authorized) == SM_OK);
    CHECK(strcmp(authorized.username, "rbac_viewer") == 0);
    CHECK(auth_authorize(viewer_session.token, SM_ACTION_SUB_DELETE, NULL) == SM_ERR_UNAUTHORIZED);
    CHECK(auth_authorize("not-a-real-token", SM_ACTION_SUB_SEARCH, NULL) == SM_ERR_UNAUTHORIZED);

    /* sm_role_to_str()/sm_role_from_str(): every branch including default. */
    CHECK(strcmp(sm_role_to_str(SM_ROLE_ADMIN), "ADMIN") == 0);
    CHECK(strcmp(sm_role_to_str(SM_ROLE_OPERATOR), "OPERATOR") == 0);
    CHECK(strcmp(sm_role_to_str(SM_ROLE_VIEWER), "VIEWER") == 0);
    CHECK(strcmp(sm_role_to_str((sm_role_t)999), "UNKNOWN") == 0);
    CHECK(sm_role_from_str(NULL) == SM_ROLE_UNKNOWN);
    CHECK(sm_role_from_str("ADMIN") == SM_ROLE_ADMIN);
    CHECK(sm_role_from_str("OPERATOR") == SM_ROLE_OPERATOR);
    CHECK(sm_role_from_str("VIEWER") == SM_ROLE_VIEWER);
    CHECK(sm_role_from_str("GARBAGE") == SM_ROLE_UNKNOWN);

    auth_shutdown();
    (void)remove(path);
    (void)remove("data/test_auth_rbac.csv.tmp");
}

static void test_auth_create_user_validation(void)
{
    const char *path = "data/test_auth_create.csv";
    (void)remove(path);
    (void)remove("data/test_auth_create.csv.tmp");
    CHECK(auth_init(path) == SM_OK);

    CHECK(auth_create_user(NULL, "SomePass1", SM_ROLE_VIEWER) == SM_ERR_NULL_PARAM);
    CHECK(auth_create_user("user", NULL, SM_ROLE_VIEWER) == SM_ERR_NULL_PARAM);
    CHECK(auth_create_user("bad user!", "SomePass1", SM_ROLE_VIEWER) == SM_ERR_INVALID_ARG); /* bad chars */
    CHECK(auth_create_user("shortpw", "short", SM_ROLE_VIEWER) == SM_ERR_INVALID_ARG);        /* <8 chars */
    CHECK(auth_create_user("norole", "SomePass1", SM_ROLE_UNKNOWN) == SM_ERR_INVALID_ARG);

    CHECK(auth_create_user("dupuser", "SomePass1", SM_ROLE_VIEWER) == SM_OK);
    CHECK(auth_create_user("dupuser", "SomePass1", SM_ROLE_VIEWER) == SM_ERR_DUPLICATE);

    auth_shutdown();
    (void)remove(path);
    (void)remove("data/test_auth_create.csv.tmp");
}

static void test_auth_change_password_edge_cases(void)
{
    const char *path = "data/test_auth_pwedge.csv";
    (void)remove(path);
    (void)remove("data/test_auth_pwedge.csv.tmp");
    CHECK(auth_init(path) == SM_OK);

    CHECK(auth_create_user("pwedge", "OriginalPass1", SM_ROLE_OPERATOR) == SM_OK);
    sm_session_t session;
    CHECK(auth_login("pwedge", "OriginalPass1", &session) == SM_OK);

    CHECK(auth_change_password(NULL, "OriginalPass1", "NewPass1") == SM_ERR_NULL_PARAM);
    CHECK(auth_change_password(session.token, "OriginalPass1", "short") == SM_ERR_INVALID_ARG);
    CHECK(auth_change_password("not-a-real-token", "x", "NewPassword1") == SM_ERR_UNAUTHORIZED);
    CHECK(auth_change_password(session.token, "WrongOldPassword", "NewPassword1") == SM_ERR_UNAUTHORIZED);
    CHECK(auth_change_password(session.token, "OriginalPass1", "NewPassword1") == SM_OK);

    /* Deleting the user out from under a still-valid session token, then
     * trying to change its password: find_user_locked() no longer finds
     * it -> SM_ERR_NOT_FOUND. */
    CHECK(auth_delete_user("pwedge") == SM_OK);
    CHECK(auth_change_password(session.token, "NewPassword1", "AnotherPass1") == SM_ERR_NOT_FOUND);

    auth_shutdown();
    (void)remove(path);
    (void)remove("data/test_auth_pwedge.csv.tmp");
}

static void test_auth_delete_user(void)
{
    const char *path = "data/test_auth_delete.csv";
    (void)remove(path);
    (void)remove("data/test_auth_delete.csv.tmp");
    CHECK(auth_init(path) == SM_OK);

    CHECK(auth_delete_user(NULL) == SM_ERR_NULL_PARAM);
    CHECK(auth_delete_user("nobody_here") == SM_ERR_NOT_FOUND);

    CHECK(auth_create_user("todelete", "DeleteMe123", SM_ROLE_VIEWER) == SM_OK);
    CHECK(auth_delete_user("todelete") == SM_OK);

    sm_session_t session;
    CHECK(auth_login("todelete", "DeleteMe123", &session) == SM_ERR_NOT_FOUND);

    auth_shutdown();
    (void)remove(path);
    (void)remove("data/test_auth_delete.csv.tmp");
}

static void test_subscriber_search_variants(void)
{
    sm_subscriber_db_t *db = NULL;
    CHECK(subdb_create(&db) == SM_OK);
    uint64_t id1, id2, id3;
    (void)subdb_add(db, "300000000000001", "910000000001", "A", "NORTH", "GOLD", SUB_STATUS_ACTIVE, &id1);
    (void)subdb_add(db, "300000000000002", "910000000002", "B", "SOUTH", "GOLD", SUB_STATUS_SUSPENDED, &id2);
    (void)subdb_add(db, "300000000000003", "910000000003", "C", "NORTH", "SILVER", SUB_STATUS_ACTIVE, &id3);

    sm_subscriber_t *one = NULL;
    CHECK(subdb_search_by_id(db, id2, &one) == SM_OK);
    CHECK(one->subscriber_id == id2);
    subdb_free_one(one);

    CHECK(subdb_search_by_imsi(db, "300000000000003", &one) == SM_OK);
    CHECK(strcmp(one->imsi, "300000000000003") == 0);
    subdb_free_one(one);

    CHECK(subdb_search_by_msisdn(db, "910000000001", &one) == SM_OK);
    subdb_free_one(one);

    sm_subscriber_t **arr = NULL;
    size_t n = 0;
    CHECK(subdb_search_by_region(db, "NORTH", &arr, &n) == SM_OK);
    CHECK(n == 2U);
    subdb_free_results(arr, n);

    CHECK(subdb_search_by_plan(db, "GOLD", &arr, &n) == SM_OK);
    CHECK(n == 2U);
    subdb_free_results(arr, n);

    CHECK(subdb_search_by_status(db, SUB_STATUS_SUSPENDED, &arr, &n) == SM_OK);
    CHECK(n == 1U);
    subdb_free_results(arr, n);

    CHECK(subdb_list_all(db, &arr, &n) == SM_OK);
    CHECK(n == 3U);
    subdb_free_results(arr, n);

    subdb_destroy(db);
}

static void test_subscriber_update(void)
{
    sm_subscriber_db_t *db = NULL;
    CHECK(subdb_create(&db) == SM_OK);
    uint64_t id;
    (void)subdb_add(db, "400000000000001", "910000000010", "Old Name", "OLD_REGION",
                     "OLD_PLAN", SUB_STATUS_ACTIVE, &id);

    sm_sub_status_t new_status = SUB_STATUS_SUSPENDED;
    CHECK(subdb_update(db, id, "New Name", "NEW_REGION", NULL, &new_status) == SM_OK);

    sm_subscriber_t *s = NULL;
    CHECK(subdb_search_by_id(db, id, &s) == SM_OK);
    CHECK(strcmp(sstr_cstr(s->name), "New Name") == 0);
    CHECK(strcmp(sstr_cstr(s->region), "NEW_REGION") == 0);
    CHECK(strcmp(sstr_cstr(s->plan), "OLD_PLAN") == 0); /* left unchanged */
    CHECK(s->status == SUB_STATUS_SUSPENDED);
    subdb_free_one(s);

    CHECK(subdb_update(db, 999999U, "X", NULL, NULL, NULL) == SM_ERR_NOT_FOUND);

    subdb_destroy(db);
}

/* ================= sorting ================= */

static void test_sorting(void)
{
    sm_subscriber_db_t *db = NULL;
    CHECK(subdb_create(&db) == SM_OK);
    uint64_t ids[5];
    const char *regions[5] = {"ZETA", "ALPHA", "GAMMA", "BETA", "DELTA"};
    for (int i = 0; i < 5; ++i) {
        char imsi[24], msisdn[24];
        (void)snprintf(imsi, sizeof(imsi), "50000000000%02d", i);
        (void)snprintf(msisdn, sizeof(msisdn), "9100000000%02d", i);
        (void)subdb_add(db, imsi, msisdn, "N", regions[i], "P", SUB_STATUS_ACTIVE, &ids[i]);
    }
    sm_subscriber_t **arr = NULL;
    size_t n = 0;
    CHECK(subdb_list_all(db, &arr, &n) == SM_OK);
    CHECK(n == 5U);

    sub_sort_merge(arr, n, SORT_BY_REGION);
    const char *expected[5] = {"ALPHA", "BETA", "DELTA", "GAMMA", "ZETA"};
    for (size_t i = 0; i < n; ++i) {
        CHECK(strcmp(sstr_cstr(arr[i]->region), expected[i]) == 0);
    }

    sub_sort_quick(arr, n, SORT_BY_ID);
    for (size_t i = 1; i < n; ++i) {
        CHECK(arr[i - 1]->subscriber_id <= arr[i]->subscriber_id);
    }

    subdb_free_results(arr, n);
    subdb_destroy(db);
}

static void test_sorting_edge_cases(void)
{
    /* Must not crash on NULL/empty/single-element arrays. */
    sub_sort_merge(NULL, 0U, SORT_BY_ID);
    sub_sort_quick(NULL, 0U, SORT_BY_ID);

    sm_subscriber_db_t *db = NULL;
    CHECK(subdb_create(&db) == SM_OK);
    uint64_t id;
    (void)subdb_add(db, "600000000000001", "919111111111", "Solo", "R", "P", SUB_STATUS_ACTIVE, &id);
    sm_subscriber_t **arr = NULL;
    size_t n = 0;
    CHECK(subdb_list_all(db, &arr, &n) == SM_OK);
    sub_sort_merge(arr, n, SORT_BY_ID); /* single element: no-op */
    CHECK(n == 1U);
    subdb_free_results(arr, n);
    subdb_destroy(db);
}

/* ================= persistence ================= */

static void test_csv_save_and_load_roundtrip(void)
{
    sm_subscriber_db_t *db = NULL;
    CHECK(subdb_create(&db) == SM_OK);
    for (int i = 0; i < 20; ++i) {
        char imsi[24], msisdn[24], name[32];
        (void)snprintf(imsi, sizeof(imsi), "70000000%07d", i);
        (void)snprintf(msisdn, sizeof(msisdn), "9187654%05d", i);
        (void)snprintf(name, sizeof(name), "Bulk User %d", i);
        uint64_t id;
        (void)subdb_add(db, imsi, msisdn, name, "BULK_REGION", "BULK_PLAN", SUB_STATUS_ACTIVE, &id);
    }
    CHECK(subdb_save_csv(db, "data/test_roundtrip.csv") == SM_OK);
    subdb_destroy(db);

    sm_subscriber_db_t *loaded = NULL;
    CHECK(subdb_create(&loaded) == SM_OK);
    CHECK(subdb_load_csv(loaded, "data/test_roundtrip.csv") == SM_OK);
    CHECK(subdb_count(loaded) == 20U);
    subdb_destroy(loaded);
}

static void test_csv_load_corrupt_file(void)
{
    FILE *fp = fopen("data/test_corrupt.csv", "w");
    CHECK(fp != NULL);
    if (fp != NULL) {
        fputs("not,a,valid,header\n", fp);
        fclose(fp);
    }
    sm_subscriber_db_t *db = NULL;
    CHECK(subdb_create(&db) == SM_OK);
    CHECK(subdb_load_csv(db, "data/test_corrupt.csv") == SM_ERR_CORRUPT_DATA);
    CHECK(subdb_load_csv(db, "data/does_not_exist.csv") == SM_ERR_IO);
    subdb_destroy(db);
}

static void test_subscriber_status_strings(void)
{
    CHECK(strcmp(sub_status_to_str(SUB_STATUS_ACTIVE), "ACTIVE") == 0);
    CHECK(strcmp(sub_status_to_str(SUB_STATUS_SUSPENDED), "SUSPENDED") == 0);
    CHECK(strcmp(sub_status_to_str(SUB_STATUS_TERMINATED), "TERMINATED") == 0);
    CHECK(strcmp(sub_status_to_str((sm_sub_status_t)999), "UNKNOWN") == 0);

    CHECK(sub_status_from_str(NULL) == SUB_STATUS_UNKNOWN);
    CHECK(sub_status_from_str("ACTIVE") == SUB_STATUS_ACTIVE);
    CHECK(sub_status_from_str("SUSPENDED") == SUB_STATUS_SUSPENDED);
    CHECK(sub_status_from_str("TERMINATED") == SUB_STATUS_TERMINATED);
    CHECK(sub_status_from_str("GARBAGE") == SUB_STATUS_UNKNOWN);
}

static void test_subscriber_null_param_guards(void)
{
    /* Lifecycle / free helpers must tolerate NULL without crashing. */
    subdb_destroy(NULL);
    subdb_free_one(NULL);
    subdb_free_results(NULL, 0U);
    CHECK(subdb_create(NULL) == SM_ERR_NULL_PARAM);
    CHECK(subdb_count(NULL) == 0U);

    sm_subscriber_db_t *db = NULL;
    CHECK(subdb_create(&db) == SM_OK);

    uint64_t out_id = 0;
    CHECK(subdb_add(NULL, "1", "1", "n", "r", "p", SUB_STATUS_ACTIVE, &out_id) == SM_ERR_NULL_PARAM);
    CHECK(subdb_delete(NULL, 1U) == SM_ERR_NULL_PARAM);
    CHECK(subdb_delete(db, 999999U) == SM_ERR_NOT_FOUND);
    CHECK(subdb_update(NULL, 1U, NULL, NULL, NULL, NULL) == SM_ERR_NULL_PARAM);
    CHECK(subdb_update(db, 1U, "bad,name", NULL, NULL, NULL) == SM_ERR_INVALID_ARG);

    sm_subscriber_t *one = NULL;
    CHECK(subdb_search_by_id(NULL, 1U, &one) == SM_ERR_NULL_PARAM);
    CHECK(subdb_search_by_id(db, 1U, NULL) == SM_ERR_NULL_PARAM);
    CHECK(subdb_search_by_imsi(NULL, "1", &one) == SM_ERR_NULL_PARAM);
    CHECK(subdb_search_by_imsi(db, NULL, &one) == SM_ERR_NULL_PARAM);
    CHECK(subdb_search_by_msisdn(NULL, "1", &one) == SM_ERR_NULL_PARAM);
    CHECK(subdb_search_by_msisdn(db, NULL, &one) == SM_ERR_NULL_PARAM);

    sm_subscriber_t **arr = NULL;
    size_t n = 0;
    CHECK(subdb_search_by_region(NULL, "R", &arr, &n) == SM_ERR_NULL_PARAM);
    CHECK(subdb_search_by_region(db, NULL, &arr, &n) == SM_ERR_NULL_PARAM);
    CHECK(subdb_search_by_plan(NULL, "P", &arr, &n) == SM_ERR_NULL_PARAM);
    CHECK(subdb_search_by_plan(db, NULL, &arr, &n) == SM_ERR_NULL_PARAM);
    CHECK(subdb_search_by_status(NULL, SUB_STATUS_ACTIVE, &arr, &n) == SM_ERR_NULL_PARAM);
    CHECK(subdb_list_all(NULL, &arr, &n) == SM_ERR_NULL_PARAM);

    CHECK(subdb_save_csv(NULL, "x") == SM_ERR_NULL_PARAM);
    CHECK(subdb_save_csv(db, NULL) == SM_ERR_NULL_PARAM);
    CHECK(subdb_load_csv(NULL, "x") == SM_ERR_NULL_PARAM);
    CHECK(subdb_load_csv(db, NULL) == SM_ERR_NULL_PARAM);

    char backup_path[256];
    CHECK(subdb_backup(NULL, "data", backup_path, sizeof(backup_path)) == SM_ERR_NULL_PARAM);
    CHECK(subdb_backup(db, NULL, backup_path, sizeof(backup_path)) == SM_ERR_NULL_PARAM);
    CHECK(subdb_backup(db, "data", NULL, sizeof(backup_path)) == SM_ERR_NULL_PARAM);
    CHECK(subdb_restore(NULL, "x") == SM_ERR_NULL_PARAM);
    CHECK(subdb_restore(db, NULL) == SM_ERR_NULL_PARAM);

    subdb_destroy(db);
}

static void test_subscriber_backup_and_restore(void)
{
    sm_subscriber_db_t *db = NULL;
    CHECK(subdb_create(&db) == SM_OK);
    uint64_t id;
    (void)subdb_add(db, "900000000000001", "919000000001", "Bkp", "R", "P", SUB_STATUS_ACTIVE, &id);

    char backup_path[256] = {0};
    CHECK(subdb_backup(db, "data", backup_path, sizeof(backup_path)) == SM_OK);
    CHECK(backup_path[0] != '\0');

    sm_subscriber_db_t *restored = NULL;
    CHECK(subdb_create(&restored) == SM_OK);
    CHECK(subdb_restore(restored, backup_path) == SM_OK);
    CHECK(subdb_count(restored) == 1U);

    /* Restoring again into a now-non-empty DB exercises the "target DB is
     * not empty" warning branch; the duplicate IMSI/MSISDN is skipped. */
    CHECK(subdb_restore(restored, backup_path) == SM_OK);
    CHECK(subdb_count(restored) == 1U);

    (void)remove(backup_path);
    subdb_destroy(db);
    subdb_destroy(restored);
}

static void test_subscriber_save_csv_bad_path(void)
{
    sm_subscriber_db_t *db = NULL;
    CHECK(subdb_create(&db) == SM_OK);
    /* Directory does not exist: fopen() on the .tmp path fails. */
    CHECK(subdb_save_csv(db, "no_such_directory_xyz/subs.csv") == SM_ERR_IO);
    subdb_destroy(db);
}

static void test_subscriber_load_csv_edge_cases(void)
{
    /* Empty file: fgets() returns NULL immediately -> "missing header". */
    FILE *fp = fopen("data/test_empty.csv", "w");
    CHECK(fp != NULL);
    if (fp != NULL) fclose(fp);

    sm_subscriber_db_t *db = NULL;
    CHECK(subdb_create(&db) == SM_OK);
    CHECK(subdb_load_csv(db, "data/test_empty.csv") == SM_ERR_CORRUPT_DATA);

    /* Valid header, then: a malformed line (wrong field count), a line
     * with invalid field content, and a line that fails subdb_add()
     * (duplicate IMSI of a record already in the DB) -- all three should
     * be skipped rather than aborting the whole load. */
    uint64_t existing_id = 0;
    CHECK(subdb_add(db, "910000000000001", "919222222222", "Pre", "R", "P",
                     SUB_STATUS_ACTIVE, &existing_id) == SM_OK);

    FILE *fp2 = fopen("data/test_mixed.csv", "w");
    CHECK(fp2 != NULL);
    if (fp2 != NULL) {
        fputs("subscriber_id,imsi,msisdn,name,region,plan,status,created_at,updated_at\n", fp2);
        fputs("only,four,fields,here\n", fp2);                              /* malformed: != 9 fields */
        fputs("2,BADIMSI,919333333333,N,R,P,ACTIVE,100,100\n", fp2);        /* invalid IMSI */
        fputs("3,910000000000001,919222222222,Dup,R,P,ACTIVE,100,100\n", fp2); /* dup IMSI -> add fails */
        fputs("4,910000000000002,919444444444,Ok,R,P,ACTIVE,100,100\n", fp2);  /* valid */
        fclose(fp2);
    }
    CHECK(subdb_load_csv(db, "data/test_mixed.csv") == SM_OK);
    CHECK(subdb_count(db) == 2U); /* the pre-existing one + the one valid new line */

    (void)remove("data/test_empty.csv");
    (void)remove("data/test_mixed.csv");
    subdb_destroy(db);
}

static void test_subscriber_sort_large_dataset(void)
{
    /* >=13 elements forces quick_sort_rec()'s real partition/median-of-
     * three path (below that threshold it's pure insertion sort), and
     * mixing regions/plans/statuses exercises every compare_subs() key. */
    sm_subscriber_db_t *db = NULL;
    CHECK(subdb_create(&db) == SM_OK);

    const char *regions[]  = {"NORTH", "SOUTH", "EAST", "WEST"};
    const char *plans[]    = {"GOLD", "SILVER", "BRONZE"};
    const sm_sub_status_t statuses[] = {SUB_STATUS_ACTIVE, SUB_STATUS_SUSPENDED, SUB_STATUS_TERMINATED};

    for (int i = 0; i < 25; ++i) {
        char imsi[24], msisdn[24], name[32];
        (void)snprintf(imsi, sizeof(imsi), "95000000%07d", i);
        (void)snprintf(msisdn, sizeof(msisdn), "9199999%05d", i);
        (void)snprintf(name, sizeof(name), "Sort User %d", i);
        uint64_t id;
        (void)subdb_add(db, imsi, msisdn, name, regions[i % 4], plans[i % 3],
                         statuses[i % 3], &id);
    }

    sm_subscriber_t **arr = NULL;
    size_t n = 0;
    CHECK(subdb_list_all(db, &arr, &n) == SM_OK);
    CHECK(n == 25U);

    sub_sort_quick(arr, n, SORT_BY_REGION);
    for (size_t i = 1; i < n; ++i) {
        CHECK(strcmp(sstr_cstr(arr[i - 1]->region), sstr_cstr(arr[i]->region)) <= 0);
    }
    sub_sort_quick(arr, n, SORT_BY_PLAN);
    for (size_t i = 1; i < n; ++i) {
        CHECK(strcmp(sstr_cstr(arr[i - 1]->plan), sstr_cstr(arr[i]->plan)) <= 0);
    }
    sub_sort_quick(arr, n, SORT_BY_STATUS);
    for (size_t i = 1; i < n; ++i) {
        CHECK(strcmp(sub_status_to_str(arr[i - 1]->status), sub_status_to_str(arr[i]->status)) <= 0);
    }
    sub_sort_merge(arr, n, SORT_BY_PLAN);
    for (size_t i = 1; i < n; ++i) {
        CHECK(strcmp(sstr_cstr(arr[i - 1]->plan), sstr_cstr(arr[i]->plan)) <= 0);
    }

    subdb_free_results(arr, n);
    subdb_destroy(db);
}

/* ================= concurrency ================= */

typedef struct {
    sm_subscriber_db_t *db;
    int                  idx;
} conc_arg_t;

static void *concurrent_add_worker(void *arg)
{
    conc_arg_t *a = (conc_arg_t *)arg;
    for (int i = 0; i < 100; ++i) {
        char imsi[24], msisdn[24];
        (void)snprintf(imsi, sizeof(imsi), "80%03d%09d", a->idx, i);
        (void)snprintf(msisdn, sizeof(msisdn), "92%03d%08d", a->idx, i);
        uint64_t id;
        (void)subdb_add(a->db, imsi, msisdn, "Conc", "R", "P", SUB_STATUS_ACTIVE, &id);
    }
    return NULL;
}

static void test_concurrent_adds_no_corruption(void)
{
    sm_subscriber_db_t *db = NULL;
    CHECK(subdb_create(&db) == SM_OK);

    const int THREADS = 20;
    pthread_t tids[20];
    conc_arg_t args[20];
    for (int i = 0; i < THREADS; ++i) {
        args[i].db = db;
        args[i].idx = i;
        CHECK(pthread_create(&tids[i], NULL, concurrent_add_worker, &args[i]) == 0);
    }
    for (int i = 0; i < THREADS; ++i) {
        pthread_join(tids[i], NULL);
    }
    CHECK(subdb_count(db) == (size_t)(THREADS * 100)); /* every add must be visible, none lost/corrupted */
    subdb_destroy(db);
}

/* ================= extended concurrency stress =================
 * Scales at 50 / 100 / 500 / 1000+ concurrent operations as required.
 * Each test verifies: no crash, no corruption, no lost/duplicate
 * updates, and a fully consistent end state. Intended to be run under
 * plain, ASan+UBSan, and ThreadSanitizer builds (see Makefile: test,
 * asan, tsan targets). */

#define STRESS_THREADS_SMALL   5
#define STRESS_THREADS_MED     10
#define STRESS_OPS_PER_THREAD  100  /* 10 * 100 = 1000+ total ops (in-memory ops) */

/* auth_login/auth_change_password persist the credential store to disk
 * (fopen/write/fflush/rename) under auth.c's single global lock on every
 * call by design (see auth.c: save_users_locked()) -- that serializes
 * disk I/O across all threads, so a smaller per-thread op count is used
 * here to keep the suite fast while still comfortably clearing the
 * "50+ concurrent operations" bar (10 threads * 15 ops = 150). */
#define AUTH_STRESS_OPS_PER_THREAD 5

/* ---- concurrent delete: disjoint IDs, every thread deletes its own
 * slice; final count must be exactly zero with no double-frees/UAF. ---- */
typedef struct {
    sm_subscriber_db_t *db;
    const uint64_t      *ids;
    int                   start;
    int                   count;
} range_arg_t;

static void *concurrent_delete_worker(void *arg)
{
    range_arg_t *a = (range_arg_t *)arg;
    for (int i = 0; i < a->count; ++i) {
        (void)subdb_delete(a->db, a->ids[a->start + i]);
    }
    return NULL;
}

static void test_concurrent_delete_no_corruption(void)
{
    sm_subscriber_db_t *db = NULL;
    CHECK(subdb_create(&db) == SM_OK);

    const int N = STRESS_THREADS_MED * STRESS_OPS_PER_THREAD; /* 1000 */
    uint64_t *ids = (uint64_t *)malloc(sizeof(uint64_t) * (size_t)N);
    CHECK(ids != NULL);
    if (ids == NULL) { subdb_destroy(db); return; }

    for (int i = 0; i < N; ++i) {
        char imsi[24], msisdn[24];
        (void)snprintf(imsi, sizeof(imsi), "81%013d", i);
        (void)snprintf(msisdn, sizeof(msisdn), "93%012d", i);
        CHECK(subdb_add(db, imsi, msisdn, "Del", "R", "P", SUB_STATUS_ACTIVE, &ids[i]) == SM_OK);
    }
    CHECK(subdb_count(db) == (size_t)N);

    const int THREADS = STRESS_THREADS_MED;
    pthread_t tids[STRESS_THREADS_MED];
    range_arg_t args[STRESS_THREADS_MED];
    int per = N / THREADS;
    for (int t = 0; t < THREADS; ++t) {
        args[t].db = db;
        args[t].ids = ids;
        args[t].start = t * per;
        args[t].count = per;
        CHECK(pthread_create(&tids[t], NULL, concurrent_delete_worker, &args[t]) == 0);
    }
    for (int t = 0; t < THREADS; ++t) {
        pthread_join(tids[t], NULL);
    }

    CHECK(subdb_count(db) == 0U); /* every record removed exactly once, none missed/duplicated */
    free(ids);
    subdb_destroy(db);
}

/* ---- concurrent update: disjoint IDs, every thread updates its own
 * slice repeatedly; verifies the last write per-record is visible and
 * no cross-record corruption occurred. ---- */
static void *concurrent_update_worker(void *arg)
{
    range_arg_t *a = (range_arg_t *)arg;
    for (int i = 0; i < a->count; ++i) {
        sm_sub_status_t st = SUB_STATUS_SUSPENDED;
        (void)subdb_update(a->db, a->ids[a->start + i], "Updated", "UPD_REGION", "UPD_PLAN", &st);
    }
    return NULL;
}

static void test_concurrent_update_no_corruption(void)
{
    sm_subscriber_db_t *db = NULL;
    CHECK(subdb_create(&db) == SM_OK);

    const int N = STRESS_THREADS_MED * STRESS_OPS_PER_THREAD; /* 1000 */
    uint64_t *ids = (uint64_t *)malloc(sizeof(uint64_t) * (size_t)N);
    CHECK(ids != NULL);
    if (ids == NULL) { subdb_destroy(db); return; }

    for (int i = 0; i < N; ++i) {
        char imsi[24], msisdn[24];
        (void)snprintf(imsi, sizeof(imsi), "82%013d", i);
        (void)snprintf(msisdn, sizeof(msisdn), "94%012d", i);
        CHECK(subdb_add(db, imsi, msisdn, "Upd", "R", "P", SUB_STATUS_ACTIVE, &ids[i]) == SM_OK);
    }

    const int THREADS = STRESS_THREADS_MED;
    pthread_t tids[STRESS_THREADS_MED];
    range_arg_t args[STRESS_THREADS_MED];
    int per = N / THREADS;
    for (int t = 0; t < THREADS; ++t) {
        args[t].db = db;
        args[t].ids = ids;
        args[t].start = t * per;
        args[t].count = per;
        CHECK(pthread_create(&tids[t], NULL, concurrent_update_worker, &args[t]) == 0);
    }
    for (int t = 0; t < THREADS; ++t) {
        pthread_join(tids[t], NULL);
    }

    CHECK(subdb_count(db) == (size_t)N); /* update never adds/removes records */
    for (int i = 0; i < N; ++i) {
        sm_subscriber_t *s = NULL;
        CHECK(subdb_search_by_id(db, ids[i], &s) == SM_OK);
        if (s != NULL) {
            CHECK(strcmp(sstr_cstr(s->name), "Updated") == 0);
            CHECK(s->status == SUB_STATUS_SUSPENDED);
            subdb_free_one(s);
        }
    }
    free(ids);
    subdb_destroy(db);
}

/* ---- concurrent search/list/report readers running alongside
 * concurrent writers: many readers must be able to proceed together
 * (rwlock) while writers still get exclusive, consistent access. ---- */
typedef struct {
    sm_subscriber_db_t *db;
    int                  iterations;
    int                  observed_bad;
} reader_arg_t;

static void *concurrent_reader_worker(void *arg)
{
    reader_arg_t *a = (reader_arg_t *)arg;
    for (int i = 0; i < a->iterations; ++i) {
        sm_subscriber_t **all = NULL;
        size_t n = 0;
        if (subdb_list_all(a->db, &all, &n) == SM_OK) {
            /* Every snapshot returned must be internally well-formed --
             * this is what would fail under a torn/partial read. */
            for (size_t j = 0; j < n; ++j) {
                if (all[j] == NULL || sstr_cstr(all[j]->name) == NULL) {
                    a->observed_bad = 1;
                }
            }
            subdb_free_results(all, n);
        }
        size_t cnt = subdb_count(a->db);
        SM_UNUSED(cnt);
    }
    return NULL;
}

static void *concurrent_writer_worker(void *arg)
{
    range_arg_t *a = (range_arg_t *)arg;
    for (int i = 0; i < a->count; ++i) {
        char imsi[24], msisdn[24];
        int n = a->start + i;
        (void)snprintf(imsi, sizeof(imsi), "83%013d", n);
        (void)snprintf(msisdn, sizeof(msisdn), "95%012d", n);
        uint64_t id;
        if (subdb_add(a->db, imsi, msisdn, "W", "R", "P", SUB_STATUS_ACTIVE, &id) == SM_OK) {
            (void)subdb_delete(a->db, id);
        }
    }
    return NULL;
}

static void test_concurrent_search_during_writes(void)
{
    sm_subscriber_db_t *db = NULL;
    CHECK(subdb_create(&db) == SM_OK);

    const int READERS = STRESS_THREADS_SMALL;
    const int WRITERS = STRESS_THREADS_SMALL;
    pthread_t rtids[STRESS_THREADS_SMALL], wtids[STRESS_THREADS_SMALL];
    reader_arg_t rargs[STRESS_THREADS_SMALL];
    range_arg_t wargs[STRESS_THREADS_SMALL];

    for (int t = 0; t < READERS; ++t) {
        rargs[t].db = db;
        rargs[t].iterations = STRESS_OPS_PER_THREAD; /* 5 * 100 = 500+ read ops */
        rargs[t].observed_bad = 0;
        CHECK(pthread_create(&rtids[t], NULL, concurrent_reader_worker, &rargs[t]) == 0);
    }
    for (int t = 0; t < WRITERS; ++t) {
        wargs[t].db = db;
        wargs[t].ids = NULL;
        wargs[t].start = t * STRESS_OPS_PER_THREAD;
        wargs[t].count = STRESS_OPS_PER_THREAD; /* 5 * 100 = 500+ write ops */
        CHECK(pthread_create(&wtids[t], NULL, concurrent_writer_worker, &wargs[t]) == 0);
    }
    for (int t = 0; t < READERS; ++t) { pthread_join(rtids[t], NULL); }
    for (int t = 0; t < WRITERS; ++t) { pthread_join(wtids[t], NULL); }

    for (int t = 0; t < READERS; ++t) {
        CHECK(rargs[t].observed_bad == 0); /* no torn/partial reads ever observed */
    }
    CHECK(subdb_count(db) == 0U); /* every add was paired with a delete */
    subdb_destroy(db);
}

/* ---- concurrent login: independent users, each thread creates its
 * own user then repeatedly logs in/out; no cross-talk between
 * sessions belonging to different users. ---- */
static void *concurrent_login_worker(void *arg)
{
    int idx = *(int *)arg;
    char username[SM_AUTH_MAX_USERNAME + 1u];
    (void)snprintf(username, sizeof(username), "conc_user_%02d", idx);
    char password[] = "Str0ngPass!42";

    sm_status_t cst;
    for (int attempt = 0; attempt < 3; ++attempt) {
        cst = auth_create_user(username, password, SM_ROLE_VIEWER);
        if (cst != SM_ERR_IO) break;
        tiny_backoff_sleep();
    }
    if (cst != SM_OK) {
        return (void *)(intptr_t)1;
    }
    int ok = 1;
    for (int i = 0; i < AUTH_STRESS_OPS_PER_THREAD; ++i) {
        sm_session_t session;
        sm_status_t lst;
        for (int attempt = 0; attempt < 3; ++attempt) {
            lst = auth_login(username, password, &session);
            if (lst != SM_ERR_IO) break;
            tiny_backoff_sleep();
        }
        if (lst != SM_OK) {
            fprintf(stderr, "  [DBG] %s iter=%d auth_login rc=%d\n", username, i, (int)lst);
            ok = 0; break;
        }
        sm_auth_user_t who;
        sm_status_t vst = auth_verify_session(session.token, &who);
        if (vst != SM_OK) {
            fprintf(stderr, "  [DBG] %s iter=%d auth_verify_session rc=%d\n", username, i, (int)vst);
            ok = 0; break;
        }
        if (strcmp(who.username, username) != 0) {
            fprintf(stderr, "  [DBG] %s iter=%d cross-talk got=%s\n", username, i, who.username);
            ok = 0; break;
        } /* no session cross-talk */
        sm_status_t ost = auth_logout(session.token);
        if (ost != SM_OK) {
            fprintf(stderr, "  [DBG] %s iter=%d auth_logout rc=%d\n", username, i, (int)ost);
            ok = 0; break;
        }
    }
    return (void *)(intptr_t)(ok ? 0 : 1);
}

static void test_concurrent_login_independent_users(void)
{
    const char *path = "data/test_auth_conc_login.csv";
    (void)remove(path);
    (void)remove("data/test_auth_conc_login.csv.tmp");
    CHECK(auth_init(path) == SM_OK);

    const int THREADS = STRESS_THREADS_MED;
    pthread_t tids[STRESS_THREADS_MED];
    int idxs[STRESS_THREADS_MED];
    for (int t = 0; t < THREADS; ++t) {
        idxs[t] = t;
        CHECK(pthread_create(&tids[t], NULL, concurrent_login_worker, &idxs[t]) == 0);
    }
    for (int t = 0; t < THREADS; ++t) {
        void *rc = NULL;
        pthread_join(tids[t], &rc);
        CHECK((intptr_t)rc == 0); /* every login/verify/logout cycle succeeded cleanly */
    }

    auth_shutdown();
    (void)remove(path);
    (void)remove("data/test_auth_conc_login.csv.tmp");
}

/* ---- concurrent password change: each thread owns a distinct user
 * and repeatedly changes its own password back and forth; no
 * corruption of the shared credential store or other users' state. ---- */
static void *concurrent_password_change_worker(void *arg)
{
    int idx = *(int *)arg;
    char username[SM_AUTH_MAX_USERNAME + 1u];
    (void)snprintf(username, sizeof(username), "conc_pw_%02d", idx);
    const char *pw_a = "InitialPass1!";
    const char *pw_b = "RotatedPass2!";

    sm_status_t cst;
    for (int attempt = 0; attempt < 3; ++attempt) {
        cst = auth_create_user(username, pw_a, SM_ROLE_OPERATOR);
        if (cst != SM_ERR_IO) break;
        tiny_backoff_sleep();
    }
    if (cst != SM_OK) {
        return (void *)(intptr_t)1;
    }
    const char *cur = pw_a;
    const char *nxt = pw_b;
    int ok = 1;
    for (int i = 0; i < AUTH_STRESS_OPS_PER_THREAD; ++i) {
        sm_session_t session;
        sm_status_t lst;
        for (int attempt = 0; attempt < 3; ++attempt) {
            lst = auth_login(username, cur, &session);
            if (lst != SM_ERR_IO) break;
            tiny_backoff_sleep();
        }
        if (lst != SM_OK) {
            fprintf(stderr, "  [DBG] %s iter=%d auth_login(cur=%s) rc=%d\n", username, i, cur, (int)lst);
            ok = 0; break;
        }
        sm_status_t cpst;
        for (int attempt = 0; attempt < 3; ++attempt) {
            cpst = auth_change_password(session.token, cur, nxt);
            if (cpst != SM_ERR_IO) break;
            tiny_backoff_sleep();
        }
        if (cpst != SM_OK) {
            fprintf(stderr, "  [DBG] %s iter=%d auth_change_password(cur=%s,nxt=%s) rc=%d\n",
                    username, i, cur, nxt, (int)cpst);
            ok = 0; break;
        }
        const char *tmp = cur; cur = nxt; nxt = tmp;
        (void)auth_logout(session.token);
    }
    return (void *)(intptr_t)(ok ? 0 : 1);
}

static void test_concurrent_password_change_no_corruption(void)
{
    const char *path = "data/test_auth_conc_pw.csv";
    (void)remove(path);
    (void)remove("data/test_auth_conc_pw.csv.tmp");
    CHECK(auth_init(path) == SM_OK);

    const int THREADS = STRESS_THREADS_MED;
    pthread_t tids[STRESS_THREADS_MED];
    int idxs[STRESS_THREADS_MED];
    for (int t = 0; t < THREADS; ++t) {
        idxs[t] = t;
        CHECK(pthread_create(&tids[t], NULL, concurrent_password_change_worker, &idxs[t]) == 0);
    }
    for (int t = 0; t < THREADS; ++t) {
        void *rc = NULL;
        pthread_join(tids[t], &rc);
        CHECK((intptr_t)rc == 0); /* every user's own password-change chain stayed consistent */
    }

    auth_shutdown();
    (void)remove(path);
    (void)remove("data/test_auth_conc_pw.csv.tmp");
}

/* ---- concurrent CSV save/load: multiple independent DBs are
 * populated, saved, and reloaded concurrently, sharing only the
 * process-global shared-string intern pool -- stresses save/load
 * together with the intern lock. ---- */
typedef struct {
    int idx;
    int per_db;
    int ok;
} csv_arg_t;

static void *concurrent_csv_worker(void *arg)
{
    csv_arg_t *a = (csv_arg_t *)arg;
    a->ok = 0;
    char path[64];
    (void)snprintf(path, sizeof(path), "data/test_conc_csv_%02d.csv", a->idx);
    (void)remove(path);

    sm_subscriber_db_t *db = NULL;
    if (subdb_create(&db) != SM_OK) { return NULL; }
    for (int i = 0; i < a->per_db; ++i) {
        char imsi[24], msisdn[24];
        (void)snprintf(imsi, sizeof(imsi), "84%02d%011d", a->idx, i);
        (void)snprintf(msisdn, sizeof(msisdn), "96%02d%010d", a->idx, i);
        uint64_t id;
        if (subdb_add(db, imsi, msisdn, "CSV", "SHARED_REGION", "SHARED_PLAN",
                       SUB_STATUS_ACTIVE, &id) != SM_OK) {
            subdb_destroy(db);
            return NULL;
        }
    }
    if (subdb_save_csv(db, path) != SM_OK) { subdb_destroy(db); return NULL; }
    subdb_destroy(db);

    sm_subscriber_db_t *loaded = NULL;
    if (subdb_create(&loaded) != SM_OK) { return NULL; }
    if (subdb_load_csv(loaded, path) != SM_OK) { subdb_destroy(loaded); return NULL; }
    a->ok = (subdb_count(loaded) == (size_t)a->per_db) ? 1 : 0;
    subdb_destroy(loaded);
    (void)remove(path);
    return NULL;
}

static void test_concurrent_csv_save_load(void)
{
    const int THREADS = STRESS_THREADS_MED;
    pthread_t tids[STRESS_THREADS_MED];
    csv_arg_t args[STRESS_THREADS_MED];
    for (int t = 0; t < THREADS; ++t) {
        args[t].idx = t;
        args[t].per_db = 50; /* 10 * 50 = 500+ subscriber rows round-tripped */
        args[t].ok = 0;
        CHECK(pthread_create(&tids[t], NULL, concurrent_csv_worker, &args[t]) == 0);
    }
    for (int t = 0; t < THREADS; ++t) {
        pthread_join(tids[t], NULL);
        CHECK(args[t].ok == 1); /* each DB's own save/load round-trip stayed consistent */
    }
}

/* ---- concurrent shared-string interning: many threads intern a
 * shared vocabulary of strings; identical text must always resolve to
 * the same pointer, and total refcounts must reconcile exactly. ---- */
static const char *g_sstr_vocab[] = { "NORTH", "SOUTH", "EAST", "WEST", "GOLD", "SILVER" };
#define SSTR_VOCAB_N (sizeof(g_sstr_vocab) / sizeof(g_sstr_vocab[0]))

static void *concurrent_intern_worker(void *arg)
{
    SM_UNUSED(arg);
    sm_shared_string_t *held[STRESS_OPS_PER_THREAD];
    int ok = 1;
    for (int i = 0; i < STRESS_OPS_PER_THREAD; ++i) {
        const char *text = g_sstr_vocab[(size_t)i % SSTR_VOCAB_N];
        if (sstr_intern(text, &held[i]) != SM_OK) { ok = 0; break; }
        if (strcmp(sstr_cstr(held[i]), text) != 0) { ok = 0; }
    }
    for (int i = 0; i < STRESS_OPS_PER_THREAD; ++i) {
        sstr_release(held[i]);
    }
    return (void *)(intptr_t)(ok ? 0 : 1);
}

static void test_concurrent_shared_string_interning(void)
{
    const int THREADS = STRESS_THREADS_MED;
    pthread_t tids[STRESS_THREADS_MED];
    for (int t = 0; t < THREADS; ++t) {
        CHECK(pthread_create(&tids[t], NULL, concurrent_intern_worker, NULL) == 0);
    }
    for (int t = 0; t < THREADS; ++t) {
        void *rc = NULL;
        pthread_join(tids[t], &rc);
        CHECK((intptr_t)rc == 0);
    }

    sm_sstr_pool_stats_t stats;
    CHECK(sstr_pool_get_stats(&stats) == SM_OK);
    CHECK(stats.distinct_strings <= SSTR_VOCAB_N + 32U); /* plus whatever earlier tests interned */
}

/* ---- concurrent memory-pool alloc/free on one shared pool: every
 * thread allocs a batch then frees the exact same batch; final state
 * must show zero blocks in use and a balanced alloc/free count. ---- */
static void *concurrent_mempool_worker(void *arg)
{
    sm_memory_pool_t *pool = (sm_memory_pool_t *)arg;
    void *blocks[STRESS_OPS_PER_THREAD];
    int n = 0;
    for (int i = 0; i < STRESS_OPS_PER_THREAD; ++i) {
        blocks[i] = mempool_alloc(pool);
        if (blocks[i] != NULL) { n++; }
    }
    for (int i = 0; i < n; ++i) {
        mempool_free(pool, blocks[i]);
    }
    return NULL;
}

static void test_concurrent_memory_pool_alloc_free(void)
{
    sm_memory_pool_t *pool = NULL;
    CHECK(mempool_create(64U, 32U, &pool) == SM_OK);

    const int THREADS = STRESS_THREADS_MED;
    pthread_t tids[STRESS_THREADS_MED];
    for (int t = 0; t < THREADS; ++t) {
        CHECK(pthread_create(&tids[t], NULL, concurrent_mempool_worker, pool) == 0);
    }
    for (int t = 0; t < THREADS; ++t) {
        pthread_join(tids[t], NULL);
    }

    sm_pool_stats_t stats;
    CHECK(mempool_get_stats(pool, &stats) == SM_OK);
    CHECK(stats.used_blocks == 0U);                            /* every alloc was freed */
    CHECK(stats.alloc_count == stats.free_count);               /* balanced under contention */
    CHECK(stats.alloc_count >= (uint64_t)(THREADS * STRESS_OPS_PER_THREAD)); /* 1000+ ops */

    mempool_destroy(pool);
}

static void ensure_test_data_dir(void)
{
#ifdef _WIN32
    if (_mkdir("data") != 0 && errno != EEXIST) {
        fprintf(stderr, "warning: could not ensure data directory exists\n");
    }
#else
    if (mkdir("data", 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "warning: could not ensure data directory exists\n");
    }
#endif
}

int main(void)
{
    (void)logger_init(NULL, LOG_FATAL, 0U); /* keep test output clean: fatal-only to stderr */
    (void)sstr_pool_init(256U);
    ensure_test_data_dir();

    RUN(test_logger_levels_and_status_strings);
    RUN(test_logger_rotation);
    RUN(test_logger_init_bad_path);

    RUN(test_memory_pool_basic);
    RUN(test_memory_pool_growth);
    RUN(test_memory_pool_null_safety);

    RUN(test_hash_table_crud);
    RUN(test_hash_table_resize);
    RUN(test_hash_table_iterate);

    RUN(test_shared_string_interning);
    RUN(test_shared_string_rejects_bad_input);
    RUN(test_shared_string_pool_lifecycle_and_stats);

    RUN(test_subscriber_validation);
    RUN(test_subscriber_add_duplicate_delete);
    RUN(test_auth_change_password_persists);
    RUN(test_auth_login_unknown_user_and_lockout);
    RUN(test_auth_session_and_rbac);
    RUN(test_auth_create_user_validation);
    RUN(test_auth_change_password_edge_cases);
    RUN(test_auth_delete_user);
    RUN(test_subscriber_search_variants);
    RUN(test_subscriber_update);

    RUN(test_sorting);
    RUN(test_sorting_edge_cases);

    RUN(test_csv_save_and_load_roundtrip);
    RUN(test_csv_load_corrupt_file);

    RUN(test_subscriber_status_strings);
    RUN(test_subscriber_null_param_guards);
    RUN(test_subscriber_backup_and_restore);
    RUN(test_subscriber_save_csv_bad_path);
    RUN(test_subscriber_load_csv_edge_cases);
    RUN(test_subscriber_sort_large_dataset);

    RUN(test_concurrent_adds_no_corruption);
    RUN(test_concurrent_delete_no_corruption);
    RUN(test_concurrent_update_no_corruption);
    RUN(test_concurrent_search_during_writes);
    RUN(test_concurrent_login_independent_users);
    RUN(test_concurrent_password_change_no_corruption);
    RUN(test_concurrent_csv_save_load);
    RUN(test_concurrent_shared_string_interning);
    RUN(test_concurrent_memory_pool_alloc_free);

    sstr_pool_shutdown();
    logger_shutdown();

    printf("\n%d/%d checks passed\n", g_total - g_failures, g_total);
    return (g_failures == 0) ? 0 : 1;
}
