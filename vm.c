#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vm.h"
#include "value.h"
#include "error.h"
#include "ast.h"
#include "gc.h"
#include "natives/natives.h"

void initVM(VM* vm) {
    vm->stackTop     = vm->stack;
    vm->globalCount  = 0;
    vm->functions    = NULL;
    vm->currentFrame = NULL;
    vm->frameCount   = 0;
}

static void push(VM* vm, Value value) {
    if (vm->stackTop - vm->stack >= STACK_MAX) {
        novaError(ERR_TYPE, -1, "Stack overflow");
        return;
    }
    *vm->stackTop++ = value;
}

static Value pop(VM* vm) { return *--vm->stackTop; }

static Value peek(VM* vm) { return vm->stackTop[-1]; }

static int currentLine(VM* vm) {
    int offset = (int)(vm->ip - vm->chunk->code) - 1;
    return getLine(vm->chunk, offset);
}

static int findGlobal(VM* vm, const char* name) {
    for (int i = 0; i < vm->globalCount; i++)
        if (strcmp(vm->globals[i].name, name) == 0) return i;
    return -1;
}

static NovaFunction* findFunction(FunctionTable* functions, const char* name) {
    if (!functions) return NULL;
    for (int i = 0; i < functions->count; i++)
        if (strcmp(functions->functions[i].name, name) == 0) return &functions->functions[i];
    return NULL;
}

// Reads a big-endian 16-bit jump operand and advances ip past it.
static uint16_t readShort(VM* vm) {
    uint16_t hi = *vm->ip++;
    uint16_t lo = *vm->ip++;
    return (uint16_t)((hi << 8) | lo);
}

// Shared call machinery used by both OP_CALL (callee known by name,
// baked into the bytecode) and OP_CALL_VALUE (callee is a VAL_FUNCTION
// popped off the stack at runtime — see EXPR_CALL's compilation for
// when each path is taken). `args` is consumed (ownership transfers
// in); on success, *outResult receives the function's return value and
// this returns 1. On failure (arity mismatch, stack overflow, or an
// error raised inside the call), it returns 0 — the caller should
// check novaHasError() and unwind without pushing anything.
static int callNovaFunction(VM* vm, NovaFunction* fn, Value* args, int argCount, FunctionTable* fnTable, Value* outResult) {
    if (fn->arity != argCount) {
        novaError(ERR_ARGUMENT, currentLine(vm),
                  "Function '%s' expects %d argument(s) but got %d",
                  fn->name, fn->arity, argCount);
        for (int i = 0; i < argCount; i++) freeValue(args[i]);
        return 0;
    }

    if (vm->frameCount >= FRAMES_MAX) {
        novaError(ERR_TYPE, currentLine(vm), "Stack overflow (too much recursion)");
        for (int i = 0; i < argCount; i++) freeValue(args[i]);
        return 0;
    }

    // Heap-allocated rather than a C stack local: each CallFrame holds a
    // MAX_LOCALS-sized array, and recursive Nova calls become nested C
    // calls (one interpret() per call), so a stack-local frame here
    // would limit recursion depth to however deep C's own stack could
    // hold these large frames.
    CallFrame* newFrame = malloc(sizeof(CallFrame));
    memset(newFrame, 0, sizeof(CallFrame));
    for (int i = 0; i < argCount; i++) newFrame->locals[i] = args[i];
    newFrame->returnValue = makeNull();

    Chunk*         callerChunk     = vm->chunk;
    uint8_t*       callerIp        = vm->ip;
    CallFrame*     callerFrame     = vm->currentFrame;
    FunctionTable* callerFunctions = vm->functions;

    // Recorded purely for the GC's benefit (see frameStack's comment in
    // vm.h) — popped again right after the call returns, regardless of
    // success or error.
    vm->frameStack[vm->frameCount++] = newFrame;

    // `fnTable` — not necessarily vm->functions — is what the CALLEE's
    // body resolves its own unqualified calls against. This matters for
    // package functions specifically: a package function calling
    // another function defined in the SAME package (an ordinary,
    // unqualified call inside that package's own source) must resolve
    // against the package's own FunctionTable, not whatever table the
    // caller happens to be using — otherwise it could silently resolve
    // to an unrelated same-named function in the main program, or fail
    // to find one that genuinely exists in the package.
    interpret(vm, &fn->chunk, newFrame, fnTable);

    vm->frameCount--;
    vm->chunk        = callerChunk;
    vm->ip           = callerIp;
    vm->currentFrame = callerFrame;
    vm->functions    = callerFunctions;

    if (novaHasError()) { free(newFrame); return 0; }
    *outResult = newFrame->returnValue;
    free(newFrame);
    return 1;
}

