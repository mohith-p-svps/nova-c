// The `convert` module — see Chapter 27 of the NovaLang book.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "module_native_convert.h"
#include "../error.h"

static Value demote(Value v) {
    if (v.type == VAL_CHAR) return makeInt32(v.as.charCode);
    return v;
}

static int64_t asInt64(Value v) {
    v = demote(v);
    switch (v.type) {
        case VAL_INT16:   return v.as.i16;
        case VAL_INT32:   return v.as.i32;
        case VAL_INT64:   return v.as.i64;
        case VAL_FLOAT64: return (int64_t)v.as.f64;
        case VAL_BOOL:    return v.as.boolean ? 1 : 0;
        case VAL_BIGINT:  return mpz_get_si(*v.as.bigint);
        case VAL_BIGDEC:  return (int64_t)mpfr_get_d(*v.as.bigdec, MPFR_RNDN);
        case VAL_STRING:  return strtoll(v.as.string->data, NULL, 10);
        default:          return 0;
    }
}

static double asDouble(Value v) {
    v = demote(v);
    switch (v.type) {
        case VAL_INT16:   return (double)v.as.i16;
        case VAL_INT32:   return (double)v.as.i32;
        case VAL_INT64:   return (double)v.as.i64;
        case VAL_FLOAT64: return v.as.f64;
        case VAL_BOOL:    return v.as.boolean ? 1.0 : 0.0;
        case VAL_BIGINT:  return mpz_get_d(*v.as.bigint);
        case VAL_BIGDEC:  return mpfr_get_d(*v.as.bigdec, MPFR_RNDN);
        case VAL_STRING:  return strtod(v.as.string->data, NULL);
        default:          return 0.0;
    }
}

static Value convert_toInt(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount; (void)line;
    return makeInt64(asInt64(args[0]));
}

static Value convert_toDouble(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount; (void)line;
    return makeFloat64(asDouble(args[0]));
}

static Value convert_toLong(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount; (void)line;
    return makeInt64(asInt64(args[0]));
}

static Value convert_toBigInteger(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount; (void)line;
    Value v = demote(args[0]);
    mpz_t* r = malloc(sizeof(mpz_t));
    if (v.type == VAL_STRING) mpz_init_set_str(*r, v.as.string->data, 10);
    else if (v.type == VAL_BIGINT) { mpz_init(*r); mpz_set(*r, *v.as.bigint); }
    else mpz_init_set_si(*r, (long)asInt64(v));
    return makeBigInt(r);
}

static Value convert_toBigDecimal(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount; (void)line;
    Value v = demote(args[0]);
    mpfr_t* r = malloc(sizeof(mpfr_t));
    mpfr_init2(*r, BIGDEC_PRECISION);
    if (v.type == VAL_STRING) mpfr_set_str(*r, v.as.string->data, 10, MPFR_RNDN);
    else if (v.type == VAL_BIGDEC) mpfr_set(*r, *v.as.bigdec, MPFR_RNDN);
    else mpfr_set_d(*r, asDouble(v), MPFR_RNDN);
    return makeBigDec(r);
}

static Value convert_toString(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount; (void)line;
    return valueToStringValue(args[0]);
}

static Value convert_toChar(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    Value v = args[0];
    if (v.type == VAL_STRING && v.as.string->length >= 1)
        return makeChar((unsigned char)v.as.string->data[0]);
    if (v.type == VAL_CHAR) return v;
    if (v.type == VAL_INT16 || v.type == VAL_INT32 || v.type == VAL_INT64)
        return makeChar((int32_t)asInt64(v));
    novaError(ERR_ARGUMENT, line, "convert.toChar: expected an int or single-character string");
    return makeNull();
}

