// The `arrays` module — see Chapter 25 of the NovaLang book. The
// higher-order functions (map/filter/reduce) are the reason NativeFn
// carries a VM* — see natives.h and vm.h's callFunctionValue.

#include <stdlib.h>
#include <string.h>

#include "module_native_arrays.h"
#include "../vm.h"
#include "../error.h"

static int checkIsArray(Value v, const char* fnName, int line) {
    if (v.type != VAL_ARRAY) {
        novaError(ERR_ARGUMENT, line, "arrays.%s: expected an array (got %s)", fnName, typeName(v));
        return 0;
    }
    return 1;
}

static Value arrays_len(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    if (!checkIsArray(args[0], "len", line)) return makeNull();
    return makeInt64(args[0].as.array->count);
}

static Value arrays_push(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    if (!checkIsArray(args[0], "push", line)) return makeNull();
    arrayPush(args[0].as.array, copyValue(args[1]));
    return makeNull();
}

static Value arrays_pop(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    if (!checkIsArray(args[0], "pop", line)) return makeNull();
    NovaArray* a = args[0].as.array;
    if (a->count == 0) {
        novaError(ERR_INDEX_OUT_OF_BOUNDS, line, "arrays.pop: array is empty");
        return makeNull();
    }
    return a->items[--a->count];
}

static Value arrays_first(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    if (!checkIsArray(args[0], "first", line)) return makeNull();
    NovaArray* a = args[0].as.array;
    if (a->count == 0) { novaError(ERR_INDEX_OUT_OF_BOUNDS, line, "arrays.first: array is empty"); return makeNull(); }
    return copyValue(a->items[0]);
}

static Value arrays_last(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    if (!checkIsArray(args[0], "last", line)) return makeNull();
    NovaArray* a = args[0].as.array;
    if (a->count == 0) { novaError(ERR_INDEX_OUT_OF_BOUNDS, line, "arrays.last: array is empty"); return makeNull(); }
    return copyValue(a->items[a->count - 1]);
}

static Value arrays_contains(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    if (!checkIsArray(args[0], "contains", line)) return makeNull();
    NovaArray* a = args[0].as.array;
    for (int i = 0; i < a->count; i++)
        if (valueEquals(copyValue(a->items[i]), copyValue(args[1])).as.boolean) return TRUE_VAL;
    return FALSE_VAL;
}

static Value arrays_indexOf(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    if (!checkIsArray(args[0], "indexOf", line)) return makeNull();
    NovaArray* a = args[0].as.array;
    for (int i = 0; i < a->count; i++)
        if (valueEquals(copyValue(a->items[i]), copyValue(args[1])).as.boolean) return makeInt64(i);
    return makeInt64(-1);
}

static Value arrays_reverse(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    if (!checkIsArray(args[0], "reverse", line)) return makeNull();
    NovaArray* a = args[0].as.array;
    Value result = makeArray();
    for (int i = a->count - 1; i >= 0; i--)
        arrayPush(result.as.array, copyValue(a->items[i]));
    return result;
}

static Value arrays_slice(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    if (!checkIsArray(args[0], "slice", line)) return makeNull();
    NovaArray* a = args[0].as.array;
    int64_t start = args[1].type == VAL_INT64 ? args[1].as.i64 : args[1].type == VAL_INT32 ? args[1].as.i32 : args[1].as.i16;
    int64_t end   = args[2].type == VAL_INT64 ? args[2].as.i64 : args[2].type == VAL_INT32 ? args[2].as.i32 : args[2].as.i16;
    if (start < 0) start = 0;
    if (end > a->count) end = a->count;
    Value result = makeArray();
    for (int64_t i = start; i < end; i++)
        arrayPush(result.as.array, copyValue(a->items[i]));
    return result;
}

