#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <math.h>

#include "value.h"
#include "error.h"
#include "gc.h"
#include "chunk.h" // full NovaFunction definition, needed to print/stringify VAL_FUNCTION

// Constructors

Value makeBool(int b) {
    Value v; v.type = VAL_BOOL; v.as.boolean = b ? 1 : 0; return v;
}
Value makeInt16(int16_t v)  { Value r; r.type = VAL_INT16;   r.as.i16    = v; return r; }
Value makeInt32(int32_t v)  { Value r; r.type = VAL_INT32;   r.as.i32    = v; return r; }
Value makeInt64(int64_t v)  { Value r; r.type = VAL_INT64;   r.as.i64    = v; return r; }
Value makeFloat64(double v) { Value r; r.type = VAL_FLOAT64; r.as.f64    = v; return r; }

Value makeBigInt(mpz_t* v) {
    Value r; r.type = VAL_BIGINT; r.as.bigint = v; return r;
}
Value makeBigDec(mpfr_t* v) {
    Value r; r.type = VAL_BIGDEC; r.as.bigdec = v; return r;
}

Value makeString(const char* data, int length) {
    NovaString* s = malloc(sizeof(NovaString));
    s->data   = malloc(length + 1);
    memcpy(s->data, data, length);
    s->data[length] = '\0';
    s->length = length;
    Value r; r.type = VAL_STRING; r.as.string = s; return r;
}

Value makeNull(void) {
    Value r; r.type = VAL_NULL; r.as.i64 = 0; return r;
}

Value makeChar(int32_t code) {
    Value r; r.type = VAL_CHAR; r.as.charCode = code; return r;
}

Value makeFunction(NovaFunction* fn) {
    Value r; r.type = VAL_FUNCTION; r.as.function = fn; return r;
}

Value makeArray(void) {
    NovaArray* a = (NovaArray*)gcAllocateObject(sizeof(NovaArray), VAL_ARRAY);
    a->items = NULL; a->count = 0; a->capacity = 0;
    Value r; r.type = VAL_ARRAY; r.as.array = a; return r;
}

Value makeMap(void) {
    NovaMap* m = (NovaMap*)gcAllocateObject(sizeof(NovaMap), VAL_MAP);
    m->keys = NULL; m->values = NULL; m->count = 0; m->capacity = 0;
    Value r; r.type = VAL_MAP; r.as.map = m; return r;
}

void arrayPush(NovaArray* array, Value v) {
    if (array->count + 1 > array->capacity) {
        array->capacity = array->capacity < 8 ? 8 : array->capacity * 2;
        array->items = realloc(array->items, sizeof(Value) * array->capacity);
    }
    array->items[array->count++] = v;
}

// Raw structural equality used for map key lookup — unlike valueEquals(),
// this never raises a TypeError; mismatched types simply aren't equal.
static int rawEquals(Value a, Value b) {
    if (a.type != b.type) return 0;
    switch (a.type) {
        case VAL_BOOL:    return a.as.boolean == b.as.boolean;
        case VAL_INT16:   return a.as.i16 == b.as.i16;
        case VAL_INT32:   return a.as.i32 == b.as.i32;
        case VAL_INT64:   return a.as.i64 == b.as.i64;
        case VAL_FLOAT64: return a.as.f64 == b.as.f64;
        case VAL_BIGINT:  return mpz_cmp(*a.as.bigint, *b.as.bigint) == 0;
        case VAL_BIGDEC:  return mpfr_cmp(*a.as.bigdec, *b.as.bigdec) == 0;
        case VAL_STRING:
            return a.as.string->length == b.as.string->length &&
                   memcmp(a.as.string->data, b.as.string->data, a.as.string->length) == 0;
        case VAL_NULL:    return 1;
        case VAL_ARRAY:   return a.as.array == b.as.array;
        case VAL_MAP:     return a.as.map   == b.as.map;
        case VAL_CHAR:    return a.as.charCode == b.as.charCode;
        case VAL_FUNCTION: return a.as.function == b.as.function;
        default:          return 0;
    }
}

void mapSet(NovaMap* map, Value key, Value value) {
    for (int i = 0; i < map->count; i++) {
        if (rawEquals(map->keys[i], key)) {
            freeValue(map->values[i]);
            map->values[i] = value;
            freeValue(key); // a fresh copy of the key was already stored
            return;
        }
    }
    if (map->count + 1 > map->capacity) {
        map->capacity = map->capacity < 8 ? 8 : map->capacity * 2;
        map->keys   = realloc(map->keys,   sizeof(Value) * map->capacity);
        map->values = realloc(map->values, sizeof(Value) * map->capacity);
    }
    map->keys[map->count]   = key;
    map->values[map->count] = value;
    map->count++;
}

int mapGet(NovaMap* map, Value key, Value* out) {
    for (int i = 0; i < map->count; i++) {
        if (rawEquals(map->keys[i], key)) { *out = map->values[i]; return 1; }
    }
    return 0;
}

// Type names (for error messages)

