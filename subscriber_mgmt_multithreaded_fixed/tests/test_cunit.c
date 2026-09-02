/**
 * @file test_cunit.c
 * @brief CUnit-based unit/integration/boundary/negative/concurrency tests.
 *
 * This mirrors the coverage of tests/test_main.c (the project's original
 * dependency-free harness) but runs on the CUnit framework
 * (http://cunit.sourceforge.net/), so it can plug into tooling that expects
 * CUnit's registry/suite model and XML output (CU_automated_run_tests).
 *
 * Build:   make test-cunit   (requires libcunit1-dev / CUnit installed)
 * Run:     ./build/run_tests_cunit
 */
#include "common.h"
#include "logger.h"
#include "memory_pool.h"
#include "hash_table.h"
#include "shared_string.h"
#include "subscriber.h"
#include "auth.h"

#include <CUnit/CUnit.h>
#include <CUnit/Basic.h>

#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <stdlib.h>
#include <errno.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

/* ================= memory pool ================= */

static void test_memory_pool_basic(void)
{
    sm_memory_pool_t *pool = NULL;
    CU_ASSERT_EQUAL(mempool_create(64U, 4U, &pool), SM_OK);
    void *a = mempool_alloc(pool);
    void *b = mempool_alloc(pool);
    CU_ASSERT_PTR_NOT_NULL(a);
    CU_ASSERT_PTR_NOT_NULL(b);
    CU_ASSERT_PTR_NOT_EQUAL(a, b);

    sm_pool_stats_t stats;
    CU_ASSERT_EQUAL(mempool_get_stats(pool, &stats), SM_OK);
    CU_ASSERT_EQUAL(stats.used_blocks, 2U);
    CU_ASSERT_EQUAL(stats.peak_blocks, 2U);

    mempool_free(pool, a);
    CU_ASSERT_EQUAL(mempool_get_stats(pool, &stats), SM_OK);
    CU_ASSERT_EQUAL(stats.used_blocks, 1U);

    void *c = mempool_alloc(pool); /* should reuse freed block a */
    CU_ASSERT_PTR_EQUAL(c, a);

    mempool_free(pool, b);
    mempool_free(pool, c);
    mempool_destroy(pool);
}

static void test_memory_pool_growth(void)
{
    sm_memory_pool_t *pool = NULL;
    CU_ASSERT_EQUAL(mempool_create(sizeof(void *), 2U, &pool), SM_OK);
    void *ptrs[10];
    for (int i = 0; i < 10; ++i) {
        ptrs[i] = mempool_alloc(pool); /* forces multiple slab growths */
        CU_ASSERT_PTR_NOT_NULL(ptrs[i]);
    }
    sm_pool_stats_t stats;
    CU_ASSERT_EQUAL(mempool_get_stats(pool, &stats), SM_OK);
    CU_ASSERT(stats.total_blocks >= 10U);
    for (int i = 0; i < 10; ++i) mempool_free(pool, ptrs[i]);
    mempool_destroy(pool);
}

static void test_memory_pool_null_safety(void)
{
    CU_ASSERT_PTR_NULL(mempool_alloc(NULL));
    mempool_free(NULL, NULL); /* must not crash */
    mempool_destroy(NULL);    /* must not crash */
}

/* ================= hash table ================= */

static void test_hash_table_crud(void)
{
    sm_hash_table_t *t = NULL;
    CU_ASSERT_EQUAL(ht_create(4U, sm_hash_str, sm_streq, NULL, NULL, &t), SM_OK);

    static char k1[] = "alpha", k2[] = "beta";
    int v1 = 100, v2 = 200;

    CU_ASSERT_EQUAL(ht_put(t, k1, &v1, false), SM_OK);
    CU_ASSERT_EQUAL(ht_put(t, k2, &v2, false), SM_OK);
    CU_ASSERT_EQUAL(ht_put(t, k1, &v2, false), SM_ERR_DUPLICATE);
    CU_ASSERT_EQUAL(ht_put(t, k1, &v2, true), SM_OK); /* replace */

    void *out = NULL;
    CU_ASSERT_EQUAL(ht_get(t, k1, &out), SM_OK);
    CU_ASSERT_PTR_EQUAL(out, &v2);

    CU_ASSERT_EQUAL(ht_get(t, "missing", &out), SM_ERR_NOT_FOUND);
    CU_ASSERT_EQUAL(ht_remove(t, k2), SM_OK);
    CU_ASSERT_EQUAL(ht_remove(t, k2), SM_ERR_NOT_FOUND);
    CU_ASSERT_EQUAL(ht_size(t), 1U);

    ht_destroy(t);
}

