#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "error.h"

// Global error state — one error at a time (print-and-halt model)
static NovaError g_error;
static int       g_hasError = 0;

static const char* phaseName(NovaErrorType type) {
    switch (type) {
        case ERR_LEXER:                   return "LexerException";
        case ERR_PARSE:                   return "ParseException";
        case ERR_UNDEFINED_VARIABLE:      return "UndefinedVariableException";
        case ERR_UNDECLARED_VARIABLE:     return "UndeclaredVariableException";
        case ERR_UNDEFINED_FUNCTION:      return "UndefinedFunctionException";
        case ERR_INDEX_OUT_OF_BOUNDS:     return "NovaIndexOutOfBoundsException";
        case ERR_RETURN_OUTSIDE_FUNCTION: return "ReturnOutsideFunctionException";
        case ERR_ARGUMENT:                return "ArgumentException";
        case ERR_TYPE:                    return "TypeError";
        case ERR_DIVISION_BY_ZERO:        return "ArithmeticException";
        case SIGNAL_CONTINUE:             return "ContinueSignal";
        case SIGNAL_BREAK:                return "BreakSignal";
        default:                          return "NovaException";
    }
}

void novaError(NovaErrorType type, int line, const char* fmt, ...) {
    g_error.type = type;
    g_error.line = line;

    va_list args;
    va_start(args, fmt);
    vsnprintf(g_error.message, sizeof(g_error.message), fmt, args);
    va_end(args);

    g_hasError = 1;

    // Print to stderr in a consistent format:
    //   [ExceptionName] at line N: message
    if (line >= 0) {
        fprintf(stderr,
            "\n[%s] at line %d: %s\n",
            phaseName(type),
            line,
            g_error.message
        );
    } else {
        fprintf(stderr,
            "\n[%s]: %s\n",
            phaseName(type),
            g_error.message
        );
    }
}

int novaHasError(void) {
    return g_hasError;
}

void novaClearError(void) {
    g_hasError       = 0;
    g_error.type     = ERR_PARSE;
    g_error.line     = -1;
    g_error.message[0] = '\0';
}

const NovaError* novaGetError(void) {
    return &g_error;
}
