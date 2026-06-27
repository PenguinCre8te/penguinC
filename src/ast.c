#include "ast.h"
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  NodeList                                                           */
/* ------------------------------------------------------------------ */
void nodelist_init(NodeList *list) {
    list->items    = NULL;
    list->count    = 0;
    list->capacity = 0;
}

void nodelist_push(NodeList *list, AstNode *node) {
    if (list->count == list->capacity) {
        list->capacity = list->capacity ? list->capacity * 2 : 8;
        list->items = realloc(list->items, sizeof(AstNode *) * list->capacity);
    }
    list->items[list->count++] = node;
}

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */
static char *dup_str(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char *d = malloc(len + 1);
    memcpy(d, s, len + 1);
    return d;
}

AstNode *ast_new(NodeType type, SrcLoc loc) {
    AstNode *n = calloc(1, sizeof(AstNode));
    n->type = type;
    n->loc  = loc;
    return n;
}

AstNode *ast_new_program(SrcLoc loc) {
    AstNode *n = ast_new(NODE_PROGRAM, loc);
    nodelist_init(&n->as.program.imports);
    nodelist_init(&n->as.program.decls);
    return n;
}

AstNode *ast_new_import(SrcLoc loc, const char *mod, int is_header) {
    AstNode *n = ast_new(NODE_IMPORT, loc);
    n->as.import.module    = dup_str(mod);
    n->as.import.is_header = is_header;
    nodelist_init(&n->as.import.func_maps);
    nodelist_init(&n->as.import.links);
    return n;
}

AstNode *ast_new_link(SrcLoc loc, const char *path) {
    AstNode *n = ast_new(NODE_LINK, loc);
    n->as.link.path = dup_str(path);
    return n;
}

AstNode *ast_new_func_map(SrcLoc loc, const char *pc_name, const char *c_name) {
    AstNode *n = ast_new(NODE_FUNC_MAP, loc);
    n->as.func_map.pc_name = dup_str(pc_name);
    n->as.func_map.c_name  = dup_str(c_name);
    n->as.func_map.ret_type = NULL;
    n->as.func_map.param_types = NULL;
    n->as.func_map.param_count = 0;
    return n;
}

AstNode *ast_new_struct_decl(SrcLoc loc, const char *name) {
    AstNode *n = ast_new(NODE_STRUCT_DECL, loc);
    n->as.struct_decl.name = dup_str(name);
    nodelist_init(&n->as.struct_decl.fields);
    return n;
}

AstNode *ast_new_class_decl(SrcLoc loc, const char *name, const char *parent) {
    AstNode *n = ast_new(NODE_CLASS_DECL, loc);
    n->as.class_decl.name   = dup_str(name);
    n->as.class_decl.parent = dup_str(parent);
    nodelist_init(&n->as.class_decl.fields);
    nodelist_init(&n->as.class_decl.methods);
    return n;
}

AstNode *ast_new_func_decl(SrcLoc loc, const char *ret_type, const char *name, int is_method) {
    AstNode *n = ast_new(NODE_FUNC_DECL, loc);
    n->as.func_decl.ret_type   = dup_str(ret_type);
    n->as.func_decl.name       = dup_str(name);
    n->as.func_decl.class_name = NULL;
    n->as.func_decl.is_method  = is_method;
    nodelist_init(&n->as.func_decl.params);
    return n;
}

AstNode *ast_new_param(SrcLoc loc, const char *type, const char *name, int is_borrow, int is_lock) {
    AstNode *n = ast_new(NODE_PARAM, loc);
    n->as.param.type      = dup_str(type);
    n->as.param.name      = dup_str(name);
    n->as.param.is_borrow = is_borrow;
    n->as.param.is_lock   = is_lock;
    return n;
}

AstNode *ast_new_block(SrcLoc loc) {
    AstNode *n = ast_new(NODE_BLOCK, loc);
    nodelist_init(&n->as.block.stmts);
    return n;
}

