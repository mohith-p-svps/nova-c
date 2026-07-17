#ifndef VM_H
#define VM_H

#include "chunk.h"
#include "value.h"

#define STACK_MAX  8192
#define FRAMES_MAX 10000

typedef struct {
    char*  name;
    Value  value;
} Global;

// A function call's local variables and its pending return value. Nova has
// no closures, so a frame is entirely self-contained — no link to any
// enclosing scope is needed. Frames live on the C call stack (one per
// nested call to interpret()), so recursion depth is just C recursion
// depth.
typedef struct {
    Value locals[MAX_LOCALS];
    Value returnValue;
} CallFrame;

struct VM {
    Chunk*   chunk;
    uint8_t* ip;

    Value    stack[STACK_MAX];
    Value*   stackTop;

    Global   globals[256];
    int      globalCount;

    FunctionTable* functions;
    CallFrame*     currentFrame;   // NULL while executing top-level code

    // Every currently-active CallFrame, pushed/popped by OP_CALL right
    // alongside the recursive interpret() calls that actually drive
    // execution. This array doesn't participate in control flow at all
    // — it exists purely so the garbage collector's mark phase (gc.c)
    // has somewhere to find every locals array currently in play, since
    // C's own call stack isn't something Nova can introspect.
    CallFrame* frameStack[FRAMES_MAX];
    int        frameCount;
};
typedef struct VM VM;

void initVM(VM* vm);

void interpret(
    VM* vm,
    Chunk* chunk,
    CallFrame* frame,
    FunctionTable* functions
);

// Calls a Nova function VALUE (a VAL_FUNCTION) with the given arguments,
// from native (C) code — used by natives like arrays.map/filter/reduce
// that need to invoke a callback the Nova program passed them. `args`
// is consumed. Returns 1 and sets *outResult on success; returns 0 (with
// novaError already raised) on failure — the caller should propagate
// that failure rather than using *outResult.
int callFunctionValue(VM* vm, Value funcVal, Value* args, int argCount, Value* outResult);

#endif