const char* typeName(Value v) {
    switch (v.type) {
        case VAL_BOOL:    return "bool";
        case VAL_INT16:
        case VAL_INT32:
        case VAL_INT64:   return "int";
        case VAL_BIGINT:  return "bigint";
        case VAL_FLOAT64: return "float";
        case VAL_BIGDEC:  return "bigdecimal";
        case VAL_STRING:  return "string";
        case VAL_NULL:    return "null";
        case VAL_ARRAY:   return "array";
        case VAL_MAP:     return "map";
        case VAL_CHAR:    return "char";
        case VAL_FUNCTION: return "function";
        default:          return "unknown";
    }
}

// Truthiness

int isTruthy(Value v) {
    switch (v.type) {
        case VAL_BOOL:    return v.as.boolean != 0;
        case VAL_INT16:   return v.as.i16 != 0;
        case VAL_INT32:   return v.as.i32 != 0;
        case VAL_INT64:   return v.as.i64 != 0;
        case VAL_BIGINT:  return mpz_sgn(*v.as.bigint) != 0;
        case VAL_FLOAT64: return v.as.f64 != 0.0;
        case VAL_BIGDEC:  return !mpfr_zero_p(*v.as.bigdec);
        case VAL_STRING:  return v.as.string->length > 0;
        case VAL_NULL:    return 0;
        case VAL_ARRAY:   return v.as.array->count > 0;
        case VAL_MAP:     return v.as.map->count > 0;
        case VAL_CHAR:    return v.as.charCode != 0;
        case VAL_FUNCTION: return 1;
        default:          return 0;
    }
}

// Strict truthiness for actual conditions (if/while/and/or) — see the
// declaration comment in value.h. Only real booleans and collections
// (empty/non-empty) are acceptable; every other type is a TypeError,
// matching the language's documented condition semantics.
int isTruthyStrict(Value v) {
    switch (v.type) {
        case VAL_BOOL:  return v.as.boolean != 0;
        case VAL_ARRAY: return v.as.array->count > 0;
        case VAL_MAP:   return v.as.map->count > 0;
        default:
            novaError(ERR_TYPE, -1, "Condition must be a boolean (got %s)", typeName(v));
            return 0;
    }
}

// Promotion

Value promoteToInt32(Value v) {
    switch (v.type) {
        case VAL_BOOL:  return makeInt32((int32_t)v.as.boolean);
        case VAL_INT16: return makeInt32((int32_t)v.as.i16);
        default:        return v;
    }
}

Value promoteToInt64(Value v) {
    switch (v.type) {
        case VAL_BOOL:  return makeInt64((int64_t)v.as.boolean);
        case VAL_INT16: return makeInt64((int64_t)v.as.i16);
        case VAL_INT32: return makeInt64((int64_t)v.as.i32);
        default:        return v;
    }
}

Value promoteToBigInt(Value v) {
    mpz_t* z = malloc(sizeof(mpz_t));
    mpz_init(*z);
    switch (v.type) {
        case VAL_BOOL:   mpz_set_si(*z, (long)v.as.boolean); break;
        case VAL_INT16:  mpz_set_si(*z, (long)v.as.i16);     break;
        case VAL_INT32:  mpz_set_si(*z, (long)v.as.i32);     break;
        case VAL_INT64:  mpz_set_si(*z, (long)v.as.i64);     break;
        case VAL_BIGINT: mpz_set(*z, *v.as.bigint); freeValue(v); break;
        default:         mpz_set_si(*z, 0); break;
    }
    return makeBigInt(z);
}

Value promoteToFloat64(Value v) {
    switch (v.type) {
        case VAL_BOOL:   return makeFloat64((double)v.as.boolean);
        case VAL_INT16:  return makeFloat64((double)v.as.i16);
        case VAL_INT32:  return makeFloat64((double)v.as.i32);
        case VAL_INT64:  return makeFloat64((double)v.as.i64);
        case VAL_BIGINT: { double d = mpz_get_d(*v.as.bigint); freeValue(v); return makeFloat64(d); }
        default: return v;
    }
}

Value promoteToBigDec(Value v) {
    mpfr_t* f = malloc(sizeof(mpfr_t));
    mpfr_init2(*f, BIGDEC_PRECISION);
    switch (v.type) {
        case VAL_BOOL:    mpfr_set_si(*f, (long)v.as.boolean, MPFR_RNDN); break;
        case VAL_INT16:   mpfr_set_si(*f, (long)v.as.i16,     MPFR_RNDN); break;
        case VAL_INT32:   mpfr_set_si(*f, (long)v.as.i32,     MPFR_RNDN); break;
        case VAL_INT64:   mpfr_set_si(*f, (long)v.as.i64,     MPFR_RNDN); break;
        case VAL_BIGINT:  mpfr_set_z(*f, *v.as.bigint,        MPFR_RNDN); freeValue(v); break;
        case VAL_FLOAT64: mpfr_set_d(*f, v.as.f64,            MPFR_RNDN); break;
        case VAL_BIGDEC:  mpfr_set(*f, *v.as.bigdec,          MPFR_RNDN); freeValue(v); break;
        default: break;
    }
    return makeBigDec(f);
}

