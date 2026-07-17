// The `net` module — see Chapter 33 of the NovaLang book. Built on
// libcurl — this is the one module in this codebase with an external
// dependency: building Nova now requires libcurl's development headers
// and library (`-lcurl`), on every platform this gets compiled on,
// including Windows/MinGW (e.g. via MSYS2's mingw-w64-x86_64-curl
// package, or an equivalent).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>

#include "module_native_net.h"
#include "../error.h"

static int curlReady = 0;
static void ensureCurlGlobalInit(void) {
    if (!curlReady) { curl_global_init(CURL_GLOBAL_DEFAULT); curlReady = 1; }
}

typedef struct { char* data; size_t len; size_t cap; } CurlBuf;

static size_t bodyWriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total = size * nmemb;
    CurlBuf* buf = (CurlBuf*)userp;
    if (buf->len + total + 1 > buf->cap) {
        size_t newCap = buf->cap ? buf->cap : 256;
        while (buf->len + total + 1 > newCap) newCap *= 2;
        buf->cap = newCap;
        buf->data = realloc(buf->data, buf->cap);
    }
    memcpy(buf->data + buf->len, contents, total);
    buf->len += total;
    buf->data[buf->len] = '\0';
    return total;
}

static size_t headerWriteCallback(char* buffer, size_t size, size_t nitems, void* userdata) {
    size_t total = size * nitems;
    NovaMap* headers = (NovaMap*)userdata;
    char* colon = memchr(buffer, ':', total);
    if (colon) {
        int keyLen = (int)(colon - buffer);
        const char* valStart = colon + 1;
        int valLen = (int)(total - keyLen - 1);
        while (valLen > 0 && *valStart == ' ') { valStart++; valLen--; }
        while (valLen > 0 && (valStart[valLen - 1] == '\r' || valStart[valLen - 1] == '\n')) valLen--;
        if (keyLen > 0 && valLen >= 0)
            mapSet(headers, makeString(buffer, keyLen), makeString(valStart, valLen));
    }
    return total;
}

// Performs one HTTP request and returns the book's four-key response
// map: status (int), body (string), ok (bool), headers (map). On a
// transport-level failure (couldn't connect, DNS failure, timeout,
// etc. — NOT a non-2xx HTTP status, which is a perfectly normal,
// successfully-completed response), raises a Nova error instead, since
// there's no HTTP status at all to report in that case.
static Value performRequest(const char* url, const char* method, const char* body,
                             Value headersMap, int line) {
    ensureCurlGlobalInit();
    CURL* curl = curl_easy_init();
    if (!curl) {
        novaError(ERR_ARGUMENT, line, "net.%s: failed to initialize HTTP client", method);
        return makeNull();
    }

    CurlBuf bodyBuf; bodyBuf.data = NULL; bodyBuf.len = 0; bodyBuf.cap = 0;
    Value headersResult = makeMap();

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, bodyWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &bodyBuf);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, headerWriteCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, headersResult.as.map);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "NovaLang/0.1");

    struct curl_slist* headerList = NULL;
    if (headersMap.type == VAL_MAP) {
        for (int i = 0; i < headersMap.as.map->count; i++) {
            Value k = headersMap.as.map->keys[i];
            if (k.type != VAL_STRING) continue;
            Value vs = valueToStringValue(headersMap.as.map->values[i]);
            char line[1024];
            snprintf(line, sizeof(line), "%s: %s", k.as.string->data, vs.as.string->data);
            headerList = curl_slist_append(headerList, line);
            freeValue(vs);
        }
    }
    if (headerList) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);

    if (strcmp(method, "GET") == 0) {
        // default verb, nothing extra to set
    } else if (strcmp(method, "POST") == 0) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body ? body : "");
    } else if (strcmp(method, "DELETE") == 0) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
        if (body && body[0]) curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    } else {
        // PUT, PATCH, or any other custom verb via request()
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body ? body : "");
    }

    CURLcode res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        novaError(ERR_ARGUMENT, line, "net.%s: request to '%s' failed: %s",
                  method, url, curl_easy_strerror(res));
        free(bodyBuf.data);
        if (headerList) curl_slist_free_all(headerList);
        curl_easy_cleanup(curl);
        return makeNull();
    }

    long statusCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &statusCode);

    Value result = makeMap();
    mapSet(result.as.map, makeString("status", 6), makeInt64(statusCode));
    mapSet(result.as.map, makeString("body", 4), makeString(bodyBuf.data ? bodyBuf.data : "", (int)bodyBuf.len));
    mapSet(result.as.map, makeString("ok", 2), (statusCode >= 200 && statusCode < 300) ? TRUE_VAL : FALSE_VAL);
    mapSet(result.as.map, makeString("headers", 7), headersResult);

    free(bodyBuf.data);
    if (headerList) curl_slist_free_all(headerList);
    curl_easy_cleanup(curl);
    return result;
}

