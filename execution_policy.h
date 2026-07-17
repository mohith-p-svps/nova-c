#ifndef EXECUTION_POLICY_H
#define EXECUTION_POLICY_H

// Chapter 46 (Safe Mode) / Chapter 50 (Capability Grants). Default
// (no --safe flag) is fully permissive — every module and package is
// usable, exactly as before this feature existed. With --safe, file,
// net, os, and external packages are all blocked UNLESS individually
// re-enabled via --allow=capability1,capability2,... . Modules with no
// real-world side effects (math, convert, map, arrays, string,
// datetime, json) are never gated — they're always available regardless
// of safe mode, matching the book's own framing of what "safe" means
// here (no filesystem, network, process, or arbitrary-code-from-a-
// package access without explicit opt-in).
typedef struct {
    int safeMode;
    int allowFile;
    int allowNet;
    int allowOs;
    int allowGraphics;  // no graphics module exists yet — reserved for when it does
    int allowPackages;
} ExecutionPolicy;

extern ExecutionPolicy gPolicy;

// Scans argv[1..argc) for --safe and --allow=list,of,capabilities,
// applies the result to gPolicy, and writes a FILTERED argv (with those
// two flags removed) into outArgv, returning the filtered count. This
// lets those flags appear anywhere on the command line without
// disturbing the positional argument parsing the rest of main() already
// does (e.g. `nova run script.nova --safe` and `nova --safe run
// script.nova` both work identically). `outArgv` must have room for at
// least `argc` pointers.
int filterExecutionPolicyArgs(int argc, char** argv, char** outArgv);

// Returns 1 if the named capability ("file", "net", "os", "graphics",
// or "packages") is currently permitted, considering safe mode and any
// --allow grants. Always 1 when safe mode is off.
int capabilityAllowed(const char* name, int length);

#endif
