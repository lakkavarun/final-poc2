# Minimal CUnit-compatible stub

This is NOT the real CUnit framework. It's a small, dependency-free
stand-in that implements just the subset of the CUnit API used by
`tests/test_cunit.c` (CU_add_suite, CU_add_test with proper
suite_init/suite_cleanup lifecycle, the CU_ASSERT_* family, and the
basic/automated run entry points).

Use this when you can't install the real `libcunit1-dev` / CUnit on
your machine (no admin rights, no package manager, no internet) but
still want to compile and run `tests/test_cunit.c` as-is, unmodified,
to see pass/fail results.

It is verified to compile `tests/test_cunit.c` cleanly under
`-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wstrict-prototypes`
and run all 18 registered tests with 0 failures.

Limitations vs. the real CUnit:
- No automated XML report output (CU_automated_run_tests just runs
  the basic mode).
- No CU_ASSERT_FATAL variants, no test skipping, no timing stats.
- Meant as a local dev convenience, not a CI-grade substitute --
  once you can install real CUnit somewhere (WSL, a Linux CI runner,
  a machine where you do have install rights), switch back to
  `make test-cunit` for the authoritative run.

## Build (no install required)

    make test-cunit-stub

This compiles tests/test_cunit.c against this stub instead of a
system CUnit install, and runs the resulting binary.
