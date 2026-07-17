// The `json` module — see Chapter 29 of the NovaLang book.
//
// A small hand-written recursive-descent parser and a matching
// serializer. Scope note: \uXXXX escapes are decoded as UTF-8 for
// codepoints in the Basic Multilingual Plane only (no surrogate-pair
// handling for codepoints beyond it) — Nova's strings are plain byte
// strings with no Unicode-awareness elsewhere in the language, so this
// is a deliberately bounded, documented simplification rather than an
// attempt at full Unicode support the rest of the interpreter doesn't
// have anyway.

#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "module_native_json.h"
#include "../error.h"

typedef struct { char* data; int len; int cap; } JBuf;
static void jbufInit(JBuf* b) { b->cap = 64; b->data = malloc(b->cap); b->len = 0; }
static void jbufPush(JBuf* b, char c) {
    if (b->len + 1 > b->cap) { b->cap *= 2; b->data = realloc(b->data, b->cap); }
    b->data[b->len++] = c;
}
static void jbufAppend(JBuf* b, const char* s, int n) {
    if (b->len + n + 1 > b->cap) {
        while (b->len + n + 1 > b->cap) b->cap *= 2;
        b->data = realloc(b->data, b->cap);
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
}

typedef struct {
    const char* s;
    int pos;
    int len;
    int failed;
} JParser;

static void skipWs(JParser* p) {
    while (p->pos < p->len && isspace((unsigned char)p->s[p->pos])) p->pos++;
}

static int peekc(JParser* p) { return p->pos < p->len ? (unsigned char)p->s[p->pos] : -1; }

static void appendUtf8(JBuf* b, unsigned int codepoint) {
    if (codepoint <= 0x7F) {
        jbufPush(b, (char)codepoint);
    } else if (codepoint <= 0x7FF) {
        jbufPush(b, (char)(0xC0 | (codepoint >> 6)));
        jbufPush(b, (char)(0x80 | (codepoint & 0x3F)));
    } else {
        jbufPush(b, (char)(0xE0 | (codepoint >> 12)));
        jbufPush(b, (char)(0x80 | ((codepoint >> 6) & 0x3F)));
        jbufPush(b, (char)(0x80 | (codepoint & 0x3F)));
    }
}

static Value parseJsonString(JParser* p) {
    p->pos++;
    JBuf buf; jbufInit(&buf);
    while (p->pos < p->len && p->s[p->pos] != '"') {
        char c = p->s[p->pos];
        if (c == '\\') {
            p->pos++;
            if (p->pos >= p->len) { p->failed = 1; break; }
            char esc = p->s[p->pos];
            switch (esc) {
                case '"':  jbufPush(&buf, '"');  break;
                case '\\': jbufPush(&buf, '\\'); break;
                case '/':  jbufPush(&buf, '/');  break;
                case 'b':  jbufPush(&buf, '\b'); break;
                case 'f':  jbufPush(&buf, '\f'); break;
                case 'n':  jbufPush(&buf, '\n'); break;
                case 'r':  jbufPush(&buf, '\r'); break;
                case 't':  jbufPush(&buf, '\t'); break;
                case 'u': {
                    if (p->pos + 4 >= p->len) { p->failed = 1; break; }
                    char hex[5] = { p->s[p->pos+1], p->s[p->pos+2], p->s[p->pos+3], p->s[p->pos+4], 0 };
                    unsigned int code = (unsigned int)strtoul(hex, NULL, 16);
                    appendUtf8(&buf, code);
                    p->pos += 4;
                    break;
                }
                default:
                    p->failed = 1;
                    break;
            }
            p->pos++;
        } else {
            jbufPush(&buf, c);
            p->pos++;
        }
        if (p->failed) break;
    }
    if (p->failed || p->pos >= p->len || p->s[p->pos] != '"') { p->failed = 1; free(buf.data); return makeNull(); }
    p->pos++;
    Value r = makeString(buf.data, buf.len);
    free(buf.data);
    return r;
}

static Value parseJsonNumber(JParser* p) {
    int start = p->pos;
    if (peekc(p) == '-') p->pos++;
    while (isdigit(peekc(p))) p->pos++;
    int isFloat = 0;
    if (peekc(p) == '.') { isFloat = 1; p->pos++; while (isdigit(peekc(p))) p->pos++; }
    if (peekc(p) == 'e' || peekc(p) == 'E') {
        isFloat = 1; p->pos++;
        if (peekc(p) == '+' || peekc(p) == '-') p->pos++;
        while (isdigit(peekc(p))) p->pos++;
    }
    int len = p->pos - start;
    char* buf = malloc(len + 1);
    memcpy(buf, p->s + start, len);
    buf[len] = '\0';

    Value result;
    if (isFloat) {
        result = makeFloat64(strtod(buf, NULL));
    } else {
        errno = 0;
        char* endp;
        long long v = strtoll(buf, &endp, 10);
        if (errno == ERANGE) {
            mpz_t* z = malloc(sizeof(mpz_t));
            mpz_init_set_str(*z, buf, 10);
            result = makeBigInt(z);
        } else {
            result = makeInt64(v);
        }
    }
    free(buf);
    return result;
}

static Value parseJsonValue(JParser* p);

static Value parseJsonArray(JParser* p) {
    p->pos++;
    Value result = makeArray();
    skipWs(p);
    if (peekc(p) == ']') { p->pos++; return result; }
    for (;;) {
        skipWs(p);
        Value v = parseJsonValue(p);
        if (p->failed) return result;
        arrayPush(result.as.array, v);
        skipWs(p);
        if (peekc(p) == ',') { p->pos++; continue; }
        if (peekc(p) == ']') { p->pos++; break; }
        p->failed = 1;
        break;
    }
    return result;
}

static Value parseJsonObject(JParser* p) {
    p->pos++;
    Value result = makeMap();
    skipWs(p);
    if (peekc(p) == '}') { p->pos++; return result; }
    for (;;) {
        skipWs(p);
        if (peekc(p) != '"') { p->failed = 1; break; }
        Value key = parseJsonString(p);
        if (p->failed) break;
        skipWs(p);
        if (peekc(p) != ':') { p->failed = 1; break; }
        p->pos++;
        skipWs(p);
        Value val = parseJsonValue(p);
        if (p->failed) break;
        mapSet(result.as.map, key, val);
        skipWs(p);
        if (peekc(p) == ',') { p->pos++; continue; }
        if (peekc(p) == '}') { p->pos++; break; }
        p->failed = 1;
        break;
    }
    return result;
}

static Value parseJsonValue(JParser* p) {
    skipWs(p);
    int c = peekc(p);
    if (c == '"') return parseJsonString(p);
    if (c == '{') return parseJsonObject(p);
    if (c == '[') return parseJsonArray(p);
    if (c == '-' || isdigit(c)) return parseJsonNumber(p);
    if (strncmp(p->s + p->pos, "true", 4) == 0)  { p->pos += 4; return TRUE_VAL; }
    if (strncmp(p->s + p->pos, "false", 5) == 0) { p->pos += 5; return FALSE_VAL; }
    if (strncmp(p->s + p->pos, "null", 4) == 0)  { p->pos += 4; return makeNull(); }
    p->failed = 1;
    return makeNull();
}

static Value json_parse(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    if (args[0].type != VAL_STRING) {
        novaError(ERR_ARGUMENT, line, "json.parse: expected a string (got %s)", typeName(args[0]));
        return makeNull();
    }
    JParser p;
    p.s = args[0].as.string->data;
    p.len = args[0].as.string->length;
    p.pos = 0;
    p.failed = 0;

    Value result = parseJsonValue(&p);
    skipWs(&p);
    if (p.failed || p.pos != p.len) {
        novaError(ERR_ARGUMENT, line, "json.parse: invalid JSON");
        return makeNull();
    }
    return result;
}

static void jsonAppendEscapedString(JBuf* b, const char* data, int length) {
    jbufPush(b, '"');
    for (int i = 0; i < length; i++) {
        unsigned char c = (unsigned char)data[i];
        switch (c) {
            case '"':  jbufAppend(b, "\\\"", 2); break;
            case '\\': jbufAppend(b, "\\\\", 2); break;
            case '\n': jbufAppend(b, "\\n", 2); break;
            case '\r': jbufAppend(b, "\\r", 2); break;
            case '\t': jbufAppend(b, "\\t", 2); break;
            default:
                if (c < 0x20) {
                    char esc[8];
                    int n = snprintf(esc, sizeof(esc), "\\u%04x", c);
                    jbufAppend(b, esc, n);
                } else {
                    jbufPush(b, (char)c);
                }
        }
    }
    jbufPush(b, '"');
}

static void jsonAppendValue(JBuf* b, Value v, int indent, int depth);

static void jsonIndent(JBuf* b, int indent, int depth) {
    if (indent <= 0) return;
    jbufPush(b, '\n');
    for (int i = 0; i < indent * depth; i++) jbufPush(b, ' ');
}

static void jsonAppendValue(JBuf* b, Value v, int indent, int depth) {
    char buf[512];
    switch (v.type) {
        case VAL_NULL: jbufAppend(b, "null", 4); break;
        case VAL_BOOL: jbufAppend(b, v.as.boolean ? "true" : "false", v.as.boolean ? 4 : 5); break;
        case VAL_INT16: { int n = snprintf(buf, sizeof(buf), "%d", v.as.i16); jbufAppend(b, buf, n); break; }
        case VAL_INT32: { int n = snprintf(buf, sizeof(buf), "%d", v.as.i32); jbufAppend(b, buf, n); break; }
        case VAL_INT64: { int n = snprintf(buf, sizeof(buf), "%lld", (long long)v.as.i64); jbufAppend(b, buf, n); break; }
        case VAL_BIGINT: { int n = gmp_snprintf(buf, sizeof(buf), "%Zd", *v.as.bigint); jbufAppend(b, buf, n); break; }
        case VAL_CHAR: { char one[2] = { (char)v.as.charCode, 0 }; jsonAppendEscapedString(b, one, 1); break; }
        case VAL_FLOAT64: {
            int n = snprintf(buf, sizeof(buf), "%g", v.as.f64);
            int hasDotOrExp = 0;
            for (int i = 0; i < n; i++) if (buf[i] == '.' || buf[i] == 'e' || buf[i] == 'E') hasDotOrExp = 1;
            jbufAppend(b, buf, n);
            if (!hasDotOrExp) jbufAppend(b, ".0", 2);
            break;
        }
        case VAL_BIGDEC: {
            int n = mpfr_snprintf(buf, sizeof(buf), "%.17Rg", *v.as.bigdec);
            int hasDotOrExp = 0;
            for (int i = 0; i < n; i++) if (buf[i] == '.' || buf[i] == 'e' || buf[i] == 'E') hasDotOrExp = 1;
            jbufAppend(b, buf, n);
            if (!hasDotOrExp) jbufAppend(b, ".0", 2);
            break;
        }
        case VAL_STRING: jsonAppendEscapedString(b, v.as.string->data, v.as.string->length); break;
        case VAL_ARRAY: {
            NovaArray* a = v.as.array;
            jbufPush(b, '[');
            for (int i = 0; i < a->count; i++) {
                if (i > 0) jbufPush(b, ',');
                jsonIndent(b, indent, depth + 1);
                jsonAppendValue(b, a->items[i], indent, depth + 1);
            }
            if (a->count > 0) jsonIndent(b, indent, depth);
            jbufPush(b, ']');
            break;
        }
        case VAL_MAP: {
            NovaMap* m = v.as.map;
            jbufPush(b, '{');
            for (int i = 0; i < m->count; i++) {
                if (i > 0) jbufPush(b, ',');
                jsonIndent(b, indent, depth + 1);
                if (m->keys[i].type == VAL_STRING) {
                    jsonAppendEscapedString(b, m->keys[i].as.string->data, m->keys[i].as.string->length);
                } else {
                    Value ks = valueToStringValue(m->keys[i]);
                    jsonAppendEscapedString(b, ks.as.string->data, ks.as.string->length);
                    freeValue(ks);
                }
                jbufPush(b, ':');
                if (indent > 0) jbufPush(b, ' ');
                jsonAppendValue(b, m->values[i], indent, depth + 1);
            }
            if (m->count > 0) jsonIndent(b, indent, depth);
            jbufPush(b, '}');
            break;
        }
        default:
            jbufAppend(b, "null", 4);
            break;
    }
}

static Value json_stringify(VM* vm, Value* args, int argCount, int line) {
    (void)vm;
    if (argCount < 1 || argCount > 2) {
        novaError(ERR_ARGUMENT, line, "json.stringify expects 1 or 2 argument(s) but got %d", argCount);
        return makeNull();
    }
    int indent = 0;
    if (argCount >= 2) {
        if (args[1].type == VAL_INT64) indent = (int)args[1].as.i64;
        else if (args[1].type == VAL_INT32) indent = args[1].as.i32;
        else if (args[1].type == VAL_INT16) indent = args[1].as.i16;
    }
    JBuf buf; jbufInit(&buf);
    jsonAppendValue(&buf, args[0], indent, 0);
    Value result = makeString(buf.data, buf.len);
    free(buf.data);
    return result;
}

static Value json_isValid(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount; (void)line;
    if (args[0].type != VAL_STRING) return FALSE_VAL;
    JParser p;
    p.s = args[0].as.string->data;
    p.len = args[0].as.string->length;
    p.pos = 0;
    p.failed = 0;
    parseJsonValue(&p);
    skipWs(&p);
    return (!p.failed && p.pos == p.len) ? TRUE_VAL : FALSE_VAL;
}

static NativeFnEntry jsonFunctions[] = {
    {"parse",     json_parse,     1},
    {"stringify", json_stringify, -1}, // variable arity: stringify(value) or stringify(value, indent)
    {"isValid",   json_isValid,   1},
};

NativeModule jsonModule = {
    "json",
    jsonFunctions,
    sizeof(jsonFunctions) / sizeof(jsonFunctions[0])
};