static int checkUrlArg(Value v, const char* fn, int line) {
    if (v.type != VAL_STRING) {
        novaError(ERR_ARGUMENT, line, "net.%s: url must be a string", fn);
        return 0;
    }
    return 1;
}

static Value net_get(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    if (!checkUrlArg(args[0], "get", line)) return makeNull();
    return performRequest(args[0].as.string->data, "GET", NULL, makeNull(), line);
}

static Value net_post(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    if (!checkUrlArg(args[0], "post", line)) return makeNull();
    Value bodyStr = valueToStringValue(args[1]);
    Value r = performRequest(args[0].as.string->data, "POST", bodyStr.as.string->data, makeNull(), line);
    freeValue(bodyStr);
    return r;
}

static Value net_put(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    if (!checkUrlArg(args[0], "put", line)) return makeNull();
    Value bodyStr = valueToStringValue(args[1]);
    Value r = performRequest(args[0].as.string->data, "PUT", bodyStr.as.string->data, makeNull(), line);
    freeValue(bodyStr);
    return r;
}

static Value net_delete(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    if (!checkUrlArg(args[0], "delete", line)) return makeNull();
    return performRequest(args[0].as.string->data, "DELETE", NULL, makeNull(), line);
}

static Value net_patch(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    if (!checkUrlArg(args[0], "patch", line)) return makeNull();
    Value bodyStr = valueToStringValue(args[1]);
    Value r = performRequest(args[0].as.string->data, "PATCH", bodyStr.as.string->data, makeNull(), line);
    freeValue(bodyStr);
    return r;
}

static Value net_getWithHeaders(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    if (!checkUrlArg(args[0], "getWithHeaders", line)) return makeNull();
    return performRequest(args[0].as.string->data, "GET", NULL, args[1], line);
}

static Value net_postWithHeaders(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    if (!checkUrlArg(args[0], "postWithHeaders", line)) return makeNull();
    Value bodyStr = valueToStringValue(args[1]);
    Value r = performRequest(args[0].as.string->data, "POST", bodyStr.as.string->data, args[2], line);
    freeValue(bodyStr);
    return r;
}

static Value net_request(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    if (!checkUrlArg(args[0], "request", line)) return makeNull();
    if (args[1].type != VAL_STRING) {
        novaError(ERR_ARGUMENT, line, "net.request: method must be a string");
        return makeNull();
    }
    Value bodyStr = valueToStringValue(args[2]);
    Value r = performRequest(args[0].as.string->data, args[1].as.string->data, bodyStr.as.string->data, args[3], line);
    freeValue(bodyStr);
    return r;
}

static Value net_encode(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    if (args[0].type != VAL_STRING) {
        novaError(ERR_ARGUMENT, line, "net.encode: expected a string");
        return makeNull();
    }
    ensureCurlGlobalInit();
    CURL* curl = curl_easy_init();
    char* escaped = curl_easy_escape(curl, args[0].as.string->data, args[0].as.string->length);
    Value r = makeString(escaped, (int)strlen(escaped));
    curl_free(escaped);
    curl_easy_cleanup(curl);
    return r;
}

static Value net_decode(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    if (args[0].type != VAL_STRING) {
        novaError(ERR_ARGUMENT, line, "net.decode: expected a string");
        return makeNull();
    }
    ensureCurlGlobalInit();
    CURL* curl = curl_easy_init();
    int outLen;
    char* decoded = curl_easy_unescape(curl, args[0].as.string->data, args[0].as.string->length, &outLen);
    Value r = makeString(decoded, outLen);
    curl_free(decoded);
    curl_easy_cleanup(curl);
    return r;
}

static Value net_isOk(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount; (void)line;
    if (args[0].type != VAL_MAP) return FALSE_VAL;
    Value out;
    if (!mapGet(args[0].as.map, makeString("ok", 2), &out)) return FALSE_VAL;
    return (out.type == VAL_BOOL && out.as.boolean) ? TRUE_VAL : FALSE_VAL;
}

static NativeFnEntry netFunctions[] = {
    {"get",             net_get,             1},
    {"post",            net_post,            2},
    {"put",             net_put,             2},
    {"delete",          net_delete,          1},
    {"patch",           net_patch,           2},
    {"getWithHeaders",  net_getWithHeaders,  2},
    {"postWithHeaders", net_postWithHeaders, 3},
    {"request",         net_request,         4},
    {"encode",          net_encode,          1},
    {"decode",          net_decode,          1},
    {"isOk",            net_isOk,            1},
};

NativeModule netModule = {
    "net",
    netFunctions,
    sizeof(netFunctions) / sizeof(netFunctions[0])
};
