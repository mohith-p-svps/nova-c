#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "compiler.h"
#include "error.h"
#include "natives/natives.h"
#include "package_loader.h"
#include "execution_policy.h"

// --- Module aliases (from 'use' statements) --------------------------
//
// A single flat table mapping each alias in scope to what it actually
// refers to: either a built-in native module (see natives/natives.h) or a
// loaded package's own FunctionTable (see package_loader.h). Populated
// entirely by registerUseStatement, called for every top-level 'use'
// statement in a dedicated pre-pass (compileProgram's "Pass 0") before
// anything else compiles — this is what lets a function body reference
// a module/package regardless of where its 'use' appears textually in
// the file, the same reason function signatures get their own early
// pass.
typedef struct {
    const char* aliasStart;
    int         aliasLength;
    int         isBuiltin;          // 1 = built-in module, 0 = package
    int         builtinModuleIndex; // valid if isBuiltin
    FunctionTable* packageTable;    // valid if !isBuiltin
} ModuleAlias;

#define MAX_MODULE_ALIASES 32
static ModuleAlias moduleAliases[MAX_MODULE_ALIASES];
static int moduleAliasCount = 0;

static ModuleAlias* findModuleAlias(Token alias) {
    for (int i = 0; i < moduleAliasCount; i++) {
        if (moduleAliases[i].aliasLength == alias.length &&
            memcmp(moduleAliases[i].aliasStart, alias.start, alias.length) == 0)
            return &moduleAliases[i];
    }
    return NULL;
}

// Processes one top-level 'use' statement: tries a built-in module
// first, then falls back to loading a local/installed .nova package.
// Errors (unknown module/package) are raised via novaError() and simply
// leave no alias registered — a later module.function(...) referencing
// it then fails its own, separate "unknown module" check.
static void registerUseStatement(Stmt* stmt) {
    if (moduleAliasCount >= MAX_MODULE_ALIASES) {
        novaError(ERR_PARSE, -1, "Too many 'use' statements (max %d)", MAX_MODULE_ALIASES);
        return;
    }

    Token name  = stmt->useStmt.name;
    Token alias = stmt->useStmt.alias;

    int builtinIdx = findNativeModule(name.start, name.length);
    if (builtinIdx != -1) {
        // file/net/os are the only built-in modules with real-world
        // side effects (filesystem, network, process/system access) —
        // see execution_policy.h for why only these three (plus
        // external packages) are ever gated by safe mode.
        if (!capabilityAllowed(name.start, name.length)) {
            char modName[32];
            int mlen = name.length < 31 ? name.length : 31;
            memcpy(modName, name.start, mlen);
            modName[mlen] = '\0';
            novaError(ERR_PARSE, name.line,
                      "Module '%s' is blocked in safe mode (run with --allow=%s to permit it)",
                      modName, modName);
            return;
        }
        ModuleAlias* a = &moduleAliases[moduleAliasCount++];
        a->aliasStart = alias.start;
        a->aliasLength = alias.length;
        a->isBuiltin = 1;
        a->builtinModuleIndex = builtinIdx;
        return;
    }

    if (!capabilityAllowed("packages", 8)) {
        novaError(ERR_PARSE, name.line,
                  "External packages are blocked in safe mode (run with --allow=packages to permit them)");
        return;
    }

    char nameBuf[128];
    int len = name.length < 127 ? name.length : 127;
    memcpy(nameBuf, name.start, len);
    nameBuf[len] = '\0';

    FunctionTable* table = loadPackage(nameBuf);
    if (!table) return; // loadPackage already raised a novaError

    ModuleAlias* a = &moduleAliases[moduleAliasCount++];
    a->aliasStart = alias.start;
    a->aliasLength = alias.length;
    a->isBuiltin = 0;
    a->packageTable = table;
}

static void compileExpr(Expr* expr, Chunk* chunk);
static void compileStmt(Stmt* stmt, Chunk* chunk);
static void compileBlock(Stmt* block, Chunk* chunk);

static void emitByte(Chunk* chunk, uint8_t byte, int line) {
    writeChunk(chunk, byte, line);
}

// --- Stack depth tracking ----------------------------------------------
//
// Tracks how many values are sitting on the VM's operand stack at the
// current point in compilation. This exists so `break`/`continue` can
// correctly unwind any values left behind by enclosing constructs (most
// notably an `if`'s condition, which is normally cleaned up by an
// OP_POP that a jump out of the `if` would otherwise skip) before
// jumping to the loop's exit/condition-recheck point. Without this,
// every break/continue taken through a nested `if` permanently strands
// one value on the stack per `if` layer it escapes — see the
// STMT_BREAK/STMT_CONTINUE compilation below.
//
// Every opcode that changes the operand stack's height must adjust this
// counter at the same call site that emits it. `adjustDepth` is the
// single place that happens, so anything that pushes/pops should route
// through it (directly or via the small emit* helpers below) rather
// than calling emitByte() directly for stack-affecting opcodes.
static int stackDepth = 0;

static void adjustDepth(int delta) {
    stackDepth += delta;
}

// Verifies the operand stack invariant: every well-formed chunk must
// leave the operand stack exactly as it found it (empty) once control
// falls off the end of its statements and reaches the implicit
// return/OP_RETURN appended after it. This is always-on (not an
// assert(), which release builds commonly compile out) because it's the
// single cheapest, most direct way to catch a future opcode addition
// that pushes or pops without updating stackDepth at its emission site
// — exactly the kind of mistake that's otherwise silent until it
// corrupts an unrelated value or overflows the stack at runtime, far
// from wherever the real mistake was made. `context` names the chunk
// being checked (e.g. "top-level code" or "function 'foo'") so the
// error points at something actionable.
static void checkStackBalanced(const char* context) {
    if (stackDepth != 0) {
        novaError(ERR_PARSE, -1,
                  "[internal compiler error] operand stack imbalance in %s: "
                  "expected depth 0 at end of compilation, got %d. "
                  "This means some opcode emission added or removed a value "
                  "without updating stackDepth (see compiler.c's emitOp/"
                  "emitOp1/adjustDepth) — check any new or recently changed "
                  "expression/statement compilation for a missing depth update.",
                  context, stackDepth);
    }
}

// Emits an opcode with no operand bytes that changes the stack height
// by `delta` (e.g. -1 for OP_POP/OP_PRINT, -1 for any binary op that
// pops 2 and pushes 1, 0 for OP_NEGATE/OP_NOT which pop 1 push 1, etc).
static void emitOp(Chunk* chunk, uint8_t op, int delta, int line) {
    emitByte(chunk, op, line);
    adjustDepth(delta);
}

// Emits an opcode followed by one operand byte, changing the stack
// height by `delta` (e.g. OP_CONSTANT [index] is +1, OP_SET_LOCAL [slot]
// is -1).
static void emitOp1(Chunk* chunk, uint8_t op, uint8_t operand, int delta, int line) {
    emitByte(chunk, op, line);
    emitByte(chunk, operand, line);
    adjustDepth(delta);
}

