#include <stdio.h>
#include "disassembler.h"

static int simpleInstruction(const char* name, int offset) {
    printf("%s\n", name);
    return offset + 1;
}

static int jumpInstruction(const char* name, int sign, Chunk* chunk, int offset) {
    uint16_t jump = (uint16_t)((chunk->code[offset + 1] << 8) | chunk->code[offset + 2]);
    printf("%-16s -> %d\n", name, offset + 3 + sign * jump);
    return offset + 3;
}

static int disassembleInstruction(Chunk* chunk, int offset) {
    int line = getLine(chunk, offset);
    if (line >= 0) printf("%04d [line %3d] ", offset, line);
    else           printf("%04d [         ] ", offset);

    uint8_t instruction = chunk->code[offset];
    switch (instruction) {

    case OP_CONSTANT: {
        int index = chunk->code[offset + 1];
        printf("OP_CONSTANT     [%d] = ", index);
        printValue(chunk->constants[index]);
        printf("\n");
        return offset + 2;
    }
    case OP_ADD:              return simpleInstruction("OP_ADD",          offset);
    case OP_SUBTRACT:         return simpleInstruction("OP_SUBTRACT",     offset);
    case OP_MULTIPLY:         return simpleInstruction("OP_MULTIPLY",     offset);
    case OP_DIVIDE:           return simpleInstruction("OP_DIVIDE",       offset);
    case OP_MODULO:           return simpleInstruction("OP_MODULO",      offset);
    case OP_CONCAT:           return simpleInstruction("OP_CONCAT",       offset);
    case OP_NOT:              return simpleInstruction("OP_NOT",          offset);
    case OP_NEGATE:           return simpleInstruction("OP_NEGATE",       offset);
    case OP_EQUAL:            return simpleInstruction("OP_EQUAL",        offset);
    case OP_NOT_EQUAL:        return simpleInstruction("OP_NOT_EQUAL",    offset);
    case OP_LESS:             return simpleInstruction("OP_LESS",         offset);
    case OP_GREATER:          return simpleInstruction("OP_GREATER",      offset);
    case OP_LESS_EQUAL:       return simpleInstruction("OP_LESS_EQUAL",   offset);
    case OP_GREATER_EQUAL:    return simpleInstruction("OP_GREATER_EQUAL",offset);
    case OP_DEFINE_GLOBAL:
        printf("OP_DEFINE_GLOBAL '%s'\n", chunk->names[chunk->code[offset+1]]);
        return offset + 2;
    case OP_SET_GLOBAL:
        printf("OP_SET_GLOBAL   '%s'\n", chunk->names[chunk->code[offset+1]]);
        return offset + 2;
    case OP_GET_GLOBAL:
        printf("OP_GET_GLOBAL   '%s'\n", chunk->names[chunk->code[offset+1]]);
        return offset + 2;
    case OP_GET_LOCAL:
        printf("OP_GET_LOCAL    [%d]\n", chunk->code[offset+1]);
        return offset + 2;
    case OP_SET_LOCAL:
        printf("OP_SET_LOCAL    [%d]\n", chunk->code[offset+1]);
        return offset + 2;
    case OP_CALL:
        printf("OP_CALL         '%s' (%d args)\n",
               chunk->names[chunk->code[offset+1]], chunk->code[offset+2]);
        return offset + 3;
    case OP_CALL_VALUE:
        printf("OP_CALL_VALUE   (%d args)\n", chunk->code[offset+1]);
        return offset + 2;
    case OP_CALL_NATIVE: {
        int idx = (chunk->code[offset+1] << 8) | chunk->code[offset+2];
        printf("OP_CALL_NATIVE  [native #%d] (%d args)\n", idx, chunk->code[offset+3]);
        return offset + 4;
    }
    case OP_CALL_PACKAGE_FN:
        printf("OP_CALL_PACKAGE_FN [table %d, fn %d] (%d args)\n",
               chunk->code[offset+1], chunk->code[offset+2], chunk->code[offset+3]);
        return offset + 4;
    case OP_TO_STRING:        return simpleInstruction("OP_TO_STRING",      offset);
    case OP_FUNC_RETURN:      return simpleInstruction("OP_FUNC_RETURN",   offset);
    case OP_BUILD_ARRAY:
        printf("OP_BUILD_ARRAY  (%d elements)\n", chunk->code[offset+1]);
        return offset + 2;
    case OP_BUILD_MAP:
        printf("OP_BUILD_MAP    (%d pairs)\n", chunk->code[offset+1]);
        return offset + 2;
    case OP_INDEX_GET:        return simpleInstruction("OP_INDEX_GET",     offset);
    case OP_INDEX_SET:        return simpleInstruction("OP_INDEX_SET",     offset);
    case OP_LEN:              return simpleInstruction("OP_LEN",           offset);
    case OP_PRINT:            return simpleInstruction("OP_PRINT", offset);
    case OP_POP:              return simpleInstruction("OP_POP",  offset);
    case OP_JUMP:             return jumpInstruction("OP_JUMP",           1, chunk, offset);
    case OP_JUMP_IF_FALSE:    return jumpInstruction("OP_JUMP_IF_FALSE",  1, chunk, offset);
    case OP_LOOP:             return jumpInstruction("OP_LOOP",          -1, chunk, offset);
    case OP_RETURN:           return simpleInstruction("OP_RETURN", offset);
    default:
        printf("UNKNOWN %d\n", instruction);
        return offset + 1;
    }
}

void disassembleChunk(Chunk* chunk, const char* name) {
    printf("== %s ==\n", name);
    int offset = 0;
    while (offset < chunk->count)
        offset = disassembleInstruction(chunk, offset);
}
