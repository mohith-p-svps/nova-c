#include <string.h>

#include "execution_policy.h"

ExecutionPolicy gPolicy;

static int matches(const char* s, int len, const char* lit) {
    return (int)strlen(lit) == len && strncmp(s, lit, len) == 0;
}

int filterExecutionPolicyArgs(int argc, char** argv, char** outArgv) {
    int sawSafe = 0;
    int grantFile = 0, grantNet = 0, grantOs = 0, grantGraphics = 0, grantPackages = 0;

    int outCount = 0;
    outArgv[outCount++] = argv[0];

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--safe") == 0) {
            sawSafe = 1;
            continue;
        }
        if (strncmp(argv[i], "--allow=", 8) == 0) {
            const char* p = argv[i] + 8;
            while (*p) {
                const char* start = p;
                while (*p && *p != ',') p++;
                int len = (int)(p - start);
                if      (matches(start, len, "file"))     grantFile = 1;
                else if (matches(start, len, "net"))      grantNet = 1;
                else if (matches(start, len, "os"))       grantOs = 1;
                else if (matches(start, len, "graphics")) grantGraphics = 1;
                else if (matches(start, len, "packages")) grantPackages = 1;
                if (*p == ',') p++;
            }
            continue;
        }
        outArgv[outCount++] = argv[i];
    }

    gPolicy.safeMode = sawSafe;
    if (sawSafe) {
        // Deny by default; only what was explicitly granted is allowed.
        gPolicy.allowFile     = grantFile;
        gPolicy.allowNet      = grantNet;
        gPolicy.allowOs       = grantOs;
        gPolicy.allowGraphics = grantGraphics;
        gPolicy.allowPackages = grantPackages;
    } else {
        gPolicy.allowFile = gPolicy.allowNet = gPolicy.allowOs =
            gPolicy.allowGraphics = gPolicy.allowPackages = 1;
    }

    return outCount;
}

int capabilityAllowed(const char* name, int length) {
    if (!gPolicy.safeMode) return 1;
    if (matches(name, length, "file"))     return gPolicy.allowFile;
    if (matches(name, length, "net"))      return gPolicy.allowNet;
    if (matches(name, length, "os"))       return gPolicy.allowOs;
    if (matches(name, length, "graphics")) return gPolicy.allowGraphics;
    if (matches(name, length, "packages")) return gPolicy.allowPackages;
    return 1; // unknown/ungated capability names are never blocked
}