static int identifierConstant(Chunk* chunk, Token name) {
    return addName(chunk, name.start, name.length);
}

// --- Scoping ----------------------------------------------------------
//
// Every block (`{ ... }` — if/elif/else bodies, while/for/for-in bodies,
// function bodies) introduces its own scope: names declared inside are
// invisible once the block ends, and may shadow a same-named variable
// from an enclosing scope without disturbing it. Two different storage
// strategies back this, depending on whether we're inside a function:
//
//   - Inside a function: names live in that function's flat local-slot
//     table (LocalVar/FuncCompiler, below), same mechanism as always —
//     what's new is that each local now remembers the scope depth it
//     was declared at, so ending a scope can strip exactly the locals
//     that belong to it (resolveLocal simply won't find them anymore)
//     without disturbing anything from an enclosing scope.
//
//   - At the top level (current == NULL): there's no CallFrame to hold
//     local slots in — top-level code has always used the VM's global
//     table for everything. To get real block scoping here without
//     changing that storage, a `let` inside a nested top-level block
//     registers its GLOBAL under a compiler-mangled name (e.g. `x$4`)
//     instead of its literal source name, and a parallel TopScope chain
//     (mirroring the function-local scope chain) remembers which
//     mangled name each source name currently refers to. A `let`
//     directly at the true top level (never inside any block) keeps
//     using its literal, unmangled name — preserving predictable
//     disassembly and behavior for ordinary top-level programs.
//
// Function bodies are never nested inside another scope lexically — a
// `fn` can only be declared at the top level — so a function's local
// scope chain always starts completely fresh, with no parent. That,
// combined with getVariable/setVariable refusing to fall back to a
// global from inside a function (see below), is what makes a function
// its own fully isolated island: it cannot see top-level variables at
// all, only its own parameters and locals.

typedef struct {
    const char* start;
    int         length;
    int         depth;
} LocalVar;

typedef struct FuncCompiler {
    LocalVar locals[MAX_LOCALS];
    int      localCount;
    int      scopeDepth;
    struct FuncCompiler* enclosing;
} FuncCompiler;

static FuncCompiler* current = NULL; // NULL while compiling top-level code

// Set once at the top of compileProgram, read by EXPR_VARIABLE/EXPR_CALL
// to decide whether a bare identifier refers to a declared function
// (making it eligible for first-class use) — see isFunctionReference.
static FunctionTable* gFunctions = NULL;

static int resolveLocal(FuncCompiler* fc, Token name) {
    if (!fc) return -1;
    for (int i = fc->localCount - 1; i >= 0; i--) {
        LocalVar* l = &fc->locals[i];
        if (l->length == name.length && memcmp(l->start, name.start, name.length) == 0)
            return i;
    }
    return -1;
}

// Returns the index of a top-level function named `name`, or -1. Linear
// scan is fine here — function counts are small (MAX_FUNCTIONS = 64) and
// this only runs at compile time, never per-instruction at runtime.
static int findFunctionIndexByName(FunctionTable* functions, Token name) {
    if (!functions) return -1;
    for (int i = 0; i < functions->count; i++) {
        NovaFunction* f = &functions->functions[i];
        int len = (int)strlen(f->name);
        if (len == name.length && strncmp(f->name, name.start, name.length) == 0)
            return i;
    }
    return -1;
}

// A bare identifier compiles as a reference to a FUNCTION (rather than a
// variable read) when it isn't shadowed by a local and a function with
// that exact name exists. Locals always win — a parameter or `let`
// inside a function body shadows a same-named top-level function, the
// same way it would shadow a global variable. This one rule is shared by
// EXPR_VARIABLE (bare `square` used as a value, e.g. `let f = square`)
// and EXPR_CALL (deciding whether `name(...)` is a direct call to a
// known function vs. a call through whatever value a variable holds).
static int isFunctionReference(Token name) {
    if (current && resolveLocal(current, name) != -1) return 0; // local shadows
    return findFunctionIndexByName(gFunctions, name) != -1;
}

static int addLocal(FuncCompiler* fc, Token name) {
    if (fc->localCount >= MAX_LOCALS) {
        novaError(ERR_PARSE, name.line, "Too many local variables in one function (max %d)", MAX_LOCALS);
        return fc->localCount - 1;
    }
    LocalVar* l = &fc->locals[fc->localCount];
    l->start  = name.start;
    l->length = name.length;
    l->depth  = fc->scopeDepth;
    return fc->localCount++;
}

// --- Top-level scope chain (see the big comment above) ---------------

#define MAX_SCOPE_VARS 64

typedef struct {
    const char* start;
    int         length;
    char*       runtimeName; // the actual name registered as a global (mangled if depth > 0)
} TopScopeVar;

typedef struct TopScope {
    TopScopeVar vars[MAX_SCOPE_VARS];
    int         count;
    int         depth; // 0 = true top level (unmangled names), >0 = a nested block
    struct TopScope* parent;
} TopScope;

static TopScope* currentTopScope = NULL;

static char* dupTokenText(Token name) {
    char* buf = malloc(name.length + 1);
    memcpy(buf, name.start, name.length);
    buf[name.length] = '\0';
    return buf;
}

// Begins a new scope — a function-local block scope if we're inside a
// function, or a top-level block scope otherwise. Every block calls
// this at its start (see compileBlock) and endScope at its end; loop
// constructs additionally wrap their own hidden bookkeeping variables
// ($end, $dir, etc.) in their own scope layer, one level out from the
// body's own, so that bookkeeping is cleaned up as soon as the loop
// itself ends rather than lingering until some further-enclosing block
// closes.
static void beginScope(void) {
    if (current) {
        current->scopeDepth++;
        return;
    }
    TopScope* s = malloc(sizeof(TopScope));
    s->count  = 0;
    s->depth  = currentTopScope ? currentTopScope->depth + 1 : 0;
    s->parent = currentTopScope;
    currentTopScope = s;
}

static void endScope(void) {
    if (current) {
        current->scopeDepth--;
        while (current->localCount > 0 &&
               current->locals[current->localCount - 1].depth > current->scopeDepth)
            current->localCount--;
        return;
    }
    TopScope* s = currentTopScope;
    currentTopScope = s->parent;
    // The mangled name strings this scope owns are intentionally
    // leaked, not freed — they're embedded by reference into already-
    // emitted bytecode (chunk->names[]) for the rest of the program's
    // life, matching this compiler's existing convention of never
    // bothering to free its own working memory (see makeHiddenToken).
    free(s);
}

// Searches the top-level scope chain (innermost first) for `name`,
// returning the mangled-or-not runtime name it currently refers to, or
// NULL if it isn't declared in any currently-open top-level scope (in
// which case the caller falls back to the literal source name, exactly
// as before this feature existed — preserving the existing runtime
// "undeclared/undefined" error behavior for names never let-bound
// anywhere visible).
static char* lookupTopScopeVar(Token name) {
    for (TopScope* s = currentTopScope; s; s = s->parent) {
        for (int i = s->count - 1; i >= 0; i--) {
            if (s->vars[i].length == name.length &&
                memcmp(s->vars[i].start, name.start, name.length) == 0)
                return s->vars[i].runtimeName;
        }
    }
    return NULL;
}

