#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "parser.h"
#include "error.h"

static void advanceParser(Parser* parser) {
    parser->previous = parser->current;
    parser->current  = scanToken(&parser->lexer);
}

static int check(Parser* parser, TokenType type) {
    return parser->current.type == type;
}

static int match(Parser* parser, TokenType type) {
    if (!check(parser, type)) return 0;
    advanceParser(parser);
    return 1;
}

static void consumeNewline(Parser* parser) {
    if (check(parser, TOKEN_NEWLINE)) { advanceParser(parser); return; }
    if (check(parser, TOKEN_EOF))     return;
    novaError(ERR_PARSE, parser->current.line,
              "Expected newline or end of file, got '%.*s'",
              parser->current.length, parser->current.start);
}

static void consume(Parser* parser, TokenType type, const char* message) {
    if (check(parser, type)) { advanceParser(parser); return; }
    novaError(ERR_PARSE, parser->current.line, "%s (got '%.*s')",
              message, parser->current.length, parser->current.start);
    exit(1);
}

static void skipNewlines(Parser* parser) {
    while (check(parser, TOKEN_NEWLINE))
        advanceParser(parser);
}

void initParser(Parser* parser, const char* source) {
    initLexer(&parser->lexer, source);
    advanceParser(parser);
}

static Expr* expression(Parser* parser);
static Stmt* statement(Parser* parser);

static Value parseStringToken(Token t) {
    // Triple-quoted strings are taken literally — no escape processing.
    if (t.value.integer == 1) {
        return makeString(t.start, t.length);
    }

    const char* src = t.start + 1;
    int         raw = t.length - 2;

    char* buf = malloc(raw + 1);
    int   out = 0;

    for (int i = 0; i < raw; i++) {
        if (src[i] == '\\' && i + 1 < raw) {
            i++;
            switch (src[i]) {
                case 'n':  buf[out++] = '\n'; break;
                case 't':  buf[out++] = '\t'; break;
                case '"':  buf[out++] = '"';  break;
                case '\\': buf[out++] = '\\'; break;
                default:   buf[out++] = '\\'; buf[out++] = src[i]; break;
            }
        } else {
            buf[out++] = src[i];
        }
    }
    buf[out] = '\0';

    Value v = makeString(buf, out);
    free(buf);
    return v;
}

// Synthesizes a Token for a compiler-internal identifier that can't
// collide with anything the user could type (leading '$', plus a
// monotonically increasing counter so repeated uses — e.g. several
// multi-variable `let`s in the same program — never collide with each
// other either). The backing buffer is deliberately leaked: these
// tokens get embedded by value into long-lived AST nodes, and this is a
// short-running script interpreter, so it's not worth the bookkeeping
// to free it later.
static Token makeHiddenToken(const char* base) {
    static int counter = 0;
    char* buf = malloc(32);
    int len = snprintf(buf, 32, "$%s%d", base, counter++);
    Token t;
    t.type          = TOKEN_IDENTIFIER;
    t.start         = buf;
    t.length        = len;
    t.line          = -1;
    t.value.integer = 0;
    return t;
}

static Token syntheticToken(TokenType type, const char* text, int line) {
    Token t;
    t.type          = type;
    t.start         = text;
    t.length        = (int)strlen(text);
    t.line          = line;
    t.value.integer = 0;
    return t;
}

// Small growable buffer used only while splitting an interpolated
// string's raw content into literal-text chunks.
typedef struct { char* data; int len; int cap; } PBuf;
static void pbufInit(PBuf* b) { b->cap = 64; b->data = malloc(b->cap); b->len = 0; }
static void pbufPush(PBuf* b, char c) {
    if (b->len + 1 > b->cap) { b->cap *= 2; b->data = realloc(b->data, b->cap); }
    b->data[b->len++] = c;
}

