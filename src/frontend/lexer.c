#include "lexer.h"
#include "error.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */
static char cur_char(Lexer *lex) {
    return lex->src[lex->pos];
}

static char peek_char(Lexer *lex) {
    return lex->src[lex->pos + 1];
}

static void advance(Lexer *lex) {
    if (cur_char(lex) == '\n') {
        lex->line++;
        lex->col = 1;
    } else {
        lex->col++;
    }
    lex->pos++;
}

static void skip_whitespace_and_comments(Lexer *lex) {
    for (;;) {
        char c = cur_char(lex);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance(lex);
        } else if (c == '/' && peek_char(lex) == '/') {
            /* single-line comment */
            while (cur_char(lex) != '\n' && cur_char(lex) != '\0')
                advance(lex);
        } else {
            break;
        }
    }
}

static Token make_token(Lexer *lex, TokenType type) {
    Token t;
    t.type  = type;
    t.loc   = lex->filename ? (SrcLoc){lex->filename, lex->line, lex->col}
                            : (SrcLoc){NULL, lex->line, lex->col};
    t.value = NULL;
    return t;
}

static Token make_token_with_value(Lexer *lex, TokenType type, const char *start, size_t len) {
    Token t = make_token(lex, type);
    t.value = malloc(len + 1);
    memcpy(t.value, start, len);
    t.value[len] = '\0';
    return t;
}

/* ------------------------------------------------------------------ */
/*  Keyword lookup                                                     */
/* ------------------------------------------------------------------ */
typedef struct { const char *word; TokenType type; } Keyword;

static const Keyword keywords[] = {
    {"int",      TOK_INT},
    {"void",     TOK_VOID},
    {"string",   TOK_STRING},
    {"bool",     TOK_BOOL},
    {"float",    TOK_FLOAT},
    {"struct",   TOK_STRUCT},
    {"class",    TOK_CLASS},
    {"extends",  TOK_EXTENDS},
    {"if",       TOK_IF},
    {"else",     TOK_ELSE},
    {"switch",   TOK_SWITCH},
    {"case",     TOK_CASE},
    {"match",    TOK_MATCH},
    {"for",      TOK_FOR},
    {"while",    TOK_WHILE},
    {"return",   TOK_RETURN},
    {"mut",      TOK_MUT},
    {"mutable",  TOK_MUT},
    {"shared",   TOK_SHARED},
    {"borrow",   TOK_BORROW},
    {"lock",     TOK_LOCK},
    {"using",    TOK_USING},
    {"unsafe",   TOK_UNSAFE},
    {"new",      TOK_NEW},
    {"self",     TOK_SELF},
    {"super",    TOK_SUPER},
    {"sizeof",   TOK_SIZEOF},
    {"true",     TOK_TRUE},
    {"false",    TOK_FALSE},
    {"func",     TOK_FUNC},
    {"in",       TOK_IN},
    {"import",   TOK_IMPORT},
    {"link",     TOK_LINK},
    {"typedef",  TOK_TYPEDEF},
    {"enum",     TOK_ENUM},
    {"union",    TOK_UNION},
    {"extern",   TOK_EXTERN},
    {"static",   TOK_STATIC},
    {"const",    TOK_CONST},
    {"volatile", TOK_VOLATILE},
    {"register", TOK_REGISTER},
    {"inline",   TOK_INLINE},
    {"auto",     TOK_AUTO},
    {"goto",     TOK_GOTO},
    {"break",    TOK_BREAK},
    {"continue", TOK_CONTINUE},
    {"default",  TOK_DEFAULT},
    {"do",       TOK_DO},
    {"NULL",     TOK_NULL_LIT},
    {NULL, 0},
};

static TokenType check_keyword(const char *word, size_t len) {
    for (const Keyword *kw = keywords; kw->word; kw++) {
        if (strlen(kw->word) == len && memcmp(kw->word, word, len) == 0)
            return kw->type;
    }
    return TOK_IDENT;
}