// Fabricates an identifier not present in the source, for the hidden
// bookkeeping variables a `for` loop needs (its end-value and direction).
// The leading '$' can never start a real Nova identifier, so these never
// collide with user-declared names. The buffer is intentionally never
// freed — it must outlive compilation, and the compiler doesn't bother
// freeing any of its working memory (matching the rest of this codebase).
static Token hiddenToken(const char* tag) {
    static int counter = 0;
    char* buf = malloc(32);
    snprintf(buf, 32, "$%s%d", tag, counter++);
    Token t;
    t.type          = TOKEN_IDENTIFIER;
    t.start         = buf;
    t.length        = (int)strlen(buf);
    t.line          = -1;
    t.value.integer = 0;
    return t;
}

// declareVariable consumes the value on top of the stack and binds it to
// `name` — used by `let`, function parameters, and for-loop bookkeeping.
// declareVariable consumes the value on top of the stack and binds it to
// `name` — used by `let`, function parameters, and for-loop bookkeeping.
static void declareVariable(Chunk* chunk, Token name, int line) {
    if (current) {
        int slot = addLocal(current, name);
        emitOp1(chunk, OP_SET_LOCAL, (uint8_t)slot, -1, line);
        return;
    }

    // Top-level: a `let` directly at the true top level (depth 0, or no
    // scope open at all — shouldn't normally happen since compileProgram
    // opens the root scope up front, but handled defensively) keeps its
    // literal name; a `let` inside any nested top-level block gets a
    // mangled name instead, so it can shadow an outer variable of the
    // same name without disturbing it — see lookupTopScopeVar.
    char* runtimeName;
    if (!currentTopScope || currentTopScope->depth == 0) {
        runtimeName = dupTokenText(name);
    } else {
        static int mangleCounter = 0;
        char* buf = malloc(name.length + 24);
        snprintf(buf, name.length + 24, "%.*s$%d", name.length, name.start, mangleCounter++);
        runtimeName = buf;
    }

    if (currentTopScope && currentTopScope->count < MAX_SCOPE_VARS) {
        TopScopeVar* v = &currentTopScope->vars[currentTopScope->count++];
        v->start = name.start;
        v->length = name.length;
        v->runtimeName = runtimeName;
    }

    int slot = addName(chunk, runtimeName, (int)strlen(runtimeName));
    emitOp1(chunk, OP_DEFINE_GLOBAL, (uint8_t)slot, -1, line);
}

static void getVariable(Chunk* chunk, Token name, int line) {
    if (current) {
        int slot = resolveLocal(current, name);
        if (slot != -1) {
            emitOp1(chunk, OP_GET_LOCAL, (uint8_t)slot, +1, line);
        } else {
            // Functions are fully isolated islands: they can see their
            // own parameters and locals, and nothing from any outer
            // scope. This is a compile-time error, not a runtime one,
            // because the compiler already knows for certain this name
            // will never resolve — there's no "maybe it exists by the
            // time this runs" the way an unresolved top-level global
            // legitimately has. A placeholder null is still pushed (the
            // program will never actually run, since novaHasError() now
            // prevents that) purely so stackDepth accounting — and the
            // checkStackBalanced self-check built on top of it — stays
            // consistent for the rest of compilation, rather than
            // reporting a second, confusing "stack imbalance" error on
            // top of this real one.
            novaError(ERR_PARSE, line,
                      "Variable '%.*s' is not in this function's scope",
                      name.length, name.start);
            int nullIdx = addConstant(chunk, makeNull());
            emitOp1(chunk, OP_CONSTANT, (uint8_t)nullIdx, +1, line);
        }
        return;
    }

    char* runtimeName = lookupTopScopeVar(name);
    int slot = runtimeName ? addName(chunk, runtimeName, (int)strlen(runtimeName))
                           : identifierConstant(chunk, name);
    emitOp1(chunk, OP_GET_GLOBAL, (uint8_t)slot, +1, line);
}

static void setVariable(Chunk* chunk, Token name, int line) {
    if (current) {
        int slot = resolveLocal(current, name);
        if (slot != -1) {
            emitOp1(chunk, OP_SET_LOCAL, (uint8_t)slot, -1, line);
        } else {
            // See getVariable's identical situation above — the caller
            // already pushed the value it expected this function to
            // consume, so a placeholder pop keeps stackDepth consistent
            // even though this program will never actually run.
            novaError(ERR_PARSE, line,
                      "Variable '%.*s' is not in this function's scope",
                      name.length, name.start);
            emitOp(chunk, OP_POP, -1, line);
        }
        return;
    }

    char* runtimeName = lookupTopScopeVar(name);
    int slot = runtimeName ? addName(chunk, runtimeName, (int)strlen(runtimeName))
                           : identifierConstant(chunk, name);
    emitOp1(chunk, OP_SET_GLOBAL, (uint8_t)slot, -1, line);
}

static void emitNullConstant(Chunk* chunk, int line) {
    int idx = addConstant(chunk, makeNull());
    emitOp1(chunk, OP_CONSTANT, (uint8_t)idx, +1, line);
}

// --- Jump / loop backpatching helpers -------------------------------------

static int emitJump(Chunk* chunk, uint8_t instruction, int line) {
    emitByte(chunk, instruction, line);
    emitByte(chunk, 0xff, line);
    emitByte(chunk, 0xff, line);
    return chunk->count - 3;
}

static void patchJumpTo(Chunk* chunk, int offset, int target) {
    int jump = target - offset - 3;
    chunk->code[offset + 1] = (uint8_t)((jump >> 8) & 0xff);
    chunk->code[offset + 2] = (uint8_t)(jump & 0xff);
}

static void patchJump(Chunk* chunk, int offset) {
    patchJumpTo(chunk, offset, chunk->count);
}

static void emitLoop(Chunk* chunk, int loopStart, int line) {
    emitByte(chunk, OP_LOOP, line);
    int jump = chunk->count + 2 - loopStart;
    emitByte(chunk, (uint8_t)((jump >> 8) & 0xff), line);
    emitByte(chunk, (uint8_t)(jump & 0xff), line);
}