void coerce(Value* a, Value* b) {
    if (a->type == b->type) return;
    ValueType target = a->type > b->type ? a->type : b->type;
    // Strings never coerce with numbers — caller must type-check first
    if (a->type == VAL_STRING || b->type == VAL_STRING) return;
    if (target == VAL_INT32)   { if (a->type < VAL_INT32)   *a = promoteToInt32(*a);    if (b->type < VAL_INT32)   *b = promoteToInt32(*b);   return; }
    if (target == VAL_INT64)   { if (a->type < VAL_INT64)   *a = promoteToInt64(*a);    if (b->type < VAL_INT64)   *b = promoteToInt64(*b);   return; }
    if (target == VAL_BIGINT)  { if (a->type < VAL_BIGINT)  *a = promoteToBigInt(*a);   if (b->type < VAL_BIGINT)  *b = promoteToBigInt(*b);  return; }
    if (target == VAL_FLOAT64) { if (a->type != VAL_FLOAT64) *a = promoteToFloat64(*a); if (b->type != VAL_FLOAT64) *b = promoteToFloat64(*b); return; }
    if (target == VAL_BIGDEC)  { if (a->type != VAL_BIGDEC)  *a = promoteToBigDec(*a);  if (b->type != VAL_BIGDEC)  *b = promoteToBigDec(*b);  return; }
}

// Demotion

static Value demoteIfWhole(Value v) {
    if (v.type == VAL_FLOAT64) {
        double d = v.as.f64;
        if (isfinite(d) && floor(d) == d &&
            d >= (double)INT64_MIN && d <= (double)INT64_MAX)
            return makeInt64((int64_t)d);
    }
    if (v.type == VAL_BIGDEC) {
        if (mpfr_integer_p(*v.as.bigdec)) {
            mpz_t* z = malloc(sizeof(mpz_t));
            mpz_init(*z);
            mpfr_get_z(*z, *v.as.bigdec, MPFR_RNDZ);
            freeValue(v);
            return makeBigInt(z);
        }
    }
    if (v.type == VAL_BIGINT) {
        if (mpz_fits_slong_p(*v.as.bigint)) {
            int64_t n = (int64_t)mpz_get_si(*v.as.bigint);
            freeValue(v);
            return makeInt64(n);
        }
    }
    return v;
}

// Arithmetic

// A char participates in arithmetic via its ASCII code, becoming a
// plain int the moment it's used with +, -, *, /, %, or a comparison —
// e.g. 'A' + 1 is 66 (an int), not a char. This is applied at the top
// of every arithmetic/comparison entry point below, before any of the
// existing type-checking logic runs, so a char is simply indistinguishable
// from an int32 everywhere past this point.
static Value demoteChar(Value v) {
    if (v.type == VAL_CHAR) return makeInt32(v.as.charCode);
    return v;
}

Value valueAdd(Value a, Value b) {
    a = demoteChar(a); b = demoteChar(b);
    // Array concatenation: [1,2] + [3,4] -> [1,2,3,4]. Checked before the
    // general "arrays can't use +" rejection below, and requires BOTH
    // sides to be arrays — concatenating an array with anything else is
    // a type error, same as every other mismatched-type case here.
    if (a.type == VAL_ARRAY || b.type == VAL_ARRAY) {
        if (a.type != VAL_ARRAY || b.type != VAL_ARRAY) {
            novaError(ERR_TYPE, -1, "Cannot use '+' on %s and %s", typeName(a), typeName(b));
            freeValue(a); freeValue(b);
            return makeInt64(0);
        }
        Value result = makeArray();
        NovaArray* out = result.as.array;
        for (int i = 0; i < a.as.array->count; i++)
            arrayPush(out, copyValue(a.as.array->items[i]));
        for (int i = 0; i < b.as.array->count; i++)
            arrayPush(out, copyValue(b.as.array->items[i]));
        // a and b themselves are reference types (freeValue no-ops on
        // them; the GC owns their storage), so there's nothing to free
        // here beyond what freeValue would already skip.
        return result;
    }
    // Type error: can't add strings with +, use concat
    if (a.type == VAL_STRING || b.type == VAL_STRING) {
        novaError(ERR_TYPE, -1, "Cannot use '+' on string — use string concatenation");
        freeValue(a); freeValue(b);
        return makeInt64(0);
    }
    if (a.type == VAL_BOOL || b.type == VAL_BOOL) {
        novaError(ERR_TYPE, -1, "Cannot use '+' on bool");
        freeValue(a); freeValue(b);
        return makeInt64(0);
    }
    if (a.type == VAL_NULL || b.type == VAL_NULL || a.type == VAL_MAP || b.type == VAL_MAP) {
        novaError(ERR_TYPE, -1, "Cannot use '+' on %s and %s", typeName(a), typeName(b));
        freeValue(a); freeValue(b);
        return makeInt64(0);
    }
    coerce(&a, &b);
    switch (a.type) {
        case VAL_INT16: { int32_t r = (int32_t)a.as.i16 + b.as.i16; if (r > INT16_MAX || r < INT16_MIN) return valueAdd(promoteToInt32(a), promoteToInt32(b)); return makeInt16((int16_t)r); }
        case VAL_INT32: { int64_t r = (int64_t)a.as.i32 + b.as.i32; if (r > INT32_MAX || r < INT32_MIN) return valueAdd(promoteToInt64(a), promoteToInt64(b)); return makeInt32((int32_t)r); }
        case VAL_INT64: { int64_t r; if (__builtin_add_overflow(a.as.i64, b.as.i64, &r)) return valueAdd(promoteToBigInt(a), promoteToBigInt(b)); return makeInt64(r); }
        case VAL_BIGINT: { mpz_t* r = malloc(sizeof(mpz_t)); mpz_init(*r); mpz_add(*r, *a.as.bigint, *b.as.bigint); freeValue(a); freeValue(b); return demoteIfWhole(makeBigInt(r)); }
        case VAL_FLOAT64: return demoteIfWhole(makeFloat64(a.as.f64 + b.as.f64));
        case VAL_BIGDEC: { mpfr_t* r = malloc(sizeof(mpfr_t)); mpfr_init2(*r, BIGDEC_PRECISION); mpfr_add(*r, *a.as.bigdec, *b.as.bigdec, MPFR_RNDN); freeValue(a); freeValue(b); return demoteIfWhole(makeBigDec(r)); }
        default: return makeInt64(0);
    }
}