// Splits an interpolated string's raw inner content (from the single
// TOKEN_ISTRING the lexer produced — see interpString in lexer.c) into
// alternating literal-text and {expr} chunks, and builds an expression
// tree that concatenates them all: "text1" + TOSTRING(expr1) + "text2"
// + ... Each {expr} substring is parsed by spinning up a fresh,
// self-contained Parser over just that substring and calling the
// normal expression() entry point on it — this file's recursive-descent
// functions don't need any special "interpolation mode" to support
// arbitrary expressions inside {}, including ones with their own
// nested string literals, calls, indexing, etc.
static Expr* parseInterpolatedString(Parser* parser) {
    Token t = parser->previous; // the TOKEN_ISTRING just matched
    const char* src = t.start;
    int len = t.length;
    int i = 0;
    Expr* result = NULL;
    Token plusTok = syntheticToken(TOKEN_PLUS, "+", t.line);

    for (;;) {
        PBuf buf; pbufInit(&buf);
        while (i < len && src[i] != '{') {
            if (src[i] == '\\' && i + 1 < len) {
                i++;
                switch (src[i]) {
                    case 'n':  pbufPush(&buf, '\n'); break;
                    case 't':  pbufPush(&buf, '\t'); break;
                    case '"':  pbufPush(&buf, '"');  break;
                    case '\\': pbufPush(&buf, '\\'); break;
                    default:   pbufPush(&buf, '\\'); pbufPush(&buf, src[i]); break;
                }
                i++;
            } else {
                pbufPush(&buf, src[i]);
                i++;
            }
        }
        Expr* textPart = newLiteralExpr(makeString(buf.data, buf.len));
        free(buf.data);
        result = result ? newBinaryExpr(result, plusTok, textPart) : textPart;

        if (i >= len) break; // no '{' found — that was the last (or only) chunk

        i++; // skip '{'
        int start = i;
        int depth = 1;
        while (i < len && depth > 0) {
            if (src[i] == '"') {
                i++;
                while (i < len && src[i] != '"') {
                    if (src[i] == '\\' && i + 1 < len) i++;
                    i++;
                }
                if (i < len) i++; // closing quote of the nested string
                continue;
            }
            if (src[i] == '{') depth++;
            else if (src[i] == '}') { depth--; if (depth == 0) break; }
            i++;
        }
        int exprLen = i - start;
        i++; // skip the matching '}' (the lexer already validated brace balance)

        char* exprSrc = malloc(exprLen + 1);
        memcpy(exprSrc, src + start, exprLen);
        exprSrc[exprLen] = '\0';

        Parser subParser;
        initParser(&subParser, exprSrc);
        Expr* exprNode = expression(&subParser);

        Expr* toStr = newToStringExpr(exprNode);
        result = newBinaryExpr(result, plusTok, toStr);
    }

    return result;
}

