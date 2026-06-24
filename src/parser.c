#include "parser.h"
#include "error.h"
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */
static Lexer *P;

static Token *cur(void)  { return lexer_current(P); }
static Token *peek(void) { return lexer_peek(P); }
static Token *adv(void)  { return lexer_advance(P); }
static int match(TokenType t) { return lexer_match(P, t); }
static Token expect(TokenType t) { return lexer_expect(P, t); }

static int cur_is(TokenType t) { return cur()->type == t; }
static int peek_is(TokenType t) { return peek()->type == t; }

/* ------------------------------------------------------------------ */
/*  Forward declarations                                               */
/* ------------------------------------------------------------------ */
static AstNode *parse_statement(void);
static AstNode *parse_expr(void);
static AstNode *parse_range(void);
static AstNode *parse_or(void);
static AstNode *parse_and(void);
static AstNode *parse_comparison(void);
static AstNode *parse_additive(void);
static AstNode *parse_multiplicative(void);
static AstNode *parse_unary(void);
static AstNode *parse_postfix(void);
static AstNode *parse_primary(void);
static AstNode *parse_struct_decl(void);
static AstNode *parse_class_decl(void);

/* Any token that could be a type name: keywords like int, void, string, etc.
   AND any user-defined identifier. This allows typedefs and custom types
   without hardcoding. */
static int is_ident_like_type(void) {
    switch (cur()->type) {
        case TOK_IDENT:
        case TOK_INT: case TOK_VOID: case TOK_STRING:
        case TOK_BOOL: case TOK_FLOAT:
        case TOK_LOCK:
            return 1;
        default:
            return 0;
    }
}

/* ------------------------------------------------------------------ */
/*  Dotted name: std.io, foo.bar.baz                                   */
/* ------------------------------------------------------------------ */
static char *parse_dotted_name(void) {
    Token first = expect(TOK_IDENT);

    /* Fast path: no dot following */
    if (!cur_is(TOK_DOT) || !peek_is(TOK_IDENT)) {
        return first.value;
    }

    /* Collect segments into a buffer */
    size_t cap = 64;
    size_t len = strlen(first.value);
    char *name = malloc(cap);
    name[0] = '\0';
    strcat(name, first.value);

    while (cur_is(TOK_DOT) && peek_is(TOK_IDENT)) {
        adv(); /* consume '.' */
        Token part = expect(TOK_IDENT);
        size_t seg_len = 1 + strlen(part.value); /* '.' + name */
        while (len + seg_len >= cap) {
            cap *= 2;
            name = realloc(name, cap);
        }
        strcat(name, ".");
        strcat(name, part.value);
        len += seg_len;
    }
    return name;
}

/* ------------------------------------------------------------------ */
/*  Type parsing                                                       */
/*  Any IDENT in type position is a type name. This allows typedefs,   */
/*  custom types, etc. without hardcoding keywords.                    */
/* ------------------------------------------------------------------ */
static AstNode *parse_type(void) {
    SrcLoc loc = cur()->loc;

    /* borrow type modifier */
    if (match(TOK_BORROW)) {
        int is_mut = match(TOK_MUT);
        AstNode *inner = parse_type();
        return ast_new_borrow_expr(loc, inner, is_mut);
    }

    /* base type: any identifier or type keyword in type position */
    if (!is_ident_like_type()) {
        error_at(cur()->loc, ERR_PARSER,
                 "expected type name, got '%s'",
                 token_type_name(cur()->type));
    }
    Token t = *adv();
    char *base = t.value;

    /* pointer suffix: type* */
    AstNode *ty = ast_new_ident(loc, base);
    while (match(TOK_STAR)) {
        ty = ast_new_ptr_type(loc, base);
        base = ty->as.ptr_type.base_type;
    }
    return ty;
}

/* ------------------------------------------------------------------ */
/*  Parameter list                                                     */
/* ------------------------------------------------------------------ */
static AstNode *parse_param(void) {
    SrcLoc loc = cur()->loc;
    int is_borrow = match(TOK_BORROW);

    AstNode *type = parse_type();
    Token name = expect(TOK_IDENT);
    return ast_new_param(loc, type->as.ident.name, name.value, is_borrow, 0);
}

