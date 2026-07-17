// The `file` module — see Chapter 31 of the NovaLang book.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "module_native_file.h"
#include "../error.h"

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#define mkdirOne(p) _mkdir(p)
#else
#include <unistd.h>
#include <dirent.h>
#define mkdirOne(p) mkdir(p, 0755)
#endif

static char* readWholeFile(const char* path, long* outSize) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    char* buf = malloc(size + 1);
    size_t got = fread(buf, 1, size, f);
    buf[got] = '\0';
    fclose(f);
    if (outSize) *outSize = (long)got;
    return buf;
}

static Value file_read(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    if (args[0].type != VAL_STRING) {
        novaError(ERR_ARGUMENT, line, "file.read: path must be a string");
        return makeNull();
    }
    long size;
    char* data = readWholeFile(args[0].as.string->data, &size);
    if (!data) {
        novaError(ERR_ARGUMENT, line, "file.read: could not open '%s'", args[0].as.string->data);
        return makeNull();
    }
    Value r = makeString(data, (int)size);
    free(data);
    return r;
}

static Value file_lines(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    if (args[0].type != VAL_STRING) {
        novaError(ERR_ARGUMENT, line, "file.lines: path must be a string");
        return makeNull();
    }
    long size;
    char* data = readWholeFile(args[0].as.string->data, &size);
    if (!data) {
        novaError(ERR_ARGUMENT, line, "file.lines: could not open '%s'", args[0].as.string->data);
        return makeNull();
    }
    Value result = makeArray();
    char* lineStart = data;
    for (long i = 0; i <= size; i++) {
        if (i == size || data[i] == '\n') {
            int len = (int)(data + i - lineStart);
            if (len > 0 && lineStart[len - 1] == '\r') len--; // tolerate CRLF
            arrayPush(result.as.array, makeString(lineStart, len));
            lineStart = data + i + 1;
        }
    }
    // If the file ends with a trailing newline, the loop above already
    // emits every real line and stops; if it doesn't, the last partial
    // line was still captured by the i == size case. Either way, avoid
    // one spurious trailing empty line for files ending in '\n':
    if (size > 0 && data[size - 1] == '\n' && result.as.array->count > 0) {
        Value last = result.as.array->items[result.as.array->count - 1];
        if (last.type == VAL_STRING && last.as.string->length == 0)
            result.as.array->count--;
    }
    free(data);
    return result;
}

static Value file_write(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    if (args[0].type != VAL_STRING || args[1].type != VAL_STRING) {
        novaError(ERR_ARGUMENT, line, "file.write: path and content must be strings");
        return makeNull();
    }
    FILE* f = fopen(args[0].as.string->data, "wb");
    if (!f) {
        novaError(ERR_ARGUMENT, line, "file.write: could not open '%s' for writing", args[0].as.string->data);
        return FALSE_VAL;
    }
    fwrite(args[1].as.string->data, 1, args[1].as.string->length, f);
    fclose(f);
    return TRUE_VAL;
}

static Value file_append(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    if (args[0].type != VAL_STRING || args[1].type != VAL_STRING) {
        novaError(ERR_ARGUMENT, line, "file.append: path and content must be strings");
        return makeNull();
    }
    FILE* f = fopen(args[0].as.string->data, "ab");
    if (!f) {
        novaError(ERR_ARGUMENT, line, "file.append: could not open '%s' for appending", args[0].as.string->data);
        return FALSE_VAL;
    }
    fwrite(args[1].as.string->data, 1, args[1].as.string->length, f);
    fclose(f);
    return TRUE_VAL;
}

static Value file_writeLines(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    if (args[0].type != VAL_STRING || args[1].type != VAL_ARRAY) {
        novaError(ERR_ARGUMENT, line, "file.writeLines: expected (string path, array lines)");
        return makeNull();
    }
    FILE* f = fopen(args[0].as.string->data, "wb");
    if (!f) {
        novaError(ERR_ARGUMENT, line, "file.writeLines: could not open '%s' for writing", args[0].as.string->data);
        return FALSE_VAL;
    }
    NovaArray* arr = args[1].as.array;
    for (int i = 0; i < arr->count; i++) {
        Value s = valueToStringValue(arr->items[i]);
        fwrite(s.as.string->data, 1, s.as.string->length, f);
        fputc('\n', f);
        freeValue(s);
    }
    fclose(f);
    return TRUE_VAL;
}

static Value file_exists(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount; (void)line;
    struct stat st;
    return (args[0].type == VAL_STRING && stat(args[0].as.string->data, &st) == 0) ? TRUE_VAL : FALSE_VAL;
}

static Value file_delete(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount; (void)line;
    if (args[0].type != VAL_STRING) return FALSE_VAL;
    return remove(args[0].as.string->data) == 0 ? TRUE_VAL : FALSE_VAL;
}

static Value file_copy(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    if (args[0].type != VAL_STRING || args[1].type != VAL_STRING) {
        novaError(ERR_ARGUMENT, line, "file.copy: src and dest must be strings");
        return FALSE_VAL;
    }
    FILE* src = fopen(args[0].as.string->data, "rb");
    if (!src) return FALSE_VAL;
    FILE* dst = fopen(args[1].as.string->data, "wb");
    if (!dst) { fclose(src); return FALSE_VAL; }
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0) fwrite(buf, 1, n, dst);
    fclose(src);
    fclose(dst);
    return TRUE_VAL;
}