static Expr* primary(Parser* parser) {

    if (match(parser, TOKEN_NUMBER))
        return newLiteralExpr(makeInt64(parser->previous.value.integer));

    if (match(parser, TOKEN_FLOAT))
        return newLiteralExpr(makeFloat64(parser->previous.value.real));

    if (match(parser, TOKEN_TRUE))
        return newLiteralExpr(TRUE_VAL);

    if (match(parser, TOKEN_FALSE))
        return newLiteralExpr(FALSE_VAL);

    if (match(parser, TOKEN_NULL))
        return newLiteralExpr(NULL_VAL);

    if (match(parser, TOKEN_STRING))
        return newLiteralExpr(parseStringToken(parser->previous));

    if (match(parser, TOKEN_CHAR))
        return newLiteralExpr(makeChar((int32_t)parser->previous.value.integer));

    if (match(parser, TOKEN_ISTRING))
        return parseInterpolatedString(parser);

    if (match(parser, TOKEN_IDENTIFIER)) {
        Token name = parser->previous;

        // module.function(args) — whether `name` is actually a known
        // module alias or package is a compile-time question (aliases
        // are registered by 'use', which the compiler processes before
        // anything else — see compileProgram's use-pass); the parser
        // just recognizes the shape.
        if (match(parser, TOKEN_DOT)) {
            consume(parser, TOKEN_IDENTIFIER, "Expected a function name after '.'");
            Token funcName = parser->previous;
            consume(parser, TOKEN_LPAREN, "Expected '(' after module function name");
            Expr* args[MAX_ARGS];
            int   argCount = 0;
            if (!check(parser, TOKEN_RPAREN)) {
                do {
                    if (argCount >= MAX_ARGS) {
                        novaError(ERR_PARSE, parser->current.line,
                                  "Too many arguments (max %d)", MAX_ARGS);
                        break;
                    }
                    args[argCount++] = expression(parser);
                } while (match(parser, TOKEN_COMMA));
            }
            consume(parser, TOKEN_RPAREN, "Expected ')' after arguments");
            return newModuleCallExpr(name, funcName, args, argCount);
        }

        if (match(parser, TOKEN_LPAREN)) {
            Expr* args[MAX_ARGS];
            int   argCount = 0;
            if (!check(parser, TOKEN_RPAREN)) {
                do {
                    if (argCount >= MAX_ARGS) {
                        novaError(ERR_PARSE, parser->current.line,
                                  "Too many arguments (max %d)", MAX_ARGS);
                        break;
                    }
                    args[argCount++] = expression(parser);
                } while (match(parser, TOKEN_COMMA));
            }
            consume(parser, TOKEN_RPAREN, "Expected ')' after arguments");
            return newCallExpr(name, args, argCount);
        }
        return newVariableExpr(name);
    }

    if (match(parser, TOKEN_LPAREN)) {
        Expr* expr = expression(parser);
        consume(parser, TOKEN_RPAREN, "Expected ')'");
        return expr;
    }

    // [a, b, c]
    if (match(parser, TOKEN_LBRACKET)) {
        Expr* arr = newArrayLiteralExpr();
        if (!check(parser, TOKEN_RBRACKET)) {
            do {
                addArrayElement(arr, expression(parser));
            } while (match(parser, TOKEN_COMMA));
        }
        consume(parser, TOKEN_RBRACKET, "Expected ']' after array elements");
        return arr;
    }

    // {key: value, key: value}
    if (match(parser, TOKEN_LBRACE)) {
        Expr* map = newMapLiteralExpr();
        if (!check(parser, TOKEN_RBRACE)) {
            do {
                Expr* key = expression(parser);
                consume(parser, TOKEN_COLON, "Expected ':' after map key");
                Expr* value = expression(parser);
                addMapEntry(map, key, value);
            } while (match(parser, TOKEN_COMMA));
        }
        consume(parser, TOKEN_RBRACE, "Expected '}' after map entries");
        return map;
    }

    novaError(ERR_PARSE, parser->current.line,
              "Expected an expression, got '%.*s'",
              parser->current.length, parser->current.start);
    exit(1);
}

// postfix -> primary ('[' expression ']')*
// Handles reads: arr[i], map["key"], and chains like matrix[i][j].
// (Index *assignment* is handled separately at the statement level,
// since it needs to know the final target rather than just its value.)
static Expr* postfix(Parser* parser) {
    Expr* expr = primary(parser);
    while (match(parser, TOKEN_LBRACKET)) {
        Expr* index = expression(parser);
        consume(parser, TOKEN_RBRACKET, "Expected ']' after index");
        expr = newIndexExpr(expr, index);
    }
    return expr;
}

// unary -> ('-' | 'not') unary | primary
static Expr* unary(Parser* parser) {
    if (match(parser, TOKEN_MINUS) || match(parser, TOKEN_NOT)) {
        Token op = parser->previous;
        Expr* operand = unary(parser);
        return newUnaryExpr(op, operand);
    }
    return postfix(parser);
}

// factor -> unary (('*' | '/' | '%') unary)*
static Expr* factor(Parser* parser) {
    Expr* expr = unary(parser);
    while (match(parser, TOKEN_STAR) || match(parser, TOKEN_SLASH) || match(parser, TOKEN_PERCENT)) {
        Token op = parser->previous;
        expr = newBinaryExpr(expr, op, unary(parser));
    }
    return expr;
}

// term -> factor (('+' | '-') factor)*
static Expr* term(Parser* parser) {
    Expr* expr = factor(parser);
    while (match(parser, TOKEN_PLUS) || match(parser, TOKEN_MINUS)) {
        Token op = parser->previous;
        expr = newBinaryExpr(expr, op, factor(parser));
    }
    return expr;
}