Value valueSub(Value a, Value b) {
    a = demoteChar(a); b = demoteChar(b);
    if (a.type == VAL_STRING || b.type == VAL_STRING || a.type == VAL_BOOL || b.type == VAL_BOOL || a.type == VAL_NULL || b.type == VAL_NULL || a.type == VAL_ARRAY || b.type == VAL_ARRAY || a.type == VAL_MAP || b.type == VAL_MAP) {
        novaError(ERR_TYPE, -1, "Cannot use '-' on %s and %s", typeName(a), typeName(b));
        freeValue(a); freeValue(b); return makeInt64(0);
    }
    coerce(&a, &b);
    switch (a.type) {
        case VAL_INT16: { int32_t r = (int32_t)a.as.i16 - b.as.i16; if (r > INT16_MAX || r < INT16_MIN) return valueSub(promoteToInt32(a), promoteToInt32(b)); return makeInt16((int16_t)r); }
        case VAL_INT32: { int64_t r = (int64_t)a.as.i32 - b.as.i32; if (r > INT32_MAX || r < INT32_MIN) return valueSub(promoteToInt64(a), promoteToInt64(b)); return makeInt32((int32_t)r); }
        case VAL_INT64: { int64_t r; if (__builtin_sub_overflow(a.as.i64, b.as.i64, &r)) return valueSub(promoteToBigInt(a), promoteToBigInt(b)); return makeInt64(r); }
        case VAL_BIGINT: { mpz_t* r = malloc(sizeof(mpz_t)); mpz_init(*r); mpz_sub(*r, *a.as.bigint, *b.as.bigint); freeValue(a); freeValue(b); return demoteIfWhole(makeBigInt(r)); }
        case VAL_FLOAT64: return demoteIfWhole(makeFloat64(a.as.f64 - b.as.f64));
        case VAL_BIGDEC: { mpfr_t* r = malloc(sizeof(mpfr_t)); mpfr_init2(*r, BIGDEC_PRECISION); mpfr_sub(*r, *a.as.bigdec, *b.as.bigdec, MPFR_RNDN); freeValue(a); freeValue(b); return demoteIfWhole(makeBigDec(r)); }
        default: return makeInt64(0);
    }
}

// Repeats `count` copies of a string's or array's contents. Negative
// counts clamp to 0 (an empty result), matching the general convention
// of "repeated zero or more times" rather than raising an error for a
// case that has an obvious, harmless interpretation.
static int64_t asPlainInt(Value v) {
    switch (v.type) {
        case VAL_INT16: return v.as.i16;
        case VAL_INT32: return v.as.i32;
        case VAL_INT64: return v.as.i64;
        default:        return 0;
    }
}

static int isPlainInt(Value v) {
    return v.type == VAL_INT16 || v.type == VAL_INT32 || v.type == VAL_INT64;
}

static Value repeatString(Value str, int64_t count) {
    if (count < 0) count = 0;
    int64_t newLen = (int64_t)str.as.string->length * count;
    char* buf = malloc((size_t)newLen + 1);
    int64_t pos = 0;
    for (int64_t i = 0; i < count; i++) {
        memcpy(buf + pos, str.as.string->data, str.as.string->length);
        pos += str.as.string->length;
    }
    buf[newLen] = '\0';
    Value result = makeString(buf, (int)newLen);
    free(buf);
    freeValue(str);
    return result;
}

static Value repeatArray(Value arr, int64_t count) {
    if (count < 0) count = 0;
    Value result = makeArray();
    NovaArray* out = result.as.array;
    for (int64_t i = 0; i < count; i++)
        for (int j = 0; j < arr.as.array->count; j++)
            arrayPush(out, copyValue(arr.as.array->items[j]));
    // arr itself is a reference type — nothing to free (see valueAdd's
    // array-concatenation comment for why freeValue is a no-op here).
    return result;
}