// Public wrapper around callNovaFunction for native (C) module functions
// that need to invoke a Nova function value passed to them — e.g.
// arrays.map(arr, fn). A VAL_FUNCTION can only ever come from the
// currently-running program's own function table (package functions
// are never exposed as bare values, only callable via module.fn(...)
// syntax — see compiler.c's EXPR_VARIABLE), so vm->functions is always
// the right table for the callee's own body to resolve against here.
int callFunctionValue(VM* vm, Value funcVal, Value* args, int argCount, Value* outResult) {
    if (funcVal.type != VAL_FUNCTION) {
        novaError(ERR_TYPE, currentLine(vm), "Cannot call a %s", typeName(funcVal));
        for (int i = 0; i < argCount; i++) freeValue(args[i]);
        return 0;
    }
    return callNovaFunction(vm, funcVal.as.function, args, argCount, vm->functions, outResult);
}

// Runs `chunk` to completion (top-level OP_RETURN) or until a function
// body's implicit/explicit `send` (OP_FUNC_RETURN). `frame` holds the
// locals for this call — NULL when running top-level code. A function
// call (OP_CALL) recurses into this same function with a fresh frame, so
// Nova's call stack rides directly on C's: recursion depth is bounded by
// the C stack rather than by any frame array Nova manages itself.
void interpret(VM* vm, Chunk* chunk, CallFrame* frame, FunctionTable* functions) {
    vm->chunk        = chunk;
    vm->ip           = chunk->code;
    vm->currentFrame = frame;
    vm->functions    = functions;

    for (;;) {
        if (novaHasError()) return;
        maybeCollectGarbage(vm);

        uint8_t instruction = *vm->ip++;

        switch (instruction) {

        case OP_CONSTANT: {
            int index = *vm->ip++;
            push(vm, copyValue(chunk->constants[index]));
            break;
        }

        case OP_DEFINE_GLOBAL: {
            int   index = *vm->ip++;
            char* name  = chunk->names[index];
            Value val   = pop(vm);
            int   slot  = findGlobal(vm, name);
            if (slot == -1) { slot = vm->globalCount++; vm->globals[slot].name = name; }
            else freeValue(vm->globals[slot].value);
            vm->globals[slot].value = copyValue(val);
            freeValue(val);
            break;
        }

        case OP_SET_GLOBAL: {
            int   index = *vm->ip++;
            char* name  = chunk->names[index];
            Value val   = pop(vm);
            int   slot  = findGlobal(vm, name);
            if (slot == -1) {
                novaError(ERR_UNDECLARED_VARIABLE, currentLine(vm),
                          "Variable '%s' is not declared (use 'let' first)", name);
                freeValue(val);
                return;
            }
            freeValue(vm->globals[slot].value);
            vm->globals[slot].value = copyValue(val);
            freeValue(val);
            break;
        }

        case OP_GET_GLOBAL: {
            int   index = *vm->ip++;
            char* name  = chunk->names[index];
            int   slot  = findGlobal(vm, name);
            if (slot == -1) {
                novaError(ERR_UNDEFINED_VARIABLE, currentLine(vm),
                          "Variable '%s' is not defined", name);
                return;
            }
            push(vm, copyValue(vm->globals[slot].value));
            break;
        }

        case OP_GET_LOCAL: {
            int slot = *vm->ip++;
            push(vm, copyValue(vm->currentFrame->locals[slot]));
            break;
        }

        case OP_SET_LOCAL: {
            int slot = *vm->ip++;
            Value val = pop(vm);
            freeValue(vm->currentFrame->locals[slot]);
            vm->currentFrame->locals[slot] = val;
            break;
        }

        case OP_CALL: {
            int   nameIdx  = *vm->ip++;
            int   argCount = *vm->ip++;
            char* name     = chunk->names[nameIdx];

            NovaFunction* fn = findFunction(vm->functions, name);
            if (!fn) {
                novaError(ERR_UNDEFINED_FUNCTION, currentLine(vm),
                          "Function '%s' is not defined", name);
                for (int i = 0; i < argCount; i++) freeValue(pop(vm));
                return;
            }

            Value args[MAX_ARGS];
            for (int i = argCount - 1; i >= 0; i--) args[i] = pop(vm);

            Value result;
            if (!callNovaFunction(vm, fn, args, argCount, vm->functions, &result)) return;
            push(vm, result);
            break;
        }

        case OP_CALL_VALUE: {
            int argCount = *vm->ip++;
            Value args[MAX_ARGS];
            for (int i = argCount - 1; i >= 0; i--) args[i] = pop(vm);
            Value funcVal = pop(vm);

            if (funcVal.type != VAL_FUNCTION) {
                novaError(ERR_TYPE, currentLine(vm), "Cannot call a %s", typeName(funcVal));
                for (int i = 0; i < argCount; i++) freeValue(args[i]);
                freeValue(funcVal);
                return;
            }

            Value result;
            if (!callNovaFunction(vm, funcVal.as.function, args, argCount, vm->functions, &result)) return;
            push(vm, result);
            break;
        }

        case OP_CALL_NATIVE: {
            int hi = *vm->ip++, lo = *vm->ip++;
            int globalIdx = (hi << 8) | lo;
            int argCount  = *vm->ip++;

            Value args[MAX_ARGS];
            for (int i = argCount - 1; i >= 0; i--) args[i] = pop(vm);

            Value result = callNative(vm, globalIdx, args, argCount, currentLine(vm));
            for (int i = 0; i < argCount; i++) freeValue(args[i]);
            if (novaHasError()) return;
            push(vm, result);
            break;
        }

        case OP_CALL_PACKAGE_FN: {
            int tableSlot = *vm->ip++;
            int funcIndex = *vm->ip++;
            int argCount  = *vm->ip++;

            Value args[MAX_ARGS];
            for (int i = argCount - 1; i >= 0; i--) args[i] = pop(vm);

            FunctionTable* table = chunk->packageTables[tableSlot];
            NovaFunction*  fn    = &table->functions[funcIndex];

            Value result;
            if (!callNovaFunction(vm, fn, args, argCount, table, &result)) return;
            push(vm, result);
            break;
        }

        case OP_FUNC_RETURN: {
            Value v = pop(vm);
            if (vm->currentFrame) vm->currentFrame->returnValue = v;
            else freeValue(v); // shouldn't happen — defensive only
            return;
        }

        case OP_BUILD_ARRAY: {
            int count = *vm->ip++;
            Value* items = malloc(sizeof(Value) * count);
            // pushed left-to-right, so pop in reverse to restore order
            for (int i = count - 1; i >= 0; i--) items[i] = pop(vm);
            Value arr = makeArray();
            for (int i = 0; i < count; i++) arrayPush(arr.as.array, items[i]);
            free(items);
            push(vm, arr);
            break;
        }

        case OP_BUILD_MAP: {
            int pairCount = *vm->ip++;
            Value* keys = malloc(sizeof(Value) * pairCount);
            Value* vals = malloc(sizeof(Value) * pairCount);
            for (int i = pairCount - 1; i >= 0; i--) { vals[i] = pop(vm); keys[i] = pop(vm); }
            Value map = makeMap();
            for (int i = 0; i < pairCount; i++) mapSet(map.as.map, keys[i], vals[i]);
            free(keys);
            free(vals);
            push(vm, map);
            break;
        }

        case OP_INDEX_GET: {
            Value index  = pop(vm);
            Value object = pop(vm);

            if (object.type == VAL_ARRAY) {
                if (index.type != VAL_INT16 && index.type != VAL_INT32 && index.type != VAL_INT64) {
                    novaError(ERR_TYPE, currentLine(vm), "Array index must be an integer, got %s", typeName(index));
                    freeValue(object); freeValue(index); return;
                }
                int64_t i = index.type == VAL_INT16 ? index.as.i16
                          : index.type == VAL_INT32 ? index.as.i32 : index.as.i64;
                if (i < 0 || i >= object.as.array->count) {
                    novaError(ERR_INDEX_OUT_OF_BOUNDS, currentLine(vm),
                              "Array index %lld out of bounds (length %d)",
                              (long long)i, object.as.array->count);
                    freeValue(object); return;
                }
                push(vm, copyValue(object.as.array->items[i]));
            } else if (object.type == VAL_MAP) {
                Value out;
                if (!mapGet(object.as.map, index, &out)) {
                    novaError(ERR_INDEX_OUT_OF_BOUNDS, currentLine(vm), "Map has no such key");
                    freeValue(object); freeValue(index); return;
                }
                push(vm, copyValue(out));
                freeValue(index);
            } else {
                novaError(ERR_TYPE, currentLine(vm), "Cannot index into a %s", typeName(object));
                freeValue(object); freeValue(index); return;
            }
            break;
        }

        case OP_INDEX_SET: {
            Value value  = pop(vm);
            Value index  = pop(vm);
            Value object = pop(vm);

            if (object.type == VAL_ARRAY) {
                if (index.type != VAL_INT16 && index.type != VAL_INT32 && index.type != VAL_INT64) {
                    novaError(ERR_TYPE, currentLine(vm), "Array index must be an integer, got %s", typeName(index));
                    freeValue(value); return;
                }
                int64_t i = index.type == VAL_INT16 ? index.as.i16
                          : index.type == VAL_INT32 ? index.as.i32 : index.as.i64;
                if (i < 0 || i >= object.as.array->count) {
                    novaError(ERR_INDEX_OUT_OF_BOUNDS, currentLine(vm),
                              "Array index %lld out of bounds (length %d)",
                              (long long)i, object.as.array->count);
                    freeValue(value); return;
                }
                freeValue(object.as.array->items[i]);
                object.as.array->items[i] = value;
            } else if (object.type == VAL_MAP) {
                mapSet(object.as.map, index, value);
            } else {
                novaError(ERR_TYPE, currentLine(vm), "Cannot index into a %s", typeName(object));
                freeValue(index); freeValue(value); return;
            }
            break;
        }

        case OP_LEN: {
            Value v = pop(vm);
            int64_t len;
            switch (v.type) {
                case VAL_ARRAY:  len = v.as.array->count;    break;
                case VAL_MAP:    len = v.as.map->count;      break;
                case VAL_STRING: len = v.as.string->length;  break;
                default:
                    novaError(ERR_TYPE, currentLine(vm), "Cannot take len() of a %s", typeName(v));
                    freeValue(v);
                    return;
            }
            freeValue(v);
            push(vm, makeInt64(len));
            break;
        }

        case OP_TO_STRING: {
            Value v = pop(vm);
            Value s = valueToStringValue(v);
            freeValue(v); // valueToStringValue never frees its input — see its comment in value.c
            push(vm, s);
            break;
        }

        // OP_ADD dispatches to numeric add OR string concat based on runtime types
        case OP_ADD: {
            Value b = pop(vm);
            Value a = pop(vm);
            if (a.type == VAL_STRING && b.type == VAL_STRING) {
                push(vm, valueConcat(a, b));
            } else if (a.type == VAL_STRING || b.type == VAL_STRING) {
                novaError(ERR_TYPE, currentLine(vm),
                          "Cannot mix string and %s with '+'",
                          a.type == VAL_STRING ? typeName(b) : typeName(a));
                freeValue(a); freeValue(b);
                return;
            } else {
                push(vm, valueAdd(a, b));
            }
            break;
        }

        case OP_SUBTRACT: { Value b = pop(vm); Value a = pop(vm); push(vm, valueSub(a, b)); break; }
        case OP_MULTIPLY: { Value b = pop(vm); Value a = pop(vm); push(vm, valueMul(a, b)); break; }

        case OP_DIVIDE: {
            Value b = pop(vm); Value a = pop(vm);
            if ((b.type == VAL_INT16  && b.as.i16 == 0) ||
                (b.type == VAL_INT32  && b.as.i32 == 0) ||
                (b.type == VAL_INT64  && b.as.i64 == 0) ||
                (b.type == VAL_BIGINT && mpz_sgn(*b.as.bigint) == 0)) {
                novaError(ERR_DIVISION_BY_ZERO, currentLine(vm), "Division by zero");
                freeValue(a); freeValue(b); return;
            }
            push(vm, valueDiv(a, b));
            break;
        }

        case OP_MODULO: {
            Value b = pop(vm); Value a = pop(vm);
            if ((b.type == VAL_INT16  && b.as.i16 == 0) ||
                (b.type == VAL_INT32  && b.as.i32 == 0) ||
                (b.type == VAL_INT64  && b.as.i64 == 0) ||
                (b.type == VAL_BIGINT && mpz_sgn(*b.as.bigint) == 0)) {
                novaError(ERR_DIVISION_BY_ZERO, currentLine(vm), "Division by zero");
                freeValue(a); freeValue(b); return;
            }
            push(vm, valueMod(a, b));
            break;
        }

        case OP_CONCAT: {
            Value b = pop(vm); Value a = pop(vm);
            push(vm, valueConcat(a, b));
            break;
        }

        case OP_NOT: {
            Value v = pop(vm);
            push(vm, valueBoolNot(v));
            break;
        }

        case OP_NEGATE: {
            Value v = pop(vm);
            push(vm, valueNegate(v));
            break;
        }

        case OP_EQUAL:         { Value b = pop(vm); Value a = pop(vm); push(vm, valueEquals(a, b));        break; }
        case OP_NOT_EQUAL:      { Value b = pop(vm); Value a = pop(vm); push(vm, valueNotEquals(a, b));     break; }
        case OP_LESS:           { Value b = pop(vm); Value a = pop(vm); push(vm, valueLess(a, b));          break; }
        case OP_GREATER:        { Value b = pop(vm); Value a = pop(vm); push(vm, valueGreater(a, b));       break; }
        case OP_LESS_EQUAL:     { Value b = pop(vm); Value a = pop(vm); push(vm, valueLessEqual(a, b));     break; }
        case OP_GREATER_EQUAL:  { Value b = pop(vm); Value a = pop(vm); push(vm, valueGreaterEqual(a, b));  break; }

        case OP_PRINT: {
            Value newlineFlag = pop(vm);
            Value v = pop(vm);
            printValue(v);
            if (isTruthy(newlineFlag)) printf("\n");
            freeValue(v);
            freeValue(newlineFlag);
            break;
        }

        case OP_POP: {
            Value v = pop(vm);
            freeValue(v);
            break;
        }

        case OP_JUMP: {
            uint16_t offset = readShort(vm);
            vm->ip += offset;
            break;
        }

        case OP_JUMP_IF_FALSE: {
            uint16_t offset = readShort(vm);
            int truthy = isTruthyStrict(peek(vm));
            if (novaHasError()) return;
            if (!truthy) vm->ip += offset;
            break;
        }

        case OP_LOOP: {
            uint16_t offset = readShort(vm);
            vm->ip -= offset;
            break;
        }

        case OP_RETURN:
            return;
        }
    }
}
