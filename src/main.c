#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "parser.h"
#include "error.h"
#include "codegen.h"
#include "typecheck.h"
#include "printast.h"

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        error_fatal(ERR_PARSER, "cannot open file '%s'", path);
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(len + 1);
    size_t n = fread(buf, 1, len, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

#define PENGUINC_VERSION "0.1.0"

static void print_usage(void) {
    fprintf(stderr,
        "penguinc " PENGUINC_VERSION " — penguinC compiler\n"
        "\n"
        "Usage: penguinc [options] <file.pc>\n"
        "\n"
        "Options:\n"
        "  -o <file>      Set output file (default: derive from input)\n"
        "  -c             Compile only, do not link\n"
        "  -O0            No optimizations (default)\n"
        "  -O1            Basic optimizations\n"
        "  -O2            Default optimizations\n"
        "  -O3            Aggressive optimizations\n"
        "  --for-test     Clean error output for testing (no colors, no source context)\n"
        "  --version      Print version\n"
        "  --help         Print this help\n"
        "  --print-ast    Print the AST\n"
    );
}

int main(int argc, char **argv) {
    int printast = 0;
    if (argc < 2) {
        print_usage();
        return 1;
    }

    const char *input_file = NULL;
    const char *output_file = NULL;
    int compile_only = 0;
    int for_test = 0;
    OptLevel opt = OPT_NONE;
    LinkPaths links = {0};

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output_file = argv[++i];
        } else if (strcmp(argv[i], "-c") == 0) {
            compile_only = 1;
        } else if (strcmp(argv[i], "--for-test") == 0) {
            for_test = 1;
        } else if (strcmp(argv[i], "-O0") == 0) {
            opt = OPT_NONE;
        } else if (strcmp(argv[i], "-O1") == 0) {
            opt = OPT_BASIC;
        } else if (strcmp(argv[i], "-O2") == 0) {
            opt = OPT_DEFAULT;
        } else if (strcmp(argv[i], "-O3") == 0) {
            opt = OPT_AGGRESSIVE;
        } else if (strcmp(argv[i], "--version") == 0) {
            fprintf(stderr, "penguinc " PENGUINC_VERSION "\n");
            return 0;
        } else if (strcmp(argv[i], "--print-ast") == 0) {
            printast = 1;
        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage();
            return 0;
        } else if (argv[i][0] != '-') {
            input_file = argv[i];
        } else {
            fprintf(stderr, "penguinc: unknown option '%s'\n", argv[i]);
            return 1;
        }
    }

    if (!input_file) {
        fprintf(stderr, "penguinc: no input file specified\n");
        return 1;
    }

    if (!output_file) {
        /* Derive output name from input: foo.pc -> foo */
        static char default_out[512];
        strncpy(default_out, input_file, sizeof(default_out) - 1);
        default_out[sizeof(default_out) - 1] = '\0';
        char *dot = strrchr(default_out, '.');
        if (dot) *dot = '\0';
        output_file = default_out;
    }

    char *src = read_file(input_file);
    error_set_source(input_file, src);
    if (for_test) error_set_test_mode(1);
    AstNode *ast = parse_file(input_file, src);

    if (printast) {
        print_ast(ast, 0);
    }

    int tc_errors = typecheck(ast, input_file, src);
    if (tc_errors > 0) {
        if (!for_test)
            fprintf(stderr, "penguinc: compilation aborted due to type errors\n");
        free(src);
        return 1;
    }

    codegen(ast, output_file, opt, &links);

    /* Auto-link when not compile-only: use resolved link paths */
    if (!compile_only && links.count > 0) {
        /* Find stdlib and runtime directories */
        const char *stdlib = getenv("STDLIB");
        if (!stdlib) stdlib = "stdlib";
        const char *runtime = getenv("RUNTIME");
        if (!runtime) runtime = "runtime";

        /* The object file has .o appended by codegen */
        char obj_file[512];
        snprintf(obj_file, sizeof(obj_file), "%s.o", output_file);

        /* Build gcc command */
        char cmd[4096];
        snprintf(cmd, sizeof(cmd), "gcc \"%s\"", obj_file);

        /* Always link runtime/arc.o */
        char arc_obj[1024];
        snprintf(arc_obj, sizeof(arc_obj), "%s/arc.o", runtime);
        if (access(arc_obj, F_OK) == 0) {
            char append[1200];
            snprintf(append, sizeof(append), " \"%s\"", arc_obj);
            strncat(cmd, append, sizeof(cmd) - strlen(cmd) - 1);
        }

        /* Link resolved import paths */
        for (size_t i = 0; i < links.count; i++) {
            const char *path = links.paths[i];
            size_t plen = strlen(path);
            if (plen > 2 && strcmp(path + plen - 2, ".o") == 0) {
                /* Object file — link directly if it exists */
                if (access(path, F_OK) == 0) {
                    char append[1200];
                    snprintf(append, sizeof(append), " \"%s\"", path);
                    strncat(cmd, append, sizeof(cmd) - strlen(cmd) - 1);
                }
            } else {
                /* Library name (e.g. "libc" or "c") — pass as -l flag */
                const char *libname = path;
                if (strncmp(libname, "lib", 3) == 0) libname += 3;
                char append[1200];
                snprintf(append, sizeof(append), " -l%s", libname);
                strncat(cmd, append, sizeof(cmd) - strlen(cmd) - 1);
            }
        }

        char append_out[600];
        snprintf(append_out, sizeof(append_out), " -o \"%s\" -lm", output_file);
        strncat(cmd, append_out, sizeof(cmd) - strlen(cmd) - 1);

        /* Link */
        int ret = system(cmd);
        if (ret != 0) {
            fprintf(stderr, "penguinc: linking failed (exit code %d)\n", ret);
        }
    }

    free(src);
    for (size_t i = 0; i < links.count; i++)
        free(links.paths[i]);
    free(links.paths);
    return 0;
}
