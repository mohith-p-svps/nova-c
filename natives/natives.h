#ifndef NATIVES_H
#define NATIVES_H

#include "../value.h"

// Forward-declared — only a pointer is needed here. Full definition in
// vm.h, which natives don't otherwise need to include.
typedef struct VM VM;

// A native module function: receives the running VM (needed only by
// functions like arrays.map/filter/reduce that call back into a Nova
// function value — see vm.h's callFunctionValue), already-evaluated
// arguments, and returns a Value. On error, call novaError(...) (same
// convention as everywhere else in the interpreter) and return
// makeNull() — the VM checks novaHasError() immediately after the call
// and halts if it's set, so the returned value is never actually used
// in that case.
typedef Value (*NativeFn)(VM* vm, Value* args, int argCount, int line);

typedef struct {
    const char* name;
    NativeFn    fn;
    int         arity; // exact argument count required
} NativeFnEntry;

typedef struct {
    const char*    name; // module name, e.g. "math" — what 'use math' matches against
    NativeFnEntry* functions;
    int            functionCount;
} NativeModule;

// --- Everything below is implemented once, in natives.c ------------------
//
// To add a new built-in module: write module_X.c/.h (see module_math.c
// for the pattern), then add exactly one line to the registry array at
// the top of natives.c. Nothing in the compiler, VM, or any other
// module's file needs to change.

// Returns the index of a built-in module named `name`, or -1.
int findNativeModule(const char* name, int length);

// Returns the module's own name (for error messages).
const char* nativeModuleName(int moduleIndex);

// Looks up a function by name within a given module (by index, from
// findNativeModule). Returns a GLOBAL function index (flat across every
// registered module, stable for the process's lifetime) or -1 if no
// such function exists in that module. `outArity` receives the
// function's required argument count when found.
int findNativeFunction(int moduleIndex, const char* name, int length, int* outArity);

// Invokes the native function at `globalIndex` (as returned by
// findNativeFunction) with the given arguments.
Value callNative(VM* vm, int globalIndex, Value* args, int argCount, int line);

#endif