static NodeList parse_param_list(void) {
    NodeList params;
    nodelist_init(&params);
    if (cur_is(TOK_RPAREN)) return params;
    nodelist_push(&params, parse_param());
    while (match(TOK_COMMA)) {
        nodelist_push(&params, parse_param());
    }
    return params;
}

/* ------------------------------------------------------------------ */
/*  Block                                                              */
/* ------------------------------------------------------------------ */
static AstNode *parse_block(void) {
    SrcLoc loc = cur()->loc;
    expect(TOK_LBRACE);
    AstNode *blk = ast_new_block(loc);
    while (!cur_is(TOK_RBRACE) && !cur_is(TOK_EOF)) {
        nodelist_push(&blk->as.block.stmts, parse_statement());
    }
    expect(TOK_RBRACE);
    return blk;
}

/* ------------------------------------------------------------------ */
/*  Statements                                                         */
/* ------------------------------------------------------------------ */
static AstNode *parse_return(void) {
    SrcLoc loc = cur()->loc;
    adv();
    AstNode *val = NULL;
    if (!cur_is(TOK_SEMICOLON) && !cur_is(TOK_RBRACE)) {
        val = parse_expr();
    }
    match(TOK_SEMICOLON);
    return ast_new_return(loc, val);
}

static AstNode *parse_if(void) {
    SrcLoc loc = cur()->loc;
    adv();
    AstNode *cond = parse_expr();
    AstNode *then_blk;
    if (cur_is(TOK_LBRACE)) {
        then_blk = parse_block();
    } else {
        then_blk = ast_new_block(cur()->loc);
        nodelist_push(&then_blk->as.block.stmts, parse_statement());
    }
    AstNode *else_blk = NULL;
    if (match(TOK_ELSE)) {
        if (cur_is(TOK_IF)) {
            else_blk = parse_if();
        } else if (cur_is(TOK_LBRACE)) {
            else_blk = parse_block();
        } else {
            else_blk = ast_new_block(cur()->loc);
            nodelist_push(&else_blk->as.block.stmts, parse_statement());
        }
    }
    return ast_new_if(loc, cond, then_blk, else_blk);
}

static AstNode *parse_switch(void) {
    SrcLoc loc = cur()->loc;
    adv();
    expect(TOK_LPAREN);
    AstNode *expr = parse_expr();
    expect(TOK_RPAREN);
    AstNode *sw = ast_new_switch(loc, expr);
    expect(TOK_LBRACE);
    while (!cur_is(TOK_RBRACE) && !cur_is(TOK_EOF)) {
        SrcLoc cloc = cur()->loc;
        expect(TOK_CASE);
        int is_default = match(TOK_STAR);
        AstNode *val = NULL;
        if (!is_default) {
            val = parse_expr();
        }
        AstNode *body = parse_block();
        nodelist_push(&sw->as.switch_stmt.cases,
                      ast_new_case(cloc, val, body, is_default));
    }
    expect(TOK_RBRACE);
    return sw;
}

static AstNode *parse_match(void) {
    SrcLoc loc = cur()->loc;
    adv();
    expect(TOK_LPAREN);
    AstNode *expr = parse_expr();
    expect(TOK_RPAREN);
    AstNode *mt = ast_new_match(loc, expr);
    expect(TOK_LBRACE);
    while (!cur_is(TOK_RBRACE) && !cur_is(TOK_EOF)) {
        SrcLoc cloc = cur()->loc;
        expect(TOK_CASE);
        if (match(TOK_STAR)) {
            AstNode *body = parse_block();
            nodelist_push(&mt->as.match_stmt.cases,
                          ast_new_match_case(cloc, NULL, body, 1));
        } else {
            AstNode *pattern = parse_expr();
            if (match(TOK_IN)) {
                AstNode *range_expr = parse_expr();
                pattern = ast_new_binary(cloc, "in", pattern, range_expr);
            }
            AstNode *body = parse_block();
            nodelist_push(&mt->as.match_stmt.cases,
                          ast_new_match_case(cloc, pattern, body, 0));
        }
    }
    expect(TOK_RBRACE);
    return mt;
}

