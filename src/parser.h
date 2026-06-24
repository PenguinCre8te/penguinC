#ifndef PENGUINC_PARSER_H
#define PENGUINC_PARSER_H

#include "ast.h"
#include "lexer.h"

/* ------------------------------------------------------------------ */
/*  Parser state                                                       */
/* ------------------------------------------------------------------ */
typedef struct {
    Lexer lexer;
} Parser;

/* Initialise parser and parse a source file, returning the program AST */
AstNode *parse_file(const char *filename, const char *src);

#endif /* PENGUINC_PARSER_H */
