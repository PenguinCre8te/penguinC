#ifndef PENGUINC_ERROR_H
#define PENGUINC_ERROR_H

#include "ast.h"
#include <stdio.h>
#include <stdarg.h>

/* Error severity levels */
typedef enum {
    ERR_LEXER,
    ERR_PARSER,
    ERR_SEMANTIC,
} ErrorKind;

/* Print a formatted error message with source location and exit */
void error_at(SrcLoc loc, ErrorKind kind, const char *fmt, ...);

/* Print a formatted error message without location and exit */
void error_fatal(ErrorKind kind, const char *fmt, ...);

#endif /* PENGUINC_ERROR_H */