static AstNode *parse_for(void) {
    SrcLoc loc = cur()->loc;
    adv();
    Token var = expect(TOK_IDENT);
    expect(TOK_IN);
    AstNode *iter = parse_expr();
    AstNode *body = parse_block();
    return ast_new_for(loc, var.value, iter, body);
}

static AstNode *parse_while(void) {
    SrcLoc loc = cur()->loc;
    adv();
    AstNode *cond = parse_expr();
    AstNode *body = parse_block();
    return ast_new_while(loc, cond, body);
}

static AstNode *parse_using(void) {
    SrcLoc loc = cur()->loc;
    adv();
    expect(TOK_LPAREN);
    AstNode *resource = parse_expr();
    expect(TOK_RPAREN);
    AstNode *body = parse_block();
    return ast_new_using(loc, resource, body);
}

static AstNode *parse_unsafe(void) {
    SrcLoc loc = cur()->loc;
    adv();
    AstNode *body = parse_block();
    return ast_new_unsafe(loc, body);
}

/* ------------------------------------------------------------------ */
/*  Variable declaration or expression statement                       */
/* ------------------------------------------------------------------ */
static AstNode *parse_var_decl_or_expr(void) {
    SrcLoc loc = cur()->loc;

    /* mut type name = init; */
    if (cur_is(TOK_MUT)) {
        adv();
        AstNode *type = parse_type();
        Token name = expect(TOK_IDENT);
        AstNode *init = NULL;
        if (match(TOK_ASSIGN)) {
            init = parse_expr();
        }
        match(TOK_SEMICOLON);
        return ast_new_var_decl(loc, type->as.ident.name, name.value, init, 1);
    }

    /* borrow [mut] name = expr; */
    if (cur_is(TOK_BORROW)) {
        adv();
        int is_mut = match(TOK_MUT);
        Token name = expect(TOK_IDENT);
        expect(TOK_ASSIGN);
        AstNode *init = parse_expr();
        match(TOK_SEMICOLON);
        return ast_new_var_decl(loc, is_mut ? "borrow mut" : "borrow",
                                name.value, init, is_mut);
    }

    /* Heuristic: IDENT [*...] IDENT = var decl (type name) */
    if (is_ident_like_type() && (peek_is(TOK_IDENT) || peek_is(TOK_STAR))) {
        AstNode *type = parse_type();
        Token name = expect(TOK_IDENT);
        AstNode *init = NULL;
        if (match(TOK_ASSIGN)) {
            init = parse_expr();
        }
        match(TOK_SEMICOLON);
        return ast_new_var_decl(loc, type->as.ident.name, name.value, init, 0);
    }

    /* Otherwise it's an expression or assignment statement */
    AstNode *expr = parse_expr();
    /* Check for assignment: expr = expr, expr += expr, etc. */
    if (cur_is(TOK_ASSIGN) || cur_is(TOK_PLUS_ASSIGN) ||
        cur_is(TOK_MINUS_ASSIGN) || cur_is(TOK_STAR_ASSIGN) ||
        cur_is(TOK_SLASH_ASSIGN)) {
        Token *op = cur();
        adv();
        AstNode *rhs = parse_expr();
        match(TOK_SEMICOLON);
        return ast_new_assign(loc, expr, rhs, op->value);
    }
    match(TOK_SEMICOLON);
    return expr;
}

/* ------------------------------------------------------------------ */
/*  Statement dispatch                                                  */
/* ------------------------------------------------------------------ */
static AstNode *parse_statement(void) {
    switch (cur()->type) {
        case TOK_RETURN:  return parse_return();
        case TOK_IF:      return parse_if();
        case TOK_SWITCH:  return parse_switch();
        case TOK_MATCH:   return parse_match();
        case TOK_FOR:     return parse_for();
        case TOK_WHILE:   return parse_while();
        case TOK_USING:   return parse_using();
        case TOK_UNSAFE:  return parse_unsafe();
        case TOK_STRUCT:  return parse_struct_decl();
        case TOK_CLASS:   return parse_class_decl();
        default:
            return parse_var_decl_or_expr();
    }
}

