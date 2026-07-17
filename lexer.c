#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "lexer.h"
#include "error.h"

void initLexer(Lexer* lexer, const char* source) {
    lexer->start   = source;
    lexer->current = source;
    lexer->line    = 1;
}

static int isAtEnd(Lexer* lexer) { return *lexer->current == '\0'; }

static char advance(Lexer* lexer) {
    lexer->current++;
    return lexer->current[-1];
}

static char peek(Lexer* lexer)     { return *lexer->current; }
static char peekNext(Lexer* lexer) { return isAtEnd(lexer) ? '\0' : lexer->current[1]; }

// Consumes the current character if it matches `expected`. Used for
// two-character operators like == != <= >=.
static int matchChar(Lexer* lexer, char expected) {
    if (isAtEnd(lexer) || peek(lexer) != expected) return 0;
    lexer->current++;
    return 1;
}

static void skipWhitespace(Lexer* lexer) {
    for (;;) {
        switch (peek(lexer)) {
            case ' ': case '\r': case '\t':
                advance(lexer);
                break;
            case '#':
                // Comments run to the end of the line; the newline itself
                // is handled by scanToken so it still produces a token.
                while (!isAtEnd(lexer) && peek(lexer) != '\n') advance(lexer);
                break;
            default:
                return;
        }
    }
}

static Token makeToken(Lexer* lexer, TokenType type) {
    Token t;
    t.type          = type;
    t.start         = lexer->start;
    t.length        = (int)(lexer->current - lexer->start);
    t.line          = lexer->line;
    t.value.integer = 0;
    return t;
}

static Token errorToken(Lexer* lexer, const char* message) {
    novaError(ERR_LEXER, lexer->line, "%s", message);
    Token t;
    t.type          = TOKEN_ERROR;
    t.start         = message;
    t.length        = (int)strlen(message);
    t.line          = lexer->line;
    t.value.integer = 0;
    return t;
}

static Token number(Lexer* lexer) {
    while (isdigit(peek(lexer))) advance(lexer);
    int isFloat = (peek(lexer) == '.' && isdigit(peekNext(lexer)));
    if (isFloat) { advance(lexer); while (isdigit(peek(lexer))) advance(lexer); }

    Token t = makeToken(lexer, isFloat ? TOKEN_FLOAT : TOKEN_NUMBER);
    if (isFloat) {
        char buf[64];
        int len = t.length < 63 ? t.length : 63;
        memcpy(buf, t.start, len); buf[len] = '\0';
        t.value.real = strtod(buf, NULL);
    } else {
        int64_t v = 0;
        for (int i = 0; i < t.length; i++) v = v * 10 + (t.start[i] - '0');
        t.value.integer = v;
    }
    return t;
}

static Token string(Lexer* lexer) {
    while (!isAtEnd(lexer) && peek(lexer) != '"') {
        if (peek(lexer) == '\n') lexer->line++;
        if (peek(lexer) == '\\') advance(lexer);
        advance(lexer);
    }

    if (isAtEnd(lexer))
        return errorToken(lexer, "Unterminated string.");

    advance(lexer); // closing quote
    return makeToken(lexer, TOKEN_STRING); // value.integer defaults to 0 — "not triple-quoted"
}

// """triple-quoted strings""" — content is taken literally (no escape
// processing, per the language's documented behavior), and may span
// multiple lines. Only ends at the next literal """.
static Token tripleString(Lexer* lexer) {
    const char* contentStart = lexer->current;
    for (;;) {
        if (isAtEnd(lexer))
            return errorToken(lexer, "Unterminated triple-quoted string.");
        if (peek(lexer) == '"' && lexer->current[1] != '\0' &&
            lexer->current[1] == '"' && lexer->current[2] == '"')
            break;
        if (peek(lexer) == '\n') lexer->line++;
        advance(lexer);
    }
    Token t;
    t.type   = TOKEN_STRING;
    t.start  = contentStart;
    t.length = (int)(lexer->current - contentStart);
    t.line   = lexer->line;
    t.value.integer = 1; // triple-quoted flag — parser skips escape processing
    advance(lexer); advance(lexer); advance(lexer); // closing """
    return t;
}