// comparison -> term (('<' | '>' | '<=' | '>=') term)*
static Expr* comparison(Parser* parser) {
    Expr* expr = term(parser);
    while (match(parser, TOKEN_LESS) || match(parser, TOKEN_GREATER) ||
           match(parser, TOKEN_LESS_EQUAL) || match(parser, TOKEN_GREATER_EQUAL)) {
        Token op = parser->previous;
        expr = newBinaryExpr(expr, op, term(parser));
    }
    return expr;
}

// equality -> comparison (('==' | '!=') comparison)*
static Expr* equality(Parser* parser) {
    Expr* expr = comparison(parser);
    while (match(parser, TOKEN_EQUAL_EQUAL) || match(parser, TOKEN_BANG_EQUAL)) {
        Token op = parser->previous;
        expr = newBinaryExpr(expr, op, comparison(parser));
    }
    return expr;
}

// logicAnd -> equality ('and' equality)*
static Expr* logicAnd(Parser* parser) {
    Expr* expr = equality(parser);
    while (match(parser, TOKEN_AND)) {
        Token op = parser->previous;
        expr = newLogicalExpr(op, expr, equality(parser));
    }
    return expr;
}

// logicOr -> logicAnd ('or' logicAnd)*
static Expr* logicOr(Parser* parser) {
    Expr* expr = logicAnd(parser);
    while (match(parser, TOKEN_OR)) {
        Token op = parser->previous;
        expr = newLogicalExpr(op, expr, logicAnd(parser));
    }
    return expr;
}

static Expr* expression(Parser* parser) { return logicOr(parser); }

static Stmt* letStatement(Parser* parser) {
    consume(parser, TOKEN_IDENTIFIER, "Expected variable name after 'let'");
    Token names[MAX_PARAMS];
    int   nameCount = 0;
    names[nameCount++] = parser->previous;
    while (match(parser, TOKEN_COMMA)) {
        consume(parser, TOKEN_IDENTIFIER, "Expected variable name after ','");
        if (nameCount < MAX_PARAMS) names[nameCount++] = parser->previous;
    }
    consume(parser, TOKEN_EQUAL, "Expected '=' after variable name(s)");
    Expr* initializer = expression(parser);
    consumeNewline(parser);

    if (nameCount == 1)
        return newLetStmt(names[0], initializer);

    // Multiple names: evaluate the initializer expression exactly once
    // into a hidden variable, then bind each real name to a read of
    // that hidden variable. This guarantees any side effects in the
    // initializer (e.g. a function call) run once, not once per name,
    // while still giving every name the same value.
    Stmt* blockStmt = newTransparentBlockStmt();
    Token hidden = makeHiddenToken("letinit");
    addStmtToBlock(blockStmt, newLetStmt(hidden, initializer));
    for (int i = 0; i < nameCount; i++)
        addStmtToBlock(blockStmt, newLetStmt(names[i], newVariableExpr(hidden)));
    return blockStmt;
}

// Parses:  identifier = expression
// Called when we already consumed the identifier token (sits in parser->previous)
static Stmt* assignStatement(Parser* parser) {
    Token name = parser->previous;   // the identifier
    consume(parser, TOKEN_EQUAL, "Expected '=' after variable name");
    Expr* value = expression(parser);
    consumeNewline(parser);
    return newAssignStmt(name, value);
}

// print expression [, newlineExpr]
// No parentheses required — `print x` and `print(x)` both work, since
// (x) just parses as a parenthesized expression like anywhere else. The
// second argument is optional and controls whether a trailing newline
// is printed; when omitted, the compiler defaults it to `true`.
static Stmt* printStatement(Parser* parser) {
    Expr* expr = expression(parser);
    Expr* newlineExpr = NULL;
    if (match(parser, TOKEN_COMMA)) {
        newlineExpr = expression(parser);
    }
    consumeNewline(parser);
    return newPrintStmt(expr, newlineExpr);
}