AstNode *ast_new_return(SrcLoc loc, AstNode *value) {
    AstNode *n = ast_new(NODE_RETURN, loc);
    n->as.ret.value = value;
    return n;
}

AstNode *ast_new_if(SrcLoc loc, AstNode *cond, AstNode *then_blk, AstNode *else_blk) {
    AstNode *n = ast_new(NODE_IF, loc);
    n->as.if_stmt.cond     = cond;
    n->as.if_stmt.then_blk = then_blk;
    n->as.if_stmt.else_blk = else_blk;
    return n;
}

AstNode *ast_new_switch(SrcLoc loc, AstNode *expr) {
    AstNode *n = ast_new(NODE_SWITCH, loc);
    n->as.switch_stmt.expr = expr;
    nodelist_init(&n->as.switch_stmt.cases);
    return n;
}

AstNode *ast_new_case(SrcLoc loc, AstNode *value, AstNode *body, int is_default) {
    AstNode *n = ast_new(NODE_CASE, loc);
    n->as.case_stmt.value      = value;
    n->as.case_stmt.body       = body;
    n->as.case_stmt.is_default = is_default;
    return n;
}

AstNode *ast_new_match(SrcLoc loc, AstNode *expr) {
    AstNode *n = ast_new(NODE_MATCH, loc);
    n->as.match_stmt.expr = expr;
    nodelist_init(&n->as.match_stmt.cases);
    return n;
}

AstNode *ast_new_match_case(SrcLoc loc, AstNode *pattern, AstNode *body, int is_default) {
    AstNode *n = ast_new(NODE_MATCH_CASE, loc);
    n->as.match_case.pattern    = pattern;
    n->as.match_case.body       = body;
    n->as.match_case.is_default = is_default;
    return n;
}

AstNode *ast_new_for(SrcLoc loc, const char *var, AstNode *iter, AstNode *body) {
    AstNode *n = ast_new(NODE_FOR, loc);
    n->as.for_stmt.var  = dup_str(var);
    n->as.for_stmt.iter = iter;
    n->as.for_stmt.body = body;
    return n;
}

AstNode *ast_new_while(SrcLoc loc, AstNode *cond, AstNode *body) {
    AstNode *n = ast_new(NODE_WHILE, loc);
    n->as.while_stmt.cond = cond;
    n->as.while_stmt.body = body;
    return n;
}

AstNode *ast_new_using(SrcLoc loc, AstNode *resource, AstNode *body) {
    AstNode *n = ast_new(NODE_USING, loc);
    n->as.using_stmt.resource = resource;
    n->as.using_stmt.body     = body;
    return n;
}

AstNode *ast_new_unsafe(SrcLoc loc, AstNode *body) {
    AstNode *n = ast_new(NODE_UNSAFE, loc);
    n->as.unsafe_stmt.body = body;
    return n;
}

AstNode *ast_new_var_decl(SrcLoc loc, const char *type, const char *name, AstNode *init, int is_mut) {
    AstNode *n = ast_new(NODE_VAR_DECL, loc);
    n->as.var_decl.type   = dup_str(type);
    n->as.var_decl.name   = dup_str(name);
    n->as.var_decl.init   = init;
    n->as.var_decl.is_mut = is_mut;
    return n;
}

AstNode *ast_new_assign(SrcLoc loc, AstNode *target, AstNode *value, const char *op) {
    AstNode *n = ast_new(NODE_ASSIGN, loc);
    n->as.assign.target = target;
    n->as.assign.value  = value;
    n->as.assign.op     = dup_str(op);
    return n;
}

AstNode *ast_new_binary(SrcLoc loc, const char *op, AstNode *left, AstNode *right) {
    AstNode *n = ast_new(NODE_BINARY, loc);
    n->as.binary.op    = dup_str(op);
    n->as.binary.left  = left;
    n->as.binary.right = right;
    return n;
}

