// The `string` module — see Chapter 26 of the NovaLang book.

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "module_native_string.h"
#include "../error.h"

static int checkIsString(Value v, const char* fnName, int line) {
    if (v.type != VAL_STRING) {
        novaError(ERR_ARGUMENT, line, "string.%s: expected a string (got %s)", fnName, typeName(v));
        return 0;
    }
    return 1;
}

static int64_t asPlainInt(Value v) {
    if (v.type == VAL_INT64) return v.as.i64;
    if (v.type == VAL_INT32) return v.as.i32;
    if (v.type == VAL_INT16) return v.as.i16;
    return 0;
}

static Value string_len(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    if (!checkIsString(args[0], "len", line)) return makeNull();
    return makeInt64(args[0].as.string->length);
}

static Value string_upper(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    if (!checkIsString(args[0], "upper", line)) return makeNull();
    NovaString* s = args[0].as.string;
    char* buf = malloc(s->length + 1);
    for (int i = 0; i < s->length; i++) buf[i] = (char)toupper((unsigned char)s->data[i]);
    buf[s->length] = '\0';
    Value r = makeString(buf, s->length);
    free(buf);
    return r;
}

static Value string_lower(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    if (!checkIsString(args[0], "lower", line)) return makeNull();
    NovaString* s = args[0].as.string;
    char* buf = malloc(s->length + 1);
    for (int i = 0; i < s->length; i++) buf[i] = (char)tolower((unsigned char)s->data[i]);
    buf[s->length] = '\0';
    Value r = makeString(buf, s->length);
    free(buf);
    return r;
}

static Value string_trim(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    if (!checkIsString(args[0], "trim", line)) return makeNull();
    NovaString* s = args[0].as.string;
    int start = 0, end = s->length;
    while (start < end && isspace((unsigned char)s->data[start])) start++;
    while (end > start && isspace((unsigned char)s->data[end - 1])) end--;
    return makeString(s->data + start, end - start);
}

static Value string_contains(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    if (!checkIsString(args[0], "contains", line)) return makeNull();
    if (!checkIsString(args[1], "contains", line)) return makeNull();
    return strstr(args[0].as.string->data, args[1].as.string->data) ? TRUE_VAL : FALSE_VAL;
}

static Value string_startsWith(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    if (!checkIsString(args[0], "startsWith", line)) return makeNull();
    if (!checkIsString(args[1], "startsWith", line)) return makeNull();
    NovaString* s = args[0].as.string;
    NovaString* p = args[1].as.string;
    if (p->length > s->length) return FALSE_VAL;
    return strncmp(s->data, p->data, p->length) == 0 ? TRUE_VAL : FALSE_VAL;
}

static Value string_endsWith(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    if (!checkIsString(args[0], "endsWith", line)) return makeNull();
    if (!checkIsString(args[1], "endsWith", line)) return makeNull();
    NovaString* s = args[0].as.string;
    NovaString* suf = args[1].as.string;
    if (suf->length > s->length) return FALSE_VAL;
    return strncmp(s->data + s->length - suf->length, suf->data, suf->length) == 0 ? TRUE_VAL : FALSE_VAL;
}

static Value string_indexOf(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    if (!checkIsString(args[0], "indexOf", line)) return makeNull();
    if (!checkIsString(args[1], "indexOf", line)) return makeNull();
    char* found = strstr(args[0].as.string->data, args[1].as.string->data);
    if (!found) return makeInt64(-1);
    return makeInt64(found - args[0].as.string->data);
}

static Value string_replace(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    if (!checkIsString(args[0], "replace", line)) return makeNull();
    if (!checkIsString(args[1], "replace", line)) return makeNull();
    if (!checkIsString(args[2], "replace", line)) return makeNull();
    const char* s   = args[0].as.string->data;
    const char* old = args[1].as.string->data;
    const char* rep = args[2].as.string->data;
    int oldLen = args[1].as.string->length;
    int repLen = args[2].as.string->length;

    if (oldLen == 0) return copyValue(args[0]);

    int cap = args[0].as.string->length + 1, len = 0;
    char* buf = malloc(cap > 0 ? cap : 1);
    const char* cursor = s;
    while (*cursor) {
        const char* match = strstr(cursor, old);
        int chunkLen = match ? (int)(match - cursor) : (int)strlen(cursor);
        int needed = len + chunkLen + repLen + 1;
        if (needed > cap) { while (needed > cap) cap *= 2; buf = realloc(buf, cap); }
        memcpy(buf + len, cursor, chunkLen);
        len += chunkLen;
        if (match) {
            memcpy(buf + len, rep, repLen);
            len += repLen;
            cursor = match + oldLen;
        } else {
            break;
        }
    }
    buf[len] = '\0';
    Value r = makeString(buf, len);
    free(buf);
    return r;
}

static Value string_reverse(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    if (!checkIsString(args[0], "reverse", line)) return makeNull();
    NovaString* s = args[0].as.string;
    char* buf = malloc(s->length + 1);
    for (int i = 0; i < s->length; i++) buf[i] = s->data[s->length - 1 - i];
    buf[s->length] = '\0';
    Value r = makeString(buf, s->length);
    free(buf);
    return r;
}