// Patches a previously-emitted 3-byte jump placeholder (from emitJump,
// opcode unspecified at that point — STMT_CONTINUE always emits OP_LOOP
// as a placeholder, see below) so it correctly reaches `target`,
// REGARDLESS of whether that target lies behind or ahead of the jump
// site. This exists specifically for `continue`, whose target direction
// differs by loop kind: in a `while`, continue jumps backward to the
// condition re-check; in a `for`, it jumps forward to the increment
// step (which then itself loops backward separately). A single opcode
// can't express both directions (OP_JUMP only ever adds its offset,
// OP_LOOP only ever subtracts — see their handlers in vm.c), so this
// function inspects the actual offset-to-target distance and rewrites
// the placeholder's opcode byte to whichever of OP_JUMP/OP_LOOP is
// correct for that distance, then fills in the matching unsigned
// magnitude. This is the one place in the compiler that decides jump
// direction after the fact rather than knowing it upfront — every other
// jump site (if/while/for's own structural jumps) always knows its
// direction at the point it's emitted.
static void patchContinueTo(Chunk* chunk, int offset, int target) {
    if (target <= offset) {
        // Backward (or self-targeting, degenerate) jump: OP_LOOP.
        chunk->code[offset] = OP_LOOP;
        int jump = offset + 3 - target;
        chunk->code[offset + 1] = (uint8_t)((jump >> 8) & 0xff);
        chunk->code[offset + 2] = (uint8_t)(jump & 0xff);
    } else {
        // Forward jump: OP_JUMP.
        chunk->code[offset] = OP_JUMP;
        int jump = target - offset - 3;
        chunk->code[offset + 1] = (uint8_t)((jump >> 8) & 0xff);
        chunk->code[offset + 2] = (uint8_t)(jump & 0xff);
    }
}

// --- Loop context, for break/continue -------------------------------------

#define MAX_LOOP_JUMPS 64

typedef struct LoopContext {
    int breakJumps[MAX_LOOP_JUMPS];
    int breakCount;
    int continueJumps[MAX_LOOP_JUMPS]; // OP_LOOP sites, patched once the continue target is known
    int continueCount;
    int loopDepth;     // stackDepth at the moment this loop's body starts compiling
    struct LoopContext* enclosing;
} LoopContext;

static LoopContext* currentLoop = NULL;

static void pushLoop(LoopContext* ctx) {
    ctx->breakCount    = 0;
    ctx->continueCount = 0;
    ctx->loopDepth      = stackDepth;
    ctx->enclosing      = currentLoop;
    currentLoop         = ctx;
}

static void popLoop(void) {
    currentLoop = currentLoop->enclosing;
}

// --- Expressions -----------------------------------------------------------

