#include <stdio.h>

#include "ast_printer.h"
#include "value.h"

static void indent(int depth) {
    for (int i = 0; i < depth; i++) printf("  ");
}

static void printStmt(Stmt* stmt, int depth);

static void printExpr(Expr* expr, int depth) {
    switch (expr->type) {

        case EXPR_LITERAL:
            indent(depth);
            printf("LITERAL ");
            printValue(expr->literal.value);
            printf("\n");
            break;

        case EXPR_VARIABLE:
            indent(depth);
            printf("VARIABLE %.*s\n",
                   expr->variable.name.length,
                   expr->variable.name.start);
            break;

        case EXPR_BINARY:
            indent(depth);
            printf("BINARY %.*s\n",
                   expr->binary.op.length,
                   expr->binary.op.start);
            printExpr(expr->binary.left,  depth + 1);
            printExpr(expr->binary.right, depth + 1);
            break;

        case EXPR_UNARY:
            indent(depth);
            printf("UNARY %.*s\n",
                   expr->unary.op.length,
                   expr->unary.op.start);
            printExpr(expr->unary.operand, depth + 1);
            break;

        case EXPR_LOGICAL:
            indent(depth);
            printf("LOGICAL %.*s\n",
                   expr->logical.op.length,
                   expr->logical.op.start);
            printExpr(expr->logical.left,  depth + 1);
            printExpr(expr->logical.right, depth + 1);
            break;

        case EXPR_CALL:
            indent(depth);
            printf("CALL %.*s\n",
                   expr->call.name.length,
                   expr->call.name.start);
            for (int i = 0; i < expr->call.argCount; i++)
                printExpr(expr->call.args[i], depth + 1);
            break;

        case EXPR_ARRAY_LITERAL:
            indent(depth);
            printf("ARRAY_LITERAL\n");
            for (int i = 0; i < expr->arrayLiteral.count; i++)
                printExpr(expr->arrayLiteral.elements[i], depth + 1);
            break;

        case EXPR_MAP_LITERAL:
            indent(depth);
            printf("MAP_LITERAL\n");
            for (int i = 0; i < expr->mapLiteral.count; i++) {
                indent(depth + 1);
                printf("PAIR\n");
                printExpr(expr->mapLiteral.keys[i],   depth + 2);
                printExpr(expr->mapLiteral.values[i], depth + 2);
            }
            break;

        case EXPR_INDEX:
            indent(depth);
            printf("INDEX\n");
            printExpr(expr->index.object, depth + 1);
            printExpr(expr->index.index,  depth + 1);
            break;

        case EXPR_TO_STRING:
            indent(depth);
            printf("TO_STRING\n");
            printExpr(expr->toString.operand, depth + 1);
            break;

        case EXPR_MODULE_CALL:
            indent(depth);
            printf("MODULE_CALL %.*s.%.*s\n",
                   expr->moduleCall.moduleAlias.length, expr->moduleCall.moduleAlias.start,
                   expr->moduleCall.funcName.length, expr->moduleCall.funcName.start);
            for (int i = 0; i < expr->moduleCall.argCount; i++)
                printExpr(expr->moduleCall.args[i], depth + 1);
            break;
    }
}

static void printStmt(Stmt* stmt, int depth) {
    switch (stmt->type) {

        case STMT_LET:
            indent(depth);
            printf("LET %.*s\n",
                   stmt->letStmt.name.length,
                   stmt->letStmt.name.start);
            printExpr(stmt->letStmt.initializer, depth + 1);
            break;

        case STMT_ASSIGN:
            indent(depth);
            printf("ASSIGN %.*s\n",
                   stmt->assignStmt.name.length,
                   stmt->assignStmt.name.start);
            printExpr(stmt->assignStmt.value, depth + 1);
            break;

        case STMT_PRINT:
            indent(depth);
            printf("PRINT\n");
            printExpr(stmt->printStmt.expression, depth + 1);
            break;

        case STMT_BLOCK:
            indent(depth);
            printf("BLOCK\n");
            for (int i = 0; i < stmt->block.count; i++)
                printStmt(stmt->block.statements[i], depth + 1);
            break;

        case STMT_IF:
            indent(depth);
            printf("IF\n");
            printExpr(stmt->ifStmt.condition, depth + 1);
            printStmt(stmt->ifStmt.thenBranch, depth + 1);
            if (stmt->ifStmt.elseBranch) {
                indent(depth);
                printf("ELSE\n");
                printStmt(stmt->ifStmt.elseBranch, depth + 1);
            }
            break;

        case STMT_WHILE:
            indent(depth);
            printf("WHILE\n");
            printExpr(stmt->whileStmt.condition, depth + 1);
            printStmt(stmt->whileStmt.body, depth + 1);
            break;

        case STMT_FOR:
            indent(depth);
            printf("FOR %.*s\n",
                   stmt->forStmt.name.length,
                   stmt->forStmt.name.start);
            printExpr(stmt->forStmt.start, depth + 1);
            printExpr(stmt->forStmt.end,   depth + 1);
            printStmt(stmt->forStmt.body,  depth + 1);
            break;

        case STMT_FOR_IN:
            indent(depth);
            printf("FOR_IN %.*s\n",
                   stmt->forInStmt.name.length,
                   stmt->forInStmt.name.start);
            printExpr(stmt->forInStmt.iterable, depth + 1);
            printStmt(stmt->forInStmt.body,     depth + 1);
            break;

        case STMT_BREAK:
            indent(depth);
            printf("BREAK\n");
            break;

        case STMT_CONTINUE:
            indent(depth);
            printf("CONTINUE\n");
            break;

        case STMT_FUNCTION_DEF:
            indent(depth);
            printf("FUNCTION_DEF %.*s(",
                   stmt->functionDef.name.length,
                   stmt->functionDef.name.start);
            for (int i = 0; i < stmt->functionDef.paramCount; i++) {
                if (i > 0) printf(", ");
                printf("%.*s", stmt->functionDef.params[i].length, stmt->functionDef.params[i].start);
            }
            printf(")\n");
            printStmt(stmt->functionDef.body, depth + 1);
            break;

        case STMT_RETURN:
            indent(depth);
            printf("SEND\n");
            if (stmt->returnStmt.value)
                printExpr(stmt->returnStmt.value, depth + 1);
            break;

        case STMT_EXPR:
            indent(depth);
            printf("EXPR_STMT\n");
            printExpr(stmt->exprStmt.expression, depth + 1);
            break;

        case STMT_INDEX_ASSIGN:
            indent(depth);
            printf("INDEX_ASSIGN\n");
            printExpr(stmt->indexAssignStmt.object, depth + 1);
            printExpr(stmt->indexAssignStmt.index,  depth + 1);
            printExpr(stmt->indexAssignStmt.value,  depth + 1);
            break;

        case STMT_USE:
            indent(depth);
            printf("USE %.*s as %.*s\n",
                   stmt->useStmt.name.length, stmt->useStmt.name.start,
                   stmt->useStmt.alias.length, stmt->useStmt.alias.start);
            break;
    }
}

void printProgram(Program* program) {
    for (int i = 0; i < program->count; i++) {
        printStmt(program->statements[i], 0);
        printf("\n");
    }
}
