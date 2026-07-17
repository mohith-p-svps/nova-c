// Nova's test file format:
//
//   # TEST: <name>
//   <one or more lines of Nova source>
//   # EXPECT
//   <one or more lines of expected stdout, verbatim>
//   # END
//
// Repeat as many of these blocks as you like in one file. Anything
// outside a block (blank lines, stray comments) is ignored, so you can
// freely add section headers etc. between tests.
//
// Each test gets a completely fresh Parser/Chunk/FunctionTable/VM — no
// state leaks between tests, same as running each block via `nova run`
// in its own process. stdout is captured via fd-level redirection to a
// temp file so it can be diffed against the expected block instead of
// just scrolling past in the terminal.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>      // dup, dup2, close on Windows/MinGW

#include "test_runner.h"
#include "parser.h"
#include "compiler.h"
#include "vm.h"
#include "error.h"

// --- tiny growable string buffer, used to accumulate each test's code
//     and expected-output blocks line by line -----------------------------

typedef struct {
    char* data;
    int   length;
    int   capacity;
} StrBuf;

static void sbInit(StrBuf* sb) {
    sb->capacity = 256;
    sb->data     = malloc(sb->capacity);
    sb->data[0]  = '\0';
    sb->length   = 0;
}

static void sbAppendLine(StrBuf* sb, const char* line) {
    int lineLen = (int)strlen(line);
    int needed  = sb->length + lineLen + 2; // +1 for '\n', +1 for '\0'
    if (needed > sb->capacity) {
        while (sb->capacity < needed) sb->capacity *= 2;
        sb->data = realloc(sb->data, sb->capacity);
    }
    memcpy(sb->data + sb->length, line, lineLen);
    sb->length += lineLen;
    sb->data[sb->length++] = '\n';
    sb->data[sb->length]   = '\0';
}

static void sbFree(StrBuf* sb) {
    free(sb->data);
    sb->data = NULL;
}

// --- small helpers ---------------------------------------------------------

static char* readFile(const char* path) {
    // See the matching comment in main.c's readFile — binary mode plus
    // null-terminating at the actual fread() return value (not the
    // pre-read file size) avoids a Windows-only bug where CRLF
    // translation in text mode leaves uninitialized memory between the
    // real end of the source and the terminator.
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    char* source = malloc(size + 1);
    size_t bytesRead = fread(source, 1, size, f);
    source[bytesRead] = '\0';
    fclose(f);
    return source;
}

// Returns a pointer into `line` past any leading whitespace, and trims
// trailing whitespace/newline in place. Doesn't allocate — `line` must
// be mutable (it's always a line we already own a copy of).
static char* trim(char* line) {
    while (*line == ' ' || *line == '\t') line++;
    int len = (int)strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' ||
                        line[len - 1] == ' '  || line[len - 1] == '\t')) {
        line[--len] = '\0';
    }
    return line;
}

// Compares two blocks of text, ignoring a single trailing newline on
// either side — forgiving about whether the very last line ends with
// one, which is easy to get inconsistent when hand-writing test files.
static int outputsMatch(const char* a, const char* b) {
    int lenA = (int)strlen(a), lenB = (int)strlen(b);
    if (lenA > 0 && a[lenA - 1] == '\n') lenA--;
    if (lenB > 0 && b[lenB - 1] == '\n') lenB--;
    return lenA == lenB && memcmp(a, b, lenA) == 0;
}

static void printIndented(const char* text) {
    if (*text == '\0') { printf("    (empty)\n"); return; }
    const char* line = text;
    while (*line) {
        const char* end = strchr(line, '\n');
        int len = end ? (int)(end - line) : (int)strlen(line);
        printf("    %.*s\n", len, line);
        line = end ? end + 1 : line + len;
    }
}

// --- running one test case -------------------------------------------------

