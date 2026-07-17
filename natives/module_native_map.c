// The `map` module — see Chapter 28 of the NovaLang book.

#include <string.h>

#include "module_native_map.h"
#include "../error.h"

static int checkIsMap(Value v, const char* fnName, int line) {
    if (v.type != VAL_MAP) {
        novaError(ERR_ARGUMENT, line, "map.%s: expected a map (got %s)", fnName, typeName(v));
        return 0;
    }
    return 1;
}

static Value map_has(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    if (!checkIsMap(args[0], "has", line)) return makeNull();
    Value out;
    return mapGet(args[0].as.map, args[1], &out) ? TRUE_VAL : FALSE_VAL;
}

static Value map_get(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    if (!checkIsMap(args[0], "get", line)) return makeNull();
    Value out;
    if (mapGet(args[0].as.map, args[1], &out)) return copyValue(out);
    return makeNull();
}

static Value map_set(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    if (!checkIsMap(args[0], "set", line)) return makeNull();
    mapSet(args[0].as.map, copyValue(args[1]), copyValue(args[2]));
    return makeNull();
}

static Value map_remove(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    if (!checkIsMap(args[0], "remove", line)) return makeNull();
    NovaMap* m = args[0].as.map;
    for (int i = 0; i < m->count; i++) {
        Value eq = valueEquals(copyValue(m->keys[i]), copyValue(args[1]));
        if (eq.as.boolean) {
            Value removed = copyValue(m->values[i]);
            for (int j = i; j < m->count - 1; j++) {
                m->keys[j]   = m->keys[j + 1];
                m->values[j] = m->values[j + 1];
            }
            m->count--;
            return removed;
        }
    }
    return makeNull();
}

static Value map_size(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    if (!checkIsMap(args[0], "size", line)) return makeNull();
    return makeInt64(args[0].as.map->count);
}

static Value map_keys(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    if (!checkIsMap(args[0], "keys", line)) return makeNull();
    NovaMap* m = args[0].as.map;
    Value result = makeArray();
    for (int i = 0; i < m->count; i++)
        arrayPush(result.as.array, copyValue(m->keys[i]));
    return result;
}

static Value map_values(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    if (!checkIsMap(args[0], "values", line)) return makeNull();
    NovaMap* m = args[0].as.map;
    Value result = makeArray();
    for (int i = 0; i < m->count; i++)
        arrayPush(result.as.array, copyValue(m->values[i]));
    return result;
}

static Value map_entries(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    if (!checkIsMap(args[0], "entries", line)) return makeNull();
    NovaMap* m = args[0].as.map;
    Value result = makeArray();
    for (int i = 0; i < m->count; i++) {
        Value pair = makeArray();
        arrayPush(pair.as.array, copyValue(m->keys[i]));
        arrayPush(pair.as.array, copyValue(m->values[i]));
        arrayPush(result.as.array, pair);
    }
    return result;
}

static Value map_merge(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    if (!checkIsMap(args[0], "merge", line)) return makeNull();
    if (!checkIsMap(args[1], "merge", line)) return makeNull();
    NovaMap* a = args[0].as.map;
    NovaMap* b = args[1].as.map;
    Value result = makeMap();
    for (int i = 0; i < a->count; i++)
        mapSet(result.as.map, copyValue(a->keys[i]), copyValue(a->values[i]));
    for (int i = 0; i < b->count; i++)
        mapSet(result.as.map, copyValue(b->keys[i]), copyValue(b->values[i])); // b overwrites on key collision
    return result;
}

static Value map_clear(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    if (!checkIsMap(args[0], "clear", line)) return makeNull();
    args[0].as.map->count = 0;
    return makeNull();
}

static NativeFnEntry mapFunctions[] = {
    {"has",     map_has,     2},
    {"get",     map_get,     2},
    {"set",     map_set,     3},
    {"remove",  map_remove,  2},
    {"size",    map_size,    1},
    {"keys",    map_keys,    1},
    {"values",  map_values,  1},
    {"entries", map_entries, 1},
    {"merge",   map_merge,   2},
    {"clear",   map_clear,   1},
};

NativeModule mapModule = {
    "map",
    mapFunctions,
    sizeof(mapFunctions) / sizeof(mapFunctions[0])
};