static void compileExpr(Expr* expr, Chunk* chunk) {
    switch (expr->type) {

    case EXPR_LITERAL: {
        int index = addConstant(chunk, copyValue(expr->literal.value));
        emitOp1(chunk, OP_CONSTANT, (uint8_t)index, +1, -1);
        break;
    }

    case EXPR_BINARY: {
        compileExpr(expr->binary.left,  chunk);
        compileExpr(expr->binary.right, chunk);
        int line = expr->binary.op.line;
        // Every binary op here pops 2 operands and pushes 1 result: net -1.
        switch (expr->binary.op.type) {
            case TOKEN_PLUS:          emitOp(chunk, OP_ADD,           -1, line); break;
            case TOKEN_MINUS:         emitOp(chunk, OP_SUBTRACT,      -1, line); break;
            case TOKEN_STAR:          emitOp(chunk, OP_MULTIPLY,      -1, line); break;
            case TOKEN_SLASH:         emitOp(chunk, OP_DIVIDE,        -1, line); break;
            case TOKEN_PERCENT:       emitOp(chunk, OP_MODULO,        -1, line); break;
            case TOKEN_EQUAL_EQUAL:   emitOp(chunk, OP_EQUAL,         -1, line); break;
            case TOKEN_BANG_EQUAL:    emitOp(chunk, OP_NOT_EQUAL,     -1, line); break;
            case TOKEN_LESS:          emitOp(chunk, OP_LESS,          -1, line); break;
            case TOKEN_GREATER:       emitOp(chunk, OP_GREATER,       -1, line); break;
            case TOKEN_LESS_EQUAL:    emitOp(chunk, OP_LESS_EQUAL,    -1, line); break;
            case TOKEN_GREATER_EQUAL: emitOp(chunk, OP_GREATER_EQUAL, -1, line); break;
            default: break;
        }
        break;
    }

    case EXPR_UNARY: {
        compileExpr(expr->unary.operand, chunk);
        int line = expr->unary.op.line;
        // Pops 1, pushes 1: net 0.
        switch (expr->unary.op.type) {
            case TOKEN_MINUS: emitOp(chunk, OP_NEGATE, 0, line); break;
            case TOKEN_NOT:   emitOp(chunk, OP_NOT,    0, line); break;
            default: break;
        }
        break;
    }

    case EXPR_LOGICAL: {
        int line = expr->logical.op.line;
        compileExpr(expr->logical.left, chunk);

        // Both branches: left is left on the stack as the OP_JUMP_IF_FALSE
        // operand (not popped on the short-circuit path), and explicitly
        // popped (-1) before compiling+pushing the right operand on the
        // path that falls through to it. Net effect either way: the
        // expression leaves exactly one value on the stack, same as any
        // other expression — stackDepth ends up +1 relative to entry.
        if (expr->logical.op.type == TOKEN_AND) {
            int endJump = emitJump(chunk, OP_JUMP_IF_FALSE, line);
            emitOp(chunk, OP_POP, -1, line);
            compileExpr(expr->logical.right, chunk);
            patchJump(chunk, endJump);
        } else { // TOKEN_OR
            int elseJump = emitJump(chunk, OP_JUMP_IF_FALSE, line);
            int endJump  = emitJump(chunk, OP_JUMP, line);
            patchJump(chunk, elseJump);
            emitOp(chunk, OP_POP, -1, line);
            compileExpr(expr->logical.right, chunk);
            patchJump(chunk, endJump);
        }
        break;
    }

    case EXPR_VARIABLE:
        if (isFunctionReference(expr->variable.name)) {
            int idx = findFunctionIndexByName(gFunctions, expr->variable.name);
            int constIdx = addConstant(chunk, makeFunction(&gFunctions->functions[idx]));
            emitOp1(chunk, OP_CONSTANT, (uint8_t)constIdx, +1, expr->variable.name.line);
        } else {
            getVariable(chunk, expr->variable.name, expr->variable.name.line);
        }
        break;

    case EXPR_TO_STRING:
        compileExpr(expr->toString.operand, chunk);
        emitOp(chunk, OP_TO_STRING, 0, -1); // pops 1, pushes 1: net 0
        break;

    case EXPR_CALL: {
        // `len` is a tiny built-in intrinsic rather than a real function
        // call — there's no module/stdlib system yet (that's Phase 6),
        // but arrays/maps are hard to use at all without some way to
        // find out how big they are.
        if (expr->call.argCount == 1 &&
            expr->call.name.length == 3 &&
            memcmp(expr->call.name.start, "len", 3) == 0) {
            compileExpr(expr->call.args[0], chunk);
            emitOp(chunk, OP_LEN, 0, expr->call.name.line); // pops 1, pushes 1
            break;
        }

        if (isFunctionReference(expr->call.name)) {
            // Direct call to a known, unshadowed top-level function —
            // exactly the original fast path: the callee is baked into
            // the bytecode by name, resolved at runtime via the name
            // table rather than read off the stack.
            for (int i = 0; i < expr->call.argCount; i++)
                compileExpr(expr->call.args[i], chunk);
            int nameSlot = identifierConstant(chunk, expr->call.name);
            emitByte(chunk, OP_CALL, expr->call.name.line);
            emitByte(chunk, (uint8_t)nameSlot, expr->call.name.line);
            emitByte(chunk, (uint8_t)expr->call.argCount, expr->call.name.line);
            // Pops argCount arguments, pushes 1 return value.
            adjustDepth(-(expr->call.argCount) + 1);
        } else {
            // Calling THROUGH a variable — a local parameter or global
            // holding a function value, e.g. `fn apply(op) { send op(5) }`
            // or `let f = square; f(5)`. The callee isn't known until
            // runtime, so it travels on the stack (pushed first, then
            // its arguments) rather than being baked into the bytecode.
            getVariable(chunk, expr->call.name, expr->call.name.line);
            for (int i = 0; i < expr->call.argCount; i++)
                compileExpr(expr->call.args[i], chunk);
            emitByte(chunk, OP_CALL_VALUE, expr->call.name.line);
            emitByte(chunk, (uint8_t)expr->call.argCount, expr->call.name.line);
            // Pops the function value + argCount arguments, pushes 1 result.
            adjustDepth(-(expr->call.argCount + 1) + 1);
        }
        break;
    }

    case EXPR_MODULE_CALL: {
        Token aliasTok = expr->moduleCall.moduleAlias;
        Token funcTok  = expr->moduleCall.funcName;
        int line = funcTok.line;
        int argCount = expr->moduleCall.argCount;

        ModuleAlias* alias = findModuleAlias(aliasTok);
        if (!alias) {
            novaError(ERR_PARSE, line, "Unknown module or package '%.*s' — did you forget a 'use' statement?",
                      aliasTok.length, aliasTok.start);
            int idx = addConstant(chunk, makeNull());
            emitOp1(chunk, OP_CONSTANT, (uint8_t)idx, +1, line); // placeholder so stackDepth stays consistent
            break;
        }

        if (alias->isBuiltin) {
            int arity;
            int globalIdx = findNativeFunction(alias->builtinModuleIndex, funcTok.start, funcTok.length, &arity);
            if (globalIdx == -1) {
                novaError(ERR_UNDEFINED_FUNCTION, line, "%.*s has no function '%.*s'",
                          aliasTok.length, aliasTok.start, funcTok.length, funcTok.start);
                int idx = addConstant(chunk, makeNull());
                emitOp1(chunk, OP_CONSTANT, (uint8_t)idx, +1, line);
                break;
            }
            // arity == -1 marks a native as variable-arity (e.g.
            // json.stringify's optional indent argument) — the native
            // function itself validates argCount in that case, rather
            // than the compiler enforcing one exact count.
            if (arity != -1 && arity != argCount) {
                novaError(ERR_ARGUMENT, line, "%.*s.%.*s expects %d argument(s) but got %d",
                          aliasTok.length, aliasTok.start, funcTok.length, funcTok.start, arity, argCount);
                int idx = addConstant(chunk, makeNull());
                emitOp1(chunk, OP_CONSTANT, (uint8_t)idx, +1, line);
                break;
            }
            for (int i = 0; i < argCount; i++)
                compileExpr(expr->moduleCall.args[i], chunk);
            emitByte(chunk, OP_CALL_NATIVE, line);
            emitByte(chunk, (uint8_t)((globalIdx >> 8) & 0xff), line); // 2-byte index — room for many modules' worth of functions
            emitByte(chunk, (uint8_t)(globalIdx & 0xff), line);
            emitByte(chunk, (uint8_t)argCount, line);
            adjustDepth(-argCount + 1);
        } else {
            FunctionTable* table = alias->packageTable;
            int funcIndex = -1, arity = 0;
            for (int i = 0; i < table->count; i++) {
                NovaFunction* f = &table->functions[i];
                if ((int)strlen(f->name) == funcTok.length && strncmp(f->name, funcTok.start, funcTok.length) == 0) {
                    funcIndex = i; arity = f->arity; break;
                }
            }
            if (funcIndex == -1) {
                novaError(ERR_UNDEFINED_FUNCTION, line, "Package '%.*s' has no function '%.*s'",
                          aliasTok.length, aliasTok.start, funcTok.length, funcTok.start);
                int idx = addConstant(chunk, makeNull());
                emitOp1(chunk, OP_CONSTANT, (uint8_t)idx, +1, line);
                break;
            }
            if (arity != argCount) {
                novaError(ERR_ARGUMENT, line, "%.*s.%.*s expects %d argument(s) but got %d",
                          aliasTok.length, aliasTok.start, funcTok.length, funcTok.start, arity, argCount);
                int idx = addConstant(chunk, makeNull());
                emitOp1(chunk, OP_CONSTANT, (uint8_t)idx, +1, line);
                break;
            }
            for (int i = 0; i < argCount; i++)
                compileExpr(expr->moduleCall.args[i], chunk);
            int tableSlot = addPackageTable(chunk, table);
            emitByte(chunk, OP_CALL_PACKAGE_FN, line);
            emitByte(chunk, (uint8_t)tableSlot, line);
            emitByte(chunk, (uint8_t)funcIndex, line);
            emitByte(chunk, (uint8_t)argCount, line);
            adjustDepth(-argCount + 1);
        }
        break;
    }

    case EXPR_ARRAY_LITERAL: {
        int count = expr->arrayLiteral.count;
        if (count > 255) {
            novaError(ERR_PARSE, -1, "Too many elements in array literal (max 255)");
            break;
        }
        for (int i = 0; i < count; i++)
            compileExpr(expr->arrayLiteral.elements[i], chunk);
        emitByte(chunk, OP_BUILD_ARRAY, -1);
        emitByte(chunk, (uint8_t)count, -1);
        // Pops `count` elements, pushes 1 array.
        adjustDepth(-count + 1);
        break;
    }

    case EXPR_MAP_LITERAL: {
        int count = expr->mapLiteral.count;
        if (count > 255) {
            novaError(ERR_PARSE, -1, "Too many entries in map literal (max 255)");
            break;
        }
        for (int i = 0; i < count; i++) {
            compileExpr(expr->mapLiteral.keys[i], chunk);
            compileExpr(expr->mapLiteral.values[i], chunk);
        }
        emitByte(chunk, OP_BUILD_MAP, -1);
        emitByte(chunk, (uint8_t)count, -1);
        // Pops 2*count (key,value pairs), pushes 1 map.
        adjustDepth(-(2 * count) + 1);
        break;
    }

    case EXPR_INDEX:
        compileExpr(expr->index.object, chunk);
        compileExpr(expr->index.index, chunk);
        emitOp(chunk, OP_INDEX_GET, -1, -1); // pops object+index (2), pushes 1
        break;

    default:
        break;
    }
}

// --- Statements -------------------------------------------------------------

static void compileBlock(Stmt* block, Chunk* chunk) {
    if (block->block.transparent) {
        // A compiler-internal grouping, not a real user block — its
        // statements belong to whatever scope is already active (see
        // the 'transparent' field's comment in ast.h).
        for (int i = 0; i < block->block.count; i++)
            compileStmt(block->block.statements[i], chunk);
        return;
    }
    beginScope();
    for (int i = 0; i < block->block.count; i++)
        compileStmt(block->block.statements[i], chunk);
    endScope();
}

