#ifndef VALUE_H
#define VALUE_H

#include <stdint.h>
#include <gmp.h>
#include <mpfr.h>

#define BIGDEC_PRECISION 256

// Heap-allocated string object
typedef struct {
    char* data;
    int   length;
} NovaString;

// Forward-declared here (only pointers are needed inside Value's union);
// fully defined below, once Value itself exists.
typedef struct NovaArray NovaArray;
typedef struct NovaMap   NovaMap;

// Forward-declared here (only a pointer is needed inside Value's union);
// fully defined in chunk.h, which is the only place that needs the full
// struct. value.h can't include chunk.h itself — chunk.h already
// includes value.h, and C has no forward-only circular includes.
typedef struct NovaFunction NovaFunction;

typedef enum {
    VAL_BOOL,       // true / false
    VAL_INT16,      // short
    VAL_INT32,      // int
    VAL_INT64,      // long long
    VAL_BIGINT,     // mpz_t*       — arbitrary precision integer
    VAL_FLOAT64,    // double
    VAL_BIGDEC,     // mpfr_t*      — arbitrary precision float
    VAL_CHAR,       // single character, stored as its ASCII code
    VAL_STRING,     // NovaString*  — heap-allocated string
    VAL_NULL,       // absence of a value
    VAL_ARRAY,      // NovaArray*   — heap-allocated, reference semantics
    VAL_MAP,        // NovaMap*     — heap-allocated, reference semantics
    VAL_FUNCTION,   // NovaFunction* — a function used as a first-class value
} ValueType;

typedef struct {
    ValueType type;
    union {
        int8_t      boolean;    // 0 = false, 1 = true
        int16_t     i16;
        int32_t     i32;
        int64_t     i64;
        mpz_t*      bigint;
        double      f64;
        mpfr_t*     bigdec;
        int32_t     charCode;   // VAL_CHAR — the character's ASCII code
        NovaString* string;
        NovaArray*  array;
        NovaMap*    map;
        NovaFunction* function; // VAL_FUNCTION — points into a FunctionTable's fixed array, stable for the run's lifetime
    } as;
} Value;

// GC object header — embedded as the FIRST member of any heap type the
// collector tracks (currently just NovaArray and NovaMap; strings and
// bignums keep their existing copy-on-write discipline and aren't
// GC-tracked, since they can't form cycles and already work). Because
// it's first, a GCObject* and the concrete NovaArray*/NovaMap* it heads
// point at the same address — the classic C "tagged struct" trick, used
// so the sweep phase can walk one generic list without knowing each
// object's concrete type until it checks `objType`.
typedef struct GCObject {
    struct GCObject* next;
    int               marked;
    ValueType         objType; // VAL_ARRAY or VAL_MAP
} GCObject;

// Arrays and maps are reference types: unlike strings/numbers, assigning
// or passing one does NOT deep-copy it — copyValue() just copies the
// pointer. This is what makes `arr[i] = x` (and nested forms like
// `matrix[i][j] = x`) mutate the variable everyone's looking at, rather
// than some disconnected clone. Their memory is managed by the tracing
// garbage collector in gc.c/gc.h, not by freeValue()/copyValue() — both
// of those treat VAL_ARRAY/VAL_MAP as plain pointer copies and leave the
// underlying object alone.
struct NovaArray {
    GCObject obj; // must be first — see GCObject's comment above
    Value* items;
    int    count;
    int    capacity;
};

// A flat key/value list, looked up by linear scan with raw value
// equality (no type coercion, no hashing). Fine for the sizes a
// hand-written Nova program is likely to use; revisit if that changes.
struct NovaMap {
    GCObject obj; // must be first — see GCObject's comment above
    Value* keys;
    Value* values;
    int    count;
    int    capacity;
};

// Constructors
Value makeBool(int b);              // b: 0 or 1
Value makeInt16(int16_t v);
Value makeInt32(int32_t v);
Value makeInt64(int64_t v);
Value makeBigInt(mpz_t* v);
Value makeFloat64(double v);
Value makeBigDec(mpfr_t* v);
Value makeString(const char* data, int length);  // copies the data
Value makeNull(void);
Value makeArray(void);
Value makeMap(void);
Value makeChar(int32_t code);
Value makeFunction(NovaFunction* fn);

void arrayPush(NovaArray* array, Value v);
void mapSet(NovaMap* map, Value key, Value value);   // takes ownership of key/value
int  mapGet(NovaMap* map, Value key, Value* out);    // *out is NOT a copy — caller must copyValue() it to keep

// Convenience
#define TRUE_VAL  makeBool(1)
#define FALSE_VAL makeBool(0)
#define NULL_VAL  makeNull()

// Promotion (integers / floats)
Value promoteToInt32(Value v);
Value promoteToInt64(Value v);
Value promoteToBigInt(Value v);
Value promoteToFloat64(Value v);
Value promoteToBigDec(Value v);

void coerce(Value* a, Value* b);

// Arithmetic (numbers only — strings use valueConcat)
Value valueAdd(Value a, Value b);
Value valueSub(Value a, Value b);
Value valueMul(Value a, Value b);
Value valueDiv(Value a, Value b);
Value valueMod(Value a, Value b);
Value valueNegate(Value v);             // unary minus

// String operations
Value valueConcat(Value a, Value b);    // string + string

// Boolean operations
Value valueBoolNot(Value v);            // !bool

// Comparisons — all return a VAL_BOOL
Value valueEquals(Value a, Value b);
Value valueNotEquals(Value a, Value b);
Value valueLess(Value a, Value b);
Value valueGreater(Value a, Value b);
Value valueLessEqual(Value a, Value b);
Value valueGreaterEqual(Value a, Value b);

// Type helpers
int isTruthy(Value v);                  // any non-zero, non-false, non-empty value
// Strict truthiness for conditions (if/while/and/or): only VAL_BOOL,
// VAL_ARRAY, and VAL_MAP are acceptable — anything else is a TypeError.
// Unlike isTruthy, this can raise a runtime error (check novaHasError()
// after calling); on error it returns 0 so callers have a safe default
// to fall back on for that one instruction before the VM's main loop
// notices the error and halts.
int isTruthyStrict(Value v);
const char* typeName(Value v);          // "bool", "int", "float", "string", etc.

// Converts any value to its string representation (same formatting as
// printValue, but returning a VAL_STRING instead of writing to stdout).
// Used by string interpolation to stringify embedded expressions.
Value valueToStringValue(Value v);

// Memory
void   freeValue(Value v);
Value  copyValue(Value v);

// Display
void printValue(Value v);

#endif