Value valueMul(Value a, Value b) {
    a = demoteChar(a); b = demoteChar(b);

    // String repetition: "ab" * 3 -> "ababab" (int on either side works).
    if (a.type == VAL_STRING && isPlainInt(b)) return repeatString(a, asPlainInt(b));
    if (isPlainInt(a) && b.type == VAL_STRING) return repeatString(b, asPlainInt(a));

    // Array repetition: [0] * 5 -> [0, 0, 0, 0, 0].
    if (a.type == VAL_ARRAY && isPlainInt(b)) return repeatArray(a, asPlainInt(b));
    if (isPlainInt(a) && b.type == VAL_ARRAY) return repeatArray(b, asPlainInt(a));

    if (a.type == VAL_STRING || b.type == VAL_STRING || a.type == VAL_BOOL || b.type == VAL_BOOL || a.type == VAL_NULL || b.type == VAL_NULL || a.type == VAL_ARRAY || b.type == VAL_ARRAY || a.type == VAL_MAP || b.type == VAL_MAP) {
        novaError(ERR_TYPE, -1, "Cannot use '*' on %s and %s", typeName(a), typeName(b));
        freeValue(a); freeValue(b); return makeInt64(0);
    }
    coerce(&a, &b);
    switch (a.type) {
        case VAL_INT16: { int32_t r = (int32_t)a.as.i16 * b.as.i16; if (r > INT16_MAX || r < INT16_MIN) return valueMul(promoteToInt32(a), promoteToInt32(b)); return makeInt16((int16_t)r); }
        case VAL_INT32: { int64_t r = (int64_t)a.as.i32 * b.as.i32; if (r > INT32_MAX || r < INT32_MIN) return valueMul(promoteToInt64(a), promoteToInt64(b)); return makeInt32((int32_t)r); }
        case VAL_INT64: { int64_t r; if (__builtin_mul_overflow(a.as.i64, b.as.i64, &r)) return valueMul(promoteToBigInt(a), promoteToBigInt(b)); return makeInt64(r); }
        case VAL_BIGINT: { mpz_t* r = malloc(sizeof(mpz_t)); mpz_init(*r); mpz_mul(*r, *a.as.bigint, *b.as.bigint); freeValue(a); freeValue(b); return demoteIfWhole(makeBigInt(r)); }
        case VAL_FLOAT64: return demoteIfWhole(makeFloat64(a.as.f64 * b.as.f64));
        case VAL_BIGDEC: { mpfr_t* r = malloc(sizeof(mpfr_t)); mpfr_init2(*r, BIGDEC_PRECISION); mpfr_mul(*r, *a.as.bigdec, *b.as.bigdec, MPFR_RNDN); freeValue(a); freeValue(b); return demoteIfWhole(makeBigDec(r)); }
        default: return makeInt64(0);
    }
}

Value valueDiv(Value a, Value b) {
    a = demoteChar(a); b = demoteChar(b);
    if (a.type == VAL_STRING || b.type == VAL_STRING || a.type == VAL_BOOL || b.type == VAL_BOOL || a.type == VAL_NULL || b.type == VAL_NULL || a.type == VAL_ARRAY || b.type == VAL_ARRAY || a.type == VAL_MAP || b.type == VAL_MAP) {
        novaError(ERR_TYPE, -1, "Cannot use '/' on %s and %s", typeName(a), typeName(b));
        freeValue(a); freeValue(b); return makeInt64(0);
    }
    coerce(&a, &b);
    switch (a.type) {
        case VAL_INT16: if (!b.as.i16) { novaError(ERR_DIVISION_BY_ZERO,-1,"Division by zero"); return makeInt16(0); } return makeInt16(a.as.i16 / b.as.i16);
        case VAL_INT32: if (!b.as.i32) { novaError(ERR_DIVISION_BY_ZERO,-1,"Division by zero"); return makeInt32(0); } return makeInt32(a.as.i32 / b.as.i32);
        case VAL_INT64: if (!b.as.i64) { novaError(ERR_DIVISION_BY_ZERO,-1,"Division by zero"); return makeInt64(0); } return makeInt64(a.as.i64 / b.as.i64);
        case VAL_BIGINT: {
            if (mpz_sgn(*b.as.bigint) == 0) { novaError(ERR_DIVISION_BY_ZERO,-1,"Division by zero"); freeValue(a); freeValue(b); return makeInt64(0); }
            mpz_t* r = malloc(sizeof(mpz_t)); mpz_init(*r);
            mpz_tdiv_q(*r, *a.as.bigint, *b.as.bigint);
            freeValue(a); freeValue(b); return demoteIfWhole(makeBigInt(r));
        }
        case VAL_FLOAT64: return demoteIfWhole(makeFloat64(a.as.f64 / b.as.f64));
        case VAL_BIGDEC: { mpfr_t* r = malloc(sizeof(mpfr_t)); mpfr_init2(*r, BIGDEC_PRECISION); mpfr_div(*r, *a.as.bigdec, *b.as.bigdec, MPFR_RNDN); freeValue(a); freeValue(b); return demoteIfWhole(makeBigDec(r)); }
        default: return makeInt64(0);
    }
}