static void compileStmt(Stmt* stmt, Chunk* chunk) {
    switch (stmt->type) {

    case STMT_LET:
        compileExpr(stmt->letStmt.initializer, chunk);
        declareVariable(chunk, stmt->letStmt.name, stmt->letStmt.name.line);
        break;

    case STMT_ASSIGN:
        compileExpr(stmt->assignStmt.value, chunk);
        setVariable(chunk, stmt->assignStmt.name, stmt->assignStmt.name.line);
        break;

    case STMT_PRINT: {
        compileExpr(stmt->printStmt.expression, chunk);
        if (stmt->printStmt.newlineExpr) {
            compileExpr(stmt->printStmt.newlineExpr, chunk);
        } else {
            int idx = addConstant(chunk, TRUE_VAL);
            emitOp1(chunk, OP_CONSTANT, (uint8_t)idx, +1, -1);
        }
        // OP_PRINT now pops two values (the value to print, then the
        // newline flag) and pushes nothing.
        emitOp(chunk, OP_PRINT, -2, -1);
        break;
    }

    case STMT_BLOCK:
        compileBlock(stmt, chunk);
        break;

    case STMT_IF: {
        compileExpr(stmt->ifStmt.condition, chunk);
        int thenJump = emitJump(chunk, OP_JUMP_IF_FALSE, -1);
        // This OP_POP discards the condition on the TRUE path (falls
        // through into the then-branch).
        emitOp(chunk, OP_POP, -1, -1);

        compileStmt(stmt->ifStmt.thenBranch, chunk);

        int elseJump = emitJump(chunk, OP_JUMP, -1);
        patchJump(chunk, thenJump);
        // This second OP_POP discards the SAME condition value, but on
        // the mutually-exclusive FALSE path — at runtime exactly one of
        // these two pops ever executes, never both. stackDepth is a
        // straight-line compile-time counter, though, so it sees both
        // emissions in sequence; counting this one with its own -1 would
        // double-decrement for something that only happens once at
        // runtime. We emit the real OP_POP byte (the false path still
        // needs it to execute) via emitByte directly, without touching
        // stackDepth again — the first OP_POP above already accounted
        // for "the condition gets popped by whichever path runs".
        emitByte(chunk, OP_POP, -1);

        if (stmt->ifStmt.elseBranch)
            compileStmt(stmt->ifStmt.elseBranch, chunk);

        patchJump(chunk, elseJump);
        break;
    }

    case STMT_WHILE: {
        int loopStart = chunk->count;
        compileExpr(stmt->whileStmt.condition, chunk);
        int exitJump = emitJump(chunk, OP_JUMP_IF_FALSE, -1);
        // Pops the condition on the fallthrough (true) path, into the body.
        emitOp(chunk, OP_POP, -1, -1);

        LoopContext ctx;
        pushLoop(&ctx);
        compileStmt(stmt->whileStmt.body, chunk);

        for (int i = 0; i < ctx.continueCount; i++)
            patchContinueTo(chunk, ctx.continueJumps[i], loopStart);

        emitLoop(chunk, loopStart, -1);
        patchJump(chunk, exitJump);
        // Pops the SAME condition value, but on the exit (false) path.
        // Although the condition is re-evaluated and re-pushed on every
        // loop iteration at runtime, the compiler only emits the
        // compileExpr(condition) call once in the bytecode listing, so
        // stackDepth only counted that one +1 once. This exit-path pop
        // is the alternate ending for that single static push (never
        // both this and the fallthrough pop for the same evaluation), so
        // it's emitted as a raw byte rather than double-counted via
        // emitOp — see the matching comment in STMT_IF above for the
        // identical situation.
        emitByte(chunk, OP_POP, -1);

        for (int i = 0; i < ctx.breakCount; i++)
            patchJumpTo(chunk, ctx.breakJumps[i], chunk->count);
        popLoop();
        break;
    }

    case STMT_FOR: {
        beginScope(); // wraps the loop variable and $end/$dir — see the matching STMT_FOR_IN comment
        compileExpr(stmt->forStmt.start, chunk);
        declareVariable(chunk, stmt->forStmt.name, stmt->forStmt.name.line);

        compileExpr(stmt->forStmt.end, chunk);
        Token endTok = hiddenToken("end");
        declareVariable(chunk, endTok, -1);

        // dir = (name <= end) ? 1 : -1
        //
        // This is a ternary, compiled the same shape as STMT_IF: the
        // OP_LESS_EQUAL pushes one bool (+1, counted once below), and
        // then exactly one of the two branches runs at runtime — never
        // both. So: the bool's pop is counted once (the first OP_POP,
        // via emitOp), the second OP_POP is its mutually-exclusive
        // alternate (raw emitByte, no double-count, same reasoning as
        // STMT_IF/STMT_WHILE above). Likewise only one of the two
        // OP_CONSTANT pushes (1 or -1) ever actually contributes a value
        // to the stack at runtime, so only the first is counted with
        // emitOp1; the second is a raw emission for the same logical
        // slot.
        getVariable(chunk, stmt->forStmt.name, -1);
        getVariable(chunk, endTok, -1);
        emitOp(chunk, OP_LESS_EQUAL, -1, -1);
        int elseDirJump = emitJump(chunk, OP_JUMP_IF_FALSE, -1);
        emitOp(chunk, OP_POP, -1, -1);
        int oneIndex = addConstant(chunk, makeInt64(1));
        emitOp1(chunk, OP_CONSTANT, (uint8_t)oneIndex, +1, -1);
        int afterDirJump = emitJump(chunk, OP_JUMP, -1);
        patchJump(chunk, elseDirJump);
        emitByte(chunk, OP_POP, -1);
        int negOneIndex = addConstant(chunk, makeInt64(-1));
        emitByte(chunk, OP_CONSTANT, -1); emitByte(chunk, (uint8_t)negOneIndex, -1);
        patchJump(chunk, afterDirJump);
        Token dirTok = hiddenToken("dir");
        declareVariable(chunk, dirTok, -1);

        int loopStart = chunk->count;
        // condition: dir * (end - name) > 0
        // Same shape as STMT_WHILE's condition: pushed once syntactically,
        // popped on whichever of the two paths (fallthrough/exit) actually
        // runs for a given evaluation — the fallthrough pop below is
        // counted; the exit pop further down is its alternate, not an
        // additional pop (see STMT_WHILE's comment for the full reasoning).
        getVariable(chunk, endTok, -1);
        getVariable(chunk, stmt->forStmt.name, -1);
        emitOp(chunk, OP_SUBTRACT, -1, -1);
        getVariable(chunk, dirTok, -1);
        emitOp(chunk, OP_MULTIPLY, -1, -1);
        int zeroIndex = addConstant(chunk, makeInt64(0));
        emitOp1(chunk, OP_CONSTANT, (uint8_t)zeroIndex, +1, -1);
        emitOp(chunk, OP_GREATER, -1, -1);
        int exitJump = emitJump(chunk, OP_JUMP_IF_FALSE, -1);
        emitOp(chunk, OP_POP, -1, -1);

        LoopContext ctx;
        pushLoop(&ctx);
        compileStmt(stmt->forStmt.body, chunk);

        int incrementStart = chunk->count;
        for (int i = 0; i < ctx.continueCount; i++)
            patchContinueTo(chunk, ctx.continueJumps[i], incrementStart);

        getVariable(chunk, stmt->forStmt.name, -1);
        getVariable(chunk, dirTok, -1);
        emitOp(chunk, OP_ADD, -1, -1);
        setVariable(chunk, stmt->forStmt.name, -1);

        emitLoop(chunk, loopStart, -1);
        patchJump(chunk, exitJump);
        emitByte(chunk, OP_POP, -1); // exit-path alternate of the fallthrough pop above

        for (int i = 0; i < ctx.breakCount; i++)
            patchJumpTo(chunk, ctx.breakJumps[i], chunk->count);
        popLoop();
        endScope();
        break;
    }

    // for x in collection { body }  desugars to an indexed loop:
    //   let $coll = collection
    //   let $idx = 0
    //   while $idx < len($coll) {
    //       let x = $coll[$idx]
    //       body
    //       $idx = $idx + 1   (this is where `continue` jumps to)
    //   }
    // Two hidden variables ($coll, $idx) play the same role STMT_FOR's
    // $end/$dir do — compile-time-only bookkeeping the user's program
    // never sees or can collide with.
    case STMT_FOR_IN: {
        beginScope(); // wraps $coll, $idx, and the loop variable
        compileExpr(stmt->forInStmt.iterable, chunk);
        Token collTok = hiddenToken("coll");
        declareVariable(chunk, collTok, -1);

        int zeroIdx = addConstant(chunk, makeInt64(0));
        emitOp1(chunk, OP_CONSTANT, (uint8_t)zeroIdx, +1, -1);
        Token idxTok = hiddenToken("idx");
        declareVariable(chunk, idxTok, -1);

        int loopStart = chunk->count;
        // condition: $idx < len($coll)
        getVariable(chunk, idxTok, -1);
        getVariable(chunk, collTok, -1);
        emitOp(chunk, OP_LEN, 0, -1); // pops 1 pushes 1, net 0
        emitOp(chunk, OP_LESS, -1, -1);
        int exitJump = emitJump(chunk, OP_JUMP_IF_FALSE, -1);
        emitOp(chunk, OP_POP, -1, -1);

        LoopContext ctx;
        pushLoop(&ctx);

        // let x = $coll[$idx]  — re-bound fresh each iteration, same as
        // STMT_FOR's loop variable, so the body always sees the current
        // element under the user-facing name.
        getVariable(chunk, collTok, -1);
        getVariable(chunk, idxTok, -1);
        emitOp(chunk, OP_INDEX_GET, -1, -1);
        declareVariable(chunk, stmt->forInStmt.name, -1);

        compileStmt(stmt->forInStmt.body, chunk);

        int incrementStart = chunk->count;
        for (int i = 0; i < ctx.continueCount; i++)
            patchContinueTo(chunk, ctx.continueJumps[i], incrementStart);

        getVariable(chunk, idxTok, -1);
        int oneIdx = addConstant(chunk, makeInt64(1));
        emitOp1(chunk, OP_CONSTANT, (uint8_t)oneIdx, +1, -1);
        emitOp(chunk, OP_ADD, -1, -1);
        setVariable(chunk, idxTok, -1);

        emitLoop(chunk, loopStart, -1);
        patchJump(chunk, exitJump);
        emitByte(chunk, OP_POP, -1); // exit-path alternate of the fallthrough pop above

        for (int i = 0; i < ctx.breakCount; i++)
            patchJumpTo(chunk, ctx.breakJumps[i], chunk->count);
        popLoop();
        endScope();
        break;
    }
    // would otherwise clean up those `if`s' condition values, silently
    // leaking one stack slot per skipped `if` layer on every iteration
    // that takes the early-exit path — eventually overflowing the VM's
    // operand stack. The pops emitted here are synthetic (compile-time
    // only): they don't reflect real control flow, just the cleanup that
    // jumping past `stackDepth - loopDepth` pending values requires.
    case STMT_BREAK:
        if (!currentLoop) {
            novaError(ERR_PARSE, -1, "'break' used outside of a loop");
            break;
        }
        for (int i = 0; i < stackDepth - currentLoop->loopDepth; i++)
            emitOp(chunk, OP_POP, -1, -1);
        if (currentLoop->breakCount < MAX_LOOP_JUMPS)
            currentLoop->breakJumps[currentLoop->breakCount++] = emitJump(chunk, OP_JUMP, -1);
        break;

    case STMT_CONTINUE:
        if (!currentLoop) {
            novaError(ERR_PARSE, -1, "'continue' used outside of a loop");
            break;
        }
        for (int i = 0; i < stackDepth - currentLoop->loopDepth; i++)
            emitOp(chunk, OP_POP, -1, -1);
        // continue's target direction depends on the loop kind (backward
        // to the condition recheck in a `while`, forward to the
        // increment step in a `for`) and isn't known until the relevant
        // loop construct finishes compiling its body. The opcode byte
        // emitted here is just a placeholder — patchContinueTo (called
        // from STMT_WHILE/STMT_FOR once the real target is known)
        // rewrites it to whichever of OP_JUMP/OP_LOOP actually matches
        // the resulting jump direction. See patchContinueTo's comment.
        if (currentLoop->continueCount < MAX_LOOP_JUMPS)
            currentLoop->continueJumps[currentLoop->continueCount++] = emitJump(chunk, OP_LOOP, -1);
        break;

    case STMT_RETURN:
        if (!current) {
            novaError(ERR_RETURN_OUTSIDE_FUNCTION, -1, "'send' used outside of a function");
            break;
        }
        if (stmt->returnStmt.value) compileExpr(stmt->returnStmt.value, chunk);
        else                        emitNullConstant(chunk, -1);
        // OP_FUNC_RETURN pops the return value and unwinds out of this
        // chunk entirely (interpret() returns), so there's no need to
        // unwind any enclosing if/loop depth here the way break/continue
        // must — execution leaves this chunk's bytecode altogether.
        emitOp(chunk, OP_FUNC_RETURN, -1, -1);
        break;

    case STMT_EXPR:
        compileExpr(stmt->exprStmt.expression, chunk);
        emitOp(chunk, OP_POP, -1, -1);
        break;

    case STMT_INDEX_ASSIGN:
        compileExpr(stmt->indexAssignStmt.object, chunk);
        compileExpr(stmt->indexAssignStmt.index, chunk);
        compileExpr(stmt->indexAssignStmt.value, chunk);
        emitOp(chunk, OP_INDEX_SET, -3, -1); // pops object+index+value (3), pushes nothing
        break;

    case STMT_FUNCTION_DEF:
        // Reaching this means a `fn` was nested inside a block (if/while/
        // for/another function) rather than declared at the top level,
        // which Nova (in this implementation) doesn't support.
        novaError(ERR_PARSE, -1, "Functions can only be defined at the top level");
        break;

    case STMT_USE:
        // Reaching this means 'use' appeared somewhere other than the
        // true top level (inside a block, loop, or function) — every
        // top-level 'use' is fully handled by registerUseStatement in
        // compileProgram's pre-pass, before compileStmt ever runs on
        // anything else, and is skipped when Pass 3 gets here normally.
        novaError(ERR_PARSE, -1, "'use' is only allowed at the top level of a file");
        break;

    default:
        break;
    }
}

