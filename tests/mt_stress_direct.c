/* Direct-library concurrency stress test: spawns many pthreads that all
 * call subdb_add / subdb_search_by_id / subdb_delete / subdb_list_all
 * concurrently against ONE shared sm_subscriber_db_t. Bypasses the
 * network layer and the (slow, unrelated) auth login hashing, so it
 * finishes in well under a second even fully sanitizer-instrumented,
 * while still directly exercising the same locked data structures the
 * TCP server uses under the hood. */
#include "common.h"
#include "logger.h"
#include "shared_string.h"
#include "subscriber.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NTHREADS   30
#define OPS_PER_THREAD 50

static sm_subscriber_db_t *g_db;
static _Atomic int g_add_ok = 0;
static _Atomic int g_add_fail = 0;
static _Atomic int g_search_ok = 0;
static _Atomic int g_delete_ok = 0;

static void *worker(void *arg)
{
    long id = (long)arg;
    for (int i = 0; i < OPS_PER_THREAD; i++) {
        char imsi[16], msisdn[11], name[32];
        snprintf(imsi, sizeof(imsi), "%015ld", id * 1000 + i);
        snprintf(msisdn, sizeof(msisdn), "%010ld", id * 1000 + i);
        snprintf(name, sizeof(name), "Stress%ld_%d", id, i);

        uint64_t new_id = 0;
        sm_status_t st = subdb_add(g_db, imsi, msisdn, name, "UNSET", "UNSET",
                                    SUB_STATUS_ACTIVE, &new_id);
        if (st == SM_OK) {
            g_add_ok++;
            sm_subscriber_t *sub = NULL;
            if (subdb_search_by_id(g_db, new_id, &sub) == SM_OK && sub != NULL) {
                g_search_ok++;
                subdb_free_one(sub);
            }
            if (i % 3 == 0) {
                if (subdb_delete(g_db, new_id) == SM_OK) {
                    g_delete_ok++;
                }
            }
        } else {
            g_add_fail++;
        }

        if (i % 10 == 0) {
            sm_subscriber_t **arr = NULL;
            size_t count = 0;
            if (subdb_list_all(g_db, &arr, &count) == SM_OK) {
                subdb_free_results(arr, count);
            }
        }
    }
    return NULL;
}

int main(void)
{
    (void)logger_init(NULL, LOG_INFO, 0);
    (void)sstr_pool_init(1024U);
    if (subdb_create(&g_db) != SM_OK) {
        fprintf(stderr, "subdb_create failed\n");
        return 1;
    }

    pthread_t tids[NTHREADS];
    for (long i = 0; i < NTHREADS; i++) {
        pthread_create(&tids[i], NULL, worker, (void *)i);
    }
    for (int i = 0; i < NTHREADS; i++) {
        pthread_join(tids[i], NULL);
    }

    printf("direct stress: %d threads x %d ops\n", NTHREADS, OPS_PER_THREAD);
    printf("  adds ok=%d fail=%d, searches ok=%d, deletes ok=%d\n",
           g_add_ok, g_add_fail, g_search_ok, g_delete_ok);

    size_t final_count = 0;
    sm_subscriber_t **arr = NULL;
    if (subdb_list_all(g_db, &arr, &final_count) == SM_OK) {
        subdb_free_results(arr, final_count);
    }
    printf("  final db count=%zu (expected adds - deletes = %d)\n",
           final_count, g_add_ok - g_delete_ok);

    subdb_destroy(g_db);
    sstr_pool_shutdown();
    logger_shutdown();

    int expected = g_add_ok - g_delete_ok;
    if ((int)final_count != expected) {
        printf("MISMATCH: final_count=%zu expected=%d\n", final_count, expected);
        return 1;
    }
    printf("OK: no corruption, counts reconcile exactly\n");
    return 0;
}
