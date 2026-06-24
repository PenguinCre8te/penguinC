#include "error.h"
#include <stdlib.h>

static const char *kind_str(ErrorKind kind) {
    switch (kind) {
        case ERR_LEXER:    return "lexer error";
        case ERR_PARSER:   return "parse error";
        case ERR_SEMANTIC: return "semantic error";
    }
    return "error";
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