static Value string_repeat(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    if (!checkIsString(args[0], "repeat", line)) return makeNull();
    NovaString* s = args[0].as.string;
    int64_t n = asPlainInt(args[1]);
    if (n < 0) n = 0;
    int64_t total = (int64_t)s->length * n;
    char* buf = malloc((size_t)total + 1);
    int64_t pos = 0;
    for (int64_t i = 0; i < n; i++) { memcpy(buf + pos, s->data, s->length); pos += s->length; }
    buf[total] = '\0';
    Value r = makeString(buf, (int)total);
    free(buf);
    return r;
}

static Value string_slice(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    if (!checkIsString(args[0], "slice", line)) return makeNull();
    NovaString* s = args[0].as.string;
    int64_t start = asPlainInt(args[1]);
    int64_t end   = asPlainInt(args[2]);
    if (start < 0) start = 0;
    if (end > s->length) end = s->length;
    if (start >= end) return makeString("", 0);
    return makeString(s->data + start, (int)(end - start));
}

static Value string_split(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    if (!checkIsString(args[0], "split", line)) return makeNull();
    if (!checkIsString(args[1], "split", line)) return makeNull();
    NovaString* s = args[0].as.string;
    NovaString* sep = args[1].as.string;
    Value result = makeArray();

    if (sep->length == 0) {
        for (int i = 0; i < s->length; i++)
            arrayPush(result.as.array, makeString(s->data + i, 1));
        return result;
    }

    const char* cursor = s->data;
    const char* end = s->data + s->length;
    while (cursor <= end) {
        const char* match = NULL;
        for (const char* p = cursor; p + sep->length <= end; p++) {
            if (memcmp(p, sep->data, sep->length) == 0) { match = p; break; }
        }
        if (match) {
            arrayPush(result.as.array, makeString(cursor, (int)(match - cursor)));
            cursor = match + sep->length;
        } else {
            arrayPush(result.as.array, makeString(cursor, (int)(end - cursor)));
            break;
        }
    }
    return result;
}

static Value string_charAt(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    if (!checkIsString(args[0], "charAt", line)) return makeNull();
    NovaString* s = args[0].as.string;
    int64_t i = asPlainInt(args[1]);
    if (i < 0 || i >= s->length) {
        novaError(ERR_INDEX_OUT_OF_BOUNDS, line, "string.charAt: index %lld out of bounds (length %d)",
                  (long long)i, s->length);
        return makeNull();
    }
    return makeString(s->data + i, 1);
}

static Value string_padLeft(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    if (!checkIsString(args[0], "padLeft", line)) return makeNull();
    if (!checkIsString(args[2], "padLeft", line)) return makeNull();
    NovaString* s = args[0].as.string;
    int64_t n = asPlainInt(args[1]);
    char padChar = args[2].as.string->length > 0 ? args[2].as.string->data[0] : ' ';
    if (n <= s->length) return copyValue(args[0]);
    int padCount = (int)(n - s->length);
    char* buf = malloc(n + 1);
    memset(buf, padChar, padCount);
    memcpy(buf + padCount, s->data, s->length);
    buf[n] = '\0';
    Value r = makeString(buf, (int)n);
    free(buf);
    return r;
}

static Value string_padRight(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    if (!checkIsString(args[0], "padRight", line)) return makeNull();
    if (!checkIsString(args[2], "padRight", line)) return makeNull();
    NovaString* s = args[0].as.string;
    int64_t n = asPlainInt(args[1]);
    char padChar = args[2].as.string->length > 0 ? args[2].as.string->data[0] : ' ';
    if (n <= s->length) return copyValue(args[0]);
    int padCount = (int)(n - s->length);
    char* buf = malloc(n + 1);
    memcpy(buf, s->data, s->length);
    memset(buf + s->length, padChar, padCount);
    buf[n] = '\0';
    Value r = makeString(buf, (int)n);
    free(buf);
    return r;
}

static Value string_isNumeric(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    if (!checkIsString(args[0], "isNumeric", line)) return makeNull();
    NovaString* s = args[0].as.string;
    if (s->length == 0) return FALSE_VAL;
    int i = 0;
    if (s->data[0] == '-' || s->data[0] == '+') i = 1;
    if (i >= s->length) return FALSE_VAL;
    int sawDigit = 0, sawDot = 0;
    for (; i < s->length; i++) {
        char c = s->data[i];
        if (isdigit((unsigned char)c)) { sawDigit = 1; continue; }
        if (c == '.' && !sawDot) { sawDot = 1; continue; }
        return FALSE_VAL;
    }
    return sawDigit ? TRUE_VAL : FALSE_VAL;
}

static NativeFnEntry stringFunctions[] = {
    {"len",        string_len,        1},
    {"upper",      string_upper,      1},
    {"lower",      string_lower,      1},
    {"trim",       string_trim,       1},
    {"contains",   string_contains,   2},
    {"startsWith", string_startsWith, 2},
    {"endsWith",   string_endsWith,   2},
    {"indexOf",    string_indexOf,    2},
    {"replace",    string_replace,    3},
    {"reverse",    string_reverse,    1},
    {"repeat",     string_repeat,     2},
    {"slice",      string_slice,      3},
    {"split",      string_split,      2},
    {"charAt",     string_charAt,     2},
    {"padLeft",    string_padLeft,    3},
    {"padRight",   string_padRight,   3},
    {"isNumeric",  string_isNumeric,  1},
};

NativeModule stringModule = {
    "string",
    stringFunctions,
    sizeof(stringFunctions) / sizeof(stringFunctions[0])
};
