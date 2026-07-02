#ifndef PENGUINC_AST_H
#define PENGUINC_AST_H

#include <stddef.h>

/* ------------------------------------------------------------------ */
/*  Token types (mirrors lexer tokens, kept here for convenience)      */
/* ------------------------------------------------------------------ */
typedef enum {
    /* Literals */
    TOK_INT_LIT,
    TOK_FLOAT_LIT,
    TOK_STRING_LIT,
    TOK_FSTRING_LIT,
    TOK_IDENT,

    /* Keywords */
    TOK_INT, TOK_VOID, TOK_STRING, TOK_BOOL, TOK_FLOAT,
    TOK_STRUCT, TOK_CLASS, TOK_EXTENDS,
    TOK_IF, TOK_ELSE, TOK_SWITCH, TOK_CASE, TOK_MATCH,
    TOK_FOR, TOK_WHILE, TOK_RETURN, TOK_IN,
    TOK_MUT, TOK_BORROW, TOK_LOCK, TOK_USING, TOK_UNSAFE,
    TOK_NEW, TOK_SELF, TOK_SUPER, TOK_SIZEOF,
    TOK_TRUE, TOK_FALSE,

    /* Operators */
    TOK_PLUS, TOK_MINUS, TOK_STAR, TOK_SLASH, TOK_PERCENT,
    TOK_EQ, TOK_NEQ, TOK_LT, TOK_GT, TOK_LE, TOK_GE,
    TOK_AND, TOK_OR, TOK_TILDE,
    TOK_ASSIGN, TOK_PLUS_ASSIGN, TOK_MINUS_ASSIGN,
    TOK_STAR_ASSIGN, TOK_SLASH_ASSIGN,
    TOK_DOT, TOK_COMMA, TOK_SEMICOLON, TOK_COLON,
    TOK_LPAREN, TOK_RPAREN, TOK_LBRACE, TOK_RBRACE,
    TOK_LBRACKET, TOK_RBRACKET,
    TOK_ARROW, TOK_DOTDOT,       /* -> and .. */
    TOK_FAT_ARROW,               /* => */
    TOK_PLUS_PLUS, TOK_MINUS_MINUS, /* ++ and -- */
    TOK_STAR_STAR,                  /* ** exponentiation */
    TOK_HASH, TOK_SEMICOLON_DEF,
    TOK_IMPORT, TOK_LINK, TOK_FUNC,

    TOK_TYPEDEF,
    TOK_ENUM,
    TOK_UNION,
    TOK_EXTERN,
    TOK_STATIC,
    TOK_CONST,
    TOK_VOLATILE,
    TOK_REGISTER,
    TOK_INLINE,
    TOK_AUTO,
    TOK_GOTO,
    TOK_BREAK,
    TOK_CONTINUE,
    TOK_DEFAULT,
    TOK_DO,
    TOK_NULL_LIT,
    TOK_QUESTION,

    /* Special */
    TOK_EOF,
    TOK_ERROR,
} TokenType;

/* ------------------------------------------------------------------ */
/*  Source location                                                    */
/* ------------------------------------------------------------------ */
typedef struct {
    const char *filename;
    int line;
    int col;
} SrcLoc;