Value valueMod(Value a, Value b) {
    a = demoteChar(a); b = demoteChar(b);
    if (a.type == VAL_STRING || b.type == VAL_STRING || a.type == VAL_BOOL || b.type == VAL_BOOL || a.type == VAL_NULL || b.type == VAL_NULL || a.type == VAL_ARRAY || b.type == VAL_ARRAY || a.type == VAL_MAP || b.type == VAL_MAP) {
        novaError(ERR_TYPE, -1, "Cannot use '%%' on %s and %s", typeName(a), typeName(b));
        freeValue(a); freeValue(b); return makeInt64(0);
    }
    coerce(&a, &b);
    switch (a.type) {
        case VAL_INT16: if (!b.as.i16) { novaError(ERR_DIVISION_BY_ZERO,-1,"Division by zero"); return makeInt16(0); } return makeInt16(a.as.i16 % b.as.i16);
        case VAL_INT32: if (!b.as.i32) { novaError(ERR_DIVISION_BY_ZERO,-1,"Division by zero"); return makeInt32(0); } return makeInt32(a.as.i32 % b.as.i32);
        case VAL_INT64: if (!b.as.i64) { novaError(ERR_DIVISION_BY_ZERO,-1,"Division by zero"); return makeInt64(0); } return makeInt64(a.as.i64 % b.as.i64);
        case VAL_BIGINT: {
            if (mpz_sgn(*b.as.bigint) == 0) { novaError(ERR_DIVISION_BY_ZERO,-1,"Division by zero"); freeValue(a); freeValue(b); return makeInt64(0); }
            mpz_t* r = malloc(sizeof(mpz_t)); mpz_init(*r);
            mpz_tdiv_r(*r, *a.as.bigint, *b.as.bigint);
            freeValue(a); freeValue(b); return demoteIfWhole(makeBigInt(r));
        }
        case VAL_FLOAT64: return demoteIfWhole(makeFloat64(fmod(a.as.f64, b.as.f64)));
        case VAL_BIGDEC: { mpfr_t* r = malloc(sizeof(mpfr_t)); mpfr_init2(*r, BIGDEC_PRECISION); mpfr_fmod(*r, *a.as.bigdec, *b.as.bigdec, MPFR_RNDN); freeValue(a); freeValue(b); return demoteIfWhole(makeBigDec(r)); }
        default: return makeInt64(0);
    }
}

Value valueNegate(Value v) {
    v = demoteChar(v);
    switch (v.type) {
        case VAL_INT16: return makeInt16((int16_t)(-v.as.i16));
        case VAL_INT32: return makeInt32(-v.as.i32);
        case VAL_INT64: return makeInt64(-v.as.i64);
        case VAL_BIGINT: { mpz_t* r = malloc(sizeof(mpz_t)); mpz_init(*r); mpz_neg(*r, *v.as.bigint); freeValue(v); return makeBigInt(r); }
        case VAL_FLOAT64: return makeFloat64(-v.as.f64);
        case VAL_BIGDEC: { mpfr_t* r = malloc(sizeof(mpfr_t)); mpfr_init2(*r, BIGDEC_PRECISION); mpfr_neg(*r, *v.as.bigdec, MPFR_RNDN); freeValue(v); return makeBigDec(r); }
        default:
            novaError(ERR_TYPE, -1, "Cannot negate %s", typeName(v));
            freeValue(v);
            return makeInt64(0);
    }
}

// Comparisons

static int compareNumeric(Value a, Value b) {
    coerce(&a, &b);
    int cmp = 0;
    switch (a.type) {
        case VAL_INT16:   cmp = (a.as.i16 > b.as.i16) - (a.as.i16 < b.as.i16); break;
        case VAL_INT32:   cmp = (a.as.i32 > b.as.i32) - (a.as.i32 < b.as.i32); break;
        case VAL_INT64:   cmp = (a.as.i64 > b.as.i64) - (a.as.i64 < b.as.i64); break;
        case VAL_BIGINT:  cmp = mpz_cmp(*a.as.bigint, *b.as.bigint); freeValue(a); freeValue(b); break;
        case VAL_FLOAT64: cmp = (a.as.f64 > b.as.f64) - (a.as.f64 < b.as.f64); break;
        case VAL_BIGDEC:  cmp = mpfr_cmp(*a.as.bigdec, *b.as.bigdec); freeValue(a); freeValue(b); break;
        default: break;
    }
    return cmp;
}

// Returns <0, 0, >0. Strings compare lexicographically by byte value;
// numbers compare after promotion. Mixing strings/bools with numbers,
// or comparing bools at all, is a TypeError.
static int valueCompare(Value a, Value b) {
    a = demoteChar(a); b = demoteChar(b);
    if (a.type == VAL_STRING && b.type == VAL_STRING) {
        int len = a.as.string->length < b.as.string->length ? a.as.string->length : b.as.string->length;
        int cmp = memcmp(a.as.string->data, b.as.string->data, len);
        if (cmp == 0) cmp = a.as.string->length - b.as.string->length;
        freeValue(a); freeValue(b);
        return cmp;
    }
    if (a.type == VAL_STRING || b.type == VAL_STRING || a.type == VAL_BOOL || b.type == VAL_BOOL ||
        a.type == VAL_NULL   || b.type == VAL_NULL   || a.type == VAL_ARRAY || b.type == VAL_ARRAY ||
        a.type == VAL_MAP    || b.type == VAL_MAP) {
        novaError(ERR_TYPE, -1, "Cannot compare %s and %s", typeName(a), typeName(b));
        freeValue(a); freeValue(b);
        return 0;
    }
    return compareNumeric(a, b);
}