// 'A' — a single character literal. Escapes \n \t \\ \' are recognized,
// same as in regular strings; anything else after a backslash is taken
// literally (matching string()'s own "unknown escape" fallback spirit).
static Token charLiteral(Lexer* lexer) {
    if (isAtEnd(lexer)) return errorToken(lexer, "Unterminated character literal.");
    int code;
    char c = advance(lexer);
    if (c == '\\') {
        if (isAtEnd(lexer)) return errorToken(lexer, "Unterminated character literal.");
        char esc = advance(lexer);
        switch (esc) {
            case 'n':  code = '\n'; break;
            case 't':  code = '\t'; break;
            case '\\': code = '\\'; break;
            case '\'': code = '\''; break;
            default:   code = (unsigned char)esc; break;
        }
    } else {
        code = (unsigned char)c;
    }
    if (peek(lexer) != '\'')
        return errorToken(lexer, "Expected closing \"'\" for character literal.");
    advance(lexer); // closing '
    Token t = makeToken(lexer, TOKEN_CHAR);
    t.value.integer = code;
    return t;
}

// $"...{expr}..." — string interpolation. The lexer's only job is to
// find where the interpolated string actually ENDS (the closing `"` at
// brace-depth 0), correctly skipping over `{`/`}` characters that
// appear inside a nested "..." string within an expression (so
// something like ${map["key"]} doesn't get confused by the braces vs.
// quotes). It emits ONE token spanning the raw, unprocessed inner
// content; the parser (which already has an expression() to call
// recursively) does the real work of splitting that content into text
// chunks and {expr} chunks — see parseInterpolatedString in parser.c.
static Token interpString(Lexer* lexer) {
    const char* contentStart = lexer->current;
    int depth = 0;
    for (;;) {
        if (isAtEnd(lexer))
            return errorToken(lexer, "Unterminated interpolated string.");
        char c = peek(lexer);
        if (c == '\\') {
            advance(lexer);
            if (!isAtEnd(lexer)) advance(lexer);
            continue;
        }
        if (c == '"' && depth == 0) break;
        if (c == '"' && depth > 0) {
            // A nested string literal inside a {...} expression — skip
            // over it wholesale so any braces/quotes inside it don't
            // disturb this scan.
            advance(lexer);
            while (!isAtEnd(lexer) && peek(lexer) != '"') {
                if (peek(lexer) == '\n') lexer->line++;
                if (peek(lexer) == '\\') advance(lexer);
                advance(lexer);
            }
            if (isAtEnd(lexer)) return errorToken(lexer, "Unterminated string inside interpolation.");
            advance(lexer);
            continue;
        }
        if (c == '{') { depth++; advance(lexer); continue; }
        if (c == '}') {
            if (depth == 0) return errorToken(lexer, "Unmatched '}' in interpolated string.");
            depth--; advance(lexer); continue;
        }
        if (c == '\n') lexer->line++;
        advance(lexer);
    }
    Token t;
    t.type   = TOKEN_ISTRING;
    t.start  = contentStart;
    t.length = (int)(lexer->current - contentStart);
    t.line   = lexer->line;
    t.value.integer = 0;
    advance(lexer); // closing quote
    return t;
}

static TokenType identifierType(const char* start, int length) {
    if (length == 3 && strncmp(start, "let",      3) == 0) return TOKEN_LET;
    if (length == 5 && strncmp(start, "print",    5) == 0) return TOKEN_PRINT;
    if (length == 4 && strncmp(start, "true",     4) == 0) return TOKEN_TRUE;
    if (length == 5 && strncmp(start, "false",    5) == 0) return TOKEN_FALSE;
    if (length == 2 && strncmp(start, "if",        2) == 0) return TOKEN_IF;
    if (length == 4 && strncmp(start, "elif",     4) == 0) return TOKEN_ELIF;
    if (length == 4 && strncmp(start, "else",     4) == 0) return TOKEN_ELSE;
    if (length == 5 && strncmp(start, "while",    5) == 0) return TOKEN_WHILE;
    if (length == 3 && strncmp(start, "for",      3) == 0) return TOKEN_FOR;
    if (length == 2 && strncmp(start, "to",        2) == 0) return TOKEN_TO;
    if (length == 2 && strncmp(start, "in",        2) == 0) return TOKEN_IN;
    if (length == 3 && strncmp(start, "use",       3) == 0) return TOKEN_USE;
    if (length == 2 && strncmp(start, "as",        2) == 0) return TOKEN_AS;
    if (length == 3 && strncmp(start, "and",      3) == 0) return TOKEN_AND;
    if (length == 2 && strncmp(start, "or",        2) == 0) return TOKEN_OR;
    if (length == 3 && strncmp(start, "not",      3) == 0) return TOKEN_NOT;
    if (length == 5 && strncmp(start, "break",    5) == 0) return TOKEN_BREAK;
    if (length == 8 && strncmp(start, "continue", 8) == 0) return TOKEN_CONTINUE;
    if (length == 2 && strncmp(start, "fn",        2) == 0) return TOKEN_FN;
    if (length == 4 && strncmp(start, "send",     4) == 0) return TOKEN_SEND;
    if (length == 4 && strncmp(start, "null",     4) == 0) return TOKEN_NULL;
    return TOKEN_IDENTIFIER;
}

