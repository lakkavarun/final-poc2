#include "CUnit/CUnit.h"
#include "CUnit/Basic.h"
#include <stdio.h>
#include <string.h>

static unsigned int g_fail = 0;
static CU_CleanupFunc g_cleanups[64];
static int g_ncleanups = 0;
static CU_InitializeFunc g_last_init = NULL;

int CU_initialize_registry(void){ return 0; }
void CU_cleanup_registry(void){
    for (int i = 0; i < g_ncleanups; ++i) {
        if (g_cleanups[i]) g_cleanups[i]();
    }
    g_ncleanups = 0;
}
CU_pSuite CU_add_suite(const char *name, CU_InitializeFunc init, CU_CleanupFunc clean){
    static CU_Suite s;
    s.name = name;
    fprintf(stderr, "== suite: %s ==\n", name);
    g_last_init = init;
    if (init) {
        if (init() != 0) {
            fprintf(stderr, "  suite_init FAILED for %s\n", name);
        }
    }
    if (clean && g_ncleanups < 64) g_cleanups[g_ncleanups++] = clean;
    return &s;
}
void *CU_add_test(CU_pSuite suite, const char *name, CU_TestFunc fn){
    (void)suite;
    static int sentinel;
    fprintf(stderr, "-- %s\n", name);
    fn();
    return &sentinel;
}
int CU_get_error(void){ return 0; }
const char *CU_get_error_msg(void){ return ""; }
unsigned int CU_get_number_of_failures(void){ return g_fail; }
void CU_assertImplementation(int value, unsigned int line, const char *cond, const char *file, const char *func, int fatal){
    (void)func; (void)fatal;
    if(!value){ g_fail++; fprintf(stderr,"  [CU_FAIL] %s:%u: %s\n", file, line, cond); }
}
void CU_basic_set_mode(CU_BasicRunMode m){ (void)m; }
int CU_basic_run_tests(void){ printf("%u failures\n", g_fail); return g_fail?1:0; }
int CU_automated_run_tests(void){ return CU_basic_run_tests(); }
