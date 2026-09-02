#ifndef CUNIT_STUB_H
#define CUNIT_STUB_H
typedef struct CU_Suite { const char *name; } CU_Suite;
typedef CU_Suite* CU_pSuite;
typedef void (*CU_TestFunc)(void);
typedef int (*CU_InitializeFunc)(void);
typedef int (*CU_CleanupFunc)(void);

#define CUE_SUCCESS 0

int CU_initialize_registry(void);
void CU_cleanup_registry(void);
CU_pSuite CU_add_suite(const char *name, CU_InitializeFunc init, CU_CleanupFunc clean);
void *CU_add_test(CU_pSuite suite, const char *name, CU_TestFunc fn);
int CU_get_error(void);
const char *CU_get_error_msg(void);
unsigned int CU_get_number_of_failures(void);

void CU_assertImplementation(int value, unsigned int line, const char *cond, const char *file, const char *func, int fatal);

#define CU_ASSERT(v) CU_assertImplementation((v)!=0, __LINE__, #v, __FILE__, "", 0)
#define CU_ASSERT_TRUE(v) CU_ASSERT(v)
#define CU_ASSERT_FALSE(v) CU_assertImplementation((v)==0, __LINE__, #v, __FILE__, "", 0)
#define CU_ASSERT_EQUAL(a,b) CU_assertImplementation((a)==(b), __LINE__, #a "==" #b, __FILE__, "", 0)
#define CU_ASSERT_PTR_EQUAL(a,b) CU_assertImplementation((void*)(a)==(void*)(b), __LINE__, #a "==" #b, __FILE__, "", 0)
#define CU_ASSERT_PTR_NOT_EQUAL(a,b) CU_assertImplementation((void*)(a)!=(void*)(b), __LINE__, #a "!=" #b, __FILE__, "", 0)
#define CU_ASSERT_PTR_NULL(v) CU_assertImplementation((v)==NULL, __LINE__, #v, __FILE__, "", 0)
#define CU_ASSERT_PTR_NOT_NULL(v) CU_assertImplementation((v)!=NULL, __LINE__, #v, __FILE__, "", 0)
#define CU_ASSERT_STRING_EQUAL(a,b) CU_assertImplementation(strcmp((a),(b))==0, __LINE__, #a "==" #b, __FILE__, "", 0)
#endif
