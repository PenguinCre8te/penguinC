#ifndef PENGUINC_LEXER_H
#define PENGUINC_LEXER_H

#include "ast.h"

/* ------------------------------------------------------------------ */
/*  Token                                                              */
/* ------------------------------------------------------------------ */
typedef struct {
    TokenType type;
    SrcLoc    loc;
    char     *value;   /* only for IDENT / string/int literals */
} Token;

/* ------------------------------------------------------------------ */
/*  Lexer state                                                        */
/* ------------------------------------------------------------------ */
typedef struct {
    const char *filename;
    const char *src;
    size_t      pos;
    int         line;
    int         col;
    Token       current;
    Token       peek;
    int         has_peek;
} Lexer;

/* Initialise the lexer and advance to the first token */
void lexer_init(Lexer *lex, const char *filename, const char *src);

/* Return the current token (do NOT free) */
Token *lexer_current(Lexer *lex);

/* Advance and return the next token */
Token *lexer_advance(Lexer *lex);

/* Peek at the next token without consuming it */
Token *lexer_peek(Lexer *lex);

/* Check if current token matches the given type, consume and return 1 on match */
int lexer_match(Lexer *lex, TokenType type);

/* Expect the current token to be of the given type; consume and return it.
   Calls error_at on mismatch. */
Token lexer_expect(Lexer *lex, TokenType type);

/* Return a human-readable name for a token type */
const char *token_type_name(TokenType type);

#endif /* PENGUINC_LEXER_H */
