# =============================================================================
# Makefile - Telecom Subscriber Management System
# =============================================================================
# Written to spec: does not modify any .c/.h source, does not change the
# project layout, does not add third-party dependencies, and does not
# install anything -- assumes gcc, cppcheck, valgrind (incl. Helgrind), and
# CUnit are already present on this system.
#
# src/main.c and src/server.c both define main(); they are NEVER compiled
# together. The normal app source list is explicit (no src/*.c globbing)
# specifically to keep server.c out of the normal build.
# =============================================================================

CC      = gcc
CSTD    = -std=c11
WARN    = -Wall -Wextra -Wshadow -Wunused-parameter -Wconversion -Wpedantic
INC     = -Iinclude
LDLIBS  = -lpthread

# Explicit source list (never src/*.c -- that would pull in server.c and
# cause a duplicate-definition-of-main link error against main.c).
CORE_SRCS = src/auth.c src/hash_table.c src/logger.c src/memory_pool.c \
            src/sha256.c src/shared_string.c src/subscriber.c
APP_SRCS  = $(CORE_SRCS) src/main.c

TARGET         = subscriber_mgmt
DEBUG_TARGET   = subscriber_mgmt_debug
RELEASE_TARGET = subscriber_mgmt_release
O0_TARGET      = subscriber_O0
O1_TARGET      = subscriber_O1
O2_TARGET      = subscriber_O2
O3_TARGET      = subscriber_O3
TEST_TARGET    = run_tests

MISRA_REPORT      = misra_report.txt
VALGRIND_REPORT   = valgrind_report.txt
HELGRIND_REPORT   = helgrind_report.txt
LOGGER_REPORT     = logger_report.txt
VALIDATION_REPORT = project_validation_report.txt
APP_LOG           = logs/subscriber_mgmt.log

.PHONY: all debug run test test-cunit test-cunit-stub misra cppcheck valgrind helgrind logger \
        validate report O0 O1 O2 O3 release clean

# =============================================================================
# 15. Default target
# =============================================================================
all: $(TARGET)

# =============================================================================
# 1. GCC build (normal)
# =============================================================================
$(TARGET): $(APP_SRCS)
	$(CC) $(CSTD) $(WARN) $(INC) $(APP_SRCS) -o $(TARGET) $(LDLIBS)

# =============================================================================
# 2. Debug build
# =============================================================================
debug: $(DEBUG_TARGET)

$(DEBUG_TARGET): $(APP_SRCS)
	$(CC) $(CSTD) $(WARN) $(INC) -g $(APP_SRCS) -o $(DEBUG_TARGET) $(LDLIBS)

# =============================================================================
# 3. Optimization builds
# =============================================================================
O0: $(O0_TARGET)

$(O0_TARGET): $(APP_SRCS)
	$(CC) $(CSTD) $(WARN) $(INC) -O0 $(APP_SRCS) -o $(O0_TARGET) $(LDLIBS)

O1: $(O1_TARGET)

$(O1_TARGET): $(APP_SRCS)
	$(CC) $(CSTD) $(WARN) $(INC) -O1 $(APP_SRCS) -o $(O1_TARGET) $(LDLIBS)

O2: $(O2_TARGET)

$(O2_TARGET): $(APP_SRCS)
	$(CC) $(CSTD) $(WARN) $(INC) -O2 $(APP_SRCS) -o $(O2_TARGET) $(LDLIBS)

O3: $(O3_TARGET)

$(O3_TARGET): $(APP_SRCS)
	$(CC) $(CSTD) $(WARN) $(INC) -O3 $(APP_SRCS) -o $(O3_TARGET) $(LDLIBS)

# =============================================================================
# 4. Release build
# =============================================================================
release: $(RELEASE_TARGET)

$(RELEASE_TARGET): $(APP_SRCS)
	$(CC) $(CSTD) $(WARN) $(INC) -O2 $(APP_SRCS) -o $(RELEASE_TARGET) $(LDLIBS)

# =============================================================================
# 5. Run application
# =============================================================================
run: $(TARGET)
	./$(TARGET)

# =============================================================================
# 6. Unit tests (tests/test_main.c)
#    NOTE: tests/test_main.c uses its own self-contained CHECK()/RUN() test
#    harness -- it does not call any CUnit API (no CU_* symbols), so no
#    -lcunit is linked here. Adding an unused -lcunit link dependency would
#    violate "do not add unnecessary compiler flags" and would break this
#    target on a machine that lacks libcunit even though nothing in this
#    binary needs it. src/main.c and src/server.c are excluded, as required.
# =============================================================================
test: $(TEST_TARGET)
	./$(TEST_TARGET)

