#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "package_loader.h"
#include "parser.h"
#include "compiler.h"
#include "vm.h"
#include "error.h"

#define MAX_LOADED_PACKAGES 64

typedef struct {
    char*          name;
    FunctionTable* table;
} LoadedPackage;

static LoadedPackage loadedPackages[MAX_LOADED_PACKAGES];
static int           loadedPackageCount = 0;

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

// Candidate search paths, tried in order. `./packages/` is a local,
// no-install-step convenience for development; ~/.nova/packages/ (or
// %USERPROFILE%\.nova\packages\ on Windows) matches the book's
// documented install location for `nova install`.
static char* findPackageFile(const char* name) {
    char path[1024];

    snprintf(path, sizeof(path), "./packages/%s.nova", name);
    FILE* f = fopen(path, "r");
    if (f) { fclose(f); return strdup(path); }

    const char* home = getenv("HOME");
    if (!home) home = getenv("USERPROFILE");
    if (home) {
        snprintf(path, sizeof(path), "%s/.nova/packages/%s.nova", home, name);
        f = fopen(path, "r");
        if (f) { fclose(f); return strdup(path); }
    }

    return NULL;
}

FunctionTable* loadPackage(const char* name) {
    for (int i = 0; i < loadedPackageCount; i++)
        if (strcmp(loadedPackages[i].name, name) == 0)
            return loadedPackages[i].table;

    char* path = findPackageFile(name);
    if (!path) {
        novaError(ERR_PARSE, -1,
                  "Package '%s' not found (looked in ./packages/ and ~/.nova/packages/)", name);
        return NULL;
    }

    char* source = readFile(path);
    free(path);
    if (!source) {
        novaError(ERR_PARSE, -1, "Package '%s': its file could not be read", name);
        return NULL;
    }

    Parser parser;
    initParser(&parser, source);
    Program* program = parse(&parser);
    if (novaHasError()) { free(source); return NULL; }

    FunctionTable* table = malloc(sizeof(FunctionTable));
    initFunctionTable(table);
    Chunk* topLevel = malloc(sizeof(Chunk));
    initChunk(topLevel);
    compileProgram(program, topLevel, table);
    if (novaHasError()) { free(source); return NULL; }

    // Run the package's own top-level code once, in its own isolated
    // VM — matching the book's "runs the file" description. Most real
    // packages will have little or no top-level code beyond `fn`
    // definitions, in which case this does nothing observable.
    VM vm;
    initVM(&vm);
    interpret(&vm, topLevel, NULL, table);
    if (novaHasError()) { free(source); return NULL; }

    if (loadedPackageCount < MAX_LOADED_PACKAGES) {
        loadedPackages[loadedPackageCount].name  = strdup(name);
        loadedPackages[loadedPackageCount].table = table;
        loadedPackageCount++;
    }

    // `source`, `topLevel`, and `table` are all intentionally left
    // alive for the rest of the process's life rather than freed here —
    // the package's function chunks (embedded in `table`) may still be
    // called at any point later in the program, and this compiler's
    // existing convention (see makeHiddenToken, etc.) is to not bother
    // freeing compile-time memory in a short-lived script interpreter.
    return table;
}
