#ifndef PACKAGE_LOADER_H
#define PACKAGE_LOADER_H

#include "chunk.h"

// Attempts to load a package named `name` — a plain .nova file, found by
// searching (in order) ./packages/<name>.nova, then
// ~/.nova/packages/<name>.nova (%USERPROFILE% on Windows). On success,
// parses and compiles it, runs its top-level code once in an isolated
// VM (this is what "the package's module-level code" means in
// practice), and returns a pointer to its FunctionTable — stable and
// reusable for the rest of the process's life. Calling this again with
// the same name returns the same table without re-reading or re-running
// the file. On failure, returns NULL with an error already raised via
// novaError().
FunctionTable* loadPackage(const char* name);

#endif
