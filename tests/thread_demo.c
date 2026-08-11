/* thread_demo.c -- small standalone demo that spawns real pthreads doing
 * actual sm_subscriber_db_t operations (subdb_add / subdb_search_by_id /
 * subdb_delete), with each worker printing its own KERNEL thread id
 * (gettid(), not pthread_self()) as it works -- interleaved, visible proof
 * that these are genuine OS threads running concurrently, not a simulated
 * or single-threaded illusion.
 *
 * A second, independent thread polls /proc/self/task while the workers run
 * and prints how many kernel threads are alive at each sample -- external
 * confirmation (from the OS itself, not from our own bookkeeping) that
 * multiple task/thread entries exist under this process at once.
 */
#define _GNU_SOURCE
#include "common.h"
#include "logger.h"
#include "shared_string.h"
#include "subscriber.h"

#include <dirent.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define NTHREADS        8
#define OPS_PER_THREAD  40

static sm_subscriber_db_t *g_db;
static _Atomic int g_running_workers = 0;
static _Atomic int g_ops_done = 0;

static pid_t my_gettid(void)
{
    return (pid_t)syscall(SYS_gettid);
}

/* Count entries under /proc/self/task -- one per live kernel thread of
 * this process, straight from the OS's own bookkeeping. */
static int count_kernel_threads(void)
{
    DIR *d = opendir("/proc/self/task");
    if (d == NULL) {
        return -1;
    }
    int count = 0;
    struct dirent *ent = readdir(d);
    while (ent != NULL) {
        if (ent->d_name[0] != '.') {
            count++;
        }
        ent = readdir(d);
    }
    closedir(d);
    return count;
}

static void sleep_us(long usec)
{
    struct timespec ts;
    ts.tv_sec = usec / 1000000L;
    ts.tv_nsec = (usec % 1000000L) * 1000L;
    nanosleep(&ts, NULL);
}

static void *worker(void *arg)
{
    long id = (long)arg;
    pid_t tid = my_gettid();
    g_running_workers++;

    printf("[worker %ld] kernel tid=%d starting %d ops\n", id, (int)tid, OPS_PER_THREAD);
    fflush(stdout);

    for (int i = 0; i < OPS_PER_THREAD; i++) {
        char imsi[16], msisdn[11], name[32];
        snprintf(imsi, sizeof(imsi), "%015ld", id * 1000 + i);
        snprintf(msisdn, sizeof(msisdn), "%010ld", id * 1000 + i);
        snprintf(name, sizeof(name), "Demo%ld_%d", id, i);

        uint64_t new_id = 0;
        sm_status_t st = subdb_add(g_db, imsi, msisdn, name, "UNSET", "UNSET",
                                    SUB_STATUS_ACTIVE, &new_id);
        if (st == SM_OK) {
            sm_subscriber_t *sub = NULL;
            if (subdb_search_by_id(g_db, new_id, &sub) == SM_OK && sub != NULL) {
                subdb_free_one(sub);
            }
            if (i % 3 == 0) {
                subdb_delete(g_db, new_id);
            }
        }

        printf("[worker %ld] kernel tid=%d op %d/%d done\n",
               id, (int)tid, i + 1, OPS_PER_THREAD);
        fflush(stdout);
        g_ops_done++;

        /* Deliberate small, randomized delay per op. Wide enough that the
         * whole run takes tens of ms (not sub-millisecond), so an external
         * /proc poller sampling every few ms reliably lands mid-flight
         * instead of racing to completion before its first sample. */
        long jitter = ((id * 137L) + ((long)i * 53L)) % 2500L;
        sleep_us(500L + jitter);
    }

    g_running_workers--;
    return NULL;
}

/* Polls /proc/self/task while workers are active. Starts sampling
 * immediately (no fixed startup delay) and keeps sampling on a short
 * interval until every worker has finished, so it can't "start too late"
 * relative to how fast the workers happen to run. */
static void *proc_poller(void *arg)
{
    (void)arg;
    int max_seen = 0;
    int samples = 0;

    /* Wait for at least one worker to actually be running before the
     * first sample, so we don't just catch the poller + main thread. */
    while (g_running_workers == 0) {
        sleep_us(200);
    }

    while (g_running_workers > 0) {
        int n = count_kernel_threads();
        if (n > max_seen) {
            max_seen = n;
        }
        samples++;
        printf("[proc-poll] /proc/self/task entries=%d  running_workers=%d  ops_done=%d\n",
               n, g_running_workers, g_ops_done);
        fflush(stdout);
        sleep_us(2000); /* 2ms between samples */
    }

    printf("[proc-poll] done: %d samples taken, max kernel-thread count observed=%d\n",
           samples, max_seen);
    fflush(stdout);
    return (void *)(long)max_seen;
}

int main(void)
{
    (void)logger_init(NULL, LOG_INFO, 0);
    if (sstr_pool_init(1024U) != SM_OK) {
        fprintf(stderr, "sstr_pool_init failed\n");
        return 1;
    }
    if (subdb_create(&g_db) != SM_OK) {
        fprintf(stderr, "subdb_create failed\n");
        return 1;
    }

    pthread_t poller_tid;
    pthread_create(&poller_tid, NULL, proc_poller, NULL);

    pthread_t tids[NTHREADS];
    for (long i = 0; i < NTHREADS; i++) {
        pthread_create(&tids[i], NULL, worker, (void *)i);
    }
    for (int i = 0; i < NTHREADS; i++) {
        pthread_join(tids[i], NULL);
    }

    void *max_seen_ptr = NULL;
    pthread_join(poller_tid, &max_seen_ptr);
    int max_seen = (int)(long)max_seen_ptr;

    size_t final_count = 0;
    sm_subscriber_t **arr = NULL;
    if (subdb_list_all(g_db, &arr, &final_count) == SM_OK) {
        subdb_free_results(arr, final_count);
    }

    printf("\n=== summary ===\n");
    printf("threads spawned=%d, ops completed=%d, final db count=%zu\n",
           NTHREADS, g_ops_done, final_count);
    printf("max kernel threads observed live via /proc/self/task=%d "
           "(main+poller+workers; expect > %d while workers ran)\n",
           max_seen, NTHREADS);

    subdb_destroy(g_db);
    sstr_pool_shutdown();
    logger_shutdown();

    if (max_seen <= NTHREADS) {
        printf("WARNING: /proc poller never observed more than %d threads alive -- "
               "sampling may have missed the concurrent window.\n", NTHREADS);
        return 1;
    }
    printf("OK: real OS-level thread concurrency confirmed by the kernel itself.\n");
    return 0;
}