// Runs `code` in a fresh, fully isolated VM and returns its captured
// stdout. Sets *errored if the program failed to parse/compile/run; in
// that case the returned string is the Nova error message instead of
// program output, so it still has something meaningful to diff/display.
static char* runSnippet(const char* code, int* errored) {
    *errored = 0;
    novaClearError();

    Parser parser;
    initParser(&parser, code);
    Program* program = parse(&parser);

    if (novaHasError()) {
        *errored = 1;
        char* msg = malloc(600);
        snprintf(msg, 600, "[parse error] %s", novaGetError()->message);
        return msg;
    }

    Chunk chunk;
    initChunk(&chunk);
    FunctionTable functions;
    initFunctionTable(&functions);
    compileProgram(program, &chunk, &functions);

    if (novaHasError()) {
        char* msg = malloc(600);
        snprintf(msg, 600, "[compile error] %s", novaGetError()->message);
        freeChunk(&chunk);
        *errored = 1;
        return msg;
    }

    // Redirect stdout to a temp file at the fd level (Windows-compatible).
    // Critical: flush stdout BEFORE redirecting, or any output already
    // queued in its buffer (e.g. the previous test's [PASS]/[FAIL] line)
    // ends up writing into the new fd instead of the real terminal.
    //
    // NOTE: we deliberately do NOT use tmpfile(). On Windows/MinGW,
    // tmpfile() tries to create its file in the root of the current
    // drive (e.g. C:\) by default, which commonly fails due to
    // permissions — and on failure it returns NULL with no obvious
    // signal, which previously caused fileno(NULL) to crash the whole
    // test run on the very first test. Creating the temp file explicitly
    // in the current directory avoids that, and we check every step.
    fflush(stdout);

    FILE* tmpf = fopen(".nova_test_capture.tmp", "w+b");
    if (!tmpf) {
        *errored = 1;
        char* msg = malloc(600);
        snprintf(msg, 600,
                 "[test runner error] could not create temp capture file "
                 "(.nova_test_capture.tmp) — check write permissions in "
                 "the current directory");
        freeChunk(&chunk);
        return msg;
    }

    int saved_fd = dup(fileno(stdout));
    if (saved_fd == -1) {
        *errored = 1;
        char* msg = malloc(600);
        snprintf(msg, 600, "[test runner error] dup(stdout) failed");
        fclose(tmpf);
        remove(".nova_test_capture.tmp");
        freeChunk(&chunk);
        return msg;
    }

    fflush(stdout);
    dup2(fileno(tmpf), fileno(stdout));

    VM vm;
    initVM(&vm);
    interpret(&vm, &chunk, NULL, &functions);

    fflush(stdout);
    dup2(saved_fd, fileno(stdout));
    close(saved_fd);

    // Read back the captured output.
    long size = ftell(tmpf);
    if (size < 0) size = 0; // defensive — ftell can fail, treat as empty capture
    rewind(tmpf);
    char* outputBuf = malloc(size + 1);
    fread(outputBuf, 1, size, tmpf);
    outputBuf[size] = '\0';
    fclose(tmpf);
    remove(".nova_test_capture.tmp"); // don't leave the scratch file lying around

    freeChunk(&chunk);

    if (novaHasError()) {
        *errored = 1;
        char* msg = malloc(size + 600);
        snprintf(msg, size + 600, "%s[runtime error] %s",
                 outputBuf, novaGetError()->message);
        free(outputBuf);
        return msg;
    }

    return outputBuf;
}

static int runOneTest(const char* name, const char* code, const char* expected) {
    int errored = 0;
    char* actual = runSnippet(code, &errored);

    // A test "passes" if its output matches expected — whether that
    // output came from normal execution or from an error message.
    // Tests that intentionally trigger an error encode the expected
    // [parse/compile/runtime error] text directly in their # EXPECT
    // block, so errored runs are graded the same way as clean ones.
    int passed = outputsMatch(actual, expected);

    if (passed) {
        printf("[PASS] %s\n", name);
    } else {
        printf("[FAIL] %s\n", name);
        printf("  expected:\n");
        printIndented(expected);
        printf("  got:\n");
        printIndented(actual);
    }

    free(actual);
    return passed;
}

// --- file-level parsing: scans for # TEST: / # EXPECT / # END --------------

typedef enum { SEEKING_TEST, COLLECTING_CODE, COLLECTING_EXPECTED } ScanState;

int runTestFile(const char* path) {
    // Force stdout unbuffered for the whole run. The fd-level redirection
    // in runSnippet() swaps stdout's underlying file out from under it
    // mid-run; without this, libc's default buffering can hold onto
    // output across that swap and attribute it to the wrong test.
    setvbuf(stdout, NULL, _IONBF, 0);

    char* source = readFile(path);
    if (!source) {
        fprintf(stderr, "Error: could not open file '%s'\n", path);
        return 1;
    }

    printf("Running tests from '%s'...\n\n", path);

    ScanState state = SEEKING_TEST;
    char testName[256] = "";
    StrBuf code, expected;
    sbInit(&code);
    sbInit(&expected);

    int total = 0, passed = 0;

    char* cursor = source;
    while (*cursor) {
        char* lineEnd = strchr(cursor, '\n');
        int   lineLen = lineEnd ? (int)(lineEnd - cursor) : (int)strlen(cursor);

        char line[2048];
        int copyLen = lineLen < (int)sizeof(line) - 1 ? lineLen : (int)sizeof(line) - 1;
        memcpy(line, cursor, copyLen);
        line[copyLen] = '\0';

        char trimmedCopy[2048];
        strcpy(trimmedCopy, line);
        char* trimmed = trim(trimmedCopy);

        switch (state) {
            case SEEKING_TEST:
                if (strncmp(trimmed, "# TEST:", 7) == 0) {
                    char* name = trim(trimmed + 7);
                    snprintf(testName, sizeof(testName), "%s", name);
                    sbFree(&code);
                    sbInit(&code);
                    state = COLLECTING_CODE;
                }
                break;

            case COLLECTING_CODE:
                if (strcmp(trimmed, "# EXPECT") == 0) {
                    sbFree(&expected);
                    sbInit(&expected);
                    state = COLLECTING_EXPECTED;
                } else {
                    sbAppendLine(&code, line);
                }
                break;

            case COLLECTING_EXPECTED:
                if (strcmp(trimmed, "# END") == 0) {
                    total++;
                    if (runOneTest(testName, code.data, expected.data)) passed++;
                    printf("\n");
                    state = SEEKING_TEST;
                } else {
                    sbAppendLine(&expected, line);
                }
                break;
        }

        cursor = lineEnd ? lineEnd + 1 : cursor + lineLen;
    }

    if (state != SEEKING_TEST) {
        fprintf(stderr,
            "Warning: file ended mid-test (missing '# EXPECT' or '# END') — "
            "last test ('%s') was not run.\n", testName);
    }

    sbFree(&code);
    sbFree(&expected);
    free(source);

    printf("%d/%d tests passed\n", passed, total);
    return passed == total ? 0 : 1;
}
