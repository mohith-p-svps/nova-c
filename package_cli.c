#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#if defined(_WIN32)
#include <direct.h>
#endif

#include "package_cli.h"

// Manifest format: one line per package, pipe-separated:
//   name|sourcePath|installDateISO8601
// Deliberately simple plain-text rather than JSON — this is an internal
// bookkeeping file, not something a person is expected to hand-edit or
// a program other than `nova` itself is expected to read.

static void packagesDir(char* out, size_t outSize) {
    const char* home = getenv("HOME");
    if (!home) home = getenv("USERPROFILE");
    if (home) {
        snprintf(out, outSize, "%s/.nova/packages", home);
    } else {
        snprintf(out, outSize, "./packages");
    }
}

static void manifestPath(char* out, size_t outSize) {
    char dir[900];
    packagesDir(dir, sizeof(dir));
    snprintf(out, outSize, "%s/.manifest", dir);
}

// Best-effort recursive-enough mkdir for the two-level path we actually
// need (~/.nova and ~/.nova/packages) — not a general mkdir -p.
static void ensureDirExists(const char* path) {
#if defined(_WIN32)
    _mkdir(path);
#else
    mkdir(path, 0755);
#endif
}

static void ensurePackagesDirExists(void) {
    const char* home = getenv("HOME");
    if (!home) home = getenv("USERPROFILE");
    if (!home) { ensureDirExists("./packages"); return; }

    char novaDir[900];
    snprintf(novaDir, sizeof(novaDir), "%s/.nova", home);
    ensureDirExists(novaDir);

    char pkgDir[900];
    snprintf(pkgDir, sizeof(pkgDir), "%s/.nova/packages", home);
    ensureDirExists(pkgDir);
}

typedef struct {
    char name[128];
    char source[768];
    char date[32];
} ManifestEntry;

static int readManifest(ManifestEntry* entries, int maxEntries) {
    char path[1024];
    manifestPath(path, sizeof(path));
    FILE* f = fopen(path, "r");
    if (!f) return 0;

    int count = 0;
    char line[1024];
    while (count < maxEntries && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0') continue;
        char* name   = strtok(line, "|");
        char* source = strtok(NULL, "|");
        char* date   = strtok(NULL, "|");
        if (!name || !source || !date) continue;
        snprintf(entries[count].name,   sizeof(entries[count].name),   "%s", name);
        snprintf(entries[count].source, sizeof(entries[count].source), "%s", source);
        snprintf(entries[count].date,   sizeof(entries[count].date),   "%s", date);
        count++;
    }
    fclose(f);
    return count;
}

static void writeManifest(ManifestEntry* entries, int count) {
    char path[1024];
    manifestPath(path, sizeof(path));
    FILE* f = fopen(path, "w");
    if (!f) return;
    for (int i = 0; i < count; i++)
        fprintf(f, "%s|%s|%s\n", entries[i].name, entries[i].source, entries[i].date);
    fclose(f);
}

int packageInstall(const char* name, const char* localPath) {
    FILE* src = fopen(localPath, "rb");
    if (!src) {
        fprintf(stderr, "Error: could not open '%s'\n", localPath);
        return 1;
    }

    ensurePackagesDirExists();
    char dir[900];
    packagesDir(dir, sizeof(dir));
    char destPath[1024];
    snprintf(destPath, sizeof(destPath), "%s/%s.nova", dir, name);

    FILE* dest = fopen(destPath, "wb");
    if (!dest) {
        fprintf(stderr, "Error: could not write to '%s'\n", destPath);
        fclose(src);
        return 1;
    }

    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0)
        fwrite(buf, 1, n, dest);
    fclose(src);
    fclose(dest);

    ManifestEntry entries[256];
    int count = readManifest(entries, 256);

    time_t now = time(NULL);
    struct tm* tmNow = localtime(&now);
    char dateStr[32];
    strftime(dateStr, sizeof(dateStr), "%Y-%m-%d", tmNow);

    int found = 0;
    for (int i = 0; i < count; i++) {
        if (strcmp(entries[i].name, name) == 0) {
            snprintf(entries[i].source, sizeof(entries[i].source), "%s", localPath);
            snprintf(entries[i].date,   sizeof(entries[i].date),   "%s", dateStr);
            found = 1;
            break;
        }
    }
    if (!found && count < 256) {
        snprintf(entries[count].name,   sizeof(entries[count].name),   "%s", name);
        snprintf(entries[count].source, sizeof(entries[count].source), "%s", localPath);
        snprintf(entries[count].date,   sizeof(entries[count].date),   "%s", dateStr);
        count++;
    }
    writeManifest(entries, count);

    printf("Installed '%s' -> %s\n", name, destPath);
    return 0;
}

int packageList(void) {
    ManifestEntry entries[256];
    int count = readManifest(entries, 256);
    if (count == 0) {
        printf("No packages installed.\n");
        return 0;
    }
    printf("Installed packages:\n");
    for (int i = 0; i < count; i++)
        printf("  %s\n", entries[i].name);
    return 0;
}

int packageInfo(const char* name) {
    ManifestEntry entries[256];
    int count = readManifest(entries, 256);
    for (int i = 0; i < count; i++) {
        if (strcmp(entries[i].name, name) == 0) {
            char dir[900];
            packagesDir(dir, sizeof(dir));
            char path[1024];
            snprintf(path, sizeof(path), "%s/%s.nova", dir, name);

            struct stat st;
            long size = (stat(path, &st) == 0) ? (long)st.st_size : -1;

            printf("Package: %s\n", entries[i].name);
            printf("  Installed from: %s\n", entries[i].source);
            printf("  Installed on:   %s\n", entries[i].date);
            if (size >= 0) printf("  Size:           %ld bytes\n", size);
            printf("  Location:       %s\n", path);
            return 0;
        }
    }
    fprintf(stderr, "Error: no package named '%s' is installed\n", name);
    return 1;
}

int packageRemove(const char* name) {
    ManifestEntry entries[256];
    int count = readManifest(entries, 256);

    int foundIdx = -1;
    for (int i = 0; i < count; i++)
        if (strcmp(entries[i].name, name) == 0) { foundIdx = i; break; }

    if (foundIdx == -1) {
        fprintf(stderr, "Error: no package named '%s' is installed\n", name);
        return 1;
    }

    char dir[900];
    packagesDir(dir, sizeof(dir));
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.nova", dir, name);
    remove(path);

    for (int i = foundIdx; i < count - 1; i++) entries[i] = entries[i + 1];
    count--;
    writeManifest(entries, count);

    printf("Removed package '%s'\n", name);
    return 0;
}

int packageIsInstalled(const char* name) {
    ManifestEntry entries[256];
    int count = readManifest(entries, 256);
    for (int i = 0; i < count; i++)
        if (strcmp(entries[i].name, name) == 0) return 1;
    return 0;
}