/* ------------------------------------------------------------------ */
/*  AST node types                                                     */
/* ------------------------------------------------------------------ */
typedef enum {
    NODE_PROGRAM,
    NODE_IMPORT,
    NODE_LINK,
    NODE_FUNC_MAP,
    NODE_STRUCT_DECL,
    NODE_CLASS_DECL,
    NODE_FUNC_DECL,
    NODE_PARAM,
    NODE_BLOCK,
    NODE_STMT_EXPR,
    NODE_RETURN,
    NODE_IF,
    NODE_SWITCH,
    NODE_CASE,
    NODE_MATCH,
    NODE_MATCH_CASE,
    NODE_FOR,
    NODE_WHILE,
    NODE_USING,
    NODE_UNSAFE,
    NODE_VAR_DECL,
    NODE_ASSIGN,
    NODE_BINARY,
    NODE_UNARY,
    NODE_CALL,
    NODE_MEMBER,
    NODE_IDENTIFIER,
    NODE_INT_LIT,
    NODE_FLOAT_LIT,
    NODE_STRING_LIT,
    NODE_RANGE,
    NODE_FSTRING,
    NODE_FSTRING_PART,
    NODE_SUPER_CALL,
    NODE_SELF_REF,
    NODE_NEW_EXPR,
    NODE_SIZEOF_EXPR,
    NODE_DROP_EXPR,
    NODE_ALLOC_EXPR,
    NODE_FREE_EXPR,
    NODEPointerType,   /* type* in declarations */
    NODE_BORROW_EXPR,
    NODE_LOCK_EXPR,
    NODE_DO_WHILE,
    NODE_C_STYLE_FOR,
    NODE_BREAK,
    NODE_CONTINUE,
    NODE_GOTO,
    NODE_LABEL,
    NODE_TERNARY,
    NODE_CAST,
    NODE_ENUM_DECL,
    NODE_UNION_DECL,
    NODE_TYPEDEF_DECL,
} NodeType;

/* ------------------------------------------------------------------ */
/*  Forward declarations                                               */
/* ------------------------------------------------------------------ */
typedef struct AstNode AstNode;

/* ------------------------------------------------------------------ */
/*  Generic node list (used for statements, params, fields, etc.)      */
/* ------------------------------------------------------------------ */
typedef struct {
    AstNode **items;
    size_t   count;
    size_t   capacity;
} NodeList;

void nodelist_init(NodeList *list);
void nodelist_push(NodeList *list, AstNode *node);

/* ------------------------------------------------------------------ */
/*  AST Node                                                           */
/* ------------------------------------------------------------------ */
struct AstNode {
    NodeType type;
    SrcLoc   loc;

    union {
        /* NODE_PROGRAM */
        struct { NodeList imports; NodeList decls; } program;

        /* NODE_IMPORT */
        struct { char *module; int is_header; NodeList func_maps; NodeList links; } import;

        /* NODE_LINK */
        struct { char *path; } link;

        /* NODE_FUNC_MAP */
        struct { char *pc_name; char *c_name; char *ret_type; char **param_types; size_t param_count; } func_map;

        /* NODE_STRUCT_DECL */
        struct { char *name; NodeList fields; } struct_decl;

        /* NODE_CLASS_DECL */
        struct { char *name; char *parent; NodeList fields; NodeList methods; } class_decl;

        /* NODE_FUNC_DECL */
        struct { char *ret_type; char *name; char *class_name; NodeList params; AstNode *body; int is_method; } func_decl;

        /* NODE_PARAM */
        struct { char *type; char *name; int is_borrow; int is_lock; } param;

        /* NODE_BLOCK */
        struct { NodeList stmts; } block;

        /* NODE_STMT_EXPR */
        struct { AstNode *expr; } stmt_expr;

        /* NODE_RETURN */
        struct { AstNode *value; } ret;

        /* NODE_IF */
        struct { AstNode *cond; AstNode *then_blk; AstNode *else_blk; } if_stmt;

        /* NODE_SWITCH */
        struct { AstNode *expr; NodeList cases; } switch_stmt;

        /* NODE_CASE */
        struct { AstNode *value; AstNode *body; int is_default; } case_stmt;

        /* NODE_MATCH */
        struct { AstNode *expr; NodeList cases; } match_stmt;

        /* NODE_MATCH_CASE */
        struct { AstNode *pattern; AstNode *body; int is_default; } match_case;

        /* NODE_FOR */
        struct { char *var; AstNode *iter; AstNode *body; } for_stmt;

        /* NODE_WHILE */
        struct { AstNode *cond; AstNode *body; } while_stmt;

        /* NODE_USING */
        struct { AstNode *resource; AstNode *body; } using_stmt;

        /* NODE_UNSAFE */
        struct { AstNode *body; } unsafe_stmt;

        /* NODE_VAR_DECL */
        struct { char *type; char *name; AstNode *init; int is_mut; } var_decl;

