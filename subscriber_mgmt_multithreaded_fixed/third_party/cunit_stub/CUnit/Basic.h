#ifndef CUNIT_BASIC_STUB_H
#define CUNIT_BASIC_STUB_H
typedef enum { CU_BRM_NORMAL, CU_BRM_SILENT, CU_BRM_VERBOSE } CU_BasicRunMode;
void CU_basic_set_mode(CU_BasicRunMode mode);
int CU_basic_run_tests(void);
int CU_automated_run_tests(void);
#endif
