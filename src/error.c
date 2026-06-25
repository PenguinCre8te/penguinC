#include "error.h"
#include <stdlib.h>
#include <string.h>

static const char *g_source = NULL;

void error_set_source(const char *filename, const char *src) {
    (void)filename;
    g_source = src;
}

static const char *kind_str(ErrorKind kind) {
    switch (kind) {
        case ERR_LEXER:    return "lexer error";
        case ERR_PARSER:   return "parse error";
        case ERR_SEMANTIC: return "semantic error";
    }
    return "error";
}

/* Find the start and end of line `line_num` (1-based) in src */
static void find_line(const char *src, int line_num,
                      const char **start, const char **end) {
    const char *p = src;
    int line = 1;
    while (*p && line < line_num) {
        if (*p == '\n') line++;
        p++;
    }
    *start = p;
    while (*p && *p != '\n') p++;
    *end = p;
}

void error_at(SrcLoc loc, ErrorKind kind, const char *fmt, ...) {
    fprintf(stderr, "\033[1;31m%s\033[0m", kind_str(kind));
    if (loc.filename) {
        fprintf(stderr, " in %s", loc.filename);
    }
    if (loc.line > 0) {
        fprintf(stderr, ":%d:%d", loc.line, loc.col);
    }
    fprintf(stderr, ": ");

    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);

    fprintf(stderr, "\n");

    /* Show the source line if available */
    if (g_source && loc.line > 0) {
        const char *line_start, *line_end;
        find_line(g_source, loc.line, &line_start, &line_end);

        /* Print the line content */
        fprintf(stderr, "  %d | ", loc.line);
        fwrite(line_start, 1, line_end - line_start, stderr);
        fprintf(stderr, "\n");

        /* Print caret pointer */
        fprintf(stderr, "  %*s | ", loc.line >= 10 ? 2 : 1, "");
        for (int i = 1; i < loc.col; i++) {
            fputc(' ', stderr);
        }
        fprintf(stderr, "\033[1;31m^\033[0m\n");
    }

    exit(1);
}

void error_fatal(ErrorKind kind, const char *fmt, ...) {
    fprintf(stderr, "\033[1;31m%s\033[0m: ", kind_str(kind));

    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);

    fprintf(stderr, "\n");
    exit(1);
}