        /* NODE_ASSIGN */
        struct { AstNode *target; AstNode *value; char *op; } assign;

        /* NODE_BINARY */
        struct { char *op; AstNode *left; AstNode *right; } binary;

        /* NODE_UNARY */
        struct { char *op; AstNode *operand; int is_prefix; } unary;

        /* NODE_CALL */
        struct { AstNode *callee; NodeList args; } call;

        /* NODE_MEMBER */
        struct { AstNode *object; char *member; } member;

        /* NODE_IDENTIFIER */
        struct { char *name; } ident;

        /* NODE_INT_LIT */
        struct { long value; } int_lit;

        /* NODE_FLOAT_LIT */
        struct { double value; } float_lit;

        /* NODE_STRING_LIT */
        struct { char *value; } string_lit;

        /* NODE_RANGE */
        struct { AstNode *start; AstNode *end; } range;

        /* NODE_FSTRING / NODE_FSTRING_PART */
        struct { NodeList parts; } fstring;
        struct { char *text; AstNode *expr; } fstring_part;

        /* NODE_SUPER_CALL */
        struct { char *method; NodeList args; } super_call;

        /* NODE_SELF_REF */
        struct { /* empty */ } self_ref;

        /* NODE_NEW_EXPR */
        struct { char *type_name; NodeList args; } new_expr;

        /* NODE_SIZEOF_EXPR */
        struct { char *type_name; } sizeof_expr;

        /* NODE_DROP_EXPR */
        struct { AstNode *expr; } drop_expr;

        /* NODE_ALLOC_EXPR */
        struct { AstNode *size; } alloc_expr;

        /* NODE_FREE_EXPR */
        struct { AstNode *expr; } free_expr;

        /* NODEPointerType */
        struct { char *base_type; } ptr_type;

        /* NODE_BORROW_EXPR */
        struct { AstNode *expr; int is_mut; } borrow_expr;

        /* NODE_LOCK_EXPR */
        struct { AstNode *expr; } lock_expr;

        /* NODE_DO_WHILE */
        struct { AstNode *cond; AstNode *body; } do_while_stmt;

        /* NODE_C_STYLE_FOR */
        struct { AstNode *init; AstNode *cond; AstNode *update; AstNode *body; } c_style_for;

        /* NODE_BREAK */
        struct { /* empty */ } break_stmt;

        /* NODE_CONTINUE */
        struct { /* empty */ } continue_stmt;

        /* NODE_GOTO */
        struct { char *label; } goto_stmt;

        /* NODE_LABEL */
        struct { char *name; } label_stmt;

        /* NODE_TERNARY */
        struct { AstNode *cond; AstNode *then_expr; AstNode *else_expr; } ternary;

        /* NODE_CAST */
        struct { char *type_name; AstNode *operand; } cast_expr;

        /* NODE_ENUM_DECL */
        struct { char *name; NodeList values; } enum_decl;

        /* NODE_UNION_DECL */
        struct { char *name; NodeList fields; } union_decl;

        /* NODE_TYPEDEF_DECL */
        struct { char *orig_type; char *new_name; } typedef_decl;
    } as;
};