Value valueEquals(Value a, Value b) {
    int eq;
    a = demoteChar(a); b = demoteChar(b);
    // null is only ever equal to null — comparing it to anything else is
    // just false, not a TypeError (unlike ordering comparisons below).
    if (a.type == VAL_NULL || b.type == VAL_NULL) {
        return makeBool(a.type == VAL_NULL && b.type == VAL_NULL);
    }
    // Arrays/maps compare by reference (same underlying object), not by
    // structural content. Comparing one to a non-array/non-map is a
    // TypeError, same as string-vs-bool below.
    if (a.type == VAL_ARRAY || b.type == VAL_ARRAY || a.type == VAL_MAP || b.type == VAL_MAP) {
        if (a.type != b.type) {
            novaError(ERR_TYPE, -1, "Cannot compare %s and %s", typeName(a), typeName(b));
            return FALSE_VAL;
        }
        if (a.type == VAL_ARRAY) return makeBool(a.as.array == b.as.array);
        return makeBool(a.as.map == b.as.map);
    }
    if (a.type == VAL_STRING && b.type == VAL_STRING) {
        eq = a.as.string->length == b.as.string->length &&
             memcmp(a.as.string->data, b.as.string->data, a.as.string->length) == 0;
        freeValue(a); freeValue(b);
        return makeBool(eq);
    }
    if (a.type == VAL_BOOL && b.type == VAL_BOOL) {
        return makeBool(a.as.boolean == b.as.boolean);
    }
    if (a.type == VAL_STRING || b.type == VAL_STRING || a.type == VAL_BOOL || b.type == VAL_BOOL) {
        novaError(ERR_TYPE, -1, "Cannot compare %s and %s", typeName(a), typeName(b));
        freeValue(a); freeValue(b);
        return FALSE_VAL;
    }
    eq = compareNumeric(a, b) == 0;
    return makeBool(eq);
}

Value valueNotEquals(Value a, Value b) {
    Value eq = valueEquals(a, b);
    return makeBool(!eq.as.boolean);
}

Value valueLess(Value a, Value b)         { return makeBool(valueCompare(a, b) <  0); }
Value valueGreater(Value a, Value b)      { return makeBool(valueCompare(a, b) >  0); }
Value valueLessEqual(Value a, Value b)    { return makeBool(valueCompare(a, b) <= 0); }
Value valueGreaterEqual(Value a, Value b) { return makeBool(valueCompare(a, b) >= 0); }

// String operations

Value valueConcat(Value a, Value b) {
    if (a.type != VAL_STRING || b.type != VAL_STRING) {
        novaError(ERR_TYPE, -1,
                  "Concatenation requires two strings, got %s and %s",
                  typeName(a), typeName(b));
        freeValue(a); freeValue(b);
        return makeString("", 0);
    }
    int newLen = a.as.string->length + b.as.string->length;
    char* buf  = malloc(newLen + 1);
    memcpy(buf,                          a.as.string->data, a.as.string->length);
    memcpy(buf + a.as.string->length,    b.as.string->data, b.as.string->length);
    buf[newLen] = '\0';
    Value result = makeString(buf, newLen);
    free(buf);
    freeValue(a); freeValue(b);
    return result;
}

// Boolean operations

Value valueBoolNot(Value v) {
    if (v.type != VAL_BOOL) {
        novaError(ERR_TYPE, -1, "Cannot apply 'not' to %s", typeName(v));
        return FALSE_VAL;
    }
    return makeBool(!v.as.boolean);
}

// Memory

void freeValue(Value v) {
    switch (v.type) {
        case VAL_BIGINT:  mpz_clear(*v.as.bigint);   free(v.as.bigint);               break;
        case VAL_BIGDEC:  mpfr_clear(*v.as.bigdec);  free(v.as.bigdec);               break;
        case VAL_STRING:  free(v.as.string->data);   free(v.as.string);               break;
        default: break;
    }
}

Value copyValue(Value v) {
    switch (v.type) {
        case VAL_BIGINT: { mpz_t* z = malloc(sizeof(mpz_t)); mpz_init_set(*z, *v.as.bigint); return makeBigInt(z); }
        case VAL_BIGDEC: { mpfr_t* f = malloc(sizeof(mpfr_t)); mpfr_init2(*f, BIGDEC_PRECISION); mpfr_set(*f, *v.as.bigdec, MPFR_RNDN); return makeBigDec(f); }
        case VAL_STRING: return makeString(v.as.string->data, v.as.string->length);
        default: return v;
    }
}

