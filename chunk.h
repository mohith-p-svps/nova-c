#ifndef CHUNK_H
#define CHUNK_H

#include <stdint.h>
#include "value.h"

#define MAX_NAMES     256
#define MAX_CONSTANTS 256
#define MAX_LOCALS    256
#define MAX_PACKAGE_TABLES 32

// Forward-declared here (Chunk needs to store pointers to package
// FunctionTables, but FunctionTable itself — defined below — embeds a
// Chunk inside each NovaFunction, so the full definition can't come
// first).
typedef struct FunctionTable FunctionTable;

typedef enum {
    OP_CONSTANT,
    OP_ADD,
    OP_SUBTRACT,
    OP_MULTIPLY,
    OP_DIVIDE,
    OP_MODULO,
    OP_CONCAT,
    OP_NOT,
    OP_NEGATE,
    OP_EQUAL,
    OP_NOT_EQUAL,
    OP_LESS,
    OP_GREATER,
    OP_LESS_EQUAL,
    OP_GREATER_EQUAL,
    OP_DEFINE_GLOBAL,   // let x = ...   (declares and sets)
    OP_SET_GLOBAL,      // x = ...       (reassigns, errors if undeclared)
    OP_GET_GLOBAL,
    OP_GET_LOCAL,       // function-local variable (param or 'let' inside a function body)
    OP_SET_LOCAL,
    OP_CALL,            // calls a named function: operands are [nameIndex, argCount]
    OP_CALL_VALUE,      // calls a function VALUE already on the stack (pushed before its args): operand is [argCount]
    OP_CALL_NATIVE,     // calls a built-in module function: operands are [globalNativeIndex, argCount]
    OP_CALL_PACKAGE_FN, // calls a function from a loaded package: operands are [packageTableSlot, funcIndexInThatTable, argCount]
    OP_FUNC_RETURN,     // 'send' — pops the return value and ends the current function call
    OP_BUILD_ARRAY,     // operand: element count — pops that many values, pushes one array
    OP_BUILD_MAP,       // operand: pair count   — pops 2x that many values (key,value,...), pushes one map
    OP_INDEX_GET,       // pops index, pops object, pushes object[index]
    OP_INDEX_SET,       // pops value, index, object; mutates object[index] = value (no push)
    OP_LEN,             // pops a value, pushes its length (array/map/string)
    OP_TO_STRING,       // pops a value, pushes its string representation (used by string interpolation)
    OP_PRINT,
    OP_POP,
    OP_JUMP,
    OP_JUMP_IF_FALSE,
    OP_LOOP,
    OP_RETURN
} OpCode;

typedef struct {
    uint8_t* code;
    int*     lines;
    int      count;
    int      capacity;

    char* names[MAX_NAMES];
    int   nameCount;

    Value constants[MAX_CONSTANTS];
    int   constantCount;

    // Packages this chunk's bytecode calls into (via OP_CALL_PACKAGE_FN).
    // Resolved once at compile time — each distinct package a chunk
    // references gets one slot here, regardless of how many of that
    // package's functions are actually called.
    FunctionTable* packageTables[MAX_PACKAGE_TABLES];
    int            packageTableCount;
} Chunk;

void initChunk(Chunk* chunk);
void writeChunk(Chunk* chunk, uint8_t byte, int line);
void freeChunk(Chunk* chunk);

int addName(Chunk* chunk, const char* start, int length);
int addConstant(Chunk* chunk, Value value);
int getLine(Chunk* chunk, int offset);

// Registers `table` as a package this chunk calls into, reusing an
// existing slot if this exact table was already registered (so calling
// several functions from the same package doesn't waste slots). Returns
// the slot index to use as OP_CALL_PACKAGE_FN's first operand.
int addPackageTable(Chunk* chunk, FunctionTable* table);

// --- Functions --------------------------------------------------------------
//
// Nova has no closures: a function is just a name, an arity, and its own
// compiled bytecode. Calls are resolved by name at runtime (like globals),
// so functions may call each other regardless of declaration order,
// including recursively.

#define MAX_FUNCTIONS 64

typedef struct NovaFunction {
    char* name;
    int   arity;
    Chunk chunk;
} NovaFunction;

struct FunctionTable {
    NovaFunction functions[MAX_FUNCTIONS];
    int count;
};

void initFunctionTable(FunctionTable* table);

#endif