/* ------------------------------------------------------------------ */
/*  Constructors                                                       */
/* ------------------------------------------------------------------ */
AstNode *ast_new(NodeType type, SrcLoc loc);
AstNode *ast_new_program(SrcLoc loc);
AstNode *ast_new_import(SrcLoc loc, const char *mod, int is_header);
AstNode *ast_new_link(SrcLoc loc, const char *path);
AstNode *ast_new_func_map(SrcLoc loc, const char *pc_name, const char *c_name);
AstNode *ast_new_struct_decl(SrcLoc loc, const char *name);
AstNode *ast_new_class_decl(SrcLoc loc, const char *name, const char *parent);
AstNode *ast_new_func_decl(SrcLoc loc, const char *ret_type, const char *name, int is_method);
AstNode *ast_new_param(SrcLoc loc, const char *type, const char *name, int is_borrow, int is_lock);
AstNode *ast_new_block(SrcLoc loc);
AstNode *ast_new_return(SrcLoc loc, AstNode *value);
AstNode *ast_new_if(SrcLoc loc, AstNode *cond, AstNode *then_blk, AstNode *else_blk);
AstNode *ast_new_switch(SrcLoc loc, AstNode *expr);
AstNode *ast_new_case(SrcLoc loc, AstNode *value, AstNode *body, int is_default);
AstNode *ast_new_match(SrcLoc loc, AstNode *expr);
AstNode *ast_new_match_case(SrcLoc loc, AstNode *pattern, AstNode *body, int is_default);
AstNode *ast_new_for(SrcLoc loc, const char *var, AstNode *iter, AstNode *body);
AstNode *ast_new_while(SrcLoc loc, AstNode *cond, AstNode *body);
AstNode *ast_new_using(SrcLoc loc, AstNode *resource, AstNode *body);
AstNode *ast_new_unsafe(SrcLoc loc, AstNode *body);
AstNode *ast_new_var_decl(SrcLoc loc, const char *type, const char *name, AstNode *init, int is_mut);
AstNode *ast_new_assign(SrcLoc loc, AstNode *target, AstNode *value, const char *op);
AstNode *ast_new_binary(SrcLoc loc, const char *op, AstNode *left, AstNode *right);
AstNode *ast_new_unary(SrcLoc loc, const char *op, AstNode *operand, int is_prefix);
AstNode *ast_new_call(SrcLoc loc, AstNode *callee);
AstNode *ast_new_member(SrcLoc loc, AstNode *object, const char *member);
AstNode *ast_new_ident(SrcLoc loc, const char *name);
AstNode *ast_new_int_lit(SrcLoc loc, long value);
AstNode *ast_new_float_lit(SrcLoc loc, double value);
AstNode *ast_new_string_lit(SrcLoc loc, const char *value);
AstNode *ast_new_range(SrcLoc loc, AstNode *start, AstNode *end);
AstNode *ast_new_fstring(SrcLoc loc);
AstNode *ast_new_fstring_part(SrcLoc loc, const char *text, AstNode *expr);
AstNode *ast_new_super_call(SrcLoc loc, const char *method);
AstNode *ast_new_self_ref(SrcLoc loc);
AstNode *ast_new_new_expr(SrcLoc loc, const char *type_name);
AstNode *ast_new_sizeof_expr(SrcLoc loc, const char *type_name);
AstNode *ast_new_drop_expr(SrcLoc loc, AstNode *expr);
AstNode *ast_new_alloc_expr(SrcLoc loc, AstNode *size);
AstNode *ast_new_free_expr(SrcLoc loc, AstNode *expr);
AstNode *ast_new_ptr_type(SrcLoc loc, const char *base_type);
AstNode *ast_new_borrow_expr(SrcLoc loc, AstNode *expr, int is_mut);
AstNode *ast_new_lock_expr(SrcLoc loc, AstNode *expr);
AstNode *ast_new_do_while(SrcLoc loc, AstNode *cond, AstNode *body);
AstNode *ast_new_c_style_for(SrcLoc loc, AstNode *init, AstNode *cond, AstNode *update, AstNode *body);
AstNode *ast_new_break(SrcLoc loc);
AstNode *ast_new_continue(SrcLoc loc);
AstNode *ast_new_goto(SrcLoc loc, const char *label);
AstNode *ast_new_label(SrcLoc loc, const char *name);
AstNode *ast_new_ternary(SrcLoc loc, AstNode *cond, AstNode *then_expr, AstNode *else_expr);
AstNode *ast_new_cast(SrcLoc loc, const char *type_name, AstNode *operand);
AstNode *ast_new_enum_decl(SrcLoc loc, const char *name);
AstNode *ast_new_union_decl(SrcLoc loc, const char *name);
AstNode *ast_new_typedef_decl(SrcLoc loc, const char *orig_type, const char *new_name);

#endif /* PENGUINC_AST_H */