static Value file_move(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount; (void)line;
    if (args[0].type != VAL_STRING || args[1].type != VAL_STRING) return FALSE_VAL;
    const char* src = args[0].as.string->data;
    const char* dst = args[1].as.string->data;
    if (rename(src, dst) == 0) return TRUE_VAL;
    // rename() can fail across filesystems/drives — fall back to copy+delete.
    Value copyArgs[2] = { args[0], args[1] };
    Value copied = file_copy(vm, copyArgs, 2, line);
    if (copied.type == VAL_BOOL && copied.as.boolean) {
        remove(src);
        return TRUE_VAL;
    }
    return FALSE_VAL;
}

static Value file_size(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    struct stat st;
    if (args[0].type != VAL_STRING || stat(args[0].as.string->data, &st) != 0) {
        novaError(ERR_ARGUMENT, line, "file.size: could not stat '%s'",
                  args[0].type == VAL_STRING ? args[0].as.string->data : "?");
        return makeNull();
    }
    return makeInt64(st.st_size);
}

static Value file_name(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount; (void)line;
    if (args[0].type != VAL_STRING) return makeString("", 0);
    const char* p = args[0].as.string->data;
    const char* slash = strrchr(p, '/');
    const char* backslash = strrchr(p, '\\');
    const char* cut = slash > backslash ? slash : backslash;
    const char* base = cut ? cut + 1 : p;
    return makeString(base, (int)strlen(base));
}

static Value file_extension(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount; (void)line;
    Value nameVal = file_name(vm, args, argCount, line);
    const char* n = nameVal.as.string->data;
    const char* dot = strrchr(n, '.');
    if (!dot || dot == n) return makeString("", 0);
    return makeString(dot + 1, (int)strlen(dot + 1));
}

static Value file_parent(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount; (void)line;
    if (args[0].type != VAL_STRING) return makeString("", 0);
    const char* p = args[0].as.string->data;
    const char* slash = strrchr(p, '/');
    const char* backslash = strrchr(p, '\\');
    const char* cut = slash > backslash ? slash : backslash;
    if (!cut) return makeString("", 0);
    return makeString(p, (int)(cut - p));
}

// Creates every missing directory along `path`, matching the book's
// "including any necessary parent directories" (mkdir -p semantics).
static Value file_mkdir(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount; (void)line;
    if (args[0].type != VAL_STRING) return FALSE_VAL;
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s", args[0].as.string->data);
    for (char* p = buf + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char saved = *p;
            *p = '\0';
            mkdirOne(buf);
            *p = saved;
        }
    }
    mkdirOne(buf);
    struct stat st;
    return (stat(buf, &st) == 0) ? TRUE_VAL : FALSE_VAL;
}

static Value file_isDir(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount; (void)line;
    struct stat st;
    if (args[0].type != VAL_STRING || stat(args[0].as.string->data, &st) != 0) return FALSE_VAL;
#if defined(_WIN32)
    return (st.st_mode & _S_IFDIR) ? TRUE_VAL : FALSE_VAL;
#else
    return S_ISDIR(st.st_mode) ? TRUE_VAL : FALSE_VAL;
#endif
}

static Value file_listFiles(VM* vm, Value* args, int argCount, int line) {
    (void)vm; (void)argCount;
    if (args[0].type != VAL_STRING) {
        novaError(ERR_ARGUMENT, line, "file.listFiles: path must be a string");
        return makeNull();
    }
    Value result = makeArray();
#if defined(_WIN32)
    char pattern[1024];
    snprintf(pattern, sizeof(pattern), "%s\\*", args[0].as.string->data);
    struct _finddata_t fileinfo;
    intptr_t handle = _findfirst(pattern, &fileinfo);
    if (handle == -1) return result;
    do {
        if (strcmp(fileinfo.name, ".") != 0 && strcmp(fileinfo.name, "..") != 0)
            arrayPush(result.as.array, makeString(fileinfo.name, (int)strlen(fileinfo.name)));
    } while (_findnext(handle, &fileinfo) == 0);
    _findclose(handle);
#else
    DIR* d = opendir(args[0].as.string->data);
    if (!d) return result;
    struct dirent* entry;
    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0)
            arrayPush(result.as.array, makeString(entry->d_name, (int)strlen(entry->d_name)));
    }
    closedir(d);
#endif
    return result;
}

static NativeFnEntry fileFunctions[] = {
    {"read",       file_read,       1},
    {"lines",      file_lines,      1},
    {"write",      file_write,      2},
    {"append",     file_append,     2},
    {"writeLines", file_writeLines, 2},
    {"exists",     file_exists,     1},
    {"delete",     file_delete,     1},
    {"copy",       file_copy,       2},
    {"move",       file_move,       2},
    {"size",       file_size,       1},
    {"name",       file_name,       1},
    {"extension",  file_extension,  1},
    {"parent",     file_parent,     1},
    {"mkdir",      file_mkdir,      1},
    {"listFiles",  file_listFiles,  1},
    {"isDir",      file_isDir,      1},
};

NativeModule fileModule = {
    "file",
    fileFunctions,
    sizeof(fileFunctions) / sizeof(fileFunctions[0])
};
