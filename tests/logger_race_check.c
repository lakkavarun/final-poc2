/* Minimal, fast TSan regression check for the gmtime() -> gmtime_r() fix in
 * src/logger.c. Spawns many threads that all call logger_log() concurrently
 * and heavily, with no auth/hashing overhead, so it runs in well under a
 * second even fully TSan-instrumented. */
#include "logger.h"
#include <pthread.h>
#include <stdio.h>

#define NTHREADS 16
#define NITERS   2000

static void *worker(void *arg)
{
    long id = (long)arg;
    for (int i = 0; i < NITERS; i++) {
        SM_LOG_INFO("RACECHK", "thread %ld iter %d", id, i);
    }
    return NULL;
}

int main(void)
{
    (void)logger_init(NULL, LOG_INFO, 0); /* stderr only, no file needed */
    pthread_t tids[NTHREADS];
    for (long i = 0; i < NTHREADS; i++) {
        pthread_create(&tids[i], NULL, worker, (void *)i);
    }
    for (int i = 0; i < NTHREADS; i++) {
        pthread_join(tids[i], NULL);
    }
    logger_shutdown();
    printf("logger_race_check: completed %d threads x %d log calls with no crash\n",
           NTHREADS, NITERS);
    return 0;
}
