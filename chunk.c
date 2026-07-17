#include <stdlib.h>
#include <string.h>

#include "chunk.h"

void initChunk(Chunk* chunk) {
    chunk->count    = 0;
    chunk->capacity = 0;
    chunk->code     = NULL;
    chunk->lines    = NULL;

    chunk->nameCount     = 0;
    chunk->constantCount = 0;
    chunk->packageTableCount = 0;
}

static void growChunk(Chunk* chunk) {
    int old      = chunk->capacity;
    chunk->capacity = old < 8 ? 8 : old * 2;
    chunk->code  = realloc(chunk->code,  sizeof(uint8_t) * chunk->capacity);
    chunk->lines = realloc(chunk->lines, sizeof(int)     * chunk->capacity);
}

void writeChunk(Chunk* chunk, uint8_t byte, int line) {
    if (chunk->count + 1 > chunk->capacity)
        growChunk(chunk);

    chunk->code [chunk->count] = byte;
    chunk->lines[chunk->count] = line;
    chunk->count++;
}

void freeChunk(Chunk* chunk) {
    for (int i = 0; i < chunk->constantCount; i++)
        freeValue(chunk->constants[i]);
    for (int i = 0; i < chunk->nameCount; i++)
        free(chunk->names[i]);

    free(chunk->code);
    free(chunk->lines);

    initChunk(chunk);
}

int addName(Chunk* chunk, const char* start, int length) {
    char* copy = malloc(length + 1);
    memcpy(copy, start, length);
    copy[length] = '\0';
    chunk->names[chunk->nameCount] = copy;
    return chunk->nameCount++;
}

int addConstant(Chunk* chunk, Value value) {
    chunk->constants[chunk->constantCount] = value;
    return chunk->constantCount++;
}

int getLine(Chunk* chunk, int offset) {
    if (offset < 0 || offset >= chunk->count) return -1;
    return chunk->lines[offset];
}

int addPackageTable(Chunk* chunk, FunctionTable* table) {
    for (int i = 0; i < chunk->packageTableCount; i++)
        if (chunk->packageTables[i] == table) return i;
    chunk->packageTables[chunk->packageTableCount] = table;
    return chunk->packageTableCount++;
}

void initFunctionTable(FunctionTable* table) {
    table->count = 0;
}