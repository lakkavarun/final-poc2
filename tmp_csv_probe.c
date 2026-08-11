#include "subscriber.h"
#include "logger.h"
#include "shared_string.h"
#include <stdio.h>

int main(void) {
    (void)logger_init(NULL, LOG_FATAL, 0U);
    (void)sstr_pool_init(256U);
    sm_subscriber_db_t *db = NULL;
    sm_status_t st = subdb_create(&db);
    printf("create=%d\n", st);
    if (st == SM_OK) {
        uint64_t id = 0;
        st = subdb_add(db, "700000000000001", "918765400001", "Probe", "BULK_REGION", "BULK_PLAN", SUB_STATUS_ACTIVE, &id);
        printf("add=%d id=%llu\n", st, (unsigned long long)id);
        st = subdb_save_csv(db, "data/tmp_probe.csv");
        printf("save=%d\n", st);
        subdb_destroy(db);
    }
    sstr_pool_shutdown();
    logger_shutdown();
    return 0;
}
