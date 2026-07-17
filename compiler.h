#ifndef COMPILER_H
#define COMPILER_H

#include "ast.h"
#include "chunk.h"

void compileProgram(
    Program* program,
    Chunk* chunk,
    FunctionTable* functions
);

#endif
