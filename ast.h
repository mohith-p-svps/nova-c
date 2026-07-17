#ifndef AST_H
#define AST_H

#include "token.h"
#include "value.h"

#define MAX_STATEMENTS 256
#define MAX_PARAMS     16
#define MAX_ARGS       16

typedef struct Expr Expr;
typedef struct Stmt Stmt;

typedef enum {
    EXPR_LITERAL,
    EXPR_VARIABLE,
    EXPR_BINARY,
    EXPR_UNARY,    // -x, not x
    EXPR_LOGICAL,  // a and b, a or b  (short-circuiting; compiled differently from EXPR_BINARY)
    EXPR_CALL,     // name(arg, arg, ...)
    EXPR_ARRAY_LITERAL,  // [a, b, c]
    EXPR_MAP_LITERAL,    // {k: v, k: v}
    EXPR_INDEX,          // object[index] — read
    EXPR_TO_STRING,      // converts its operand to a string at runtime (string interpolation)
    EXPR_MODULE_CALL     // module.function(args) — a call to a built-in module or loaded package
} ExprType;

struct Expr {

    ExprType type;

    union {

        struct {
            Value value;
        } literal;

        struct {
            Token name;
        } variable;

        struct {
            Expr* left;
            Token op;
            Expr* right;
        } binary;

        struct {
            Token op;     // TOKEN_MINUS or TOKEN_NOT
            Expr* operand;
        } unary;

        struct {
            Token op;     // TOKEN_AND or TOKEN_OR
            Expr* left;
            Expr* right;
        } logical;

        struct {
            Token name;
            Expr* args[MAX_ARGS];
            int   argCount;
        } call;

        struct {
            Expr** elements;
            int    count;
            int    capacity;
        } arrayLiteral;

        struct {
            Expr** keys;
            Expr** values;
            int    count;
            int    capacity;
        } mapLiteral;

        struct {
            Expr* object;
            Expr* index;
        } index;

        struct {
            Expr* operand;
        } toString;

        struct {
            Token moduleAlias;
            Token funcName;
            Expr* args[MAX_ARGS];
            int   argCount;
        } moduleCall;
    };
};

typedef enum {
    STMT_LET,
    STMT_ASSIGN,    // h = 123  (reassignment, no 'let')
    STMT_PRINT,
    STMT_BLOCK,
    STMT_IF,
    STMT_WHILE,
    STMT_FOR,
    STMT_FOR_IN,    // for x in collection { ... }
    STMT_BREAK,
    STMT_CONTINUE,
    STMT_FUNCTION_DEF,
    STMT_RETURN,    // 'send' — may carry no expression (bare send)
    STMT_EXPR,      // a bare expression used as a statement (e.g. a call for its side effects)
    STMT_INDEX_ASSIGN, // object[index] = value
    STMT_USE           // use moduleOrPackage [as alias]
} StmtType;

struct Stmt {

    StmtType type;

    union {

        struct {
            Token  name;
            Expr*  initializer;
        } letStmt;

        struct {
            Token  name;        // variable being reassigned
            Expr*  value;       // new value
        } assignStmt;

        struct {
            Expr* expression;
            Expr* newlineExpr;  // NULL means "default true" — print always ends with a newline unless told otherwise
        } printStmt;

        struct {
            Stmt** statements;
            int    count;
            int    capacity;
            int    transparent; // 1 = a compiler-internal grouping (e.g. multi-variable let/reassign desugaring) that should NOT introduce its own scope — its statements belong to whatever scope was already active
        } block;

        struct {
            Expr* condition;
            Stmt* thenBranch;   // always a STMT_BLOCK
            Stmt* elseBranch;   // STMT_BLOCK, nested STMT_IF (for elif), or NULL
        } ifStmt;

        struct {
            Expr* condition;
            Stmt* body;         // STMT_BLOCK
        } whileStmt;

        struct {
            Token name;          // loop variable
            Expr* start;
            Expr* end;
            Stmt* body;          // STMT_BLOCK
        } forStmt;

        struct {
            Token name;          // loop variable — bound to each element in turn
            Expr* iterable;      // the array/map/string being iterated
            Stmt* body;          // STMT_BLOCK
        } forInStmt;

        struct {
            Token name;
            Token params[MAX_PARAMS];
            int   paramCount;
            Stmt* body;          // STMT_BLOCK
        } functionDef;

        struct {
            Expr* value;          // NULL for a bare 'send'
        } returnStmt;

        struct {
            Expr* expression;
        } exprStmt;

        struct {
            Expr* object;
            Expr* index;
            Expr* value;
        } indexAssignStmt;

        struct {
            Token name;   // the module/package name as written, e.g. 'math' in 'use math'
            Token alias;  // same as name if no 'as' clause; the bound alias otherwise
        } useStmt;
    };
};

typedef struct {
    Stmt* statements[MAX_STATEMENTS];
    int   count;
} Program;

Expr* newLiteralExpr(Value value);
Expr* newVariableExpr(Token name);
Expr* newBinaryExpr(Expr* left, Token op, Expr* right);
Expr* newUnaryExpr(Token op, Expr* operand);
Expr* newLogicalExpr(Token op, Expr* left, Expr* right);
Expr* newCallExpr(Token name, Expr** args, int argCount);

Expr* newArrayLiteralExpr(void);
void  addArrayElement(Expr* arrayLiteral, Expr* element);

Expr* newMapLiteralExpr(void);
void  addMapEntry(Expr* mapLiteral, Expr* key, Expr* value);

Expr* newIndexExpr(Expr* object, Expr* index);

Stmt* newLetStmt(Token name, Expr* initializer);
Stmt* newAssignStmt(Token name, Expr* value);
Stmt* newPrintStmt(Expr* expression, Expr* newlineExpr);

Stmt* newBlockStmt(void);
Stmt* newTransparentBlockStmt(void);
void  addStmtToBlock(Stmt* block, Stmt* stmt);

Stmt* newIfStmt(Expr* condition, Stmt* thenBranch, Stmt* elseBranch);
Stmt* newWhileStmt(Expr* condition, Stmt* body);
Stmt* newForStmt(Token name, Expr* start, Expr* end, Stmt* body);
Stmt* newForInStmt(Token name, Expr* iterable, Stmt* body);
Stmt* newBreakStmt(void);
Stmt* newContinueStmt(void);

Stmt* newFunctionDefStmt(Token name, Token* params, int paramCount, Stmt* body);
Stmt* newReturnStmt(Expr* value);
Stmt* newExprStmt(Expr* expression);
Stmt* newIndexAssignStmt(Expr* object, Expr* index, Expr* value);
Stmt* newUseStmt(Token name, Token alias);

Expr* newToStringExpr(Expr* operand);
Expr* newModuleCallExpr(Token moduleAlias, Token funcName, Expr** args, int argCount);

#endif