// Registers a function's name/arity/empty-chunk slot in the table
// WITHOUT compiling its body. Must run for every top-level function
// before compileFunctionBody runs for ANY of them — see compileProgram.
// This split exists specifically so isFunctionReference (used by
// EXPR_VARIABLE/EXPR_CALL for first-class functions) can see every
// function by the time it compiles any body, preserving the
// "functions may call each other regardless of declaration order"
// guarantee that OP_CALL's runtime name lookup already gave direct
// calls — without this split, a function referenced only as a VALUE
// (not a direct call) before its own definition would wrongly compile
// as an undefined global instead of a function reference.
static NovaFunction* registerFunctionSignature(Stmt* stmt, FunctionTable* functions) {
    if (functions->count >= MAX_FUNCTIONS) {
        novaError(ERR_PARSE, stmt->functionDef.name.line,
                  "Too many function definitions (max %d)", MAX_FUNCTIONS);
        return NULL;
    }
    NovaFunction* fn = &functions->functions[functions->count++];
    int nameLen = stmt->functionDef.name.length;
    fn->name = malloc(nameLen + 1);
    memcpy(fn->name, stmt->functionDef.name.start, nameLen);
    fn->name[nameLen] = '\0';
    fn->arity = stmt->functionDef.paramCount;
    initChunk(&fn->chunk);
    return fn;
}