// block -> '{' NEWLINE* statement* '}'
// Leaves the trailing newline (if any) for the caller to consume, since
// `}` is often immediately followed by 'elif'/'else' on the same line.
static Stmt* block(Parser* parser) {
    consume(parser, TOKEN_LBRACE, "Expected '{'");
    skipNewlines(parser);
    Stmt* b = newBlockStmt();
    while (!check(parser, TOKEN_RBRACE) && !check(parser, TOKEN_EOF)) {
        if (novaHasError()) break;
        addStmtToBlock(b, statement(parser));
        skipNewlines(parser);
    }
    consume(parser, TOKEN_RBRACE, "Expected '}'");
    return b;
}

// if expr block ('elif' expr block)* ('else' block)?
static Stmt* ifStatement(Parser* parser) {
    Expr* condition   = expression(parser);
    Stmt* thenBranch  = block(parser);

    if (match(parser, TOKEN_ELIF)) {
        Stmt* elseBranch = ifStatement(parser); // recursive call consumes its own trailing newline
        return newIfStmt(condition, thenBranch, elseBranch);
    }

    Stmt* elseBranch = NULL;
    if (match(parser, TOKEN_ELSE)) {
        elseBranch = block(parser);
    }
    consumeNewline(parser);
    return newIfStmt(condition, thenBranch, elseBranch);
}

static Stmt* whileStatement(Parser* parser) {
    Expr* condition = expression(parser);
    Stmt* body      = block(parser);
    consumeNewline(parser);
    return newWhileStmt(condition, body);
}

// for identifier = expr to expr block   OR   for identifier in expr block
static Stmt* forStatement(Parser* parser) {
    consume(parser, TOKEN_IDENTIFIER, "Expected loop variable name after 'for'");
    Token name = parser->previous;

    if (match(parser, TOKEN_IN)) {
        Expr* iterable = expression(parser);
        Stmt* body     = block(parser);
        consumeNewline(parser);
        return newForInStmt(name, iterable, body);
    }

    consume(parser, TOKEN_EQUAL, "Expected '=' or 'in' after loop variable name");
    Expr* start = expression(parser);
    consume(parser, TOKEN_TO, "Expected 'to' in for loop");
    Expr* end   = expression(parser);
    Stmt* body  = block(parser);
    consumeNewline(parser);
    return newForStmt(name, start, end, body);
}

// fn name(param, param, ...) block
static Stmt* functionDefStatement(Parser* parser) {
    consume(parser, TOKEN_IDENTIFIER, "Expected function name after 'fn'");
    Token name = parser->previous;
    consume(parser, TOKEN_LPAREN, "Expected '(' after function name");

    Token params[MAX_PARAMS];
    int   paramCount = 0;
    if (!check(parser, TOKEN_RPAREN)) {
        do {
            consume(parser, TOKEN_IDENTIFIER, "Expected parameter name");
            if (paramCount >= MAX_PARAMS) {
                novaError(ERR_PARSE, parser->previous.line,
                          "Too many parameters (max %d)", MAX_PARAMS);
            } else {
                params[paramCount++] = parser->previous;
            }
        } while (match(parser, TOKEN_COMMA));
    }
    consume(parser, TOKEN_RPAREN, "Expected ')' after parameters");

    Stmt* body = block(parser);
    consumeNewline(parser);
    return newFunctionDefStmt(name, params, paramCount, body);
}

// send [expression]
static Stmt* sendStatement(Parser* parser) {
    Expr* value = NULL;
    if (!check(parser, TOKEN_NEWLINE) && !check(parser, TOKEN_EOF)) {
        value = expression(parser);
    }
    consumeNewline(parser);
    return newReturnStmt(value);
}

// use name [as alias]
static Stmt* useStatement(Parser* parser) {
    consume(parser, TOKEN_IDENTIFIER, "Expected a module or package name after 'use'");
    Token name = parser->previous;
    Token alias = name;
    if (match(parser, TOKEN_AS)) {
        consume(parser, TOKEN_IDENTIFIER, "Expected an alias name after 'as'");
        alias = parser->previous;
    }
    consumeNewline(parser);
    return newUseStmt(name, alias);
}

