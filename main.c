#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "parser.h"
#include "compiler.h"
#include "disassembler.h"
#include "ast_printer.h"
#include "vm.h"
#include "error.h"
#include "test_runner.h"
#include "package_cli.h"
#include "execution_policy.h"
#include "plugin_loader.h"

#define NOVA_VERSION "0.1.0"

// ─── helpers ───────────────────────────────────────────────────────────────

static char* readFile(const char* path) {
    // Binary mode is deliberate, not an oversight: in text mode, Windows'
    // C runtime silently translates "\r\n" -> "\n" while reading, so
    // fewer bytes land in `source` than `size` (computed from the raw,
    // pre-translation file size) accounts for. Null-terminating at
    // `size` in that case leaves a gap of uninitialized heap memory
    // between the real end of the source and the terminator — which the
    // lexer would happily scan into for whatever token happens to sit at
    // the very end of the file, silently absorbing garbage bytes into
    // it. Binary mode reads exactly `size` bytes on every platform,
    // matching what Linux/macOS already did here (text mode is a no-op
    // for them), so this doesn't change behavior there. Any leftover
    // '\r' from CRLF line endings is harmless — the lexer already
    // treats it as whitespace (see lexer.c).
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Error: could not open file '%s'\n", path);
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);

    char* source = malloc(size + 1);
    if (!source) {
        fprintf(stderr, "Error: out of memory\n");
        fclose(f);
        return NULL;
    }

    size_t bytesRead = fread(source, 1, size, f);
    source[bytesRead] = '\0'; // terminate at what was actually read, not the pre-read size
    fclose(f);
    return source;
}

static void checkExtension(const char* path) {
    int len = (int)strlen(path);
    if (len < 6 || strcmp(path + len - 5, ".nova") != 0) {
        fprintf(stderr, "Warning: file '%s' does not have a .nova extension\n", path);
    }
}

// ─── commands ──────────────────────────────────────────────────────────────

static void cmdHelp(void) {
    printf(
        "Nova " NOVA_VERSION " — the Nova language interpreter\n"
        "\n"
        "Usage:\n"
        "  nova <command> [arguments]\n"
        "\n"
        "Commands:\n"
        "  run <file>      Run a .nova source file\n"
        "  check <file>    Parse and compile without executing (syntax check)\n"
        "  ast <file>      Print the AST of a source file\n"
        "  dis <file>      Disassemble the bytecode of a source file\n"
        "  runtest <file>  Run a test file (# TEST: / # EXPECT / # END blocks)\n"
        "  install <plugin-name>   Install <plugin-name>.nova from the plugins/\n"
        "                          folder next to nova (nothing else is accepted)\n"
        "  list                    List installed packages\n"
        "  info <name>             Show details about an installed package\n"
        "  remove <name>           Remove an installed package\n"
        "  version         Show the Nova version\n"
        "  help            Show this help message\n"
        "\n"
        "Examples:\n"
        "  nova run code.nova\n"
        "  nova check code.nova\n"
        "  nova ast code.nova\n"
        "  nova dis code.nova\n"
        "  nova runtest tests.nova\n"
        "  nova install mathext\n"
    );
}

static void cmdVersion(void) {
    printf("Nova " NOVA_VERSION "\n");
}

static int cmdRun(const char* filepath) {
    checkExtension(filepath);

    char* source = readFile(filepath);
    if (!source) return 1;

    // Phase 1: Parse
    Parser parser;
    initParser(&parser, source);
    Program* program = parse(&parser);

    if (novaHasError()) { free(source); return 1; }

    // Phase 2: Compile
    Chunk chunk;
    initChunk(&chunk);
    FunctionTable functions;
    initFunctionTable(&functions);
    compileProgram(program, &chunk, &functions);

    if (novaHasError()) { freeChunk(&chunk); free(source); return 1; }

    // Phase 3: Execute
    VM vm;
    initVM(&vm);
    interpret(&vm, &chunk, NULL, &functions);

    freeChunk(&chunk);
    free(source);
    return novaHasError() ? 1 : 0;
}

static int cmdCheck(const char* filepath) {
    checkExtension(filepath);

    char* source = readFile(filepath);
    if (!source) return 1;

    // Phase 1: Parse
    Parser parser;
    initParser(&parser, source);
    Program* program = parse(&parser);

    if (novaHasError()) { free(source); return 1; }

    // Phase 2: Compile only — no execution
    Chunk chunk;
    initChunk(&chunk);
    FunctionTable functions;
    initFunctionTable(&functions);
    compileProgram(program, &chunk, &functions);
    freeChunk(&chunk);
    free(source);

    if (novaHasError()) return 1;

    printf("OK: '%s' compiled with no errors.\n", filepath);
    return 0;
}