static Value arrays_join(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    if (!checkIsArray(args[0], "join", line)) return makeNull();
    if (args[1].type != VAL_STRING) {
        novaError(ERR_ARGUMENT, line, "arrays.join: separator must be a string");
        return makeNull();
    }
    NovaArray* a = args[0].as.array;
    const char* sep = args[1].as.string->data;
    int sepLen = args[1].as.string->length;

    int cap = 64, len = 0;
    char* buf = malloc(cap);
    buf[0] = '\0';
    for (int i = 0; i < a->count; i++) {
        Value s = valueToStringValue(a->items[i]);
        int pieceLen = s.as.string->length;
        int needed = len + pieceLen + sepLen + 1;
        if (needed > cap) { while (needed > cap) cap *= 2; buf = realloc(buf, cap); }
        memcpy(buf + len, s.as.string->data, pieceLen);
        len += pieceLen;
        freeValue(s);
        if (i < a->count - 1) {
            memcpy(buf + len, sep, sepLen);
            len += sepLen;
        }
    }
    buf[len] = '\0';
    Value result = makeString(buf, len);
    free(buf);
    return result;
}

// Simple insertion sort — arrays used with this are expected to be
// modest in size (this is a scripting-language convenience function,
// not a performance-critical primitive), so O(n^2) is an acceptable
// tradeoff for the simplicity of comparing via the existing valueLess.
static Value sortedCopy(NovaArray* a, int descending) {
    Value result = makeArray();
    for (int i = 0; i < a->count; i++)
        arrayPush(result.as.array, copyValue(a->items[i]));
    NovaArray* out = result.as.array;
    for (int i = 1; i < out->count; i++) {
        Value key = out->items[i];
        int j = i - 1;
        while (j >= 0) {
            // valueLess/valueGreater free their string arguments when
            // done comparing (they were designed for consuming values
            // already popped off the VM stack) — out->items[j] and key
            // are live array elements, not throwaway values, so they
            // must be passed as copies here or a second comparison
            // against the same element later becomes a use-after-free.
            int shouldSwap = descending
                ? valueLess(copyValue(out->items[j]), copyValue(key)).as.boolean
                : valueGreater(copyValue(out->items[j]), copyValue(key)).as.boolean;
            if (!shouldSwap) break;
            out->items[j + 1] = out->items[j];
            j--;
        }
        out->items[j + 1] = key;
    }
    return result;
}

static Value arrays_sort(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    if (!checkIsArray(args[0], "sort", line)) return makeNull();
    return sortedCopy(args[0].as.array, 0);
}

static Value arrays_sortDesc(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    if (!checkIsArray(args[0], "sortDesc", line)) return makeNull();
    return sortedCopy(args[0].as.array, 1);
}

static Value arrays_sum(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    if (!checkIsArray(args[0], "sum", line)) return makeNull();
    NovaArray* a = args[0].as.array;
    Value total = makeInt64(0);
    for (int i = 0; i < a->count; i++)
        total = valueAdd(total, copyValue(a->items[i]));
    return total;
}

static Value arrays_min(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    if (!checkIsArray(args[0], "min", line)) return makeNull();
    NovaArray* a = args[0].as.array;
    if (a->count == 0) { novaError(ERR_INDEX_OUT_OF_BOUNDS, line, "arrays.min: array is empty"); return makeNull(); }
    Value best = copyValue(a->items[0]);
    for (int i = 1; i < a->count; i++)
        if (valueLess(copyValue(a->items[i]), copyValue(best)).as.boolean) best = copyValue(a->items[i]);
    return best;
}

static Value arrays_max(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    if (!checkIsArray(args[0], "max", line)) return makeNull();
    NovaArray* a = args[0].as.array;
    if (a->count == 0) { novaError(ERR_INDEX_OUT_OF_BOUNDS, line, "arrays.max: array is empty"); return makeNull(); }
    Value best = copyValue(a->items[0]);
    for (int i = 1; i < a->count; i++)
        if (valueGreater(copyValue(a->items[i]), copyValue(best)).as.boolean) best = copyValue(a->items[i]);
    return best;
}