/* ------------------------------------------------------------------ */
/*  Top-level declarations                                             */
/* ------------------------------------------------------------------ */
static AstNode *parse_struct_decl(void) {
    SrcLoc loc = cur()->loc;
    adv();
    Token name = expect(TOK_IDENT);
    AstNode *s = ast_new_struct_decl(loc, name.value);
    expect(TOK_LBRACE);
    while (!cur_is(TOK_RBRACE) && !cur_is(TOK_EOF)) {
        AstNode *type = parse_type();
        Token field = expect(TOK_IDENT);
        match(TOK_SEMICOLON);
        AstNode *field_node = ast_new_var_decl(type->loc,
            type->as.ident.name, field.value, NULL, 0);
        nodelist_push(&s->as.struct_decl.fields, field_node);
    }
    expect(TOK_RBRACE);
    match(TOK_SEMICOLON);
    return s;
}

static AstNode *parse_class_decl(void) {
    SrcLoc loc = cur()->loc;
    adv();
    Token name = expect(TOK_IDENT);
    char *parent = NULL;
    if (match(TOK_EXTENDS)) {
        Token p = expect(TOK_IDENT);
        parent = p.value;
    }
    AstNode *c = ast_new_class_decl(loc, name.value, parent);
    expect(TOK_LBRACE);
    while (!cur_is(TOK_RBRACE) && !cur_is(TOK_EOF)) {
        /* Inside class: type name ( ... ) { } = method, type name ; = field */
        if (is_ident_like_type()) {
            AstNode *ret_type = parse_type();
            /* Method/field name: can be IDENT or keyword used as name (e.g. new) */
            Token method_name = *adv();
            if (cur_is(TOK_LPAREN)) {
                expect(TOK_LPAREN);
                NodeList params = parse_param_list();
                expect(TOK_RPAREN);
                AstNode *body = parse_block();
                AstNode *method = ast_new_func_decl(method_name.loc,
                    ret_type->as.ident.name, method_name.value, 1);
                method->as.func_decl.params = params;
                method->as.func_decl.body   = body;
                nodelist_push(&c->as.class_decl.methods, method);
            } else {
                match(TOK_SEMICOLON);
                AstNode *field_node = ast_new_var_decl(ret_type->loc,
                    ret_type->as.ident.name, method_name.value, NULL, 0);
                nodelist_push(&c->as.class_decl.fields, field_node);
            }
        } else {
            error_at(cur()->loc, ERR_PARSER,
                     "expected field or method declaration inside class, got '%s'",
                     token_type_name(cur()->type));
        }
    }
    expect(TOK_RBRACE);
    return c;
}

static AstNode *parse_func_decl(int is_method) {
    SrcLoc loc = cur()->loc;
    AstNode *ret_type = parse_type();
    Token name = expect(TOK_IDENT);
    expect(TOK_LPAREN);
    NodeList params = parse_param_list();
    expect(TOK_RPAREN);
    AstNode *body = parse_block();
    AstNode *fn = ast_new_func_decl(loc, ret_type->as.ident.name, name.value, is_method);
    fn->as.func_decl.params = params;
    fn->as.func_decl.body   = body;
    return fn;
}