static void test_hash_table_resize(void)
{
    sm_hash_table_t *t = NULL;
    CU_ASSERT_EQUAL(ht_create(5U, sm_hash_u64, sm_u64eq, NULL, NULL, &t), SM_OK);

    static uint64_t keys[500];
    for (uint64_t i = 0; i < 500U; ++i) {
        keys[i] = i;
        CU_ASSERT_EQUAL(ht_put(t, &keys[i], (void *)(uintptr_t)(i + 1U), false), SM_OK);
    }
    CU_ASSERT_EQUAL(ht_size(t), 500U);
    for (uint64_t i = 0; i < 500U; ++i) {
        void *out = NULL;
        CU_ASSERT_EQUAL(ht_get(t, &keys[i], &out), SM_OK);
        CU_ASSERT_EQUAL((uintptr_t)out, i + 1U);
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
    CU_ASSERT_EQUAL(ht_create(4U, sm_hash_u64, sm_u64eq, NULL, NULL, &t), SM_OK);
    static uint64_t keys[10];
    for (uint64_t i = 0; i < 10U; ++i) {
        keys[i] = i;
        (void)ht_put(t, &keys[i], NULL, false);
    }
    size_t n = 0;
    ht_iterate(t, count_iter_cb, &n);
    CU_ASSERT_EQUAL(n, 10U);
    ht_destroy(t);
}

/* ================= shared strings ================= */

static void test_shared_string_interning(void)
{
    sm_shared_string_t *a = NULL, *b = NULL;
    CU_ASSERT_EQUAL(sstr_intern("GOLD_PLAN", &a), SM_OK);
    CU_ASSERT_EQUAL(sstr_intern("GOLD_PLAN", &b), SM_OK);
    CU_ASSERT_PTR_EQUAL(a, b); /* same backing pointer: true interning, not a copy */
    CU_ASSERT_STRING_EQUAL(sstr_cstr(a), "GOLD_PLAN");

    sstr_release(a);
    /* b still holds a reference, so the string must still be valid/interned. */
    sm_shared_string_t *c = NULL;
    CU_ASSERT_EQUAL(sstr_intern("GOLD_PLAN", &c), SM_OK);
    CU_ASSERT_PTR_EQUAL(c, b);

    sstr_release(b);
    sstr_release(c);
}

static void test_shared_string_rejects_bad_input(void)
{
    sm_shared_string_t *s = NULL;
    CU_ASSERT_EQUAL(sstr_intern(NULL, &s), SM_ERR_NULL_PARAM);
    CU_ASSERT_EQUAL(sstr_intern("", &s), SM_ERR_INVALID_ARG);
}

/* ================= subscriber CRUD / search ================= */

static void test_subscriber_validation(void)
{
    CU_ASSERT_TRUE(sub_validate_imsi("404123456789012"));
    CU_ASSERT_FALSE(sub_validate_imsi("40412"));            /* too short */
    CU_ASSERT_FALSE(sub_validate_imsi("40412A456789012"));  /* non-digit */
    CU_ASSERT_FALSE(sub_validate_imsi(NULL));

    CU_ASSERT_TRUE(sub_validate_msisdn("919876543210"));
    CU_ASSERT_FALSE(sub_validate_msisdn("123"));

    CU_ASSERT_TRUE(sub_validate_name("Asha Rao"));
    CU_ASSERT_FALSE(sub_validate_name(""));
    CU_ASSERT_FALSE(sub_validate_name("Has,Comma"));
}

static void test_subscriber_add_duplicate_delete(void)
{
    sm_subscriber_db_t *db = NULL;
    CU_ASSERT_EQUAL(subdb_create(&db), SM_OK);

    uint64_t id1 = 0, id2 = 0;
    CU_ASSERT_EQUAL(subdb_add(db, "111111111111111", "911111111111", "Test User", "REGION_A",
                               "PLAN_A", SUB_STATUS_ACTIVE, &id1), SM_OK);
    CU_ASSERT_EQUAL(subdb_count(db), 1U);

    /* duplicate IMSI */
    CU_ASSERT_EQUAL(subdb_add(db, "111111111111111", "922222222222", "Other", "REGION_A",
                               "PLAN_A", SUB_STATUS_ACTIVE, &id2), SM_ERR_DUPLICATE);
    /* duplicate MSISDN */
    CU_ASSERT_EQUAL(subdb_add(db, "222222222222222", "911111111111", "Other", "REGION_A",
                               "PLAN_A", SUB_STATUS_ACTIVE, &id2), SM_ERR_DUPLICATE);
    /* invalid IMSI */
    CU_ASSERT_EQUAL(subdb_add(db, "bad", "933333333333", "Other", "REGION_A",
                               "PLAN_A", SUB_STATUS_ACTIVE, &id2), SM_ERR_INVALID_ARG);

    CU_ASSERT_EQUAL(subdb_delete(db, id1), SM_OK);
    CU_ASSERT_EQUAL(subdb_count(db), 0U);
    CU_ASSERT_EQUAL(subdb_delete(db, id1), SM_ERR_NOT_FOUND);
    CU_ASSERT_EQUAL(subdb_delete(db, 999999U), SM_ERR_NOT_FOUND);

    subdb_destroy(db);
}

static void test_auth_change_password_persists(void)
{
    const char *path = "data/test_auth.csv";
    (void)remove(path);
    (void)remove("data/test_auth.csv.tmp");

    CU_ASSERT_EQUAL(auth_init(path), SM_OK);

    sm_session_t session;
    CU_ASSERT_EQUAL(auth_login("admin", "Ap39wb6301@@", &session), SM_OK);
    CU_ASSERT_EQUAL(auth_change_password(session.token, "Ap39wb6301@@", "NewPassword123!"), SM_OK);

    auth_shutdown();

    CU_ASSERT_EQUAL(auth_init(path), SM_OK);
    sm_session_t session2;
    CU_ASSERT_EQUAL(auth_login("admin", "NewPassword123!", &session2), SM_OK);

    auth_shutdown();
    (void)remove(path);
    (void)remove("data/test_auth.csv.tmp");
}

static void test_subscriber_search_variants(void)
{
    sm_subscriber_db_t *db = NULL;
    CU_ASSERT_EQUAL(subdb_create(&db), SM_OK);
    uint64_t id1, id2, id3;
    (void)subdb_add(db, "300000000000001", "910000000001", "A", "NORTH", "GOLD", SUB_STATUS_ACTIVE, &id1);
    (void)subdb_add(db, "300000000000002", "910000000002", "B", "SOUTH", "GOLD", SUB_STATUS_SUSPENDED, &id2);
    (void)subdb_add(db, "300000000000003", "910000000003", "C", "NORTH", "SILVER", SUB_STATUS_ACTIVE, &id3);

    sm_subscriber_t *one = NULL;
    CU_ASSERT_EQUAL(subdb_search_by_id(db, id2, &one), SM_OK);
    CU_ASSERT_EQUAL(one->subscriber_id, id2);
    subdb_free_one(one);

    CU_ASSERT_EQUAL(subdb_search_by_imsi(db, "300000000000003", &one), SM_OK);
    CU_ASSERT_STRING_EQUAL(one->imsi, "300000000000003");
    subdb_free_one(one);

    CU_ASSERT_EQUAL(subdb_search_by_msisdn(db, "910000000001", &one), SM_OK);
    subdb_free_one(one);

    sm_subscriber_t **arr = NULL;
    size_t n = 0;
    CU_ASSERT_EQUAL(subdb_search_by_region(db, "NORTH", &arr, &n), SM_OK);
    CU_ASSERT_EQUAL(n, 2U);
    subdb_free_results(arr, n);

    CU_ASSERT_EQUAL(subdb_search_by_plan(db, "GOLD", &arr, &n), SM_OK);
    CU_ASSERT_EQUAL(n, 2U);
    subdb_free_results(arr, n);

    CU_ASSERT_EQUAL(subdb_search_by_status(db, SUB_STATUS_SUSPENDED, &arr, &n), SM_OK);
    CU_ASSERT_EQUAL(n, 1U);
    subdb_free_results(arr, n);

    CU_ASSERT_EQUAL(subdb_list_all(db, &arr, &n), SM_OK);
    CU_ASSERT_EQUAL(n, 3U);
    subdb_free_results(arr, n);

    subdb_destroy(db);
}

static void test_subscriber_update(void)
{
    sm_subscriber_db_t *db = NULL;
    CU_ASSERT_EQUAL(subdb_create(&db), SM_OK);
    uint64_t id;
    (void)subdb_add(db, "400000000000001", "910000000010", "Old Name", "OLD_REGION",
                     "OLD_PLAN", SUB_STATUS_ACTIVE, &id);

    sm_sub_status_t new_status = SUB_STATUS_SUSPENDED;
    CU_ASSERT_EQUAL(subdb_update(db, id, "New Name", "NEW_REGION", NULL, &new_status), SM_OK);

    sm_subscriber_t *s = NULL;
    CU_ASSERT_EQUAL(subdb_search_by_id(db, id, &s), SM_OK);
    CU_ASSERT_STRING_EQUAL(sstr_cstr(s->name), "New Name");
    CU_ASSERT_STRING_EQUAL(sstr_cstr(s->region), "NEW_REGION");
    CU_ASSERT_STRING_EQUAL(sstr_cstr(s->plan), "OLD_PLAN"); /* left unchanged */
    CU_ASSERT_EQUAL(s->status, SUB_STATUS_SUSPENDED);
    subdb_free_one(s);

    CU_ASSERT_EQUAL(subdb_update(db, 999999U, "X", NULL, NULL, NULL), SM_ERR_NOT_FOUND);

    subdb_destroy(db);
}

/* ================= sorting ================= */

static void test_sorting(void)
{
    sm_subscriber_db_t *db = NULL;
    CU_ASSERT_EQUAL(subdb_create(&db), SM_OK);
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
    CU_ASSERT_EQUAL(subdb_list_all(db, &arr, &n), SM_OK);
    CU_ASSERT_EQUAL(n, 5U);

    sub_sort_merge(arr, n, SORT_BY_REGION);
    const char *expected[5] = {"ALPHA", "BETA", "DELTA", "GAMMA", "ZETA"};
    for (size_t i = 0; i < n; ++i) {
        CU_ASSERT_STRING_EQUAL(sstr_cstr(arr[i]->region), expected[i]);
    }

    sub_sort_quick(arr, n, SORT_BY_ID);
    for (size_t i = 1; i < n; ++i) {
        CU_ASSERT(arr[i - 1]->subscriber_id <= arr[i]->subscriber_id);
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
    CU_ASSERT_EQUAL(subdb_create(&db), SM_OK);
    uint64_t id;
    (void)subdb_add(db, "600000000000001", "919111111111", "Solo", "R", "P", SUB_STATUS_ACTIVE, &id);
    sm_subscriber_t **arr = NULL;
    size_t n = 0;
    CU_ASSERT_EQUAL(subdb_list_all(db, &arr, &n), SM_OK);
    sub_sort_merge(arr, n, SORT_BY_ID); /* single element: no-op */
    CU_ASSERT_EQUAL(n, 1U);
    subdb_free_results(arr, n);
    subdb_destroy(db);
}

/* ================= persistence ================= */

static void test_csv_save_and_load_roundtrip(void)
{
    sm_subscriber_db_t *db = NULL;
    CU_ASSERT_EQUAL(subdb_create(&db), SM_OK);
    for (int i = 0; i < 20; ++i) {
        char imsi[24], msisdn[24], name[32];
        (void)snprintf(imsi, sizeof(imsi), "70000000%07d", i);
        (void)snprintf(msisdn, sizeof(msisdn), "9187654%05d", i);
        (void)snprintf(name, sizeof(name), "Bulk User %d", i);
        uint64_t id;
        (void)subdb_add(db, imsi, msisdn, name, "BULK_REGION", "BULK_PLAN", SUB_STATUS_ACTIVE, &id);
    }
    CU_ASSERT_EQUAL(subdb_save_csv(db, "data/test_roundtrip.csv"), SM_OK);
    subdb_destroy(db);

    sm_subscriber_db_t *loaded = NULL;
    CU_ASSERT_EQUAL(subdb_create(&loaded), SM_OK);
    CU_ASSERT_EQUAL(subdb_load_csv(loaded, "data/test_roundtrip.csv"), SM_OK);
    CU_ASSERT_EQUAL(subdb_count(loaded), 20U);
    subdb_destroy(loaded);
}

static void test_csv_load_corrupt_file(void)
{
    FILE *fp = fopen("data/test_corrupt.csv", "w");
    CU_ASSERT_PTR_NOT_NULL(fp);
    if (fp != NULL) {
        fputs("not,a,valid,header\n", fp);
        fclose(fp);
    }
    sm_subscriber_db_t *db = NULL;
    CU_ASSERT_EQUAL(subdb_create(&db), SM_OK);
    CU_ASSERT_EQUAL(subdb_load_csv(db, "data/test_corrupt.csv"), SM_ERR_CORRUPT_DATA);
    CU_ASSERT_EQUAL(subdb_load_csv(db, "data/does_not_exist.csv"), SM_ERR_IO);
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
    CU_ASSERT_EQUAL(subdb_create(&db), SM_OK);

    const int THREADS = 20;
    pthread_t tids[20];
    conc_arg_t args[20];
    for (int i = 0; i < THREADS; ++i) {
        args[i].db = db;
        args[i].idx = i;
        CU_ASSERT_EQUAL(pthread_create(&tids[i], NULL, concurrent_add_worker, &args[i]), 0);
    }
    for (int i = 0; i < THREADS; ++i) {
        pthread_join(tids[i], NULL);
    }
    CU_ASSERT_EQUAL(subdb_count(db), (size_t)(THREADS * 100)); /* every add must be visible, none lost/corrupted */
    subdb_destroy(db);
}

/* ================= suite plumbing ================= */

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

static int suite_init(void)
{
    if (logger_init(NULL, LOG_FATAL, 0U) != SM_OK) { /* keep test output clean */
        return -1;
    }
    if (sstr_pool_init(256U) != SM_OK) {
        return -1;
    }
    ensure_test_data_dir();
    return 0;
}

static int suite_cleanup(void)
{
    sstr_pool_shutdown();
    logger_shutdown();
    return 0;
}

/* Helper: add a test to a suite and bail out loudly if CUnit rejects it
 * (e.g. NULL name/function), rather than silently losing coverage. */
#define ADD_TEST(suite, fn) \
    do { \
        if (CU_add_test((suite), #fn, (fn)) == NULL) { \
            fprintf(stderr, "Failed to add test %s: %s\n", #fn, CU_get_error_msg()); \
            CU_cleanup_registry(); \
            return CU_get_error(); \
        } \
    } while (0)

int main(void)
{
    if (CU_initialize_registry() != CUE_SUCCESS) {
        return (int)CU_get_error();
    }

    CU_pSuite mempool_suite = CU_add_suite("memory_pool", suite_init, suite_cleanup);
    CU_pSuite hash_suite    = CU_add_suite("hash_table", suite_init, suite_cleanup);
    CU_pSuite sstr_suite    = CU_add_suite("shared_string", suite_init, suite_cleanup);
    CU_pSuite sub_suite     = CU_add_suite("subscriber", suite_init, suite_cleanup);
    CU_pSuite sort_suite    = CU_add_suite("sorting", suite_init, suite_cleanup);
    CU_pSuite persist_suite = CU_add_suite("persistence", suite_init, suite_cleanup);
    CU_pSuite conc_suite    = CU_add_suite("concurrency", suite_init, suite_cleanup);

    if (!mempool_suite || !hash_suite || !sstr_suite || !sub_suite ||
        !sort_suite || !persist_suite || !conc_suite) {
        CU_cleanup_registry();
        return (int)CU_get_error();
    }

    ADD_TEST(mempool_suite, test_memory_pool_basic);
    ADD_TEST(mempool_suite, test_memory_pool_growth);
    ADD_TEST(mempool_suite, test_memory_pool_null_safety);

    ADD_TEST(hash_suite, test_hash_table_crud);
    ADD_TEST(hash_suite, test_hash_table_resize);
    ADD_TEST(hash_suite, test_hash_table_iterate);

    ADD_TEST(sstr_suite, test_shared_string_interning);
    ADD_TEST(sstr_suite, test_shared_string_rejects_bad_input);

    ADD_TEST(sub_suite, test_subscriber_validation);
    ADD_TEST(sub_suite, test_subscriber_add_duplicate_delete);
    ADD_TEST(sub_suite, test_auth_change_password_persists);
    ADD_TEST(sub_suite, test_subscriber_search_variants);
    ADD_TEST(sub_suite, test_subscriber_update);

    ADD_TEST(sort_suite, test_sorting);
    ADD_TEST(sort_suite, test_sorting_edge_cases);

    ADD_TEST(persist_suite, test_csv_save_and_load_roundtrip);
    ADD_TEST(persist_suite, test_csv_load_corrupt_file);

    ADD_TEST(conc_suite, test_concurrent_adds_no_corruption);

    CU_basic_set_mode(CU_BRM_VERBOSE);
    CU_basic_run_tests();

    unsigned int failures = CU_get_number_of_failures();
    CU_cleanup_registry();

    return (failures == 0U) ? 0 : 1;
}
