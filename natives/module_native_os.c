// The `os` module — see Chapter 30 of the NovaLang book.
//
// os.javaVersion() from the book is intentionally NOT implemented here
// — it's a leftover from the original Java implementation this
// interpreter doesn't have an equivalent of, and there's no meaningful
// stand-in worth returning under that name (per explicit decision:
// dropped, not repurposed or renamed).
//
// os.platform() returns a platform FAMILY ("Windows" / "Mac OS X" /
// "Linux"), not an exact build/version string like the book's
// "Windows 11" example — precise OS version detection is unreliable
// and partly deprecated in the Win32 API on modern Windows, so a family
// name is the honest, robust thing to return instead.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "module_native_os.h"
#include "../error.h"

#if defined(_WIN32)
#include <windows.h>
#include <direct.h>
#define getcwd _getcwd
#elif defined(__APPLE__)
#include <unistd.h>
#include <sys/time.h>
#include <sys/sysctl.h>
#else
#include <unistd.h>
#include <sys/time.h>
#endif

static int64_t asPlainInt(Value v) {
    if (v.type == VAL_INT64) return v.as.i64;
    if (v.type == VAL_INT32) return v.as.i32;
    if (v.type == VAL_INT16) return v.as.i16;
    return 0;
}

// --- os.input ---------------------------------------------------------

static Value os_input(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    if (args[0].type != VAL_STRING) {
        novaError(ERR_ARGUMENT, line, "os.input: prompt must be a string (got %s)", typeName(args[0]));
        return makeNull();
    }
    printf("%s", args[0].as.string->data);
    fflush(stdout);

    char buf[4096];
    if (!fgets(buf, sizeof(buf), stdin)) return makeString("", 0);
    int len = (int)strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) len--;
    return makeString(buf, len);
}

// --- os.exit / os.sleep -------------------------------------------------

static Value os_exit(VM* vm, Value* args, int argCount, int line) {
    (void)vm;
    if (argCount > 1) {
        novaError(ERR_ARGUMENT, line, "os.exit expects 0 or 1 argument(s) but got %d", argCount);
        return makeNull();
    }
    int code = argCount == 1 ? (int)asPlainInt(args[0]) : 0;
    exit(code);
    return makeNull(); // unreachable
}

static Value os_sleep(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount; (void)line;
    int64_t ms = asPlainInt(args[0]);
    if (ms < 0) ms = 0;
#if defined(_WIN32)
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
    return makeNull();
}

// --- os.time / os.clock ------------------------------------------------

static int64_t epochMillis(void) {
#if defined(_WIN32)
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    // FILETIME is 100ns intervals since 1601-01-01; shift to Unix epoch.
    return (int64_t)((t / 10000ULL) - 11644473600000ULL);
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
#endif
}

// Monotonic milliseconds, used only to compute elapsed durations (never
// compared against a wall-clock timestamp) — immune to system clock
// adjustments, which a wall-clock-based stopwatch would not be.
static int64_t monotonicMillis(void) {
#if defined(_WIN32)
    static LARGE_INTEGER freq;
    static int haveFreq = 0;
    if (!haveFreq) { QueryPerformanceFrequency(&freq); haveFreq = 1; }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (int64_t)(now.QuadPart * 1000 / freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

// os.clock() reports milliseconds since the FIRST call to it in this
// run (lazily captured) — not CPU time the way <time.h>'s own clock()
// measures it, which would misreport anything involving os.sleep()
// (sleeping consumes no CPU time, so a CPU-time clock would show ~0ms
// elapsed across a sleep) — the book's own example times a CPU-bound
// loop, but the general "measure how long an operation took" framing
// implies wall-clock elapsed time, which this is.
static int64_t clockStartMs = -1;

static Value os_time(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)args; (void)argCount; (void)line;
    return makeInt64(epochMillis());
}

static Value os_clock(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)args; (void)argCount; (void)line;
    if (clockStartMs < 0) clockStartMs = monotonicMillis();
    return makeInt64(monotonicMillis() - clockStartMs);
}

// --- os.random / os.randomInt --------------------------------------------

static int randomSeeded = 0;
static void ensureSeeded(void) {
    if (!randomSeeded) { srand((unsigned int)time(NULL)); randomSeeded = 1; }
}

static Value os_random(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)args; (void)argCount; (void)line;
    ensureSeeded();
    return makeFloat64((double)rand() / ((double)RAND_MAX + 1.0));
}

static Value os_randomInt(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount; (void)line;
    ensureSeeded();
    int64_t lo = asPlainInt(args[0]);
    int64_t hi = asPlainInt(args[1]);
    if (hi < lo) { int64_t t = lo; lo = hi; hi = t; }
    int64_t range = hi - lo + 1;
    return makeInt64(lo + (int64_t)(rand() % range));
}

// --- system information -------------------------------------------------

static Value os_platform(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)args; (void)argCount; (void)line;
#if defined(_WIN32)
    return makeString("Windows", 7);
#elif defined(__APPLE__)
    return makeString("Mac OS X", 8);
#else
    return makeString("Linux", 5);
#endif
}

static Value os_username(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)args; (void)argCount; (void)line;
#if defined(_WIN32)
    const char* u = getenv("USERNAME");
#else
    const char* u = getenv("USER");
    if (!u) u = getenv("LOGNAME");
#endif
    if (!u) u = "";
    return makeString(u, (int)strlen(u));
}

static Value os_cpuCount(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)args; (void)argCount; (void)line;
#if defined(_WIN32)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return makeInt64(si.dwNumberOfProcessors);
#elif defined(__APPLE__)
    int count = 1;
    size_t size = sizeof(count);
    sysctlbyname("hw.ncpu", &count, &size, NULL, 0);
    return makeInt64(count);
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return makeInt64(n > 0 ? n : 1);
#endif
}

static Value os_workdir(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)args; (void)argCount; (void)line;
    char buf[1024];
    if (!getcwd(buf, sizeof(buf))) return makeString("", 0);
    return makeString(buf, (int)strlen(buf));
}

static Value os_homedir(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)args; (void)argCount; (void)line;
#if defined(_WIN32)
    const char* h = getenv("USERPROFILE");
#else
    const char* h = getenv("HOME");
#endif
    if (!h) h = "";
    return makeString(h, (int)strlen(h));
}

static NativeFnEntry osFunctions[] = {
    {"input",     os_input,     1},
    {"exit",      os_exit,      -1}, // exit() or exit(code) — variable arity, see natives.h's comment on the sentinel
    {"sleep",     os_sleep,     1},
    {"time",      os_time,      0},
    {"clock",     os_clock,     0},
    {"random",    os_random,    0},
    {"randomInt", os_randomInt, 2},
    {"platform",  os_platform,  0},
    {"username",  os_username,  0},
    {"cpuCount",  os_cpuCount,  0},
    {"workdir",   os_workdir,   0},
    {"homedir",   os_homedir,   0},
};

NativeModule osModule = {
    "os",
    osFunctions,
    sizeof(osFunctions) / sizeof(osFunctions[0])
};