static void compileFunctionBody(Stmt* stmt, NovaFunction* fn) {
    FuncCompiler fc;
    fc.localCount = 0;
    fc.scopeDepth = 0;
    fc.enclosing  = current;
    current = &fc;

    // Parameters occupy slots 0..arity-1, matching the order the VM will
    // populate them in when a call comes in.
    for (int i = 0; i < stmt->functionDef.paramCount; i++)
        addLocal(&fc, stmt->functionDef.params[i]);

    // Each function body is compiled into its own independent Chunk and
    // runs with a fresh, empty operand stack at runtime (see OP_CALL in
    // vm.c) — parameters arrive directly in CallFrame->locals, never via
    // the operand stack. stackDepth must therefore start at 0 here
    // regardless of what it was left at after compiling the previous
    // function or the top-level chunk.
    int savedDepth = stackDepth;
    stackDepth = 0;

    compileStmt(stmt->functionDef.body, &fn->chunk);

    // Verify the function's body left the operand stack exactly as it
    // found it — see checkStackBalanced's comment for why this matters.
    // (An early `send` inside the body is not a violation: OP_FUNC_RETURN
    // unwinds the whole call regardless of operand stack contents, so
    // this check only concerns the implicit-fallthrough path reached
    // when compileStmt above returns normally.)
    char ctxBuf[300];
    snprintf(ctxBuf, sizeof(ctxBuf), "function '%s'", fn->name);
    checkStackBalanced(ctxBuf);

    // Implicit `send` (returns null) if the function falls off the end
    // of its body without an explicit 'send'.
    emitNullConstant(&fn->chunk, -1);
    emitByte(&fn->chunk, OP_FUNC_RETURN, -1);

    stackDepth = savedDepth;
    current = fc.enclosing;
}

void compileProgram(Program* program, Chunk* chunk, FunctionTable* functions) {
    // compileProgram is REENTRANT: loading a package (via 'use') calls
    // straight back into this same function to compile that package's
    // own file, before this call has finished. Every module-level
    // static this function (and everything it calls) relies on must
    // therefore be saved here at entry and restored at exit — otherwise
    // a nested call's resets would wipe out whatever the outer call had
    // already set up (most importantly, any module aliases registered
    // by 'use' statements processed before the one that triggered the
    // nested call — e.g. `use math as m` followed by `use mathext`).
    FunctionTable* savedGFunctions      = gFunctions;
    FuncCompiler*  savedCurrent         = current;
    TopScope*      savedTopScope        = currentTopScope;
    int            savedStackDepth      = stackDepth;
    int            savedAliasCount      = moduleAliasCount;
    ModuleAlias    savedAliases[MAX_MODULE_ALIASES];
    memcpy(savedAliases, moduleAliases, sizeof(ModuleAlias) * savedAliasCount);

    stackDepth = 0; // this chunk starts with an empty stack
    gFunctions = functions;
    current = NULL;
    currentTopScope = NULL;
    moduleAliasCount = 0;

    // Pass 0: process every top-level 'use' statement first, before
    // anything else compiles — see registerUseStatement's comment for
    // why this needs to happen even before function signatures.
    for (int i = 0; i < program->count; i++) {
        if (program->statements[i]->type == STMT_USE)
            registerUseStatement(program->statements[i]);
    }

    // Pass 1: register every top-level function's name/arity, with no
    // bodies compiled yet — see registerFunctionSignature's comment.
    NovaFunction* registered[MAX_FUNCTIONS];
    int registeredCount = 0;
    for (int i = 0; i < program->count; i++) {
        if (program->statements[i]->type == STMT_FUNCTION_DEF) {
            NovaFunction* fn = registerFunctionSignature(program->statements[i], functions);
            if (fn && registeredCount < MAX_FUNCTIONS) registered[registeredCount++] = fn;
        }
    }

    // Pass 2: compile every function's body now that ALL functions are
    // visible to isFunctionReference, regardless of which was declared
    // first.
    int idx = 0;
    for (int i = 0; i < program->count; i++) {
        if (program->statements[i]->type == STMT_FUNCTION_DEF)
            compileFunctionBody(program->statements[i], registered[idx++]);
    }

    // Pass 3: compile the remaining top-level statements as "main".
    // The root scope here is depth 0 — `let`s directly in it keep their
    // literal names (see declareVariable); only `let`s inside a nested
    // block underneath it get mangled. 'use' statements are skipped —
    // Pass 0 already fully handled them.
    beginScope();
    for (int i = 0; i < program->count; i++) {
        if (program->statements[i]->type != STMT_FUNCTION_DEF &&
            program->statements[i]->type != STMT_USE)
            compileStmt(program->statements[i], chunk);
    }
    checkStackBalanced("top-level code");
    emitByte(chunk, OP_RETURN, -1);

    // Restore the caller's state — see the big comment at the top.
    gFunctions       = savedGFunctions;
    current          = savedCurrent;
    currentTopScope  = savedTopScope;
    stackDepth       = savedStackDepth;
    moduleAliasCount = savedAliasCount;
    memcpy(moduleAliases, savedAliases, sizeof(ModuleAlias) * savedAliasCount);
}
