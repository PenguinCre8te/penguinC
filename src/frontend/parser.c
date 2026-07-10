#define _POSIX_C_SOURCE 200809L
#include "parser.h"
#include "ast.h"
#include "error.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

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
/*  CamelCase enforcement                                              */
/* ------------------------------------------------------------------ */
static void check_var_camel_case(SrcLoc loc, const char *name) {
    if (!name || !*name) return;
    /* Skip reserved names */
    if (strcmp(name, "self") == 0 || strcmp(name, "super") == 0 ||
        strcmp(name, "main") == 0 || strcmp(name, "NULL") == 0)
        return;
    /* Must start with lowercase */
    if (name[0] >= 'A' && name[0] <= 'Z') {
        warn_at(loc, "variable '%s' should use camelCase (start with lowercase)", name);
        return;
    }
    /* Must not contain underscores */
    for (const char *p = name; *p; p++) {
        if (*p == '_') {
            warn_at(loc, "variable '%s' should use camelCase (no underscores)", name);
            return;
        }
    }
}

static void check_class_camel_case(SrcLoc loc, const char *name) {
    if (!name || !*name) return;
    /* Must start with uppercase */
    if (name[0] >= 'a' && name[0] <= 'z') {
        warn_at(loc, "class/struct '%s' should use PascalCase (start with uppercase)", name);
        return;
    }
    /* Must not contain underscores */
    for (const char *p = name; *p; p++) {
        if (*p == '_') {
            warn_at(loc, "class/struct '%s' should use PascalCase (no underscores)", name);
            return;
        }
    }
}

static void check_func_camel_case(SrcLoc loc, const char *name) {
    if (!name || !*name) return;
    if (strcmp(name, "main") == 0) return;
    /* Must not contain underscores */
    for (const char *p = name; *p; p++) {
        if (*p == '_') {
            warn_at(loc, "function '%s' should use camelCase (no underscores)", name);
            return;
        }
    }
}

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

