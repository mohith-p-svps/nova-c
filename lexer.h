#ifndef LEXER_H
#define LEXER_H

#include "token.h"

typedef struct {
    const char* start;
    const char* current;
    int         line;       // current line number, starts at 1
} Lexer;

void initLexer(Lexer* lexer, const char* source);

Token scanToken(Lexer* lexer);

#endif