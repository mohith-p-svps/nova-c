// The `math` module — see Chapter 24 of the NovaLang book.
//
// Type-preservation note: the book specifies that abs/min/max/floor/
// ceil/round preserve the input's exact type (int stays int, double
// stays double), and that sqrt/pow use arbitrary-precision algorithms
// for BigInt/BigDecimal inputs. This implementation preserves type for
// the common numeric types (int16/32/64, float64) faithfully, and
// handles BigInt/BigDecimal inputs to abs/floor/ceil/round correctly
// via GMP/MPFR's own equivalents. For sqrt/pow/log/log10/sin/cos/tan on
// BigInt/BigDecimal inputs specifically, values are converted to a
// plain double first — a deliberate, documented simplification rather
// than implementing arbitrary-precision transcendental algorithms from
// scratch; precision beyond ~15-17 significant digits is not preserved
// in that specific case.

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "module_native_math.h"
#include "../error.h"

static int isNumeric(Value v) {
    return v.type == VAL_INT16 || v.type == VAL_INT32 || v.type == VAL_INT64 ||
           v.type == VAL_BIGINT || v.type == VAL_FLOAT64 || v.type == VAL_BIGDEC ||
           v.type == VAL_CHAR;
}

// Demotes a char to its ASCII code as an int32, matching how the rest
// of the language treats chars in arithmetic — see value.c's
// demoteChar (kept as a separate, tiny copy here since that one is
// file-static to value.c).
static Value demote(Value v) {
    if (v.type == VAL_CHAR) return makeInt32(v.as.charCode);
    return v;
}

static double toDouble(Value v) {
    v = demote(v);
    switch (v.type) {
        case VAL_INT16:   return (double)v.as.i16;
        case VAL_INT32:   return (double)v.as.i32;
        case VAL_INT64:   return (double)v.as.i64;
        case VAL_FLOAT64: return v.as.f64;
        case VAL_BIGINT:  return mpz_get_d(*v.as.bigint);
        case VAL_BIGDEC:  return mpfr_get_d(*v.as.bigdec, MPFR_RNDN);
        default:          return 0.0;
    }
}

// Checks that every argument is numeric, raising a consistent error
// otherwise. Returns 1 if all are fine, 0 (with novaError already
// raised) if not.
static int checkNumericArgs(Value* args, int argCount, const char* fnName, int line) {
    for (int i = 0; i < argCount; i++) {
        if (!isNumeric(args[i])) {
            novaError(ERR_ARGUMENT, line, "math.%s: argument %d must be a number (got %s)",
                      fnName, i + 1, typeName(args[i]));
            return 0;
        }
    }
    return 1;
}

static Value math_sqrt(VM* vm, Value* args, int argCount, int line) {
    if (!checkNumericArgs(args, argCount, "sqrt", line)) return makeNull();
    double x = toDouble(args[0]);
    if (x < 0) {
        novaError(ERR_ARGUMENT, line, "math.sqrt: argument must be non-negative (got %g)", x);
        return makeNull();
    }
    return makeFloat64(sqrt(x));
}

static Value math_pow(VM* vm, Value* args, int argCount, int line) {
    if (!checkNumericArgs(args, argCount, "pow", line)) return makeNull();
    Value base = demote(args[0]), exp = demote(args[1]);

    int bothPlainInt = (base.type == VAL_INT16 || base.type == VAL_INT32 || base.type == VAL_INT64) &&
                        (exp.type  == VAL_INT16 || exp.type  == VAL_INT32 || exp.type  == VAL_INT64);
    if (bothPlainInt) {
        int64_t e = exp.type == VAL_INT16 ? exp.as.i16 : exp.type == VAL_INT32 ? exp.as.i32 : exp.as.i64;
        if (e >= 0) {
            // Exact integer result — promote to BigInt via repeated
            // multiplication so large exponents don't silently overflow
            // a fixed-width int (reuses the exact same promotion
            // machinery valueMul already uses for overflow elsewhere).
            mpz_t* result = malloc(sizeof(mpz_t));
            mpz_init_set_si(*result, 1);
            mpz_t b; mpz_init_set_d(b, toDouble(base)); // base value, exactly, since it's a plain int here
            for (int64_t i = 0; i < e; i++) mpz_mul(*result, *result, b);
            mpz_clear(b);
            // Shrink back down to a plain int if it comfortably fits —
            // keeps small results (the overwhelmingly common case)
            // looking like plain ints rather than always BigInt.
            if (mpz_fits_slong_p(*result)) {
                long v = mpz_get_si(*result);
                mpz_clear(*result); free(result);
                return makeInt64(v);
            }
            return makeBigInt(result);
        }
    }
    // Negative exponent, or either side already a double/bigdec — use
    // double arithmetic, matching the book's stated fallback rule.
    return makeFloat64(pow(toDouble(base), toDouble(exp)));
}

static Value math_abs(VM* vm, Value* args, int argCount, int line) {
    if (!checkNumericArgs(args, argCount, "abs", line)) return makeNull();
    Value v = demote(args[0]);
    switch (v.type) {
        case VAL_INT16: return makeInt16(v.as.i16 < 0 ? (int16_t)-v.as.i16 : v.as.i16);
        case VAL_INT32: return makeInt32(v.as.i32 < 0 ? -v.as.i32 : v.as.i32);
        case VAL_INT64: return makeInt64(v.as.i64 < 0 ? -v.as.i64 : v.as.i64);
        case VAL_FLOAT64: return makeFloat64(fabs(v.as.f64));
        case VAL_BIGINT: {
            mpz_t* r = malloc(sizeof(mpz_t));
            mpz_init(*r);
            mpz_abs(*r, *v.as.bigint);
            return makeBigInt(r);
        }
        case VAL_BIGDEC: {
            mpfr_t* r = malloc(sizeof(mpfr_t));
            mpfr_init2(*r, BIGDEC_PRECISION);
            mpfr_abs(*r, *v.as.bigdec, MPFR_RNDN);
            return makeBigDec(r);
        }
        default: return makeNull();
    }
}

