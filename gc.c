#include <stdio.h>
#include <stdlib.h>

#include "gc.h"
#include "vm.h"

// A single global allocation list and threshold. Nova only ever runs one
// VM per process (see main.c), so there's no need to thread this state
// through every call site the way `vm` itself is threaded — it would
// just be clutter for no real benefit.
static GCObject* gcObjects     = NULL;
static size_t    objectCount   = 0;
static size_t    gcThreshold   = 128;

// Set NOVA_GC_DEBUG=1 in the environment to print a line every time a
// collection runs — handy for actually seeing the collector do its job
// rather than taking it on faith.
static int gcDebugEnabled(void) {
    static int checked = 0, enabled = 0;
    if (!checked) { enabled = getenv("NOVA_GC_DEBUG") != NULL; checked = 1; }
    return enabled;
}

void* gcAllocateObject(size_t size, ValueType type) {
    GCObject* obj = malloc(size);
    obj->next     = gcObjects;
    obj->marked   = 0;
    obj->objType  = type;
    gcObjects     = obj;
    objectCount++;
    return obj;
}

void markValue(Value v) {
    if (v.type == VAL_ARRAY) {
        GCObject* obj = (GCObject*)v.as.array;
        if (obj->marked) return; // already visited — breaks cycles
        obj->marked = 1;
        for (int i = 0; i < v.as.array->count; i++)
            markValue(v.as.array->items[i]);
    } else if (v.type == VAL_MAP) {
        GCObject* obj = (GCObject*)v.as.map;
        if (obj->marked) return;
        obj->marked = 1;
        for (int i = 0; i < v.as.map->count; i++) {
            markValue(v.as.map->keys[i]);
            markValue(v.as.map->values[i]);
        }
    }
    // every other Value type holds nothing the GC tracks — nothing to do
}

// Roots: anything the running program can reach right now without going
// through another GC object first. Because we only ever collect between
// bytecode instructions (see maybeCollectGarbage's call site in vm.c),
// the operand stack always accurately reflects every live temporary —
// there's no "in-flight" value hiding in a C local that isn't also
// sitting on vm->stack, in a global, or in some active frame's locals.
static void markRoots(VM* vm) {
    for (Value* slot = vm->stack; slot < vm->stackTop; slot++)
        markValue(*slot);

    for (int i = 0; i < vm->globalCount; i++)
        markValue(vm->globals[i].value);

    // Active call frames don't live in one array on their own — Nova
    // calls recurse straight into C's call stack (see vm.c's OP_CALL),
    // so vm->frameStack is a side list of pointers to every frame
    // currently in play, pushed/popped right alongside those recursive
    // calls purely so the GC has somewhere to look.
    for (int i = 0; i < vm->frameCount; i++) {
        CallFrame* frame = vm->frameStack[i];
        for (int j = 0; j < MAX_LOCALS; j++)
            markValue(frame->locals[j]);
        markValue(frame->returnValue);
    }
}

static void freeObject(GCObject* obj) {
    switch (obj->objType) {
        case VAL_ARRAY: {
            NovaArray* a = (NovaArray*)obj;
            free(a->items);
            free(a);
            break;
        }
        case VAL_MAP: {
            NovaMap* m = (NovaMap*)obj;
            free(m->keys);
            free(m->values);
            free(m);
            break;
        }
        default:
            free(obj);
            break;
    }
}

static void sweep(void) {
    GCObject** link = &gcObjects;
    while (*link != NULL) {
        if ((*link)->marked) {
            (*link)->marked = 0; // reset for the next cycle
            link = &(*link)->next;
        } else {
            GCObject* unreached = *link;
            *link = unreached->next;
            freeObject(unreached);
            objectCount--;
        }
    }
}

void collectGarbage(VM* vm) {
    size_t before = objectCount;
    markRoots(vm);
    sweep();
    // Grow the threshold relative to what actually survived, so a
    // program that keeps a lot of arrays alive doesn't immediately
    // re-trigger on the very next allocation.
    gcThreshold = objectCount < 64 ? 128 : objectCount * 2;

    if (gcDebugEnabled()) {
        fprintf(stderr, "[gc] collected %zu objects, %zu remain (next at %zu)\n",
                before - objectCount, objectCount, gcThreshold);
    }
}

void maybeCollectGarbage(VM* vm) {
    if (objectCount >= gcThreshold) collectGarbage(vm);
}

size_t gcObjectCount(void) { return objectCount; }