static Value convert_toBool(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount; (void)line;
    Value v = demote(args[0]);
    switch (v.type) {
        case VAL_BOOL:    return v;
        case VAL_NULL:    return FALSE_VAL;
        case VAL_STRING:  return v.as.string->length > 0 ? TRUE_VAL : FALSE_VAL;
        case VAL_INT16:   return v.as.i16 != 0 ? TRUE_VAL : FALSE_VAL;
        case VAL_INT32:   return v.as.i32 != 0 ? TRUE_VAL : FALSE_VAL;
        case VAL_INT64:   return v.as.i64 != 0 ? TRUE_VAL : FALSE_VAL;
        case VAL_FLOAT64: return v.as.f64 != 0.0 ? TRUE_VAL : FALSE_VAL;
        case VAL_ARRAY:   return v.as.array->count > 0 ? TRUE_VAL : FALSE_VAL;
        case VAL_MAP:     return v.as.map->count > 0 ? TRUE_VAL : FALSE_VAL;
        default:          return TRUE_VAL;
    }
}

static Value convert_isInt(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount; (void)line;
    ValueType t = args[0].type;
    return (t == VAL_INT16 || t == VAL_INT32 || t == VAL_INT64 || t == VAL_BIGINT) ? TRUE_VAL : FALSE_VAL;
}

static Value convert_isDouble(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount; (void)line;
    return (args[0].type == VAL_FLOAT64 || args[0].type == VAL_BIGDEC) ? TRUE_VAL : FALSE_VAL;
}

static Value convert_isString(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount; (void)line;
    return args[0].type == VAL_STRING ? TRUE_VAL : FALSE_VAL;
}

static Value convert_isBool(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount; (void)line;
    return args[0].type == VAL_BOOL ? TRUE_VAL : FALSE_VAL;
}

static Value convert_isNull(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount; (void)line;
    return args[0].type == VAL_NULL ? TRUE_VAL : FALSE_VAL;
}

static Value convert_isArray(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount; (void)line;
    return args[0].type == VAL_ARRAY ? TRUE_VAL : FALSE_VAL;
}

static Value convert_isMap(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount; (void)line;
    return args[0].type == VAL_MAP ? TRUE_VAL : FALSE_VAL;
}

static Value convert_isFunction(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount; (void)line;
    return args[0].type == VAL_FUNCTION ? TRUE_VAL : FALSE_VAL;
}

static Value convert_typeOf(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount; (void)line;
    const char* name;
    switch (args[0].type) {
        case VAL_INT16: case VAL_INT32: case VAL_INT64: case VAL_BIGINT: name = "int"; break;
        case VAL_FLOAT64: case VAL_BIGDEC: name = "double"; break;
        case VAL_STRING: name = "string"; break;
        case VAL_BOOL: name = "boolean"; break;
        case VAL_NULL: name = "null"; break;
        case VAL_ARRAY: name = "array"; break;
        case VAL_MAP: name = "map"; break;
        case VAL_CHAR: name = "char"; break;
        case VAL_FUNCTION: name = "function"; break;
        default: name = "unknown"; break;
    }
    return makeString(name, (int)strlen(name));
}

static NativeFnEntry convertFunctions[] = {
    {"toInt",        convert_toInt,        1},
    {"toDouble",     convert_toDouble,     1},
    {"toLong",       convert_toLong,       1},
    {"toBigInteger", convert_toBigInteger, 1},
    {"toBigDecimal", convert_toBigDecimal, 1},
    {"toString",     convert_toString,     1},
    {"toChar",       convert_toChar,       1},
    {"toBool",       convert_toBool,       1},
    {"isInt",        convert_isInt,        1},
    {"isDouble",     convert_isDouble,     1},
    {"isString",     convert_isString,     1},
    {"isBool",       convert_isBool,       1},
    {"isNull",       convert_isNull,       1},
    {"isArray",      convert_isArray,      1},
    {"isMap",        convert_isMap,        1},
    {"isFunction",   convert_isFunction,   1},
    {"typeOf",       convert_typeOf,       1},
};

NativeModule convertModule = {
    "convert",
    convertFunctions,
    sizeof(convertFunctions) / sizeof(convertFunctions[0])
};