AstNode *ast_new_unary(SrcLoc loc, const char *op, AstNode *operand, int is_prefix) {
    AstNode *n = ast_new(NODE_UNARY, loc);
    n->as.unary.op        = dup_str(op);
    n->as.unary.operand   = operand;
    n->as.unary.is_prefix = is_prefix;
    return n;
}

AstNode *ast_new_call(SrcLoc loc, AstNode *callee) {
    AstNode *n = ast_new(NODE_CALL, loc);
    n->as.call.callee = callee;
    nodelist_init(&n->as.call.args);
    return n;
}

AstNode *ast_new_member(SrcLoc loc, AstNode *object, const char *member) {
    AstNode *n = ast_new(NODE_MEMBER, loc);
    n->as.member.object = object;
    n->as.member.member = dup_str(member);
    return n;
}

AstNode *ast_new_ident(SrcLoc loc, const char *name) {
    AstNode *n = ast_new(NODE_IDENTIFIER, loc);
    n->as.ident.name = dup_str(name);
    return n;
}

AstNode *ast_new_int_lit(SrcLoc loc, long value) {
    AstNode *n = ast_new(NODE_INT_LIT, loc);
    n->as.int_lit.value = value;
    return n;
}

AstNode *ast_new_float_lit(SrcLoc loc, double value) {
    AstNode *n = ast_new(NODE_FLOAT_LIT, loc);
    n->as.float_lit.value = value;
    return n;
}

AstNode *ast_new_string_lit(SrcLoc loc, const char *value) {
    AstNode *n = ast_new(NODE_STRING_LIT, loc);
    n->as.string_lit.value = dup_str(value);
    return n;
}

AstNode *ast_new_range(SrcLoc loc, AstNode *start, AstNode *end) {
    AstNode *n = ast_new(NODE_RANGE, loc);
    n->as.range.start = start;
    n->as.range.end   = end;
    return n;
}

AstNode *ast_new_fstring(SrcLoc loc) {
    AstNode *n = ast_new(NODE_FSTRING, loc);
    nodelist_init(&n->as.fstring.parts);
    return n;
}

AstNode *ast_new_fstring_part(SrcLoc loc, const char *text, AstNode *expr) {
    AstNode *n = ast_new(NODE_FSTRING_PART, loc);
    n->as.fstring_part.text = dup_str(text);
    n->as.fstring_part.expr = expr;
    return n;
}

AstNode *ast_new_super_call(SrcLoc loc, const char *method) {
    AstNode *n = ast_new(NODE_SUPER_CALL, loc);
    n->as.super_call.method = dup_str(method);
    nodelist_init(&n->as.super_call.args);
    return n;
}

AstNode *ast_new_self_ref(SrcLoc loc) {
    return ast_new(NODE_SELF_REF, loc);
}

AstNode *ast_new_new_expr(SrcLoc loc, const char *type_name) {
    AstNode *n = ast_new(NODE_NEW_EXPR, loc);
    n->as.new_expr.type_name = dup_str(type_name);
    nodelist_init(&n->as.new_expr.args);
    return n;
}

AstNode *ast_new_sizeof_expr(SrcLoc loc, const char *type_name) {
    AstNode *n = ast_new(NODE_SIZEOF_EXPR, loc);
    n->as.sizeof_expr.type_name = dup_str(type_name);
    return n;
}

AstNode *ast_new_drop_expr(SrcLoc loc, AstNode *expr) {
    AstNode *n = ast_new(NODE_DROP_EXPR, loc);
    n->as.drop_expr.expr = expr;
    return n;
}

AstNode *ast_new_alloc_expr(SrcLoc loc, AstNode *size) {
    AstNode *n = ast_new(NODE_ALLOC_EXPR, loc);
    n->as.alloc_expr.size = size;
    return n;
}