static Value arrays_unique(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    if (!checkIsArray(args[0], "unique", line)) return makeNull();
    NovaArray* a = args[0].as.array;
    Value result = makeArray();
    NovaArray* out = result.as.array;
    for (int i = 0; i < a->count; i++) {
        int seen = 0;
        for (int j = 0; j < out->count; j++)
            if (valueEquals(copyValue(out->items[j]), copyValue(a->items[i])).as.boolean) { seen = 1; break; }
        if (!seen) arrayPush(out, copyValue(a->items[i]));
    }
    return result;
}

static Value arrays_flatten(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    if (!checkIsArray(args[0], "flatten", line)) return makeNull();
    NovaArray* a = args[0].as.array;
    Value result = makeArray();
    for (int i = 0; i < a->count; i++) {
        if (a->items[i].type == VAL_ARRAY) {
            NovaArray* inner = a->items[i].as.array;
            for (int j = 0; j < inner->count; j++)
                arrayPush(result.as.array, copyValue(inner->items[j]));
        } else {
            arrayPush(result.as.array, copyValue(a->items[i]));
        }
    }
    return result;
}

static Value arrays_map(VM* vm, Value* args, int argCount, int line) {
    (void)argCount;
    if (!checkIsArray(args[0], "map", line)) return makeNull();
    NovaArray* a = args[0].as.array;
    Value result = makeArray();
    for (int i = 0; i < a->count; i++) {
        Value callArgs[1] = { copyValue(a->items[i]) };
        Value out;
        if (!callFunctionValue(vm, args[1], callArgs, 1, &out)) return makeNull();
        arrayPush(result.as.array, out);
    }
    return result;
}

static Value arrays_filter(VM* vm, Value* args, int argCount, int line) {
    (void)argCount;
    if (!checkIsArray(args[0], "filter", line)) return makeNull();
    NovaArray* a = args[0].as.array;
    Value result = makeArray();
    for (int i = 0; i < a->count; i++) {
        Value callArgs[1] = { copyValue(a->items[i]) };
        Value out;
        if (!callFunctionValue(vm, args[1], callArgs, 1, &out)) return makeNull();
        int keep = isTruthy(out);
        freeValue(out);
        if (keep) arrayPush(result.as.array, copyValue(a->items[i]));
    }
    return result;
}

static Value arrays_reduce(VM* vm, Value* args, int argCount, int line) {
    (void)argCount; (void)line;
    if (!checkIsArray(args[0], "reduce", line)) return makeNull();
    NovaArray* a = args[0].as.array;
    Value acc = copyValue(args[2]);
    for (int i = 0; i < a->count; i++) {
        Value callArgs[2] = { acc, copyValue(a->items[i]) };
        Value out;
        if (!callFunctionValue(vm, args[1], callArgs, 2, &out)) return makeNull();
        acc = out;
    }
    return acc;
}

static NativeFnEntry arraysFunctions[] = {
    {"len",      arrays_len,      1},
    {"push",     arrays_push,     2},
    {"pop",      arrays_pop,      1},
    {"first",    arrays_first,    1},
    {"last",     arrays_last,     1},
    {"contains", arrays_contains, 2},
    {"indexOf",  arrays_indexOf,  2},
    {"reverse",  arrays_reverse,  1},
    {"slice",    arrays_slice,    3},
    {"join",     arrays_join,     2},
    {"sort",     arrays_sort,     1},
    {"sortDesc", arrays_sortDesc, 1},
    {"sum",      arrays_sum,      1},
    {"min",      arrays_min,      1},
    {"max",      arrays_max,      1},
    {"unique",   arrays_unique,   1},
    {"flatten",  arrays_flatten,  1},
    {"map",      arrays_map,      2},
    {"filter",   arrays_filter,   2},
    {"reduce",   arrays_reduce,   3},
};

NativeModule arraysModule = {
    "arrays",
    arraysFunctions,
    sizeof(arraysFunctions) / sizeof(arraysFunctions[0])
};
