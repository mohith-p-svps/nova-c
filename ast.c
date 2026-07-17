#include <stdlib.h>

#include "ast.h"

Expr* newLiteralExpr(Value value) {
    Expr* expr = malloc(sizeof(Expr));
    expr->type          = EXPR_LITERAL;
    expr->literal.value = value;
    return expr;
}

Expr* newVariableExpr(Token name) {
    Expr* expr = malloc(sizeof(Expr));
    expr->type          = EXPR_VARIABLE;
    expr->variable.name = name;
    return expr;
}

Expr* newBinaryExpr(Expr* left, Token op, Expr* right) {
    Expr* expr = malloc(sizeof(Expr));
    expr->type         = EXPR_BINARY;
    expr->binary.left  = left;
    expr->binary.op    = op;
    expr->binary.right = right;
    return expr;
}

Expr* newUnaryExpr(Token op, Expr* operand) {
    Expr* expr = malloc(sizeof(Expr));
    expr->type           = EXPR_UNARY;
    expr->unary.op       = op;
    expr->unary.operand  = operand;
    return expr;
}

Expr* newLogicalExpr(Token op, Expr* left, Expr* right) {
    Expr* expr = malloc(sizeof(Expr));
    expr->type          = EXPR_LOGICAL;
    expr->logical.op    = op;
    expr->logical.left  = left;
    expr->logical.right = right;
    return expr;
}

Expr* newCallExpr(Token name, Expr** args, int argCount) {
    Expr* expr = malloc(sizeof(Expr));
    expr->type           = EXPR_CALL;
    expr->call.name      = name;
    expr->call.argCount  = argCount;
    for (int i = 0; i < argCount; i++) expr->call.args[i] = args[i];
    return expr;
}

Expr* newArrayLiteralExpr(void) {
    Expr* expr = malloc(sizeof(Expr));
    expr->type                  = EXPR_ARRAY_LITERAL;
    expr->arrayLiteral.elements = NULL;
    expr->arrayLiteral.count    = 0;
    expr->arrayLiteral.capacity = 0;
    return expr;
}

void addArrayElement(Expr* arrayLiteral, Expr* element) {
    if (arrayLiteral->arrayLiteral.count + 1 > arrayLiteral->arrayLiteral.capacity) {
        arrayLiteral->arrayLiteral.capacity =
            arrayLiteral->arrayLiteral.capacity < 8 ? 8 : arrayLiteral->arrayLiteral.capacity * 2;
        arrayLiteral->arrayLiteral.elements = realloc(arrayLiteral->arrayLiteral.elements,
            sizeof(Expr*) * arrayLiteral->arrayLiteral.capacity);
    }
    arrayLiteral->arrayLiteral.elements[arrayLiteral->arrayLiteral.count++] = element;
}

Expr* newMapLiteralExpr(void) {
    Expr* expr = malloc(sizeof(Expr));
    expr->type              = EXPR_MAP_LITERAL;
    expr->mapLiteral.keys     = NULL;
    expr->mapLiteral.values   = NULL;
    expr->mapLiteral.count    = 0;
    expr->mapLiteral.capacity = 0;
    return expr;
}

void addMapEntry(Expr* mapLiteral, Expr* key, Expr* value) {
    if (mapLiteral->mapLiteral.count + 1 > mapLiteral->mapLiteral.capacity) {
        mapLiteral->mapLiteral.capacity =
            mapLiteral->mapLiteral.capacity < 8 ? 8 : mapLiteral->mapLiteral.capacity * 2;
        mapLiteral->mapLiteral.keys = realloc(mapLiteral->mapLiteral.keys,
            sizeof(Expr*) * mapLiteral->mapLiteral.capacity);
        mapLiteral->mapLiteral.values = realloc(mapLiteral->mapLiteral.values,
            sizeof(Expr*) * mapLiteral->mapLiteral.capacity);
    }
    mapLiteral->mapLiteral.keys[mapLiteral->mapLiteral.count]   = key;
    mapLiteral->mapLiteral.values[mapLiteral->mapLiteral.count] = value;
    mapLiteral->mapLiteral.count++;
}

Expr* newIndexExpr(Expr* object, Expr* index) {
    Expr* expr = malloc(sizeof(Expr));
    expr->type         = EXPR_INDEX;
    expr->index.object = object;
    expr->index.index  = index;
    return expr;
}

Expr* newToStringExpr(Expr* operand) {
    Expr* expr = malloc(sizeof(Expr));
    expr->type            = EXPR_TO_STRING;
    expr->toString.operand = operand;
    return expr;
}

Expr* newModuleCallExpr(Token moduleAlias, Token funcName, Expr** args, int argCount) {
    Expr* expr = malloc(sizeof(Expr));
    expr->type                  = EXPR_MODULE_CALL;
    expr->moduleCall.moduleAlias = moduleAlias;
    expr->moduleCall.funcName    = funcName;
    expr->moduleCall.argCount    = argCount;
    for (int i = 0; i < argCount; i++) expr->moduleCall.args[i] = args[i];
    return expr;
}