/* Forward declarations */
static char *resolve_module_path(const char *module);
static AstNode *parse_header_file(const char *filepath);
static AstNode *parse_pc_file(const char *filepath);

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
/*  Import directive parsing                                           */
/*  Supports:                                                          */
/*    #import std.console.print                                        */
/*    #import std.console.*                                            */
/*    #import std.console as io                                        */
/*    #import std.console.(print, println)                             */
/*    #import "./other.pc"                                             */
/*    #import "./other.pc" as otherscript                              */
/*    #import "./other.pc".penguin as quack                            */
/*    #import "./other.pc".*                                           */
/* ------------------------------------------------------------------ */
static AstNode *parse_import_directive(void) {
    SrcLoc loc = cur()->loc;
    char *mod_name = NULL;
    int is_header = 0;
    char *alias = NULL;
    int wildcard = 0;
    NodeList selected_names;
    nodelist_init(&selected_names);
    char *submodule = NULL;

    /* Parse the import target: string literal or dotted identifier */
    if (cur_is(TOK_STRING_LIT)) {
        Token path = *cur();
        adv();
        mod_name = path.value;
        size_t len = strlen(mod_name);
        if (len >= 3 && strcmp(mod_name + len - 3, ".ph") == 0) {
            is_header = 1;
        }
    } else if (cur_is(TOK_IDENT)) {
        mod_name = parse_dotted_name();
    } else {
        error_at(cur()->loc, ERR_PARSER,
                 "expected module name or path string after 'import', got '%s'",
                 token_type_name(cur()->type));
        match(TOK_SEMICOLON);
        return ast_new_import(loc, "error", 0);
    }

    /* Parse optional submodule: .ident after the target */
    if (cur_is(TOK_DOT) && peek_is(TOK_IDENT)) {
        /* Check if next-next is 'as', '(' or ';' — if so, this is submodule */
        Lexer saved = *P;
        adv(); /* consume '.' */
        Token sub = *cur();
        adv(); /* consume submodule ident */

        /* If followed by 'as', '.', '(', ';', or EOF — it's a submodule */
        if (cur_is(TOK_AS) || cur_is(TOK_DOT) || cur_is(TOK_LPAREN) ||
            cur_is(TOK_SEMICOLON) || cur_is(TOK_EOF)) {
            submodule = sub.value;
        } else {
            /* Not a submodule — back up and treat as part of dotted name */
            *P = saved;
            free(sub.value);
            /* Continue parsing dotted name segments */
            while (cur_is(TOK_DOT) && peek_is(TOK_IDENT)) {
                /* Re-parse the dot + ident we just backed over */
                saved = *P;
                adv(); /* dot */
                Token part = *cur();
                adv();
                size_t old_len = strlen(mod_name);
                size_t seg_len = 1 + strlen(part.value);
                mod_name = realloc(mod_name, old_len + seg_len + 1);
                mod_name[old_len] = '.';
                strcpy(mod_name + old_len + 1, part.value);
            }
        }
    }

    /* Parse optional .* wildcard */
    if (cur_is(TOK_DOT) && peek_is(TOK_STAR)) {
        adv(); /* dot */
        adv(); /* star */
        wildcard = 1;
    }

    /* Parse optional .(name1, name2) selective import (dot before paren) */
    if (cur_is(TOK_DOT) && peek_is(TOK_LPAREN)) {
        adv(); /* dot */
    }

    /* Parse optional (name1, name2) selective import */
    if (cur_is(TOK_LPAREN)) {
        adv(); /* consume '(' */
        if (!cur_is(TOK_RPAREN)) {
            Token name = expect(TOK_IDENT);
            nodelist_push(&selected_names, ast_new_ident(name.loc, name.value));
            while (match(TOK_COMMA)) {
                name = expect(TOK_IDENT);
                nodelist_push(&selected_names, ast_new_ident(name.loc, name.value));
            }
        }
        expect(TOK_RPAREN);
    }

    /* Parse optional "as" alias */
    if (match(TOK_AS)) {
        Token alias_tok = expect(TOK_IDENT);
        alias = alias_tok.value;
    }

    match(TOK_SEMICOLON);

    /* Build the import node */
    AstNode *node = ast_new_import(loc, mod_name, is_header);
    node->as.import.alias = alias;
    node->as.import.wildcard = wildcard;
    node->as.import.selected_names = selected_names;
    node->as.import.submodule = submodule;

    /* For .ph files, parse the header file and merge info */
    if (is_header) {
        AstNode *hdr = parse_header_file(mod_name);
        if (hdr) {
            /* Transfer func_maps and links from header to our import node */
            for (size_t i = 0; i < hdr->as.import.func_maps.count; i++)
                nodelist_push(&node->as.import.func_maps, hdr->as.import.func_maps.items[i]);
            for (size_t i = 0; i < hdr->as.import.links.count; i++)
                nodelist_push(&node->as.import.links, hdr->as.import.links.items[i]);
            free(hdr->as.import.module);
            free(hdr);
        }
    }

    /* For .pc files, parse the source file and extract function signatures */
    {
        size_t len = strlen(mod_name);
        if (len >= 3 && strcmp(mod_name + len - 3, ".pc") == 0) {
            AstNode *hdr = parse_pc_file(mod_name);
            if (hdr) {
                for (size_t i = 0; i < hdr->as.import.func_maps.count; i++)
                    nodelist_push(&node->as.import.func_maps, hdr->as.import.func_maps.items[i]);
                for (size_t i = 0; i < hdr->as.import.links.count; i++)
                    nodelist_push(&node->as.import.links, hdr->as.import.links.items[i]);
                free(hdr->as.import.module);
                free(hdr);
            }
        }
    }

    /* For stdlib modules, try to resolve to a .ph file */
    if (!is_header && !strchr(mod_name, '/')) {
        char *ph_path = resolve_module_path(mod_name);
        if (ph_path) {
            AstNode *hdr = parse_header_file(ph_path);
            free(ph_path);
            if (hdr) {
                for (size_t i = 0; i < hdr->as.import.func_maps.count; i++)
                    nodelist_push(&node->as.import.func_maps, hdr->as.import.func_maps.items[i]);
                for (size_t i = 0; i < hdr->as.import.links.count; i++)
                    nodelist_push(&node->as.import.links, hdr->as.import.links.items[i]);
                free(hdr->as.import.module);
                free(hdr);
            }
        }
    }

    return node;
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

    /* dotted type: module.Type (e.g. threads.thread) */
    if (cur_is(TOK_DOT) && peek_is(TOK_IDENT)) {
        adv(); /* consume '.' */
        Token part = expect(TOK_IDENT);
        size_t len = strlen(base);
        snprintf(base + len, sizeof(base) - len, ".%s", part.value);
    }

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
    check_var_camel_case(name.loc, name.value);
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
        /* Check if this looks like a var decl: type name or [shared] [mut] type name */
        if (cur_is(TOK_SHARED) || cur_is(TOK_MUT) || is_ident_like_type()) {
            /* Try to parse as var decl */
            SrcLoc init_loc = cur()->loc;
            int is_shared = match(TOK_SHARED);
            int is_mut = match(TOK_MUT);
            if (is_ident_like_type()) {
                AstNode *type = parse_type();
                Token name = expect(TOK_IDENT);
                AstNode *init_val = NULL;
                if (match(TOK_ASSIGN)) {
                    init_val = parse_expr();
                }
                init = ast_new_var_decl(init_loc, type->as.ident.name, name.value, init_val, is_mut, is_shared);
            } else {
                /* mut/shared but not followed by type - treat as expression */
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

    /* Check for 'using(expr as var) { body }' syntax */
    const char *var_name = NULL;
    if (match(TOK_AS)) {
        Token var = expect(TOK_IDENT);
        var_name = strdup(var.value);
    }

    expect(TOK_RPAREN);
    AstNode *body = parse_block();
    return ast_new_using(loc, resource, body, var_name);
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

    /* [shared] [mut] type name = init; */
    if (cur_is(TOK_SHARED) || cur_is(TOK_MUT)) {
        int is_shared = match(TOK_SHARED);
        int is_mut = match(TOK_MUT);
        AstNode *type = parse_type();
        Token name = expect(TOK_IDENT);
        check_var_camel_case(name.loc, name.value);
        AstNode *init = NULL;
        if (match(TOK_ASSIGN)) {
            init = parse_expr();
        }
        match(TOK_SEMICOLON);
        return ast_new_var_decl(loc, type->as.ident.name, name.value, init, is_mut, is_shared);
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
                                name.value, init, is_mut, 0);
    }

    /* Heuristic: IDENT [*...] IDENT = var decl (type name) */
    /* Also handles module.Type IDENT = var decl (e.g. threads.thread thread = ...) */
    if (is_ident_like_type() && (peek_is(TOK_IDENT) || peek_is(TOK_STAR))) {
        AstNode *type = parse_type();
        Token name = expect(TOK_IDENT);
        check_var_camel_case(name.loc, name.value);
        AstNode *init = NULL;
        if (match(TOK_ASSIGN)) {
            init = parse_expr();
        }
        match(TOK_SEMICOLON);
        return ast_new_var_decl(loc, type->as.ident.name, name.value, init, 0, 0);
    }

    /* Dotted type heuristic: IDENT.IDENT IDENT = var decl (e.g. threads.thread thread) */
    if (is_ident_like_type() && peek_is(TOK_DOT)) {
        /* Save entire lexer state */
        Lexer saved_lex = *P;

        adv(); /* skip first ident */
        adv(); /* skip '.' */
        int is_dotted_type = cur_is(TOK_IDENT);
        if (is_dotted_type) {
            adv(); /* skip type name ident */
            /* After module.Type, current should be IDENT (var name) or * */
            is_dotted_type = cur_is(TOK_IDENT) || cur_is(TOK_STAR);
        }

        /* Restore lexer state */
        *P = saved_lex;

        if (is_dotted_type) {
            AstNode *type = parse_type();
            Token name = expect(TOK_IDENT);
            check_var_camel_case(name.loc, name.value);
            AstNode *init = NULL;
            if (match(TOK_ASSIGN)) {
                init = parse_expr();
            }
            match(TOK_SEMICOLON);
            return ast_new_var_decl(loc, type->as.ident.name, name.value, init, 0, 0);
        }
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
    check_class_camel_case(name.loc, name.value);
    AstNode *s = ast_new_struct_decl(loc, name.value);
    expect(TOK_LBRACE);
    while (!cur_is(TOK_RBRACE) && !cur_is(TOK_EOF)) {
        AstNode *type = parse_type();
        Token field = expect(TOK_IDENT);
        match(TOK_SEMICOLON);
        AstNode *field_node = ast_new_var_decl(type->loc,
            type->as.ident.name, field.value, NULL, 0, 0);
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
    check_class_camel_case(name.loc, name.value);
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
    check_class_camel_case(name.loc, name.value);
    AstNode *u = ast_new_union_decl(loc, name.value);
    expect(TOK_LBRACE);
    while (!cur_is(TOK_RBRACE) && !cur_is(TOK_EOF)) {
        AstNode *type = parse_type();
        Token field = expect(TOK_IDENT);
        match(TOK_SEMICOLON);
        AstNode *field_node = ast_new_var_decl(type->loc,
            type->as.ident.name, field.value, NULL, 0, 0);
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
    check_class_camel_case(name.loc, name.value);
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
                    ret_type->as.ident.name, method_name.value, NULL, 0, 0);
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
    if (!is_method) check_func_camel_case(name.loc, name.value);
    expect(TOK_LPAREN);
    NodeList params = parse_param_list();
    expect(TOK_RPAREN);
    AstNode *body = parse_block();
    AstNode *fn = ast_new_func_decl(loc, ret_type->as.ident.name, name.value, is_method);
    fn->as.func_decl.params = params;
    fn->as.func_decl.body   = body;
    return fn;
}

/* ------------------------------------------------------------------ */
/*  .ph header file parsing                                            */
/* ------------------------------------------------------------------ */

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *end = s + strlen(s) - 1;
    while (end > s && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r'))
        *end-- = '\0';
    return s;
}

/* Resolve a dotted module name to a file path.
 * e.g. "std.io" -> "$STDLIB/io/io.ph" or "stdlib/io/io.ph" */
static char *resolve_module_path(const char *module) {
    const char *stdlib = getenv("STDLIB");
    if (!stdlib) stdlib = "stdlib";

    /* Build path: stdlib/<last_segment>/<last_segment>.ph */
    const char *last = strrchr(module, '.');
    last = last ? last + 1 : module;

    char path[512];
    snprintf(path, sizeof(path), "%s/%s/%s.ph", stdlib, last, last);

    /* Check if file exists */
    FILE *f = fopen(path, "r");
    if (f) {
        fclose(f);
        return strdup(path);
    }

    /* Fallback: try stdlib/<module_with_slashes>/<last>.ph */
    char mod_path[256];
    strncpy(mod_path, module, sizeof(mod_path) - 1);
    mod_path[sizeof(mod_path) - 1] = '\0';
    for (char *p = mod_path; *p; p++) {
        if (*p == '.') *p = '/';
    }
    snprintf(path, sizeof(path), "%s/%s/%s.ph", stdlib, mod_path, last);

    f = fopen(path, "r");
    if (f) {
        fclose(f);
        return strdup(path);
    }

    return NULL;
}

static AstNode *parse_header_file(const char *filepath) {
    FILE *f = fopen(filepath, "r");
    if (!f) return NULL;

    /* Create an import node to hold the parsed info */
    SrcLoc loc = {filepath, 1, 1};
    AstNode *import_node = ast_new_import(loc, filepath, 1);

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char *s = trim(line);

        /* Skip empty lines and comments */
        if (*s == '\0' || *s == '#') {
            /* Check for #link directive */
            if (strncmp(s, "#link", 5) == 0) {
                s += 5;
                while (*s == ' ' || *s == '\t') s++;
                if (*s == '"') {
                    s++;
                    char *end = strchr(s, '"');
                    if (end) {
                        *end = '\0';
                        /* Resolve link path to absolute path */
                        /* If it's just a library name (no path separators or .o), pass as-is */
                        if (strchr(s, '/') || s[0] == '.' || strcmp(s + strlen(s) - 2, ".o") == 0) {
                            char dir[1024];
                            /* Get absolute directory of the .ph file */
                            char abs_ph[1024];
                            if (realpath(filepath, abs_ph)) {
                                const char *last_slash = strrchr(abs_ph, '/');
                                if (last_slash) {
                                    size_t dirlen = last_slash - abs_ph + 1;
                                    snprintf(dir, sizeof(dir), "%.*s", (int)dirlen, abs_ph);
                                } else {
                                    strcpy(dir, "./");
                                }
                            } else {
                                /* Fallback: use STDLIB env var */
                                const char *stdlib = getenv("STDLIB");
                                if (!stdlib) stdlib = "stdlib";
                                snprintf(dir, sizeof(dir), "%s/", stdlib);
                            }
                            char resolved[1024];
                            snprintf(resolved, sizeof(resolved), "%s%s", dir, s);
                            nodelist_push(&import_node->as.import.links,
                                ast_new_link(loc, resolved));
                        } else {
                            /* Library name like "libc" — pass as-is for -l linking */
                            nodelist_push(&import_node->as.import.links,
                                ast_new_link(loc, s));
                        }
                    }
                }
            }
            continue;
        }

        /* Check for class declaration: "class name {" */
        if (strncmp(s, "class ", 6) == 0) {
            char *cs = s + 6;
            while (*cs == ' ' || *cs == '\t') cs++;
            /* Extract class name */
            char class_name[256];
            size_t cn_len = 0;
            while (*cs && *cs != ' ' && *cs != '\t' && *cs != '{' && cn_len < sizeof(class_name) - 1)
                class_name[cn_len++] = *cs++;
            class_name[cn_len] = '\0';

            /* Find opening brace */
            char *brace = strchr(cs, '{');
            if (!brace) {
                /* Single-line class? Skip */
                continue;
            }

            /* Create class declaration node */
            AstNode *class_node = ast_new_class_decl(loc, class_name, NULL);

            /* Parse methods inside the class block */
            while (fgets(line, sizeof(line), f)) {
                char *ms = trim(line);
                if (*ms == '\0') continue;
                /* Check for closing brace */
                if (*ms == '}') break;
                /* Skip comments */
                if (*ms == '#') continue;

                /* Parse method mapping: ret_type name(args) => c_name; */
                char *arrow = strstr(ms, "=>");
                if (arrow) {
                    *arrow = '\0';
                    char *decl = trim(ms);
                    char *c_name = trim(arrow + 2);
                    size_t clen = strlen(c_name);
                    if (clen > 0 && c_name[clen - 1] == ';')
                        c_name[clen - 1] = '\0';
                    c_name = trim(c_name);

                    char *open_paren = strchr(decl, '(');
                    if (open_paren) {
                        *open_paren = '\0';
                        /* Extract method name (before the paren) */
                        char *name_end = open_paren - 1;
                        while (name_end > decl && (*name_end == ' ' || *name_end == '\t'))
                            name_end--;
                        char *name_start = name_end;
                        while (name_start > decl && *(name_start - 1) != ' ' &&
                               *(name_start - 1) != '\t')
                            name_start--;

                        char method_name[256];
                        size_t name_len = name_end - name_start + 1;
                        if (name_len < sizeof(method_name)) {
                            memcpy(method_name, name_start, name_len);
                            method_name[name_len] = '\0';

                            /* Extract param types for mangled name */
                            char *args_str = open_paren + 1;
                            char *close_paren = strchr(args_str, ')');
                            if (close_paren) *close_paren = '\0';

                            /* Extract return type - try '->' after paren, else before method name */
                            char ret_type[256] = "void";
                            {
                                /* Try '-> ret_type' after closing paren first */
                                char *after_args = close_paren ? close_paren + 1 : args_str;
                                char *arrow2 = strstr(after_args, "->");
                                if (arrow2) {
                                    char *rt_start = arrow2 + 2;
                                    while (*rt_start == ' ' || *rt_start == '\t') rt_start++;
                                    char *rt_end = rt_start;
                                    while (*rt_end && *rt_end != ' ' && *rt_end != '\t' &&
                                           *rt_end != ';' && *rt_end != '\n' && *rt_end != '\r')
                                        rt_end++;
                                    size_t rt_len = rt_end - rt_start;
                                    if (rt_len > 0 && rt_len < sizeof(ret_type)) {
                                        memcpy(ret_type, rt_start, rt_len);
                                        ret_type[rt_len] = '\0';
                                    }
                                } else {
                                    /* Fallback: extract from text before method name */
                                    size_t rt_len = name_start - decl;
                                    while (rt_len > 0 && (decl[rt_len-1] == ' ' || decl[rt_len-1] == '\t'))
                                        rt_len--;
                                    if (rt_len > 0 && rt_len < sizeof(ret_type)) {
                                        memcpy(ret_type, decl, rt_len);
                                        ret_type[rt_len] = '\0';
                                    }
                                }
                            }

                            char mangled[512];
                            size_t mpos = 0;
                            mpos += snprintf(mangled + mpos, sizeof(mangled) - mpos, "_pC%s", method_name);

                            const char *param_types[32];
                            size_t param_count = 0;

                            char *arg = trim(args_str);
                            while (*arg) {
                                char *comma = strchr(arg, ',');
                                if (comma) *comma = '\0';
                                char *a = trim(arg);
                                char *space = strchr(a, ' ');
                                if (space) {
                                    *space = '\0';
                                    const char *t = a;
                                    if (param_count < 32)
                                        param_types[param_count++] = t;
                                    if (strchr(a, '*'))
                                        mangled[mpos++] = 'p';
                                    else if (strcmp(t, "int") == 0 || strcmp(t, "long") == 0) mangled[mpos++] = 'i';
                                    else if (strcmp(t, "float") == 0) mangled[mpos++] = 'f';
                                    else if (strcmp(t, "bool") == 0)  mangled[mpos++] = 'b';
                                    else if (strcmp(t, "string") == 0) mangled[mpos++] = 's';
                                    else if (strcmp(t, "void") == 0)  mangled[mpos++] = 'v';
                                    else {
                                        size_t tlen = strlen(t);
                                        mangled[mpos++] = 'p';
                                        if (tlen > 0) mangled[mpos++] = t[0];
                                    }
                                }
                                if (comma) { arg = comma + 1; }
                                else break;
                            }
                            mangled[mpos] = '\0';

                            /* Create method func_map entry */
                            AstNode *fm = ast_new_func_map(loc, mangled, c_name);
                            fm->as.func_map.ret_type = strdup(ret_type);
                            fm->as.func_map.orig_name = strdup(method_name);
                            fm->as.func_map.param_count = param_count;
                            if (param_count > 0) {
                                fm->as.func_map.param_types = malloc(param_count * sizeof(char *));
                                for (size_t pi = 0; pi < param_count; pi++)
                                    fm->as.func_map.param_types[pi] = strdup(param_types[pi]);
                            }

                            /* Create method declaration node */
                            AstNode *method = ast_new_func_decl(loc, ret_type, method_name, 1);
                            method->as.func_decl.class_name = strdup(class_name);
                            nodelist_push(&class_node->as.class_decl.methods, method);

                            /* Store func_map on the class node for later registration */
                            nodelist_push(&class_node->as.class_decl.methods, fm);
                        }
                    }
                }
            }
            nodelist_push(&import_node->as.import.func_maps, class_node);
            continue;
        }

        /* Parse function mapping: ret_type name(args) => c_name; */
        char *arrow = strstr(s, "=>");
        if (arrow) {
            /* Extract pc_name: everything before =>, minus the type */
            *arrow = '\0';
            char *decl = trim(s);
            char *c_name = trim(arrow + 2);

            /* Remove trailing semicolon from c_name */
            size_t clen = strlen(c_name);
            if (clen > 0 && c_name[clen - 1] == ';')
                c_name[clen - 1] = '\0';
            c_name = trim(c_name);

            /* Extract the function name from the declaration.
             * Format: "ret_type name(args)" or "ret_type name(args) const"
             * We need the 'name' part and the arg types for mangling. */
            char *open_paren = strchr(decl, '(');
            if (open_paren) {
                *open_paren = '\0';
                /* decl is now "ret_type name" — extract both */
                char *name_end = open_paren - 1;
                while (name_end > decl && (*name_end == ' ' || *name_end == '\t'))
                    name_end--;
                char *name_start = name_end;
                while (name_start > decl && *(name_start - 1) != ' ' &&
                       *(name_start - 1) != '\t')
                    name_start--;

                char func_name[256];
                size_t name_len = name_end - name_start + 1;
                if (name_len < sizeof(func_name)) {
                    memcpy(func_name, name_start, name_len);
                    func_name[name_len] = '\0';

                    /* Extract return type: everything before the function name */
                    char ret_type[256] = "void";
                    {
                        size_t rt_len = name_start - decl;
                        while (rt_len > 0 && (decl[rt_len-1] == ' ' || decl[rt_len-1] == '\t'))
                            rt_len--;
                        if (rt_len < sizeof(ret_type) && rt_len > 0) {
                            memcpy(ret_type, decl, rt_len);
                            ret_type[rt_len] = '\0';
                        }
                    }

                    /* Extract parameter types for name mangling */
                    char *args_str = open_paren + 1;
                    char *close_paren = strchr(args_str, ')');
                    if (close_paren) *close_paren = '\0';

                    /* Build mangled name: _pC<name><type_chars> */
                    char mangled[512];
                    size_t mpos = 0;
                    mpos += snprintf(mangled + mpos, sizeof(mangled) - mpos, "_pC%s", func_name);

                    /* Collect param types for LLVM type construction */
                    const char *param_types[32];
                    size_t param_count = 0;

                    /* Parse comma-separated args */
                    char *arg = trim(args_str);
                    while (*arg) {
                        char *comma = strchr(arg, ',');
                        if (comma) *comma = '\0';
                        char *a = trim(arg);
                        /* Extract type: everything before the param name */
                        char *space = strchr(a, ' ');
                        if (space) {
                            *space = '\0';
                            const char *t = a;
                            /* Store param type for LLVM type building */
                            if (param_count < 32)
                                param_types[param_count++] = t;
                            /* If type has *, it's a pointer → mangle as 'p' */
                            if (strchr(a, '*')) {
                                mangled[mpos++] = 'p';
                            } else if (strcmp(t, "int") == 0 || strcmp(t, "long") == 0)  mangled[mpos++] = 'i';
                            else if (strcmp(t, "float") == 0) mangled[mpos++] = 'f';
                            else if (strcmp(t, "bool") == 0)  mangled[mpos++] = 'b';
                            else if (strcmp(t, "string") == 0) mangled[mpos++] = 's';
                            else if (strcmp(t, "void") == 0)  mangled[mpos++] = 'v';
                            else {
                                size_t tlen = strlen(t);
                                mangled[mpos++] = 'p';
                                if (tlen > 0) mangled[mpos++] = t[0];
                            }
                        }
                        if (comma) { arg = comma + 1; }
                        else break;
                    }
                    mangled[mpos] = '\0';

                    /* Store the mangled name as pc_name with type info */
                    AstNode *fm = ast_new_func_map(loc, mangled, c_name);
                    fm->as.func_map.orig_name = strdup(func_name);
                    fm->as.func_map.ret_type = strdup(ret_type);
                    fm->as.func_map.param_count = param_count;
                    if (param_count > 0) {
                        fm->as.func_map.param_types = malloc(param_count * sizeof(char *));
                        for (size_t pi = 0; pi < param_count; pi++)
                            fm->as.func_map.param_types[pi] = strdup(param_types[pi]);
                    }
                    nodelist_push(&import_node->as.import.func_maps, fm);
                }
            }
        }
    }

    fclose(f);
    return import_node;
}

/* Parse a .pc file to extract function signatures for importing.
 * This is a simplified parser that extracts top-level function declarations.
 * It handles: ret_type name(params) { ... } and #link directives */
static AstNode *parse_pc_file(const char *filepath) {
    FILE *f = fopen(filepath, "r");
    if (!f) return NULL;

    SrcLoc loc = {filepath, 1, 1};
    AstNode *import_node = ast_new_import(loc, filepath, 1);

    char line[1024];
    int brace_depth = 0;
    int in_func = 0;
    while (fgets(line, sizeof(line), f)) {
        char *s = line;

        /* Track brace depth to skip function bodies */
        for (char *p = s; *p; p++) {
            if (*p == '{') brace_depth++;
            if (*p == '}') { brace_depth--; if (brace_depth < 0) brace_depth = 0; }
        }

        /* Skip if we're inside a function body */
        if (in_func) {
            if (brace_depth == 0) in_func = 0;
            continue;
        }

        /* Skip empty lines and comments */
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '\0' || *s == '\n' || *s == '\r') continue;
        if (*s == '#') {
            /* Check for #link directive */
            if (strncmp(s, "#link", 5) == 0) {
                char *ls = s + 5;
                while (*ls == ' ' || *ls == '\t') ls++;
                if (*ls == '"') {
                    ls++;
                    char *end = strchr(ls, '"');
                    if (end) {
                        *end = '\0';
                        if (strchr(ls, '/') || ls[0] == '.' || strcmp(ls + strlen(ls) - 2, ".o") == 0) {
                            char dir[1024];
                            char abs_ph[1024];
                            if (realpath(filepath, abs_ph)) {
                                const char *last_slash = strrchr(abs_ph, '/');
                                if (last_slash) {
                                    size_t dirlen = last_slash - abs_ph + 1;
                                    snprintf(dir, sizeof(dir), "%.*s", (int)dirlen, abs_ph);
                                } else {
                                    strcpy(dir, "./");
                                }
                            } else {
                                const char *stdlib = getenv("STDLIB");
                                if (!stdlib) stdlib = "stdlib";
                                snprintf(dir, sizeof(dir), "%s/", stdlib);
                            }
                            char resolved[1024];
                            snprintf(resolved, sizeof(resolved), "%s%s", dir, ls);
                            nodelist_push(&import_node->as.import.links,
                                ast_new_link(loc, resolved));
                        } else {
                            nodelist_push(&import_node->as.import.links,
                                ast_new_link(loc, ls));
                        }
                    }
                }
            }
            continue;
        }
        if (strncmp(s, "class ", 6) == 0 || strncmp(s, "struct ", 7) == 0 ||
            strncmp(s, "enum ", 5) == 0 || strncmp(s, "typedef ", 8) == 0 ||
            strncmp(s, "using", 5) == 0 || strncmp(s, "if ", 3) == 0 ||
            strncmp(s, "for ", 4) == 0 || strncmp(s, "while ", 6) == 0 ||
            strncmp(s, "return ", 7) == 0 || strncmp(s, "var ", 4) == 0 ||
            strncmp(s, "mut ", 4) == 0) {
            /* Non-function top-level constructs — skip */
            if (brace_depth == 0 && strchr(s, '{')) {
                /* Multi-line construct — skip until matching brace */
                while (fgets(line, sizeof(line), f)) {
                    for (char *p = line; *p; p++) {
                        if (*p == '{') brace_depth++;
                        if (*p == '}') { brace_depth--; if (brace_depth < 0) brace_depth = 0; }
                    }
                    if (brace_depth == 0) break;
                }
            }
            continue;
        }

        /* Try to parse function declaration: ret_type name(params) [const] { or ; */
        {
            char *open_paren = strchr(s, '(');
            if (open_paren) {
                /* Extract function name: the word before '(' */
                char *name_end = open_paren - 1;
                while (name_end > s && (*name_end == ' ' || *name_end == '\t'))
                    name_end--;
                char *name_start = name_end;
                while (name_start > s && *(name_start - 1) != ' ' &&
                       *(name_start - 1) != '\t')
                    name_start--;

                size_t name_len = name_end - name_start + 1;
                if (name_len > 0 && name_len < 256) {
                    char func_name[256];
                    memcpy(func_name, name_start, name_len);
                    func_name[name_len] = '\0';

                    /* Extract return type: everything before function name */
                    char ret_type[256] = "void";
                    {
                        size_t rt_len = name_start - s;
                        while (rt_len > 0 && (s[rt_len-1] == ' ' || s[rt_len-1] == '\t'))
                            rt_len--;
                        if (rt_len > 0 && rt_len < sizeof(ret_type)) {
                            memcpy(ret_type, s, rt_len);
                            ret_type[rt_len] = '\0';
                        }
                    }

                    /* Extract parameter types */
                    char *args_str = open_paren + 1;
                    char *close_paren = strchr(args_str, ')');
                    if (close_paren) *close_paren = '\0';

                    char mangled[512];
                    size_t mpos = 0;
                    mpos += snprintf(mangled + mpos, sizeof(mangled) - mpos, "_pC%s", func_name);

                    const char *param_types_arr[32];
                    size_t param_count = 0;

                    /* Parse comma-separated args */
                    char *arg = args_str;
                    while (*arg) {
                        while (*arg == ' ' || *arg == '\t') arg++;
                        if (*arg == '\0') break;

                        char *comma = strchr(arg, ',');
                        if (comma) *comma = '\0';

                        /* Extract type: everything before the param name */
                        char *a = arg;
                        while (*a == ' ' || *a == '\t') a++;
                        char *space = strchr(a, ' ');
                        if (space) {
                            *space = '\0';
                            /* Skip mut keyword */
                            if (strcmp(a, "mut") == 0) {
                                a = space + 1;
                                while (*a == ' ' || *a == '\t') a++;
                                space = strchr(a, ' ');
                                if (space) *space = '\0';
                            }
                            const char *t = a;
                            if (param_count < 32)
                                param_types_arr[param_count++] = t;
                            if (strchr(a, '*'))
                                mangled[mpos++] = 'p';
                            else if (strcmp(t, "int") == 0 || strcmp(t, "long") == 0) mangled[mpos++] = 'i';
                            else if (strcmp(t, "float") == 0) mangled[mpos++] = 'f';
                            else if (strcmp(t, "bool") == 0)  mangled[mpos++] = 'b';
                            else if (strcmp(t, "string") == 0) mangled[mpos++] = 's';
                            else if (strcmp(t, "void") == 0)  mangled[mpos++] = 'v';
                            else {
                                size_t tlen = strlen(t);
                                mangled[mpos++] = 'p';
                                if (tlen > 0) mangled[mpos++] = t[0];
                            }
                        }
                        if (comma) arg = comma + 1;
                        else break;
                    }
                    mangled[mpos] = '\0';

                    /* Determine c_name: for .pc imports, use the mangled name
                     * (the runtime will provide the symbol when linking the .o) */
                    AstNode *fm = ast_new_func_map(loc, mangled, mangled);
                    fm->as.func_map.orig_name = strdup(func_name);
                    fm->as.func_map.ret_type = strdup(ret_type);
                    fm->as.func_map.param_count = param_count;
                    if (param_count > 0) {
                        fm->as.func_map.param_types = malloc(param_count * sizeof(char *));
                        for (size_t pi = 0; pi < param_count; pi++)
                            fm->as.func_map.param_types[pi] = strdup(param_types_arr[pi]);
                    }
                    nodelist_push(&import_node->as.import.func_maps, fm);

                    /* Check if this function has a body (not just a prototype) */
                    /* Look for '{' after the closing paren */
                    char *after_paren = close_paren ? close_paren + 1 : open_paren + 1;
                    while (*after_paren == ' ' || *after_paren == '\t' || *after_paren == '\n' || *after_paren == '\r')
                        after_paren++;
                    if (*after_paren == '{') {
                        in_func = 1;
                        brace_depth = 1;
                    }
                }
            }
        }
    }

    fclose(f);
    return import_node;
}

static AstNode *parse_top_level(void) {
    switch (cur()->type) {
        case TOK_HASH: {
            adv(); /* consume '#' */
            if (cur_is(TOK_IMPORT)) {
                adv(); /* consume 'import' */
                return parse_import_directive();
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
        case TOK_SHARED:
        case TOK_MUT: {
            /* Top-level [shared] [mut] type name = init; */
            SrcLoc loc = cur()->loc;
            int is_shared = match(TOK_SHARED);
            int is_mut = match(TOK_MUT);
            AstNode *type = parse_type();
            Token name = expect(TOK_IDENT);
            check_var_camel_case(name.loc, name.value);
            AstNode *init = NULL;
            if (match(TOK_ASSIGN)) {
                init = parse_expr();
            }
            match(TOK_SEMICOLON);
            return ast_new_var_decl(loc, type->as.ident.name, name.value, init, is_mut, is_shared);
        }
        default:
            /* Skip storage class modifiers at top level */
            while (cur_is(TOK_STATIC) || cur_is(TOK_EXTERN) || cur_is(TOK_INLINE)) {
                adv();
            }
            if (is_ident_like_type()) {
                /* Check if this is a variable declaration: type name = ... or type name; */
                Lexer saved = *P;
                adv(); /* type */
                if (cur_is(TOK_IDENT)) {
                    adv(); /* name */
                    if (cur_is(TOK_ASSIGN) || cur_is(TOK_SEMICOLON) || cur_is(TOK_COMMA)) {
                        *P = saved;
                        /* Parse as top-level variable declaration */
                        SrcLoc loc = cur()->loc;
                        AstNode *type = parse_type();
                        Token nm = expect(TOK_IDENT);
                        check_var_camel_case(nm.loc, nm.value);
                        AstNode *init = NULL;
                        if (match(TOK_ASSIGN)) init = parse_expr();
                        match(TOK_SEMICOLON);
                        return ast_new_var_decl(loc, type->as.ident.name, nm.value, init, 0, 0);
                    }
                }
                *P = saved;
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

static AstNode *parse_power(void) {
    AstNode *left = parse_unary();
    if (cur_is(TOK_STAR_STAR)) {
        adv();
        AstNode *right = parse_power();  /* right-associative */
        left = ast_new_binary(left->loc, "**", left, right);
    }
    return left;
}

static AstNode *parse_multiplicative(void) {
    AstNode *left = parse_power();
    while (cur_is(TOK_STAR) || cur_is(TOK_SLASH) || cur_is(TOK_PERCENT)) {
        char *op = strdup(cur()->value);
        adv();
        AstNode *right = parse_power();
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
    if (match(TOK_PLUS_PLUS)) {
        return ast_new_unary(loc, "++", parse_unary(), 1);
    }
    if (match(TOK_MINUS_MINUS)) {
        return ast_new_unary(loc, "--", parse_unary(), 1);
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
        } else if (cur_is(TOK_PLUS_PLUS)) {
            SrcLoc loc = cur()->loc;
            adv();
            expr = ast_new_unary(loc, "++", expr, 0);
        } else if (cur_is(TOK_MINUS_MINUS)) {
            SrcLoc loc = cur()->loc;
            adv();
            expr = ast_new_unary(loc, "--", expr, 0);
        } else {
            break;
        }
    }
    return expr;
}

static AstNode *parse_fstring(SrcLoc loc, const char *raw) {
    AstNode *fstr = ast_new_fstring(loc);
    const char *p = raw;
    char text_buf[4096];
    size_t text_len = 0;

    while (*p) {
        if (*p == '{') {
            /* Emit any accumulated text before the expression */
            if (text_len > 0) {
                text_buf[text_len] = '\0';
                nodelist_push(&fstr->as.fstring.parts,
                    ast_new_fstring_part(loc, text_buf, NULL));
                text_len = 0;
            }
            p++; /* skip '{' */

            /* Find matching '}' counting nested braces */
            int depth = 1;
            const char *expr_start = p;
            while (*p && depth > 0) {
                if (*p == '{') depth++;
                else if (*p == '}') depth--;
                if (depth > 0) p++;
            }
            if (depth != 0)
                error_at(loc, ERR_PARSER, "unterminated '{' in f-string");
            size_t expr_len = p - expr_start;
            p++; /* skip '}' */

            /* Parse the expression using a temporary lexer */
            char *expr_str = malloc(expr_len + 1);
            memcpy(expr_str, expr_start, expr_len);
            expr_str[expr_len] = '\0';

            Lexer *saved_P = P;
            Lexer tmp_lex;
            lexer_init(&tmp_lex, loc.filename, expr_str);
            P = &tmp_lex;
            AstNode *expr = parse_expr();
            P = saved_P;
            free(expr_str);

            nodelist_push(&fstr->as.fstring.parts,
                ast_new_fstring_part(loc, NULL, expr));
        } else if (*p == '}' || *p == '\0') {
            break;
        } else {
            if (text_len < sizeof(text_buf) - 1)
                text_buf[text_len++] = *p;
            p++;
        }
    }

    /* Emit any trailing text */
    if (text_len > 0) {
        text_buf[text_len] = '\0';
        nodelist_push(&fstr->as.fstring.parts,
            ast_new_fstring_part(loc, text_buf, NULL));
    }

    /* If there's only one text part and no expressions, return a plain string */
    if (fstr->as.fstring.parts.count == 1) {
        AstNode *only = fstr->as.fstring.parts.items[0];
        if (only->type == NODE_FSTRING_PART && only->as.fstring_part.expr == NULL) {
            return ast_new_string_lit(loc, only->as.fstring_part.text);
        }
    }

    return fstr;
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
        case TOK_FSTRING_LIT: {
            Token t = *cur();
            adv();
            return parse_fstring(loc, t.value);
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