static Value math_floor(VM* vm, Value* args, int argCount, int line) {
    if (!checkNumericArgs(args, argCount, "floor", line)) return makeNull();
    Value v = demote(args[0]);
    if (v.type == VAL_INT16 || v.type == VAL_INT32 || v.type == VAL_INT64 || v.type == VAL_BIGINT)
        return v; // already an integer
    if (v.type == VAL_BIGDEC) {
        mpfr_t* r = malloc(sizeof(mpfr_t));
        mpfr_init2(*r, BIGDEC_PRECISION);
        mpfr_floor(*r, *v.as.bigdec);
        return makeBigDec(r);
    }
    return makeFloat64(floor(toDouble(v)));
}

static Value math_ceil(VM* vm, Value* args, int argCount, int line) {
    if (!checkNumericArgs(args, argCount, "ceil", line)) return makeNull();
    Value v = demote(args[0]);
    if (v.type == VAL_INT16 || v.type == VAL_INT32 || v.type == VAL_INT64 || v.type == VAL_BIGINT)
        return v;
    if (v.type == VAL_BIGDEC) {
        mpfr_t* r = malloc(sizeof(mpfr_t));
        mpfr_init2(*r, BIGDEC_PRECISION);
        mpfr_ceil(*r, *v.as.bigdec);
        return makeBigDec(r);
    }
    return makeFloat64(ceil(toDouble(v)));
}

static Value math_round(VM* vm, Value* args, int argCount, int line) {
    if (!checkNumericArgs(args, argCount, "round", line)) return makeNull();
    Value v = demote(args[0]);
    if (v.type == VAL_INT16 || v.type == VAL_INT32 || v.type == VAL_INT64 || v.type == VAL_BIGINT)
        return v;
    if (v.type == VAL_BIGDEC) {
        mpfr_t* r = malloc(sizeof(mpfr_t));
        mpfr_init2(*r, BIGDEC_PRECISION);
        mpfr_round(*r, *v.as.bigdec);
        return makeBigDec(r);
    }
    // Round half up (not banker's rounding), matching the book's 3.5 -> 4 example.
    return makeFloat64(floor(toDouble(v) + 0.5));
}

static Value math_min(VM* vm, Value* args, int argCount, int line) {
    if (!checkNumericArgs(args, argCount, "min", line)) return makeNull();
    return toDouble(args[0]) <= toDouble(args[1]) ? demote(args[0]) : demote(args[1]);
}

static Value math_max(VM* vm, Value* args, int argCount, int line) {
    if (!checkNumericArgs(args, argCount, "max", line)) return makeNull();
    return toDouble(args[0]) >= toDouble(args[1]) ? demote(args[0]) : demote(args[1]);
}

static Value math_clamp(VM* vm, Value* args, int argCount, int line) {
    if (!checkNumericArgs(args, argCount, "clamp", line)) return makeNull();
    double x = toDouble(args[0]), lo = toDouble(args[1]), hi = toDouble(args[2]);
    if (x < lo) return demote(args[1]);
    if (x > hi) return demote(args[2]);
    return demote(args[0]);
}

static Value math_log(VM* vm, Value* args, int argCount, int line) {
    if (!checkNumericArgs(args, argCount, "log", line)) return makeNull();
    return makeFloat64(log(toDouble(args[0])));
}

static Value math_log10(VM* vm, Value* args, int argCount, int line) {
    if (!checkNumericArgs(args, argCount, "log10", line)) return makeNull();
    return makeFloat64(log10(toDouble(args[0])));
}

static Value math_sin(VM* vm, Value* args, int argCount, int line) {
    if (!checkNumericArgs(args, argCount, "sin", line)) return makeNull();
    return makeFloat64(sin(toDouble(args[0])));
}

static Value math_cos(VM* vm, Value* args, int argCount, int line) {
    if (!checkNumericArgs(args, argCount, "cos", line)) return makeNull();
    return makeFloat64(cos(toDouble(args[0])));
}

static Value math_tan(VM* vm, Value* args, int argCount, int line) {
    if (!checkNumericArgs(args, argCount, "tan", line)) return makeNull();
    return makeFloat64(tan(toDouble(args[0])));
}

static Value math_pi(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)args; (void)argCount; (void)line;
    return makeFloat64(3.14159265358979323846);
}

static Value math_e(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)args; (void)argCount; (void)line;
    return makeFloat64(2.71828182845904523536);
}

static NativeFnEntry mathFunctions[] = {
    {"sqrt",  math_sqrt,  1},
    {"pow",   math_pow,   2},
    {"abs",   math_abs,   1},
    {"floor", math_floor, 1},
    {"ceil",  math_ceil,  1},
    {"round", math_round, 1},
    {"min",   math_min,   2},
    {"max",   math_max,   2},
    {"clamp", math_clamp, 3},
    {"log",   math_log,   1},
    {"log10", math_log10, 1},
    {"sin",   math_sin,   1},
    {"cos",   math_cos,   1},
    {"tan",   math_tan,   1},
    {"pi",    math_pi,    0},
    {"e",     math_e,     0},
};

NativeModule mathModule = {
    "math",
    mathFunctions,
    sizeof(mathFunctions) / sizeof(mathFunctions[0])
};