static AstNode *parse_top_level(void) {
    switch (cur()->type) {
        case TOK_HASH: {
            adv(); /* consume '#' */
            if (cur_is(TOK_IMPORT)) {
                adv(); /* consume 'import' */
                char *mod_name = NULL;
                int is_header = 0;
                if (cur_is(TOK_STRING_LIT)) {
                    Token path = *adv();
                    mod_name = path.value;
                    size_t len = strlen(mod_name);
                    if (len >= 3 && strcmp(mod_name + len - 3, ".ph") == 0)
                        is_header = 1;
                } else if (cur_is(TOK_IDENT)) {
                    mod_name = parse_dotted_name();
                } else {
                    error_at(cur()->loc, ERR_PARSER,
                             "expected module name or path string after 'import', got '%s'",
                             token_type_name(cur()->type));
                }
                match(TOK_SEMICOLON);
                return ast_new_import(cur()->loc, mod_name, is_header);
            } else if (cur_is(TOK_LINK)) {
                adv(); /* consume 'link' */
                Token path = expect(TOK_STRING_LIT);
                match(TOK_SEMICOLON);
                return ast_new_link(path.loc, path.value);
            } else {
                error_at(cur()->loc, ERR_PARSER,
                         "expected 'import' or 'link' after '#', got '%s'",
                         token_type_name(cur()->type));
            }
        }
        case TOK_STRUCT: return parse_struct_decl();
        case TOK_CLASS:  return parse_class_decl();
        default:
            if (is_ident_like_type()) {
                return parse_func_decl(0);
            }
            error_at(cur()->loc, ERR_PARSER,
                     "expected top-level declaration, got '%s'",
                     token_type_name(cur()->type));
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Program                                                            */
/* ------------------------------------------------------------------ */
static AstNode *parse_program(void) {
    SrcLoc loc = cur()->loc;
    AstNode *prog = ast_new_program(loc);

    while (!cur_is(TOK_EOF)) {
        AstNode *decl = parse_top_level();
        nodelist_push(&prog->as.program.decls, decl);
    }
    return prog;
}

/* ------------------------------------------------------------------ */
/*  Expression parsing                                                 */
/* ------------------------------------------------------------------ */
static AstNode *parse_range(void) {
    AstNode *left = parse_additive();
    if (match(TOK_DOTDOT)) {
        AstNode *right = parse_additive();
        return ast_new_range(left->loc, left, right);
    }
    return left;
}

static AstNode *parse_or(void) {
    AstNode *left = parse_and();
    while (cur_is(TOK_OR)) {
        adv();
        AstNode *right = parse_and();
        left = ast_new_binary(left->loc, "||", left, right);
    }
    return left;
}

static AstNode *parse_and(void) {
    AstNode *left = parse_comparison();
    while (cur_is(TOK_AND)) {
        adv();
        AstNode *right = parse_comparison();
        left = ast_new_binary(left->loc, "&&", left, right);
    }
    return left;
}

static AstNode *parse_comparison(void) {
    AstNode *left = parse_range();
    while (cur_is(TOK_EQ) || cur_is(TOK_NEQ) ||
           cur_is(TOK_LT) || cur_is(TOK_GT) ||
           cur_is(TOK_LE) || cur_is(TOK_GE)) {
        Token *op = cur();
        adv();
        AstNode *right = parse_range();
        left = ast_new_binary(left->loc, op->value, left, right);
    }
    return left;
}

static AstNode *parse_additive(void) {
    AstNode *left = parse_multiplicative();
    while (cur_is(TOK_PLUS) || cur_is(TOK_MINUS)) {
        Token *op = cur();
        adv();
        AstNode *right = parse_multiplicative();
        left = ast_new_binary(left->loc, op->value, left, right);
    }
    return left;
}

static AstNode *parse_multiplicative(void) {
    AstNode *left = parse_unary();
    while (cur_is(TOK_STAR) || cur_is(TOK_SLASH) || cur_is(TOK_PERCENT)) {
        Token *op = cur();
        adv();
        AstNode *right = parse_unary();
        left = ast_new_binary(left->loc, op->value, left, right);
    }
    return left;
}

static AstNode *parse_unary(void) {
    SrcLoc loc = cur()->loc;

    if (match(TOK_MINUS)) {
        return ast_new_unary(loc, "-", parse_unary(), 1);
    }
    if (match(TOK_NOT)) {
        return ast_new_unary(loc, "~", parse_unary(), 1);
    }
    if (match(TOK_STAR)) {
        return ast_new_unary(loc, "*", parse_unary(), 1);
    }
    return parse_postfix();
}

static AstNode *parse_postfix(void) {
    AstNode *expr = parse_primary();
    for (;;) {
        if (match(TOK_LPAREN)) {
            AstNode *call = ast_new_call(expr->loc, expr);
            if (!cur_is(TOK_RPAREN)) {
                nodelist_push(&call->as.call.args, parse_expr());
                while (match(TOK_COMMA)) {
                    nodelist_push(&call->as.call.args, parse_expr());
                }
            }
            expect(TOK_RPAREN);
            expr = call;
        } else if (match(TOK_DOT)) {
            Token member = *adv();
            expr = ast_new_member(expr->loc, expr, member.value);
        } else {
            break;
        }
    }
    return expr;
}

static AstNode *parse_primary(void) {
    SrcLoc loc = cur()->loc;

    switch (cur()->type) {
        case TOK_INT_LIT: {
            Token t = *adv();
            return ast_new_int_lit(loc, strtol(t.value, NULL, 10));
        }
        case TOK_FLOAT_LIT: {
            Token t = *adv();
            return ast_new_float_lit(loc, strtod(t.value, NULL));
        }
        case TOK_STRING_LIT: {
            Token t = *adv();
            return ast_new_string_lit(loc, t.value);
        }
        case TOK_TRUE:  { adv(); return ast_new_int_lit(loc, 1); }
        case TOK_FALSE: { adv(); return ast_new_int_lit(loc, 0); }

        case TOK_IDENT:
        case TOK_INT: case TOK_VOID: case TOK_STRING:
        case TOK_BOOL: case TOK_FLOAT:
        case TOK_LOCK: {
            Token t = *adv();
            return ast_new_ident(loc, t.value);
        }

        case TOK_SELF: {
            adv();
            return ast_new_self_ref(loc);
        }

        case TOK_SUPER: {
            adv();
            if (match(TOK_DOT)) {
                Token method = *adv(); /* accept any token as method name */
                AstNode *sc = ast_new_super_call(loc, method.value);
                expect(TOK_LPAREN);
                if (!cur_is(TOK_RPAREN)) {
                    nodelist_push(&sc->as.super_call.args, parse_expr());
                    while (match(TOK_COMMA)) {
                        nodelist_push(&sc->as.super_call.args, parse_expr());
                    }
                }
                expect(TOK_RPAREN);
                return sc;
            }
            return ast_new_self_ref(loc);
        }

        case TOK_LPAREN: {
            adv();
            AstNode *expr = parse_expr();
            expect(TOK_RPAREN);
            return expr;
        }

        case TOK_LBRACE: {
            /* Struct/array initializer: { expr, expr, ... } */
            AstNode *init = ast_new_call(loc, NULL); /* reuse call node as container */
            adv();
            if (!cur_is(TOK_RBRACE)) {
                nodelist_push(&init->as.call.args, parse_expr());
                while (match(TOK_COMMA)) {
                    nodelist_push(&init->as.call.args, parse_expr());
                }
            }
            expect(TOK_RBRACE);
            return init;
        }

        case TOK_NEW: {
            adv();
            /* Accept any type name (ident or type keyword) after new */
            if (!is_ident_like_type()) {
                error_at(cur()->loc, ERR_PARSER,
                         "expected type name after 'new', got '%s'",
                         token_type_name(cur()->type));
            }
            Token type_name = *adv();
            AstNode *ne = ast_new_new_expr(loc, type_name.value);
            expect(TOK_LPAREN);
            if (!cur_is(TOK_RPAREN)) {
                nodelist_push(&ne->as.new_expr.args, parse_expr());
                while (match(TOK_COMMA)) {
                    nodelist_push(&ne->as.new_expr.args, parse_expr());
                }
            }
            expect(TOK_RPAREN);
            return ne;
        }

        case TOK_SIZEOF: {
            adv();
            expect(TOK_LPAREN);
            if (!is_ident_like_type()) {
                error_at(cur()->loc, ERR_PARSER,
                         "expected type name in sizeof, got '%s'",
                         token_type_name(cur()->type));
            }
            Token type_name = *adv();
            expect(TOK_RPAREN);
            return ast_new_sizeof_expr(loc, type_name.value);
        }

        default:
            error_at(cur()->loc, ERR_PARSER,
                     "expected expression, got '%s'",
                     token_type_name(cur()->type));
    }
    return NULL;
}

static AstNode *parse_expr(void) {
    return parse_or();
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */
AstNode *parse_file(const char *filename, const char *src) {
    Lexer lex;
    lexer_init(&lex, filename, src);
    P = &lex;
    return parse_program();
}
