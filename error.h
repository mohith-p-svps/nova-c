#ifndef ERROR_H
#define ERROR_H

// Full exception hierarchy from NovaLang Java version, mapped to C enum.
// NovaException (abstract)
//   LexerException
//   ParseException
//   NovaRuntimeException
//     UndefinedVariableException
//     UndeclaredVariableException
//     UndefinedFunctionException
//     NovaIndexOutOfBoundsException
//     ReturnOutsideFunctionException
//     ArgumentException
//     TypeError
//     ContinueException  (control flow signal, not a true error)
//     BreakException     (control flow signal, not a true error)

typedef enum {
    // Lexer phase
    ERR_LEXER,                      // unexpected character, malformed token

    // Parser phase
    ERR_PARSE,                      // unexpected token, missing expected token

    // Runtime phase
    ERR_UNDEFINED_VARIABLE,         // variable used but never assigned
    ERR_UNDECLARED_VARIABLE,        // variable used before let declaration
    ERR_UNDEFINED_FUNCTION,         // call to unknown function
    ERR_INDEX_OUT_OF_BOUNDS,        // array/string index out of range
    ERR_RETURN_OUTSIDE_FUNCTION,    // return statement outside any function
    ERR_ARGUMENT,                   // wrong number or type of arguments
    ERR_TYPE,                       // type mismatch in operation
    ERR_DIVISION_BY_ZERO,           // integer divide by zero

    // Control flow signals (not printed as errors — used internally)
    SIGNAL_CONTINUE,
    SIGNAL_BREAK,

} NovaErrorType;

typedef struct {
    NovaErrorType type;
    int           line;             // source line number, -1 if unknown
    char          message[512];     // formatted message
} NovaError;

// Report an error: prints a formatted message to stderr and sets
// the global error state. Callers should halt execution after this.
void novaError(NovaErrorType type, int line, const char* fmt, ...);

// Returns 1 if an error has been raised since last clear.
int  novaHasError(void);

// Clear the error state (used between phases if needed).
void novaClearError(void);

// Access the last raised error.
const NovaError* novaGetError(void);

#endif