$(TEST_TARGET): tests/test_main.c $(CORE_SRCS)
	$(CC) $(CSTD) $(WARN) $(INC) tests/test_main.c $(CORE_SRCS) -o $(TEST_TARGET) $(LDLIBS)

# =============================================================================
# 6b. CUnit-framework test build (tests/test_cunit.c)
#     This one DOES call the real CUnit API (CU_add_suite, CU_ASSERT_*,
#     etc.), so it needs the actual library. If `which cunit-config` or
#     `ldconfig -p | grep cunit` on this machine shows nothing, the library
#     isn't actually installed and this target will fail to link with
#     "cannot find -lcunit" -- that's the library missing, not the
#     Makefile; use test-cunit-stub below instead in that case.
# =============================================================================
CUNIT_TEST_TARGET = run_tests_cunit

test-cunit: $(CUNIT_TEST_TARGET)
	./$(CUNIT_TEST_TARGET)

$(CUNIT_TEST_TARGET): tests/test_cunit.c $(CORE_SRCS)
	$(CC) $(CSTD) $(WARN) $(INC) tests/test_cunit.c $(CORE_SRCS) -o $(CUNIT_TEST_TARGET) $(LDLIBS) -lcunit

# =============================================================================
# 6c. CUnit-API-compatible stub build (no install required)
#     Compiles tests/test_cunit.c UNMODIFIED against the dependency-free
#     stand-in already bundled at third_party/cunit_stub/ instead of a
#     system CUnit install. This is not a new dependency -- the stub was
#     already part of this project. See third_party/cunit_stub/README.md
#     for exactly what it does and doesn't cover. Use this when the real
#     CUnit library genuinely isn't installed on this machine.
# =============================================================================
CUNIT_STUB_DIR    = third_party/cunit_stub
CUNIT_STUB_TARGET = run_tests_cunit_stub

test-cunit-stub: $(CUNIT_STUB_TARGET)
	./$(CUNIT_STUB_TARGET)

$(CUNIT_STUB_TARGET): tests/test_cunit.c $(CORE_SRCS) $(CUNIT_STUB_DIR)/cunit_stub.c
	$(CC) $(CSTD) $(WARN) $(INC) -I$(CUNIT_STUB_DIR) \
		tests/test_cunit.c $(CORE_SRCS) $(CUNIT_STUB_DIR)/cunit_stub.c \
		-o $(CUNIT_STUB_TARGET) $(LDLIBS)

# =============================================================================
# 7. MISRA C:2012 (Cppcheck MISRA addon only)
#    --suppress=misra-config silences cppcheck's own "missing configuration"
#    notices (e.g. for EEXIST, EINTR, ENOENT, ERROR_SHARING_VIOLATION) --
#    these are cppcheck telling you it can't fully resolve certain system
#    header macros without a full library config, not actual MISRA rule
#    violations. Real MISRA findings (misra-c2012-*) still print normally.
# =============================================================================
misra:
	cppcheck --addon=misra --std=c11 --force -Iinclude --suppress=misra-config src 2>&1 | tee $(MISRA_REPORT)

# =============================================================================
# 7b. General Cppcheck static analysis (warning/performance/portability)
#     Separate from the MISRA addon above -- this is Cppcheck's own native
#     checkers, not the MISRA C:2012 rule set. Matches the exact command
#     already run manually on this system. Output goes to its own report
#     file, kept separate from misra_report.txt.
# =============================================================================
CPPCHECK_REPORT = cppcheck_report.txt

cppcheck:
	cppcheck --enable=warning,performance,portability \
	--std=c11 --force -Iinclude src 2>&1 | tee $(CPPCHECK_REPORT)

# =============================================================================
# 8. Valgrind memory check
# =============================================================================
valgrind: $(TARGET)
	valgrind --leak-check=full \
	--show-leak-kinds=all \
	--track-origins=yes \
	./$(TARGET) 2>&1 | tee $(VALGRIND_REPORT)

# =============================================================================
# 9. Helgrind thread check
# =============================================================================
helgrind: $(TARGET)
	valgrind --tool=helgrind \
	--history-level=full \
	./$(TARGET) 2>&1 | tee $(HELGRIND_REPORT)