/* ------------------------------------------------------------------ */
/*  Tokenise one token                                                 */
/* ------------------------------------------------------------------ */
static Token next_token(Lexer *lex) {
    skip_whitespace_and_comments(lex);

    char c = cur_char(lex);
    if (c == '\0')
        return make_token(lex, TOK_EOF);

    int start_line = lex->line;
    int start_col  = lex->col;

    /* ---- f-strings: f"..." (must check before identifiers) ---- */
    if (c == 'f' && (peek_char(lex) == '"' || peek_char(lex) == '\'')) {
        char quote = peek_char(lex);
        advance(lex); /* skip 'f' */
        advance(lex); /* skip opening quote */
        char buf[4096];
        size_t len = 0;
        while (cur_char(lex) != quote && cur_char(lex) != '\0') {
            if (cur_char(lex) == '\\') {
                advance(lex);
                if (cur_char(lex) == '\0')
                    error_at((SrcLoc){lex->filename, lex->line, lex->col},
                             ERR_LEXER, "unterminated escape in f-string");
                char esc = cur_char(lex);
                char resolved;
                switch (esc) {
                    case 'n':  resolved = '\n'; break;
                    case 't':  resolved = '\t'; break;
                    case 'r':  resolved = '\r'; break;
                    case '\\': resolved = '\\'; break;
                    case '"':  resolved = '"';  break;
                    case '\'': resolved = '\''; break;
                    case '0':  resolved = '\0'; break;
                    default:   resolved = esc;  break;
                }
                if (len < sizeof(buf) - 1)
                    buf[len++] = resolved;
                advance(lex);
                continue;
            }
            if (len < sizeof(buf) - 1)
                buf[len++] = cur_char(lex);
            advance(lex);
        }
        if (cur_char(lex) == '\0')
            error_at((SrcLoc){lex->filename, start_line, start_col},
                     ERR_LEXER, "unterminated f-string literal");
        advance(lex); /* skip closing quote */
        buf[len] = '\0';
        Token t = make_token_with_value(lex, TOK_FSTRING_LIT, buf, len);
        t.loc.line = start_line;
        t.loc.col  = start_col;
        return t;
    }

    /* ---- identifiers / keywords ---- */
    if (isalpha(c) || c == '_') {
        const char *start = &lex->src[lex->pos];
        size_t len = 0;
        while (isalnum(cur_char(lex)) || cur_char(lex) == '_') {
            len++;
            advance(lex);
        }
        TokenType type = check_keyword(start, len);
        Token t = make_token_with_value(lex, type, start, len);
        t.loc.line = start_line;
        t.loc.col  = start_col;
        return t;
    }

    /* ---- numbers (int / float) ---- */
    if (isdigit(c)) {
        const char *start = &lex->src[lex->pos];
        size_t len = 0;
        int is_float = 0;
        while (isdigit(cur_char(lex))) {
            len++;
            advance(lex);
        }
        /* Only consume '.' as decimal point if followed by a digit */
        if (cur_char(lex) == '.' && isdigit(peek_char(lex))) {
            is_float = 1;
            len++;
            advance(lex); /* consume '.' */
            while (isdigit(cur_char(lex))) {
                len++;
                advance(lex);
            }
        }
        TokenType type = is_float ? TOK_FLOAT_LIT : TOK_INT_LIT;
        Token t = make_token_with_value(lex, type, start, len);
        t.loc.line = start_line;
        t.loc.col  = start_col;
        return t;
    }

    /* ---- string literals ---- */
    if (c == '"' || c == '\'') {
        char quote = c;
        advance(lex); /* skip opening quote */
        char buf[4096];
        size_t len = 0;
        while (cur_char(lex) != quote && cur_char(lex) != '\0') {
            if (cur_char(lex) == '\\') {
                advance(lex);
                if (cur_char(lex) == '\0') {
                    error_at(lex->filename ? (SrcLoc){lex->filename, lex->line, lex->col}
                                           : (SrcLoc){NULL, lex->line, lex->col},
                             ERR_LEXER, "unterminated escape sequence in string literal");
                }
                char esc = cur_char(lex);
                char resolved;
                switch (esc) {
                    case 'n':  resolved = '\n'; break;
                    case 't':  resolved = '\t'; break;
                    case 'r':  resolved = '\r'; break;
                    case '\\': resolved = '\\'; break;
                    case '"':  resolved = '"';  break;
                    case '\'': resolved = '\''; break;
                    case '0':  resolved = '\0'; break;
                    default:   resolved = esc;  break;
                }
                if (len < sizeof(buf) - 1)
                    buf[len++] = resolved;
                advance(lex);
                continue;
            }
            if (len < sizeof(buf) - 1)
                buf[len++] = cur_char(lex);
            advance(lex);
        }
        if (cur_char(lex) == '\0') {
            error_at(lex->filename ? (SrcLoc){lex->filename, start_line, start_col}
                                   : (SrcLoc){NULL, start_line, start_col},
                     ERR_LEXER, "unterminated string literal");
        }
        advance(lex); /* skip closing quote */
        buf[len] = '\0';
        Token t = make_token_with_value(lex, TOK_STRING_LIT, buf, len);
        t.loc.line = start_line;
        t.loc.col  = start_col;
        return t;
    }

    /* ---- two-char operators ---- */
    {
        char c2 = peek_char(lex);
        switch (c) {
            case '+':
                advance(lex);
                if (c2 == '=') { advance(lex); return make_token_with_value(lex, TOK_PLUS_ASSIGN, "+=", 2); }
                if (c2 == '+') { advance(lex); return make_token_with_value(lex, TOK_PLUS_PLUS, "++", 2); }
                return make_token_with_value(lex, TOK_PLUS, "+", 1);
            case '-':
                advance(lex);
                if (c2 == '=') { advance(lex); return make_token_with_value(lex, TOK_MINUS_ASSIGN, "-=", 2); }
                if (c2 == '>') { advance(lex); return make_token_with_value(lex, TOK_ARROW, "->", 2); }
                if (c2 == '-') { advance(lex); return make_token_with_value(lex, TOK_MINUS_MINUS, "--", 2); }
                return make_token_with_value(lex, TOK_MINUS, "-", 1);
            case '*':
                advance(lex);
                if (c2 == '*') { advance(lex); return make_token_with_value(lex, TOK_STAR_STAR, "**", 2); }
                if (c2 == '=') { advance(lex); return make_token_with_value(lex, TOK_STAR_ASSIGN, "*=", 2); }
                return make_token_with_value(lex, TOK_STAR, "*", 1);
            case '/':
                advance(lex);
                if (c2 == '=') { advance(lex); return make_token_with_value(lex, TOK_SLASH_ASSIGN, "/=", 2); }
                return make_token_with_value(lex, TOK_SLASH, "/", 1);
            case '%':
                advance(lex);
                return make_token_with_value(lex, TOK_PERCENT, "%", 1);
            case '=':
                advance(lex);
                if (c2 == '=') { advance(lex); return make_token_with_value(lex, TOK_EQ, "==", 2); }
                if (c2 == '>') { advance(lex); return make_token_with_value(lex, TOK_FAT_ARROW, "=>", 2); }
                return make_token_with_value(lex, TOK_ASSIGN, "=", 1);
            case '!':
                advance(lex);
                if (c2 == '=') { advance(lex); return make_token_with_value(lex, TOK_NEQ, "!=", 2); }
                error_at((SrcLoc){lex->filename, start_line, start_col},
                         ERR_LEXER, "unexpected character '!'; did you mean '!='?");
                /* fallthrough */
            case '<':
                advance(lex);
                if (c2 == '=') { advance(lex); return make_token_with_value(lex, TOK_LE, "<=", 2); }
                return make_token_with_value(lex, TOK_LT, "<", 1);
            case '>':
                advance(lex);
                if (c2 == '=') { advance(lex); return make_token_with_value(lex, TOK_GE, ">=", 2); }
                return make_token_with_value(lex, TOK_GT, ">", 1);
            case '&':
                advance(lex);
                if (c2 == '&') { advance(lex); return make_token_with_value(lex, TOK_AND, "&&", 2); }
                error_at((SrcLoc){lex->filename, start_line, start_col},
                         ERR_LEXER, "unexpected character '&'; did you mean '&&'?");
                /* fallthrough */
            case '|':
                advance(lex);
                if (c2 == '|') { advance(lex); return make_token_with_value(lex, TOK_OR, "||", 2); }
                error_at((SrcLoc){lex->filename, start_line, start_col},
                         ERR_LEXER, "unexpected character '|'; did you mean '||'?");
                /* fallthrough */
            case '.':
                advance(lex);
                if (c2 == '.') { advance(lex); return make_token_with_value(lex, TOK_DOTDOT, "..", 2); }
                return make_token_with_value(lex, TOK_DOT, ".", 1);
        }
    }

    /* ---- single-char tokens ---- */
    advance(lex);
    switch (c) {
        case '(':  return make_token_with_value(lex, TOK_LPAREN, "(", 1);
        case ')':  return make_token_with_value(lex, TOK_RPAREN, ")", 1);
        case '{':  return make_token_with_value(lex, TOK_LBRACE, "{", 1);
        case '}':  return make_token_with_value(lex, TOK_RBRACE, "}", 1);
        case '[':  return make_token_with_value(lex, TOK_LBRACKET, "[", 1);
        case ']':  return make_token_with_value(lex, TOK_RBRACKET, "]", 1);
        case ';':  return make_token_with_value(lex, TOK_SEMICOLON, ";", 1);
        case ':':  return make_token_with_value(lex, TOK_COLON, ":", 1);
        case ',':  return make_token_with_value(lex, TOK_COMMA, ",", 1);
        case '#':  return make_token_with_value(lex, TOK_HASH, "#", 1);
        case '~':  return make_token_with_value(lex, TOK_TILDE, "~", 1);
        case '?':  return make_token_with_value(lex, TOK_QUESTION, "?", 1);
        default:
            error_at((SrcLoc){lex->filename, start_line, start_col},
                     ERR_LEXER, "unexpected character '%c'", c);
    }

    /* unreachable */
    return make_token(lex, TOK_EOF);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */
void lexer_init(Lexer *lex, const char *filename, const char *src) {
    lex->filename = filename;
    lex->src      = src;
    lex->pos      = 0;
    lex->line     = 1;
    lex->col      = 1;
    lex->has_peek = 0;
    lex->current  = next_token(lex);
    lex->peek     = (Token){0};
}

Token *lexer_current(Lexer *lex) {
    return &lex->current;
}

Token *lexer_advance(Lexer *lex) {
    if (lex->has_peek) {
        lex->current  = lex->peek;
        lex->has_peek = 0;
    } else {
        lex->current = next_token(lex);
    }
    return &lex->current;
}

Token *lexer_peek(Lexer *lex) {
    if (!lex->has_peek) {
        lex->peek     = next_token(lex);
        lex->has_peek = 1;
    }
    return &lex->peek;
}

int lexer_match(Lexer *lex, TokenType type) {
    if (lex->current.type == type) {
        lexer_advance(lex);
        return 1;
    }
    return 0;
}

Token lexer_expect(Lexer *lex, TokenType type) {
    Token *t = lexer_current(lex);
    if (t->type != type) {
        error_at(t->loc, ERR_PARSER, "expected '%s', got '%s'",
                 token_type_name(type), token_type_name(t->type));
    }
    Token result = *t;
    lexer_advance(lex);
    return result;
}

const char *token_type_name(TokenType type) {
    switch (type) {
        case TOK_INT_LIT:      return "integer literal";
        case TOK_FLOAT_LIT:    return "float literal";
        case TOK_STRING_LIT:   return "string literal";
        case TOK_FSTRING_LIT:  return "f-string literal";
        case TOK_IDENT:        return "identifier";
        case TOK_INT:          return "'int'";
        case TOK_VOID:         return "'void'";
        case TOK_STRING:       return "'string'";
        case TOK_BOOL:         return "'bool'";
        case TOK_FLOAT:        return "'float'";
        case TOK_STRUCT:       return "'struct'";
        case TOK_CLASS:        return "'class'";
        case TOK_EXTENDS:      return "'extends'";
        case TOK_IF:           return "'if'";
        case TOK_ELSE:         return "'else'";
        case TOK_SWITCH:       return "'switch'";
        case TOK_CASE:         return "'case'";
        case TOK_MATCH:        return "'match'";
        case TOK_FOR:          return "'for'";
        case TOK_WHILE:        return "'while'";
        case TOK_RETURN:       return "'return'";
        case TOK_IN:           return "'in'";
        case TOK_MUT:          return "'mut'";
        case TOK_SHARED:       return "'shared'";
        case TOK_BORROW:       return "'borrow'";
        case TOK_LOCK:         return "'lock'";
        case TOK_USING:        return "'using'";
        case TOK_UNSAFE:       return "'unsafe'";
        case TOK_NEW:          return "'new'";
        case TOK_SELF:         return "'self'";
        case TOK_SUPER:        return "'super'";
        case TOK_SIZEOF:       return "'sizeof'";
        case TOK_TRUE:         return "'true'";
        case TOK_FALSE:        return "'false'";
        case TOK_PLUS:         return "'+'";
        case TOK_MINUS:        return "'-'";
        case TOK_STAR:         return "'*'";
        case TOK_SLASH:        return "'/'";
        case TOK_PERCENT:      return "'%'";
        case TOK_EQ:           return "'=='";
        case TOK_NEQ:          return "'!='";
        case TOK_LT:           return "'<'";
        case TOK_GT:           return "'>'";
        case TOK_LE:           return "'<='";
        case TOK_GE:           return "'>='";
        case TOK_AND:          return "'&&'";
        case TOK_OR:           return "'||'";
        case TOK_TILDE:         return "'~'";
        case TOK_ASSIGN:       return "'='";
        case TOK_PLUS_ASSIGN:  return "'+='";
        case TOK_PLUS_PLUS:    return "'++'";
        case TOK_MINUS_MINUS:  return "'--'";
        case TOK_MINUS_ASSIGN: return "'-='";
        case TOK_STAR_ASSIGN:  return "'*='";
        case TOK_STAR_STAR:    return "'**'";
        case TOK_SLASH_ASSIGN: return "'/='";
        case TOK_DOT:          return "'.'";
        case TOK_COMMA:        return "','";
        case TOK_SEMICOLON:    return "';'";
        case TOK_COLON:        return "':'";
        case TOK_LPAREN:       return "'('";
        case TOK_RPAREN:       return "')'";
        case TOK_LBRACE:       return "'{'";
        case TOK_RBRACE:       return "'}'";
        case TOK_LBRACKET:     return "'['";
        case TOK_RBRACKET:     return "']'";
        case TOK_ARROW:        return "'->'";
        case TOK_FAT_ARROW:    return "'=>'";
        case TOK_DOTDOT:       return "'..'";
        case TOK_HASH:         return "'#'";
        case TOK_SEMICOLON_DEF: return "';'";
        case TOK_IMPORT:       return "'import'";
        case TOK_LINK:         return "'link'";
        case TOK_FUNC:         return "'func'";
        case TOK_TYPEDEF:      return "'typedef'";
        case TOK_ENUM:         return "'enum'";
        case TOK_UNION:        return "'union'";
        case TOK_EXTERN:       return "'extern'";
        case TOK_STATIC:       return "'static'";
        case TOK_CONST:        return "'const'";
        case TOK_VOLATILE:     return "'volatile'";
        case TOK_REGISTER:     return "'register'";
        case TOK_INLINE:       return "'inline'";
        case TOK_AUTO:         return "'auto'";
        case TOK_GOTO:         return "'goto'";
        case TOK_BREAK:        return "'break'";
        case TOK_CONTINUE:     return "'continue'";
        case TOK_DEFAULT:      return "'default'";
        case TOK_DO:           return "'do'";
        case TOK_NULL_LIT:     return "'NULL'";
        case TOK_QUESTION:     return "'?'";
        case TOK_EOF:          return "end of file";
        case TOK_ERROR:        return "error";
    }
    return "unknown token";
}