// Growable buffer used only by valueToStringValue's array/map recursion
// below — kept local to this file, not exposed via value.h.
typedef struct { char* data; int len; int cap; } VBuf;
static void vbufInit(VBuf* b) { b->cap = 64; b->data = malloc(b->cap); b->len = 0; b->data[0] = '\0'; }
static void vbufAppend(VBuf* b, const char* s, int n) {
    if (b->len + n + 1 > b->cap) {
        while (b->len + n + 1 > b->cap) b->cap *= 2;
        b->data = realloc(b->data, b->cap);
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}
static void vbufAppendValue(VBuf* b, Value v); // forward decl for recursion

static void vbufAppendScalarString(VBuf* b, Value v) {
    Value s = valueToStringValue(v);
    vbufAppend(b, s.as.string->data, s.as.string->length);
    freeValue(s);
}

static void vbufAppendValue(VBuf* b, Value v) {
    if (v.type == VAL_ARRAY) {
        vbufAppend(b, "[", 1);
        for (int i = 0; i < v.as.array->count; i++) {
            if (i > 0) vbufAppend(b, ", ", 2);
            vbufAppendValue(b, v.as.array->items[i]);
        }
        vbufAppend(b, "]", 1);
    } else if (v.type == VAL_MAP) {
        vbufAppend(b, "{", 1);
        for (int i = 0; i < v.as.map->count; i++) {
            if (i > 0) vbufAppend(b, ", ", 2);
            vbufAppendValue(b, v.as.map->keys[i]);
            vbufAppend(b, ": ", 2);
            vbufAppendValue(b, v.as.map->values[i]);
        }
        vbufAppend(b, "}", 1);
    } else {
        vbufAppendScalarString(b, v);
    }
}

// Converts any value to its string representation, same formatting as
// printValue. Always returns a NEW, independently-owned string — never
// aliases or frees its input, regardless of the input's type. This is
// deliberately read-only (unlike some other value.c functions, which
// consume/free their operands) because callers have different needs:
// OP_TO_STRING's handler owns a popped, unique value and frees it
// itself after conversion, while vbufAppendValue's recursion below
// reads live array/map elements it must NOT free.
Value valueToStringValue(Value v) {
    char buf[4096];
    int n;
    switch (v.type) {
        case VAL_STRING:
            return copyValue(v);
        case VAL_BOOL:
            return v.as.boolean ? makeString("true", 4) : makeString("false", 5);
        case VAL_INT16: n = snprintf(buf, sizeof(buf), "%d", v.as.i16); return makeString(buf, n);
        case VAL_INT32: n = snprintf(buf, sizeof(buf), "%d", v.as.i32); return makeString(buf, n);
        case VAL_INT64: n = snprintf(buf, sizeof(buf), "%" PRId64, v.as.i64); return makeString(buf, n);
        case VAL_BIGINT:
            n = gmp_snprintf(buf, sizeof(buf), "%Zd", *v.as.bigint);
            return makeString(buf, n);
        case VAL_FLOAT64: n = snprintf(buf, sizeof(buf), "%g", v.as.f64); return makeString(buf, n);
        case VAL_BIGDEC:
            n = mpfr_snprintf(buf, sizeof(buf), "%.20Rf", *v.as.bigdec);
            return makeString(buf, n);
        case VAL_CHAR: buf[0] = (char)v.as.charCode; return makeString(buf, 1);
        case VAL_NULL: return makeString("null", 4);
        case VAL_FUNCTION:
            n = snprintf(buf, sizeof(buf), "<fn %s>", v.as.function->name);
            return makeString(buf, n);
        case VAL_ARRAY:
        case VAL_MAP: {
            VBuf b; vbufInit(&b);
            vbufAppendValue(&b, v);
            Value result = makeString(b.data, b.len);
            free(b.data);
            return result;
        }
        default: return makeString("", 0);
    }
}

// Display

void printValue(Value v) {
    switch (v.type) {
        case VAL_BOOL:    printf("%s",       v.as.boolean ? "true" : "false"); break;
        case VAL_INT16:   printf("%d",       v.as.i16);                        break;
        case VAL_INT32:   printf("%d",       v.as.i32);                        break;
        case VAL_INT64:   printf("%" PRId64, v.as.i64);                        break;
        case VAL_BIGINT:  gmp_printf("%Zd",  *v.as.bigint);                    break;
        case VAL_FLOAT64: printf("%g",       v.as.f64);                        break;
        case VAL_BIGDEC:  mpfr_printf("%.20Rf", *v.as.bigdec);                 break;
        case VAL_STRING:  printf("%s",       v.as.string->data);               break;
        case VAL_NULL:    printf("null");                                     break;
        case VAL_CHAR:    printf("%c",       (char)v.as.charCode);            break;
        case VAL_FUNCTION: printf("<fn %s>", v.as.function->name);            break;
        case VAL_ARRAY: {
            printf("[");
            for (int i = 0; i < v.as.array->count; i++) {
                if (i > 0) printf(", ");
                printValue(v.as.array->items[i]);
            }
            printf("]");
            break;
        }
        case VAL_MAP: {
            printf("{");
            for (int i = 0; i < v.as.map->count; i++) {
                if (i > 0) printf(", ");
                printValue(v.as.map->keys[i]);
                printf(": ");
                printValue(v.as.map->values[i]);
            }
            printf("}");
            break;
        }
    }
}