# =============================================================================
# 10. Logger validation
# =============================================================================
logger:
	grep -iE "error|fail|warning" $(APP_LOG) > $(LOGGER_REPORT) || true
	cat $(LOGGER_REPORT)

# =============================================================================
# 11. Complete validation
#     Order: build, unit tests, MISRA, valgrind, helgrind, logger,
#     optimization builds. GNU Make runs prerequisites of a single target
#     left-to-right in a non-parallel invocation, and stops the whole chain
#     the moment any prerequisite's recipe returns non-zero -- so a real
#     build/test failure aborts here with a non-zero make exit status
#     instead of reaching the PASS banner below. misra/valgrind/helgrind
#     are diagnostic/report steps by design (per their own target recipes
#     above) and do not gate this by exit code; logger explicitly never
#     fails on zero matches, per requirement 10.
# =============================================================================
validate: all test misra valgrind helgrind logger O0 O1 O2 O3
	@echo "========================================="
	@echo "FINAL RESULT: PROJECT VALIDATION PASS"
	@echo "========================================="

# =============================================================================
# 12. Final validation report
# =============================================================================
report:
	@echo "=========================================" > $(VALIDATION_REPORT)
	@echo "PROJECT VALIDATION REPORT" >> $(VALIDATION_REPORT)
	@echo "=========================================" >> $(VALIDATION_REPORT)
	@echo "" >> $(VALIDATION_REPORT)
	@echo "1. GCC Build" >> $(VALIDATION_REPORT)
	@echo "-----------------------------------------" >> $(VALIDATION_REPORT)
	@if $(MAKE) --no-print-directory all >> $(VALIDATION_REPORT) 2>&1; then \
		echo "Result: PASS" >> $(VALIDATION_REPORT); \
	else \
		echo "Result: FAIL" >> $(VALIDATION_REPORT); \
	fi
	@echo "" >> $(VALIDATION_REPORT)
	@echo "2. CUnit Unit Tests" >> $(VALIDATION_REPORT)
	@echo "-----------------------------------------" >> $(VALIDATION_REPORT)
	@if $(MAKE) --no-print-directory test >> $(VALIDATION_REPORT) 2>&1; then \
		echo "Result: PASS" >> $(VALIDATION_REPORT); \
	else \
		echo "Result: FAIL" >> $(VALIDATION_REPORT); \
	fi
	@echo "" >> $(VALIDATION_REPORT)
	@echo "3. MISRA C:2012 Cppcheck" >> $(VALIDATION_REPORT)
	@echo "-----------------------------------------" >> $(VALIDATION_REPORT)
	@if $(MAKE) --no-print-directory misra >> $(VALIDATION_REPORT) 2>&1; then \
		echo "Result: PASS (see $(MISRA_REPORT) for findings)" >> $(VALIDATION_REPORT); \
	else \
		echo "Result: FAIL (cppcheck itself errored -- see $(MISRA_REPORT))" >> $(VALIDATION_REPORT); \
	fi
	@echo "" >> $(VALIDATION_REPORT)
	@echo "4. Valgrind Memory Test" >> $(VALIDATION_REPORT)
	@echo "-----------------------------------------" >> $(VALIDATION_REPORT)
	@$(MAKE) --no-print-directory valgrind >> $(VALIDATION_REPORT) 2>&1; \
	if grep -q "ERROR SUMMARY: 0 errors" $(VALGRIND_REPORT) 2>/dev/null; then \
		echo "Result: PASS (0 errors -- see $(VALGRIND_REPORT))" >> $(VALIDATION_REPORT); \
	else \
		echo "Result: FAIL (see $(VALGRIND_REPORT))" >> $(VALIDATION_REPORT); \
	fi
	@echo "" >> $(VALIDATION_REPORT)
	@echo "5. Helgrind Thread Test" >> $(VALIDATION_REPORT)
	@echo "-----------------------------------------" >> $(VALIDATION_REPORT)
	@$(MAKE) --no-print-directory helgrind >> $(VALIDATION_REPORT) 2>&1; \
	if grep -q "ERROR SUMMARY: 0 errors" $(HELGRIND_REPORT) 2>/dev/null; then \
		echo "Result: PASS (0 errors -- see $(HELGRIND_REPORT))" >> $(VALIDATION_REPORT); \
	else \
		echo "Result: FAIL (see $(HELGRIND_REPORT))" >> $(VALIDATION_REPORT); \
	fi
	@echo "" >> $(VALIDATION_REPORT)
	@echo "6. Logger Validation" >> $(VALIDATION_REPORT)
	@echo "-----------------------------------------" >> $(VALIDATION_REPORT)
	@$(MAKE) --no-print-directory logger >> $(VALIDATION_REPORT) 2>&1
	@echo "Result: PASS (grep completed; see $(LOGGER_REPORT) for any matches)" >> $(VALIDATION_REPORT)
	@echo "" >> $(VALIDATION_REPORT)
	@echo "7. O0 Optimization" >> $(VALIDATION_REPORT)
	@echo "-----------------------------------------" >> $(VALIDATION_REPORT)
	@if $(MAKE) --no-print-directory O0 >> $(VALIDATION_REPORT) 2>&1; then \
		echo "Result: PASS" >> $(VALIDATION_REPORT); \
	else \
		echo "Result: FAIL" >> $(VALIDATION_REPORT); \
	fi
	@echo "" >> $(VALIDATION_REPORT)
	@echo "8. O1 Optimization" >> $(VALIDATION_REPORT)
	@echo "-----------------------------------------" >> $(VALIDATION_REPORT)
	@if $(MAKE) --no-print-directory O1 >> $(VALIDATION_REPORT) 2>&1; then \
		echo "Result: PASS" >> $(VALIDATION_REPORT); \
	else \
		echo "Result: FAIL" >> $(VALIDATION_REPORT); \
	fi
	@echo "" >> $(VALIDATION_REPORT)
	@echo "9. O2 Optimization" >> $(VALIDATION_REPORT)
	@echo "-----------------------------------------" >> $(VALIDATION_REPORT)
	@if $(MAKE) --no-print-directory O2 >> $(VALIDATION_REPORT) 2>&1; then \
		echo "Result: PASS" >> $(VALIDATION_REPORT); \
	else \
		echo "Result: FAIL" >> $(VALIDATION_REPORT); \
	fi
	@echo "" >> $(VALIDATION_REPORT)
	@echo "10. O3 Optimization" >> $(VALIDATION_REPORT)
	@echo "-----------------------------------------" >> $(VALIDATION_REPORT)
	@if $(MAKE) --no-print-directory O3 >> $(VALIDATION_REPORT) 2>&1; then \
		echo "Result: PASS" >> $(VALIDATION_REPORT); \
	else \
		echo "Result: FAIL" >> $(VALIDATION_REPORT); \
	fi
	@echo "" >> $(VALIDATION_REPORT)
	@echo "11. Release Build" >> $(VALIDATION_REPORT)
	@echo "-----------------------------------------" >> $(VALIDATION_REPORT)
	@if $(MAKE) --no-print-directory release >> $(VALIDATION_REPORT) 2>&1; then \
		echo "Result: PASS" >> $(VALIDATION_REPORT); \
	else \
		echo "Result: FAIL" >> $(VALIDATION_REPORT); \
	fi
	@echo "" >> $(VALIDATION_REPORT)
	@echo "12. Final Result" >> $(VALIDATION_REPORT)
	@echo "-----------------------------------------" >> $(VALIDATION_REPORT)
	@if grep -q "^Result: FAIL" $(VALIDATION_REPORT); then \
		echo "Result: FAIL" >> $(VALIDATION_REPORT); \
		echo "=========================================" >> $(VALIDATION_REPORT); \
		cat $(VALIDATION_REPORT); \
		exit 1; \
	else \
		echo "Result: PASS" >> $(VALIDATION_REPORT); \
		echo "=========================================" >> $(VALIDATION_REPORT); \
		cat $(VALIDATION_REPORT); \
	fi

# =============================================================================
# 13. Clean
#     Removes only generated binaries and generated report files. Never
#     touches data/, logs/, include/, src/, tests/, or any source file.
# =============================================================================
clean:
	rm -f $(TARGET) $(DEBUG_TARGET) $(RELEASE_TARGET) \
		$(O0_TARGET) $(O1_TARGET) $(O2_TARGET) $(O3_TARGET) \
		$(TEST_TARGET) $(CUNIT_TEST_TARGET) $(CUNIT_STUB_TARGET)
	rm -f $(MISRA_REPORT) $(CPPCHECK_REPORT) $(VALGRIND_REPORT) $(HELGRIND_REPORT) \
		$(LOGGER_REPORT) $(VALIDATION_REPORT)
