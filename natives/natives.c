#include <string.h>

#include "natives.h"
#include "module_native_math.h"
#include "module_native_convert.h"
#include "module_native_map.h"
#include "module_native_arrays.h"
#include "module_native_string.h"
#include "module_native_datetime.h"
#include "module_native_json.h"
#include "module_native_os.h"
#include "module_native_file.h"
#include "module_native_net.h"
#include "graphics/module_native_graphics.h"

// To add a new built-in module: #include its header above, then add
// one line to this array. Nothing else in the compiler or VM needs to
// change — see natives.h's comment.
static NativeModule* registry[] = {
    &mathModule,
    &convertModule,
    &mapModule,
    &arraysModule,
    &stringModule,
    &datetimeModule,
    &jsonModule,
    &osModule,
    &fileModule,
    &netModule,
    &graphicsModule,
};
#define MODULE_COUNT ((int)(sizeof(registry) / sizeof(registry[0])))

int findNativeModule(const char* name, int length) {
    for (int i = 0; i < MODULE_COUNT; i++) {
        if ((int)strlen(registry[i]->name) == length &&
            strncmp(registry[i]->name, name, length) == 0)
            return i;
    }
    return -1;
}

const char* nativeModuleName(int moduleIndex) {
    return registry[moduleIndex]->name;
}

static int moduleBaseIndex(int moduleIndex) {
    int base = 0;
    for (int i = 0; i < moduleIndex; i++) base += registry[i]->functionCount;
    return base;
}

int findNativeFunction(int moduleIndex, const char* name, int length, int* outArity) {
    NativeModule* m = registry[moduleIndex];
    for (int i = 0; i < m->functionCount; i++) {
        if ((int)strlen(m->functions[i].name) == length &&
            strncmp(m->functions[i].name, name, length) == 0) {
            if (outArity) *outArity = m->functions[i].arity;
            return moduleBaseIndex(moduleIndex) + i;
        }
    }
    return -1;
}

Value callNative(VM* vm, int globalIndex, Value* args, int argCount, int line) {
    int remaining = globalIndex;
    for (int m = 0; m < MODULE_COUNT; m++) {
        if (remaining < registry[m]->functionCount)
            return registry[m]->functions[remaining].fn(vm, args, argCount, line);
        remaining -= registry[m]->functionCount;
    }
    return makeNull(); // unreachable if globalIndex came from findNativeFunction
}
