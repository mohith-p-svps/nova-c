#ifndef GC_H
#define GC_H

#include <stddef.h>
#include "value.h"

// VM is only ever used here as a pointer — gc.c includes vm.h itself to
// get the full definition. Forward-declaring it here lets gc.h stay
// independent of vm.h (which doesn't need to know about the GC at all;
// only vm.c does, to trigger collection and supply roots).
typedef struct VM VM;

// Allocates a GC-tracked object of `size` bytes, tags it with `type`
// (VAL_ARRAY or VAL_MAP), and links it into the global allocation list.
// `size` must be sizeof(NovaArray) or sizeof(NovaMap) — whichever struct
// embeds the GCObject header as its first member, since this function
// hands back a raw pointer the caller then casts and initializes.
void* gcAllocateObject(size_t size, ValueType type);

// Marks `v` and (for arrays/maps) everything reachable from it. Safe to
// call on any Value — non-GC-tracked types are simply ignored.
void markValue(Value v);

// Runs a full mark-and-sweep cycle right now, using `vm`'s stack,
// globals, and active call frames as roots.
void collectGarbage(VM* vm);

// Runs collectGarbage(vm) only if the number of live tracked objects has
// crossed the current threshold. This is what vm.c calls between
// instructions — cheap to call constantly, since it's a no-op until
// enough garbage has piled up to be worth a sweep.
void maybeCollectGarbage(VM* vm);

// Diagnostics — how many array/map objects are currently tracked.
size_t gcObjectCount(void);

#endif
