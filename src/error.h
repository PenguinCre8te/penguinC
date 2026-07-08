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

/* Store source context for error display */
void error_set_source(const char *filename, const char *src);

/* Test mode: clean output without colors or source context */
void error_set_test_mode(int enabled);
int error_get_test_mode(void);

/* Print error header + source context without exiting (for typecheck) */
void print_error_context(SrcLoc loc, const char *fmt, ...);

/* Print a formatted error message with source location and exit */
void error_at(SrcLoc loc, ErrorKind kind, const char *fmt, ...);

/* Print a formatted error message without location and exit */
void error_fatal(ErrorKind kind, const char *fmt, ...);

/* Print a formatted warning with source location (does not exit) */
void warn_at(SrcLoc loc, const char *fmt, ...);

#endif /* PENGUINC_ERROR_H */