static Token identifier(Lexer* lexer) {
    while (isalnum(peek(lexer)) || peek(lexer) == '_') advance(lexer);
    return makeToken(lexer,
        identifierType(lexer->start, (int)(lexer->current - lexer->start)));
}

Token scanToken(Lexer* lexer) {
    skipWhitespace(lexer);
    lexer->start = lexer->current;
    if (isAtEnd(lexer)) return makeToken(lexer, TOKEN_EOF);

    char c = advance(lexer);

    // Collapse consecutive newlines into a single NEWLINE token,
    // skipping blank lines entirely.
    if (c == '\n') {
        lexer->line++;
        // skip any further blank lines that follow
        while (peek(lexer) == '\n' || peek(lexer) == '\r') {
            if (peek(lexer) == '\n') lexer->line++;
            advance(lexer);
        }
        return makeToken(lexer, TOKEN_NEWLINE);
    }

    if (c == '"') {
        // """triple-quoted""" if the next two characters are also
        // quotes; a plain string otherwise. peekNext() already guards
        // against reading past the end of the source.
        if (peek(lexer) == '"' && peekNext(lexer) == '"') {
            advance(lexer); advance(lexer); // consume the other two opening quotes
            return tripleString(lexer);
        }
        return string(lexer);
    }
    if (c == '$') {
        if (matchChar(lexer, '"')) return interpString(lexer);
        return errorToken(lexer, "Expected '\"' after '$'.");
    }
    if (c == '\'')               return charLiteral(lexer);
    if (isdigit(c))             return number(lexer);
    if (isalpha(c) || c == '_') return identifier(lexer);

    switch (c) {
        case '+':
            if (matchChar(lexer, '+')) return makeToken(lexer, TOKEN_PLUS_PLUS);
            if (matchChar(lexer, '=')) return makeToken(lexer, TOKEN_PLUS_EQUAL);
            return makeToken(lexer, TOKEN_PLUS);
        case '-':
            if (matchChar(lexer, '-')) return makeToken(lexer, TOKEN_MINUS_MINUS);
            if (matchChar(lexer, '=')) return makeToken(lexer, TOKEN_MINUS_EQUAL);
            return makeToken(lexer, TOKEN_MINUS);
        case '*':
            if (matchChar(lexer, '=')) return makeToken(lexer, TOKEN_STAR_EQUAL);
            return makeToken(lexer, TOKEN_STAR);
        case '/':
            if (matchChar(lexer, '=')) return makeToken(lexer, TOKEN_SLASH_EQUAL);
            return makeToken(lexer, TOKEN_SLASH);
        case '%':
            if (matchChar(lexer, '=')) return makeToken(lexer, TOKEN_PERCENT_EQUAL);
            return makeToken(lexer, TOKEN_PERCENT);
        case '(': return makeToken(lexer, TOKEN_LPAREN);
        case ')': return makeToken(lexer, TOKEN_RPAREN);
        case '{': return makeToken(lexer, TOKEN_LBRACE);
        case '}': return makeToken(lexer, TOKEN_RBRACE);
        case '[': return makeToken(lexer, TOKEN_LBRACKET);
        case ']': return makeToken(lexer, TOKEN_RBRACKET);
        case ',': return makeToken(lexer, TOKEN_COMMA);
        case ':': return makeToken(lexer, TOKEN_COLON);
        case '.': return makeToken(lexer, TOKEN_DOT);
        case '=':
            return makeToken(lexer, matchChar(lexer, '=') ? TOKEN_EQUAL_EQUAL : TOKEN_EQUAL);
        case '!':
            if (matchChar(lexer, '=')) return makeToken(lexer, TOKEN_BANG_EQUAL);
            return errorToken(lexer, "Unexpected character.");
        case '<':
            return makeToken(lexer, matchChar(lexer, '=') ? TOKEN_LESS_EQUAL : TOKEN_LESS);
        case '>':
            return makeToken(lexer, matchChar(lexer, '=') ? TOKEN_GREATER_EQUAL : TOKEN_GREATER);
    }

    return errorToken(lexer, "Unexpected character.");
}
