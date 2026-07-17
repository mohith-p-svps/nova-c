#ifndef PARSER_H
#define PARSER_H

#include "lexer.h"
#include "ast.h"

typedef struct {

    Lexer lexer;

    Token current;
    Token previous;

} Parser;

void initParser(
    Parser* parser,
    const char* source
);

Program* parse(
    Parser* parser
);

#endif