Stmt* newLetStmt(Token name, Expr* initializer) {
    Stmt* stmt = malloc(sizeof(Stmt));
    stmt->type                = STMT_LET;
    stmt->letStmt.name        = name;
    stmt->letStmt.initializer = initializer;
    return stmt;
}

Stmt* newAssignStmt(Token name, Expr* value) {
    Stmt* stmt = malloc(sizeof(Stmt));
    stmt->type               = STMT_ASSIGN;
    stmt->assignStmt.name    = name;
    stmt->assignStmt.value   = value;
    return stmt;
}

Stmt* newPrintStmt(Expr* expression, Expr* newlineExpr) {
    Stmt* stmt = malloc(sizeof(Stmt));
    stmt->type                  = STMT_PRINT;
    stmt->printStmt.expression  = expression;
    stmt->printStmt.newlineExpr = newlineExpr;
    return stmt;
}

Stmt* newBlockStmt(void) {
    Stmt* stmt = malloc(sizeof(Stmt));
    stmt->type             = STMT_BLOCK;
    stmt->block.statements  = NULL;
    stmt->block.count       = 0;
    stmt->block.capacity    = 0;
    stmt->block.transparent = 0;
    return stmt;
}

Stmt* newTransparentBlockStmt(void) {
    Stmt* stmt = malloc(sizeof(Stmt));
    stmt->type              = STMT_BLOCK;
    stmt->block.statements  = NULL;
    stmt->block.count       = 0;
    stmt->block.capacity    = 0;
    stmt->block.transparent = 1;
    return stmt;
}

void addStmtToBlock(Stmt* block, Stmt* s) {
    if (block->block.count + 1 > block->block.capacity) {
        block->block.capacity = block->block.capacity < 8 ? 8 : block->block.capacity * 2;
        block->block.statements = realloc(block->block.statements,
                                           sizeof(Stmt*) * block->block.capacity);
    }
    block->block.statements[block->block.count++] = s;
}

Stmt* newIfStmt(Expr* condition, Stmt* thenBranch, Stmt* elseBranch) {
    Stmt* stmt = malloc(sizeof(Stmt));
    stmt->type               = STMT_IF;
    stmt->ifStmt.condition   = condition;
    stmt->ifStmt.thenBranch  = thenBranch;
    stmt->ifStmt.elseBranch  = elseBranch;
    return stmt;
}

Stmt* newWhileStmt(Expr* condition, Stmt* body) {
    Stmt* stmt = malloc(sizeof(Stmt));
    stmt->type              = STMT_WHILE;
    stmt->whileStmt.condition = condition;
    stmt->whileStmt.body      = body;
    return stmt;
}

Stmt* newForStmt(Token name, Expr* start, Expr* end, Stmt* body) {
    Stmt* stmt = malloc(sizeof(Stmt));
    stmt->type           = STMT_FOR;
    stmt->forStmt.name    = name;
    stmt->forStmt.start   = start;
    stmt->forStmt.end     = end;
    stmt->forStmt.body    = body;
    return stmt;
}

Stmt* newForInStmt(Token name, Expr* iterable, Stmt* body) {
    Stmt* stmt = malloc(sizeof(Stmt));
    stmt->type               = STMT_FOR_IN;
    stmt->forInStmt.name     = name;
    stmt->forInStmt.iterable = iterable;
    stmt->forInStmt.body     = body;
    return stmt;
}

Stmt* newBreakStmt(void) {
    Stmt* stmt = malloc(sizeof(Stmt));
    stmt->type = STMT_BREAK;
    return stmt;
}

Stmt* newContinueStmt(void) {
    Stmt* stmt = malloc(sizeof(Stmt));
    stmt->type = STMT_CONTINUE;
    return stmt;
}

Stmt* newFunctionDefStmt(Token name, Token* params, int paramCount, Stmt* body) {
    Stmt* stmt = malloc(sizeof(Stmt));
    stmt->type                    = STMT_FUNCTION_DEF;
    stmt->functionDef.name        = name;
    stmt->functionDef.paramCount  = paramCount;
    for (int i = 0; i < paramCount; i++) stmt->functionDef.params[i] = params[i];
    stmt->functionDef.body        = body;
    return stmt;
}

Stmt* newReturnStmt(Expr* value) {
    Stmt* stmt = malloc(sizeof(Stmt));
    stmt->type             = STMT_RETURN;
    stmt->returnStmt.value  = value;
    return stmt;
}

Stmt* newExprStmt(Expr* expression) {
    Stmt* stmt = malloc(sizeof(Stmt));
    stmt->type              = STMT_EXPR;
    stmt->exprStmt.expression = expression;
    return stmt;
}

Stmt* newIndexAssignStmt(Expr* object, Expr* index, Expr* value) {
    Stmt* stmt = malloc(sizeof(Stmt));
    stmt->type                    = STMT_INDEX_ASSIGN;
    stmt->indexAssignStmt.object  = object;
    stmt->indexAssignStmt.index   = index;
    stmt->indexAssignStmt.value   = value;
    return stmt;
}

Stmt* newUseStmt(Token name, Token alias) {
    Stmt* stmt = malloc(sizeof(Stmt));
    stmt->type          = STMT_USE;
    stmt->useStmt.name  = name;
    stmt->useStmt.alias = alias;
    return stmt;
}
