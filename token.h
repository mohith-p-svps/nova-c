#ifndef TOKEN_H
#define TOKEN_H

#include <stdint.h>

typedef enum {
    TOKEN_LET,
    TOKEN_PRINT,
    TOKEN_TRUE,         // true
    TOKEN_FALSE,        // false

    TOKEN_IF,
    TOKEN_ELIF,
    TOKEN_ELSE,
    TOKEN_WHILE,
    TOKEN_FOR,
    TOKEN_TO,
    TOKEN_IN,
    TOKEN_USE,
    TOKEN_AS,
    TOKEN_AND,
    TOKEN_OR,
    TOKEN_NOT,
    TOKEN_BREAK,
    TOKEN_CONTINUE,
    TOKEN_FN,
    TOKEN_SEND,
    TOKEN_NULL,

    TOKEN_IDENTIFIER,
    TOKEN_NUMBER,
    TOKEN_FLOAT,
    TOKEN_STRING,       // "hello" — value.integer used as a flag: 1 = triple-quoted (raw, no escapes), 0 = normal
    TOKEN_CHAR,         // 'A' — value.integer holds the ASCII code
    TOKEN_ISTRING,      // $"...{expr}..." — raw inner content (start/length), re-parsed by the parser

    TOKEN_EQUAL,         // =
    TOKEN_EQUAL_EQUAL,   // ==
    TOKEN_BANG_EQUAL,    // !=
    TOKEN_LESS,          // <
    TOKEN_GREATER,       // >
    TOKEN_LESS_EQUAL,    // <=
    TOKEN_GREATER_EQUAL, // >=

    TOKEN_PLUS,
    TOKEN_MINUS,
    TOKEN_STAR,
    TOKEN_SLASH,
    TOKEN_PERCENT,

    TOKEN_PLUS_EQUAL,    // +=
    TOKEN_MINUS_EQUAL,   // -=
    TOKEN_STAR_EQUAL,    // *=
    TOKEN_SLASH_EQUAL,   // /=
    TOKEN_PERCENT_EQUAL, // %=
    TOKEN_PLUS_PLUS,     // ++
    TOKEN_MINUS_MINUS,   // --

    TOKEN_LPAREN,
    TOKEN_RPAREN,
    TOKEN_LBRACE,
    TOKEN_RBRACE,
    TOKEN_LBRACKET,
    TOKEN_RBRACKET,
    TOKEN_COMMA,
    TOKEN_COLON,
    TOKEN_DOT,       // used only for module.function(...) calls — see EXPR_MODULE_CALL

    TOKEN_NEWLINE,

    TOKEN_EOF,
    TOKEN_ERROR

} TokenType;

typedef struct {
    TokenType   type;
    const char* start;
    int         length;
    int         line;

    union {
        int64_t integer;
        double  real;
        // strings use start+length directly from source — no copy needed at token stage
    } value;

} Token;

#endif