static Stmt* statement(Parser* parser) {
    if (match(parser, TOKEN_LET))      return letStatement(parser);
    if (match(parser, TOKEN_PRINT))    return printStatement(parser);
    if (match(parser, TOKEN_IF))       return ifStatement(parser);
    if (match(parser, TOKEN_WHILE))    return whileStatement(parser);
    if (match(parser, TOKEN_FOR))      return forStatement(parser);
    if (match(parser, TOKEN_FN))       return functionDefStatement(parser);
    if (match(parser, TOKEN_SEND))     return sendStatement(parser);
    if (match(parser, TOKEN_USE))      return useStatement(parser);

    if (match(parser, TOKEN_BREAK)) {
        consumeNewline(parser);
        return newBreakStmt();
    }
    if (match(parser, TOKEN_CONTINUE)) {
        consumeNewline(parser);
        return newContinueStmt();
    }

    // Could be  identifier = expression  (reassignment) or  identifier(...)
    // used as a statement (a call for its side effects), or one of the
    // compound-assignment / increment / multi-variable forms below. Need
    // one token of lookahead: consume the identifier, then check what
    // follows.
    if (check(parser, TOKEN_IDENTIFIER)) {
        advanceParser(parser);   // now parser->previous = the identifier
        if (check(parser, TOKEN_EQUAL)) {
            return assignStatement(parser);
        }

        // Compound assignment: x += expr, x -= expr, x *= expr, x /= expr, x %= expr.
        // Desugars to  x = x OP expr  — reuses the existing assignment
        // and binary-op machinery entirely, so no compiler/VM changes
        // are needed for this feature.
        if (check(parser, TOKEN_PLUS_EQUAL) || check(parser, TOKEN_MINUS_EQUAL) ||
            check(parser, TOKEN_STAR_EQUAL) || check(parser, TOKEN_SLASH_EQUAL) ||
            check(parser, TOKEN_PERCENT_EQUAL)) {
            Token name = parser->previous;
            TokenType compoundType = parser->current.type;
            advanceParser(parser); // consume the compound-assign token
            Expr* rhs = expression(parser);
            consumeNewline(parser);

            TokenType binType; const char* binText;
            switch (compoundType) {
                case TOKEN_PLUS_EQUAL:    binType = TOKEN_PLUS;    binText = "+"; break;
                case TOKEN_MINUS_EQUAL:   binType = TOKEN_MINUS;   binText = "-"; break;
                case TOKEN_STAR_EQUAL:    binType = TOKEN_STAR;    binText = "*"; break;
                case TOKEN_SLASH_EQUAL:   binType = TOKEN_SLASH;   binText = "/"; break;
                default:                  binType = TOKEN_PERCENT; binText = "%"; break;
            }
            Token binOp = syntheticToken(binType, binText, name.line);
            Expr* combined = newBinaryExpr(newVariableExpr(name), binOp, rhs);
            return newAssignStmt(name, combined);
        }

        // Increment/decrement: x++, x--. Desugars to x = x + 1 / x = x - 1.
        if (check(parser, TOKEN_PLUS_PLUS) || check(parser, TOKEN_MINUS_MINUS)) {
            Token name = parser->previous;
            int isInc = check(parser, TOKEN_PLUS_PLUS);
            advanceParser(parser);
            consumeNewline(parser);
            Token binOp = isInc ? syntheticToken(TOKEN_PLUS, "+", name.line)
                                 : syntheticToken(TOKEN_MINUS, "-", name.line);
            Expr* one = newLiteralExpr(makeInt64(1));
            Expr* combined = newBinaryExpr(newVariableExpr(name), binOp, one);
            return newAssignStmt(name, combined);
        }

        // Multi-variable reassignment: i, j, k = 0 (no 'let' — every
        // name must already be declared). Same "evaluate once" approach
        // as multi-variable let.
        if (check(parser, TOKEN_COMMA)) {
            Token names[MAX_PARAMS];
            int   nameCount = 0;
            names[nameCount++] = parser->previous;
            while (match(parser, TOKEN_COMMA)) {
                consume(parser, TOKEN_IDENTIFIER, "Expected variable name after ','");
                if (nameCount < MAX_PARAMS) names[nameCount++] = parser->previous;
            }
            consume(parser, TOKEN_EQUAL, "Expected '=' after variable name(s)");
            Expr* initializer = expression(parser);
            consumeNewline(parser);
            Stmt* blockStmt = newTransparentBlockStmt();
            Token hidden = makeHiddenToken("massign");
            addStmtToBlock(blockStmt, newLetStmt(hidden, initializer));
            for (int i = 0; i < nameCount; i++)
                addStmtToBlock(blockStmt, newAssignStmt(names[i], newVariableExpr(hidden)));
            return blockStmt;
        }

        if (check(parser, TOKEN_DOT)) {
            Token aliasTok = parser->previous;
            advanceParser(parser); // consume '.'
            consume(parser, TOKEN_IDENTIFIER, "Expected a function name after '.'");
            Token funcTok = parser->previous;
            consume(parser, TOKEN_LPAREN, "Expected '(' after module function name");
            Expr* args[MAX_ARGS];
            int   argCount = 0;
            if (!check(parser, TOKEN_RPAREN)) {
                do {
                    if (argCount >= MAX_ARGS) {
                        novaError(ERR_PARSE, parser->current.line,
                                  "Too many arguments (max %d)", MAX_ARGS);
                        break;
                    }
                    args[argCount++] = expression(parser);
                } while (match(parser, TOKEN_COMMA));
            }
            consume(parser, TOKEN_RPAREN, "Expected ')' after arguments");
            consumeNewline(parser);
            return newExprStmt(newModuleCallExpr(aliasTok, funcTok, args, argCount));
        }
        if (check(parser, TOKEN_LPAREN)) {
            Token name = parser->previous;
            advanceParser(parser); // consume '('
            Expr* args[MAX_ARGS];
            int   argCount = 0;
            if (!check(parser, TOKEN_RPAREN)) {
                do {
                    if (argCount >= MAX_ARGS) {
                        novaError(ERR_PARSE, parser->current.line,
                                  "Too many arguments (max %d)", MAX_ARGS);
                        break;
                    }
                    args[argCount++] = expression(parser);
                } while (match(parser, TOKEN_COMMA));
            }
            consume(parser, TOKEN_RPAREN, "Expected ')' after arguments");
            consumeNewline(parser);
            return newExprStmt(newCallExpr(name, args, argCount));
        }
        if (check(parser, TOKEN_LBRACKET)) {
            // arr[i] = value, or chained: matrix[i][j] = value.
            // `object` accumulates everything up to (not including) the
            // final index — that's the thing whose slot actually gets
            // written to. Everything before the last bracket is just a
            // read (e.g. evaluating matrix[i] to get the inner array).
            Expr* object = newVariableExpr(parser->previous);
            Expr* index  = NULL;
            do {
                advanceParser(parser); // consume '['
                index = expression(parser);
                consume(parser, TOKEN_RBRACKET, "Expected ']' after index");
                if (check(parser, TOKEN_LBRACKET))
                    object = newIndexExpr(object, index);
            } while (check(parser, TOKEN_LBRACKET));

            if (match(parser, TOKEN_EQUAL)) {
                Expr* value = expression(parser);
                consumeNewline(parser);
                return newIndexAssignStmt(object, index, value);
            }
            // No '=' followed — it's just a read, used as a statement
            // (its value is discarded, e.g. a side-effecting expression).
            Expr* full = newIndexExpr(object, index);
            consumeNewline(parser);
            return newExprStmt(full);
        }
        // Not an assignment, call, or index — fall through to error
    }

    novaError(ERR_PARSE, parser->current.line,
              "Unknown statement starting with '%.*s'",
              parser->current.length, parser->current.start);
    exit(1);
}

Program* parse(Parser* parser) {
    Program* program = malloc(sizeof(Program));
    program->count   = 0;

    skipNewlines(parser);

    while (!check(parser, TOKEN_EOF)) {
        if (novaHasError()) break;
        program->statements[program->count++] = statement(parser);
        skipNewlines(parser);
    }
    return program;
}
