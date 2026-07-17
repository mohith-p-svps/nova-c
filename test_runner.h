#ifndef TEST_RUNNER_H
#define TEST_RUNNER_H

// Runs every test case found in `path` (Nova's `# TEST:` / `# EXPECT` /
// `# END` format — see test_runner.c for the full format description).
// Returns 0 if every test passed, 1 if any failed or the file itself
// couldn't be read — suitable for use as a process exit code.
int runTestFile(const char* path);

#endif
