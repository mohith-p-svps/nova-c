#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

#include "plugin_loader.h"
#include "package_cli.h"

#if defined(_WIN32)
#include <windows.h>
#include <tlhelp32.h>
#else
#include <unistd.h>
#endif
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

// Fills `out` with the directory containing the running executable
// (no trailing slash), best-effort across platforms. Falls back to
// argv[0]'s own directory (or "." if argv[0] has no directory part) if
// the platform-specific lookup isn't available or fails — that fallback
// won't be exe-location-independent, but it keeps `nova` usable rather
// than refusing to start.
static void getExecutableDir(const char* argv0, char* out, size_t outSize) {
    char path[1024] = {0};

#if defined(_WIN32)
    DWORD n = GetModuleFileNameA(NULL, path, sizeof(path));
    if (n == 0 || n >= sizeof(path)) path[0] = '\0';
#elif defined(__APPLE__)
    uint32_t sz = sizeof(path);
    if (_NSGetExecutablePath(path, &sz) != 0) path[0] = '\0';
#else
    ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (n <= 0) path[0] = '\0'; else path[n] = '\0';
#endif

    if (path[0] == '\0') {
        // Platform lookup unavailable/failed — fall back to argv[0].
        snprintf(path, sizeof(path), "%s", argv0 ? argv0 : ".");
    }

    // Strip the executable's filename, keeping the directory part.
    // Handle both slash styles since argv[0] or GetModuleFileNameA can
    // hand back either, depending on how nova was launched.
    char* lastSlash     = strrchr(path, '/');
    char* lastBackslash = strrchr(path, '\\');
    char* cut = lastSlash;
    if (lastBackslash && (!cut || lastBackslash > cut)) cut = lastBackslash;

    if (cut) *cut = '\0';
    else     snprintf(path, sizeof(path), "."); // no directory part at all

    snprintf(out, outSize, "%s", path);
}

// Trims the trailing newline fgets leaves in place, and any surrounding
// whitespace, so a stray space before hitting enter doesn't read as "no".
static void trim(char* s) {
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r' || s[len - 1] == ' ' || s[len - 1] == '\t')) {
        s[--len] = '\0';
    }
}

static int askYesNo(const char* prompt) {
    printf("%s", prompt);
    fflush(stdout);

    char line[64];
    if (!fgets(line, sizeof(line), stdin)) return 0; // no input available (e.g. piped/non-interactive) — default to no
    trim(line);
    return (line[0] == 'y' || line[0] == 'Y');
}

// An identifier for the "session" nova is running in — in practice, the
// process ID of whatever launched us (the shell/terminal). This stays
// the same for every command run inside one terminal window/tab, and
// changes the moment a new terminal is opened, which is exactly the
// granularity the once-per-terminal plugin check wants: `./nova run`,
// `./nova version`, etc. all share one session key, but a second
// terminal window gets a different one.
static unsigned long getSessionKey(void) {
#if defined(_WIN32)
    DWORD myPid = GetCurrentProcessId();
    DWORD parentPid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32 pe;
        pe.dwSize = sizeof(pe);
        if (Process32First(snap, &pe)) {
            do {
                if (pe.th32ProcessID == myPid) {
                    parentPid = pe.th32ParentProcessID;
                    break;
                }
            } while (Process32Next(snap, &pe));
        }
        CloseHandle(snap);
    }
    return (unsigned long)parentPid;
#else
    return (unsigned long)getppid();
#endif
}

// Path to a small marker file recording "the plugin check already ran
// for this terminal session" — lives in the OS temp directory (not next
// to the executable, which might be read-only or shared between users).
// Note: process IDs do eventually get reused by the OS, so in rare cases
// (long-running machine, heavy process churn) a brand new terminal could
// theoretically inherit a stale marker from an old, unrelated session and
// skip a check it should have done. Harmless in practice — worst case
// you don't get re-prompted about a plugin sitting in the folder until
// the marker's session key comes back around.
static void getSessionMarkerPath(char* out, size_t outSize) {
    char tempDir[512];
#if defined(_WIN32)
    if (GetTempPathA(sizeof(tempDir), tempDir) == 0) snprintf(tempDir, sizeof(tempDir), ".");
#else
    const char* t = getenv("TMPDIR");
    snprintf(tempDir, sizeof(tempDir), "%s", (t && t[0]) ? t : "/tmp");
#endif
    size_t len = strlen(tempDir);
    if (len > 0 && (tempDir[len - 1] == '/' || tempDir[len - 1] == '\\')) tempDir[len - 1] = '\0';

    snprintf(out, outSize, "%s/.nova_plugins_checked_%lu", tempDir, getSessionKey());
}

int findPluginInFolder(const char* argv0, const char* name, char* outPath, size_t outSize) {
    char exeDir[900];
    getExecutableDir(argv0, exeDir, sizeof(exeDir));

    char filename[256];
    size_t len = strlen(name);
    if (len >= 5 && strcmp(name + len - 5, ".nova") == 0) {
        snprintf(filename, sizeof(filename), "%s", name);
    } else {
        snprintf(filename, sizeof(filename), "%s.nova", name);
    }

    char candidate[1300];
    snprintf(candidate, sizeof(candidate), "%s/plugins/%s", exeDir, filename);

    FILE* f = fopen(candidate, "rb");
    if (!f) return 0;
    fclose(f);

    snprintf(outPath, outSize, "%s", candidate);
    return 1;
}

void checkAndOfferPlugins(const char* argv0) {
    char markerPath[600];
    getSessionMarkerPath(markerPath, sizeof(markerPath));

    FILE* already = fopen(markerPath, "r");
    if (already) {
        fclose(already);
        return; // already checked once during this terminal session — just start
    }

    char exeDir[900];
    getExecutableDir(argv0, exeDir, sizeof(exeDir));

    char pluginsDir[1024];
    snprintf(pluginsDir, sizeof(pluginsDir), "%s/plugins", exeDir);

    DIR* dir = opendir(pluginsDir);
    if (dir) {
        struct dirent* entry;
        while ((entry = readdir(dir)) != NULL) {
            const char* filename = entry->d_name;
            size_t len = strlen(filename);
            if (len < 6 || strcmp(filename + len - 5, ".nova") != 0) continue; // not a .nova file

            char name[256];
            int nameLen = (int)len - 5; // strip ".nova"
            if (nameLen <= 0 || nameLen >= (int)sizeof(name)) continue;
            memcpy(name, filename, nameLen);
            name[nameLen] = '\0';

            if (packageIsInstalled(name)) continue; // already in use — just start, no prompt

            char fullPath[1500];
            snprintf(fullPath, sizeof(fullPath), "%s/%s", pluginsDir, filename);

            char prompt[512];
            snprintf(prompt, sizeof(prompt),
                     "Found plugin '%s' in the plugins folder — not currently in use.\n"
                     "Use it? [y/N]: ", name);

            if (askYesNo(prompt)) {
                packageInstall(name, fullPath); // prints its own "Installed '...' -> ..." confirmation
            } else {
                printf("Skipping '%s' for now — it'll be offered again next terminal session\n"
                       "(or run 'nova install %s' any time to use it right away).\n", name, name);
            }
        }
        closedir(dir);
    }

    // Mark this session as checked (whether or not a plugins folder even
    // existed) so every subsequent `./nova ...` in this same terminal
    // starts up silently instead of re-scanning.
    FILE* stamp = fopen(markerPath, "w");
    if (stamp) fclose(stamp);
}
