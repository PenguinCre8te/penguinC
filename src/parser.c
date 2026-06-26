#define _POSIX_C_SOURCE 200809L
#include "parser.h"
#include "ast.h"
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
static AstNode *parse_ternary(void);
static AstNode *parse_assignment(void);
static AstNode *parse_and(void);
static AstNode *parse_comparison(void);
static AstNode *parse_additive(void);
static AstNode *parse_multiplicative(void);
static AstNode *parse_unary(void);
static AstNode *parse_postfix(void);
static AstNode *parse_primary(void);
static AstNode *parse_struct_decl(void);
static AstNode *parse_class_decl(void);
static AstNode *parse_enum_decl(void);
static AstNode *parse_union_decl(void);

/* Any token that could be a type name: keywords like int, void, string, etc.
   AND any user-defined identifier. This allows typedefs and custom types
   without hardcoding. */
static int is_ident_like_type(void) {
    switch (cur()->type) {
        case TOK_IDENT:
        case TOK_INT: case TOK_VOID: case TOK_STRING:
        case TOK_BOOL: case TOK_FLOAT:
        case TOK_LOCK:
        case TOK_CONST: case TOK_VOLATILE:
        case TOK_STRUCT: case TOK_UNION: case TOK_ENUM:
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
    Token t = *cur();
    adv();
    char base[256];
    snprintf(base, sizeof(base), "%s", t.value);

    /* pointer suffix: type* */
    while (match(TOK_STAR)) {
        size_t len = strlen(base);
        if (len < sizeof(base) - 1) {
            base[len] = '*';
            base[len + 1] = '\0';
        }
    }
    return ast_new_ident(loc, base);
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

/* Check if '(' is followed by a keyword type token (for cast detection).
   We only match keyword types (int, void, etc.) not plain identifiers,
   because (ident) is always a grouped expression in our grammar. */
static int looks_like_cast(void) {
    if (!cur_is(TOK_LPAREN)) return 0;
    Token *p = peek();
    switch (p->type) {
        case TOK_INT: case TOK_VOID: case TOK_STRING:
        case TOK_BOOL: case TOK_FLOAT:
        case TOK_IDENT:
            return 1;
        default:
            return 0;
    }
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
        int is_default = 0;

        if (cur_is(TOK_DEFAULT)) {
            /* C-style default: */
            is_default = 1;
            adv();
            expect(TOK_COLON);
        } else {
            expect(TOK_CASE);
            if (cur_is(TOK_STAR)) {
                /* penguinC-style default: case * */
                is_default = 1;
                adv();
            }
        }

        AstNode *val = NULL;
        if (!is_default) {
            val = parse_expr();
        }
        /* Parse case body: support both `case val { ... }` and `case val: ...` */
        AstNode *body;
        if (cur_is(TOK_COLON)) {
            /* C-style: case val: stmts... */
            adv(); /* consume ':' */
            body = ast_new_block(cloc);
            while (!cur_is(TOK_RBRACE) && !cur_is(TOK_EOF) &&
                   !cur_is(TOK_CASE) && !cur_is(TOK_DEFAULT)) {
                nodelist_push(&body->as.block.stmts, parse_statement());
            }
        } else {
            /* penguinC-style: case val { ... } */
            body = parse_block();
        }
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

static AstNode *parse_for_c_style(SrcLoc loc) {
    /* for (init; cond; update) body */
    expect(TOK_LPAREN);

    /* init: can be var decl, expression, or empty */
    AstNode *init = NULL;
    if (!cur_is(TOK_SEMICOLON)) {
        /* Check if this looks like a var decl: type name or mut type name */
        if (cur_is(TOK_MUT) || is_ident_like_type()) {
            /* Try to parse as var decl */
            SrcLoc init_loc = cur()->loc;
            int is_mut = match(TOK_MUT);
            if (is_ident_like_type()) {
                AstNode *type = parse_type();
                Token name = expect(TOK_IDENT);
                AstNode *init_val = NULL;
                if (match(TOK_ASSIGN)) {
                    init_val = parse_expr();
                }
                init = ast_new_var_decl(init_loc, type->as.ident.name, name.value, init_val, is_mut);
            } else {
                /* mut but not followed by type - treat as expression */
                init = parse_expr();
            }
        } else {
            init = parse_expr();
        }
    }
    expect(TOK_SEMICOLON);

    /* cond: expression or empty */
    AstNode *cond = NULL;
    if (!cur_is(TOK_SEMICOLON)) {
        cond = parse_expr();
    }
    expect(TOK_SEMICOLON);

    /* update: expression or empty */
    AstNode *update = NULL;
    if (!cur_is(TOK_RPAREN)) {
        update = parse_expr();
    }
    expect(TOK_RPAREN);

    AstNode *body = parse_block();
    return ast_new_c_style_for(loc, init, cond, update, body);
}

static AstNode *parse_for(void) {
    SrcLoc loc = cur()->loc;
    adv();

    /* C-style for: for (init; cond; update) body */
    if (cur_is(TOK_LPAREN)) {
        return parse_for_c_style(loc);
    }

    /* penguinC style: for ident in expr body */
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

static AstNode *parse_do_while(void) {
    SrcLoc loc = cur()->loc;
    adv(); /* consume 'do' */
    AstNode *body = parse_block();
    expect(TOK_WHILE);
    expect(TOK_LPAREN);
    AstNode *cond = parse_expr();
    expect(TOK_RPAREN);
    match(TOK_SEMICOLON);
    return ast_new_do_while(loc, cond, body);
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

    /* Consume storage class modifiers and type qualifiers (ignored for now) */
    while (cur_is(TOK_STATIC) || cur_is(TOK_EXTERN) || cur_is(TOK_CONST) ||
           cur_is(TOK_VOLATILE) || cur_is(TOK_REGISTER) || cur_is(TOK_INLINE) ||
           cur_is(TOK_AUTO)) {
        adv();
    }

    /* typedef type newname; */
    if (cur_is(TOK_TYPEDEF)) {
        adv(); /* consume 'typedef' */
        AstNode *type = parse_type();
        Token name = expect(TOK_IDENT);
        match(TOK_SEMICOLON);
        return ast_new_typedef_decl(loc, type->as.ident.name, name.value);
    }

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
    match(TOK_SEMICOLON);
    return expr;
}

/* ------------------------------------------------------------------ */
/*  Statement dispatch                                                  */
/* ------------------------------------------------------------------ */
static AstNode *parse_statement(void) {
    /* Check for label: ident followed by ':' */
    if (cur_is(TOK_IDENT) && peek_is(TOK_COLON)) {
        SrcLoc loc = cur()->loc;
        Token name = *cur();
        adv(); /* consume ident */
        adv(); /* consume ':' */
        return ast_new_label(loc, name.value);
    }
    switch (cur()->type) {
        case TOK_RETURN:   return parse_return();
        case TOK_IF:       return parse_if();
        case TOK_SWITCH:   return parse_switch();
        case TOK_MATCH:    return parse_match();
        case TOK_FOR:      return parse_for();
        case TOK_WHILE:    return parse_while();
        case TOK_DO:       return parse_do_while();
        case TOK_USING:    return parse_using();
        case TOK_UNSAFE:   return parse_unsafe();
        case TOK_STRUCT:   return parse_struct_decl();
        case TOK_UNION:    return parse_union_decl();
        case TOK_ENUM:     return parse_enum_decl();
        case TOK_CLASS:    return parse_class_decl();
        case TOK_BREAK: {
            SrcLoc loc = cur()->loc;
            adv();
            match(TOK_SEMICOLON);
            return ast_new_break(loc);
        }
        case TOK_CONTINUE: {
            SrcLoc loc = cur()->loc;
            adv();
            match(TOK_SEMICOLON);
            return ast_new_continue(loc);
        }
        case TOK_GOTO: {
            SrcLoc loc = cur()->loc;
            adv();
            Token label = expect(TOK_IDENT);
            match(TOK_SEMICOLON);
            return ast_new_goto(loc, label.value);
        }
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

static AstNode *parse_enum_decl(void) {
    SrcLoc loc = cur()->loc;
    adv(); /* consume 'enum' */
    Token name = expect(TOK_IDENT);
    AstNode *e = ast_new_enum_decl(loc, name.value);
    expect(TOK_LBRACE);
    while (!cur_is(TOK_RBRACE) && !cur_is(TOK_EOF)) {
        Token val_name = expect(TOK_IDENT);
        AstNode *val_node;
        if (match(TOK_ASSIGN)) {
            AstNode *val_expr = parse_expr();
            /* Store as a binary: name = expr */
            val_node = ast_new_binary(val_name.loc, "=",
                ast_new_ident(val_name.loc, val_name.value), val_expr);
        } else {
            val_node = ast_new_ident(val_name.loc, val_name.value);
        }
        nodelist_push(&e->as.enum_decl.values, val_node);
        match(TOK_COMMA);
    }
    expect(TOK_RBRACE);
    match(TOK_SEMICOLON);
    return e;
}

static AstNode *parse_union_decl(void) {
    SrcLoc loc = cur()->loc;
    adv(); /* consume 'union' */
    Token name = expect(TOK_IDENT);
    AstNode *u = ast_new_union_decl(loc, name.value);
    expect(TOK_LBRACE);
    while (!cur_is(TOK_RBRACE) && !cur_is(TOK_EOF)) {
        AstNode *type = parse_type();
        Token field = expect(TOK_IDENT);
        match(TOK_SEMICOLON);
        AstNode *field_node = ast_new_var_decl(type->loc,
            type->as.ident.name, field.value, NULL, 0);
        nodelist_push(&u->as.union_decl.fields, field_node);
    }
    expect(TOK_RBRACE);
    match(TOK_SEMICOLON);
    return u;
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
                Token method_name = *cur();
                adv();
            if (cur_is(TOK_LPAREN)) {
                expect(TOK_LPAREN);
                NodeList params = parse_param_list();
                expect(TOK_RPAREN);
                AstNode *body = parse_block();
                AstNode *method = ast_new_func_decl(method_name.loc,
                    ret_type->as.ident.name, method_name.value, 1);
                method->as.func_decl.class_name = strdup(name.value);
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
                    Token path = *cur();
                    adv();
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
                return NULL;
            }
        }
        case TOK_STRUCT: return parse_struct_decl();
        case TOK_UNION:  return parse_union_decl();
        case TOK_ENUM:   return parse_enum_decl();
        case TOK_CLASS:  return parse_class_decl();
        case TOK_TYPEDEF: {
            SrcLoc loc = cur()->loc;
            adv(); /* consume 'typedef' */
            AstNode *type = parse_type();
            Token name = expect(TOK_IDENT);
            match(TOK_SEMICOLON);
            return ast_new_typedef_decl(loc, type->as.ident.name, name.value);
        }
        default:
            /* Skip storage class modifiers at top level */
            while (cur_is(TOK_STATIC) || cur_is(TOK_EXTERN) || cur_is(TOK_INLINE)) {
                adv();
            }
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

static AstNode *parse_ternary(void) {
    AstNode *cond = parse_or();
    if (cur_is(TOK_QUESTION)) {
        SrcLoc loc = cond->loc;
        adv(); /* consume '?' */
        AstNode *then_expr = parse_expr(); /* right-associative */
        expect(TOK_COLON);
        AstNode *else_expr = parse_ternary();
        return ast_new_ternary(loc, cond, then_expr, else_expr);
    }
    return cond;
}

static AstNode *parse_assignment(void) {
    AstNode *lhs = parse_ternary();
    if (cur_is(TOK_ASSIGN) || cur_is(TOK_PLUS_ASSIGN) ||
        cur_is(TOK_MINUS_ASSIGN) || cur_is(TOK_STAR_ASSIGN) ||
        cur_is(TOK_SLASH_ASSIGN)) {
        SrcLoc loc = lhs->loc;
        char *op = strdup(cur()->value);
        adv();
        AstNode *rhs = parse_assignment(); /* right-associative */
        return ast_new_assign(loc, lhs, rhs, op);
    }
    return lhs;
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
        char *op = strdup(cur()->value);
        adv();
        AstNode *right = parse_range();
        left = ast_new_binary(left->loc, op, left, right);
    }
    return left;
}

static AstNode *parse_additive(void) {
    AstNode *left = parse_multiplicative();
    while (cur_is(TOK_PLUS) || cur_is(TOK_MINUS)) {
        char *op = strdup(cur()->value);
        adv();
        AstNode *right = parse_multiplicative();
        left = ast_new_binary(left->loc, op, left, right);
    }
    return left;
}

static AstNode *parse_multiplicative(void) {
    AstNode *left = parse_unary();
    while (cur_is(TOK_STAR) || cur_is(TOK_SLASH) || cur_is(TOK_PERCENT)) {
        char *op = strdup(cur()->value);
        adv();
        AstNode *right = parse_unary();
        left = ast_new_binary(left->loc, op, left, right);
    }
    return left;
}

static AstNode *parse_unary(void) {
    SrcLoc loc = cur()->loc;

    if (match(TOK_MINUS)) {
        return ast_new_unary(loc, "-", parse_unary(), 1);
    }
    if (match(TOK_TILDE)) {
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
            Token member = *cur();
            adv();
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
            Token t = *cur();
            adv();
            return ast_new_int_lit(loc, strtol(t.value, NULL, 10));
        }
        case TOK_FLOAT_LIT: {
            Token t = *cur();
            adv();
            return ast_new_float_lit(loc, strtod(t.value, NULL));
        }
        case TOK_STRING_LIT: {
            Token t = *cur();
            adv();
            return ast_new_string_lit(loc, t.value);
        }
        case TOK_TRUE:  { adv(); return ast_new_int_lit(loc, 1); }
        case TOK_FALSE: { adv(); return ast_new_int_lit(loc, 0); }

        case TOK_NULL_LIT: {
            adv();
            return ast_new_ident(loc, "NULL");
        }

        case TOK_IDENT:
        case TOK_INT: case TOK_VOID: case TOK_STRING:
        case TOK_BOOL: case TOK_FLOAT:
        case TOK_LOCK: {
            Token t = *cur();
            adv();
            return ast_new_ident(loc, t.value);
        }

        case TOK_SELF: {
            adv();
            return ast_new_self_ref(loc);
        }

        case TOK_SUPER: {
            adv();
            if (match(TOK_DOT)) {
                Token method = *cur();
                adv(); /* accept any token as method name */
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
            /* Check if this looks like a cast: (type)expr */
            if (looks_like_cast()) {
                adv(); /* consume '(' */
                /* Parse the type name (could be multi-word like "long int") */
                SrcLoc cast_loc = cur()->loc;
                /* Build type name from tokens */
                Token first = *cur();
                adv();
                char type_buf[256];
                snprintf(type_buf, sizeof(type_buf), "%s", first.value);
                /* Handle pointer types: (int *) */
                while (match(TOK_STAR)) {
                    strncat(type_buf, "*", sizeof(type_buf) - strlen(type_buf) - 1);
                }
                expect(TOK_RPAREN);
                AstNode *operand = parse_unary();
                return ast_new_cast(cast_loc, type_buf, operand);
            }
            /* Grouped expression */
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
            Token type_name = *cur();
            adv();
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
            Token type_name = *cur();
            adv();
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
    return parse_assignment();
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