static int cmdAst(const char* filepath) {
    checkExtension(filepath);

    char* source = readFile(filepath);
    if (!source) return 1;

    Parser parser;
    initParser(&parser, source);
    Program* program = parse(&parser);

    if (novaHasError()) { free(source); return 1; }

    printProgram(program);

    free(source);
    return 0;
}

static int cmdDis(const char* filepath) {
    checkExtension(filepath);

    char* source = readFile(filepath);
    if (!source) return 1;

    // Parse
    Parser parser;
    initParser(&parser, source);
    Program* program = parse(&parser);

    if (novaHasError()) { free(source); return 1; }

    // Compile
    Chunk chunk;
    initChunk(&chunk);
    FunctionTable functions;
    initFunctionTable(&functions);
    compileProgram(program, &chunk, &functions);

    if (novaHasError()) { freeChunk(&chunk); free(source); return 1; }

    // Disassemble
    for (int i = 0; i < functions.count; i++) {
        char label[256];
        snprintf(label, sizeof(label), "fn %s", functions.functions[i].name);
        disassembleChunk(&functions.functions[i].chunk, label);
        printf("\n");
    }
    disassembleChunk(&chunk, filepath);

    freeChunk(&chunk);
    free(source);
    return 0;
}

// ─── entry point ───────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {

    // --safe / --allow=... may appear anywhere on the command line —
    // strip them out here (applying their effect to gPolicy) so every
    // command below sees a normal, unaffected argv exactly as before
    // this feature existed.
    char* filteredArgv[64];
    int filteredArgc = argc < 64 ? argc : 64;
    filteredArgc = filterExecutionPolicyArgs(filteredArgc, argv, filteredArgv);
    argc = filteredArgc;
    argv = filteredArgv;

    checkAndOfferPlugins(argv[0]);

    if (argc < 2) {
        cmdHelp();
        return 0;
    }

    const char* cmd = argv[1];

    if (strcmp(cmd, "help")    == 0 || strcmp(cmd, "--help")    == 0 || strcmp(cmd, "-h") == 0) {
        cmdHelp();    return 0;
    }
    if (strcmp(cmd, "version") == 0 || strcmp(cmd, "--version") == 0 || strcmp(cmd, "-v") == 0) {
        cmdVersion(); return 0;
    }

    // Commands that require a file argument
    if (strcmp(cmd, "run")     == 0 ||
        strcmp(cmd, "check")   == 0 ||
        strcmp(cmd, "ast")     == 0 ||
        strcmp(cmd, "dis")     == 0 ||
        strcmp(cmd, "runtest") == 0) {

        if (argc < 3) {
            fprintf(stderr, "Error: '%s' requires a file argument\n", cmd);
            fprintf(stderr, "Usage: nova %s <file>\n", cmd);
            return 1;
        }

        const char* filepath = argv[2];

        if      (strcmp(cmd, "run")     == 0) return cmdRun(filepath);
        else if (strcmp(cmd, "check")   == 0) return cmdCheck(filepath);
        else if (strcmp(cmd, "ast")     == 0) return cmdAst(filepath);
        else if (strcmp(cmd, "dis")     == 0) return cmdDis(filepath);
        else if (strcmp(cmd, "runtest") == 0) return runTestFile(filepath);
    }

    if (strcmp(cmd, "list") == 0) {
        return packageList();
    }
    if (strcmp(cmd, "install") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: nova install <plugin-name>\n"
                            "Installs <plugin-name>.nova from the plugins folder next to nova as a package.\n");
            return 1;
        }
        char pluginPath[1300];
        if (!findPluginInFolder(argv[0], argv[2], pluginPath, sizeof(pluginPath))) {
            fprintf(stderr, "Error: no plugin named '%s' found in the plugins folder\n", argv[2]);
            return 1;
        }
        char name[256];
        snprintf(name, sizeof(name), "%s", argv[2]);
        size_t nlen = strlen(name);
        if (nlen >= 5 && strcmp(name + nlen - 5, ".nova") == 0) name[nlen - 5] = '\0';
        return packageInstall(name, pluginPath);
    }
    if (strcmp(cmd, "info") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: nova info <name>\n");
            return 1;
        }
        return packageInfo(argv[2]);
    }
    if (strcmp(cmd, "remove") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: nova remove <name>\n");
            return 1;
        }
        return packageRemove(argv[2]);
    }

    fprintf(stderr, "Error: unknown command '%s'\n", cmd);
    fprintf(stderr, "Run 'nova help' to see available commands.\n");
    return 1;
}