AstNode *ast_new_free_expr(SrcLoc loc, AstNode *expr) {
    AstNode *n = ast_new(NODE_FREE_EXPR, loc);
    n->as.free_expr.expr = expr;
    return n;
}

AstNode *ast_new_ptr_type(SrcLoc loc, const char *base_type) {
    AstNode *n = ast_new(NODEPointerType, loc);
    n->as.ptr_type.base_type = dup_str(base_type);
    return n;
}

AstNode *ast_new_borrow_expr(SrcLoc loc, AstNode *expr, int is_mut) {
    AstNode *n = ast_new(NODE_BORROW_EXPR, loc);
    n->as.borrow_expr.expr   = expr;
    n->as.borrow_expr.is_mut = is_mut;
    return n;
}

AstNode *ast_new_lock_expr(SrcLoc loc, AstNode *expr) {
    AstNode *n = ast_new(NODE_LOCK_EXPR, loc);
    n->as.lock_expr.expr = expr;
    return n;
}

AstNode *ast_new_do_while(SrcLoc loc, AstNode *cond, AstNode *body) {
    AstNode *n = ast_new(NODE_DO_WHILE, loc);
    n->as.do_while_stmt.cond = cond;
    n->as.do_while_stmt.body = body;
    return n;
}

AstNode *ast_new_c_style_for(SrcLoc loc, AstNode *init, AstNode *cond, AstNode *update, AstNode *body) {
    AstNode *n = ast_new(NODE_C_STYLE_FOR, loc);
    n->as.c_style_for.init   = init;
    n->as.c_style_for.cond   = cond;
    n->as.c_style_for.update = update;
    n->as.c_style_for.body   = body;
    return n;
}

AstNode *ast_new_break(SrcLoc loc) {
    return ast_new(NODE_BREAK, loc);
}

AstNode *ast_new_continue(SrcLoc loc) {
    return ast_new(NODE_CONTINUE, loc);
}

AstNode *ast_new_goto(SrcLoc loc, const char *label) {
    AstNode *n = ast_new(NODE_GOTO, loc);
    n->as.goto_stmt.label = dup_str(label);
    return n;
}

AstNode *ast_new_label(SrcLoc loc, const char *name) {
    AstNode *n = ast_new(NODE_LABEL, loc);
    n->as.label_stmt.name = dup_str(name);
    return n;
}

AstNode *ast_new_ternary(SrcLoc loc, AstNode *cond, AstNode *then_expr, AstNode *else_expr) {
    AstNode *n = ast_new(NODE_TERNARY, loc);
    n->as.ternary.cond      = cond;
    n->as.ternary.then_expr = then_expr;
    n->as.ternary.else_expr = else_expr;
    return n;
}

AstNode *ast_new_cast(SrcLoc loc, const char *type_name, AstNode *operand) {
    AstNode *n = ast_new(NODE_CAST, loc);
    n->as.cast_expr.type_name = dup_str(type_name);
    n->as.cast_expr.operand   = operand;
    return n;
}

AstNode *ast_new_enum_decl(SrcLoc loc, const char *name) {
    AstNode *n = ast_new(NODE_ENUM_DECL, loc);
    n->as.enum_decl.name = dup_str(name);
    nodelist_init(&n->as.enum_decl.values);
    return n;
}

AstNode *ast_new_union_decl(SrcLoc loc, const char *name) {
    AstNode *n = ast_new(NODE_UNION_DECL, loc);
    n->as.union_decl.name = dup_str(name);
    nodelist_init(&n->as.union_decl.fields);
    return n;
}

AstNode *ast_new_typedef_decl(SrcLoc loc, const char *orig_type, const char *new_name) {
    AstNode *n = ast_new(NODE_TYPEDEF_DECL, loc);
    n->as.typedef_decl.orig_type = dup_str(orig_type);
    n->as.typedef_decl.new_name  = dup_str(new_name);
    return n;
}
