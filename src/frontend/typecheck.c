#include "typecheck.h"
#include "error.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* ------------------------------------------------------------------ */
/*  Internal helpers                                                   */
/* ------------------------------------------------------------------ */

static TCType make_type(TCTypeKind kind) {
    TCType t = {0};
    t.kind = kind;
    return t;
}

static TCType make_pointer_type(TCType pointee) {
    TCType t = {0};
    t.kind = TC_POINTER;
    t.pointee = malloc(sizeof(TCType));
    *t.pointee = pointee;
    return t;
}

static TCType make_struct_type(const char *name) {
    TCType t = {0};
    t.kind = TC_STRUCT;
    t.name = strdup(name);
    return t;
}

static TCType make_class_type(const char *name) {
    TCType t = {0};
    t.kind = TC_CLASS;
    t.name = strdup(name);
    return t;
}

static void free_tc_type(TCType *t) {
    if (!t) return;
    if (t->name) free(t->name);
    if (t->pointee) {
        free_tc_type(t->pointee);
        free(t->pointee);
    }
    if (t->fn.param_kinds) free(t->fn.param_kinds);
}

static int types_equal(TCType a, TCType b) {
    if (a.kind != b.kind) return 0;
    if (a.kind == TC_STRUCT || a.kind == TC_CLASS || a.kind == TC_ENUM) {
        if (!a.name || !b.name) return a.name == b.name;
        return strcmp(a.name, b.name) == 0;
    }
    if (a.kind == TC_POINTER) {
        if (!a.pointee || !b.pointee) return 0;
        return types_equal(*a.pointee, *b.pointee);
    }
    return 1;
}

static int is_numeric(TCType t) {
    return t.kind == TC_INT || t.kind == TC_FLOAT;
}

static int is_coercible(TCType from, TCType to) {
    if (types_equal(from, to)) return 1;
    if (from.kind == TC_TYPE_ERROR || to.kind == TC_TYPE_ERROR) return 1;
    if (from.kind == TC_UNKNOWN) return 1;
    if (from.kind == TC_INT && to.kind == TC_FLOAT) return 1;
    if (from.kind == TC_BOOL && to.kind == TC_INT) return 1;
    if (from.kind == TC_FLOAT && to.kind == TC_INT) return 1;
    if (from.kind == TC_INT && to.kind == TC_BOOL) return 1;
    return 0;
}

static const char *type_name_str(TCType t) {
    switch (t.kind) {
        case TC_INT:     return "int";
        case TC_FLOAT:   return "float";
        case TC_BOOL:    return "bool";
        case TC_VOID:    return "void";
        case TC_STRING:  return "string";
        case TC_STRUCT:  return t.name ? t.name : "struct";
        case TC_CLASS:   return t.name ? t.name : "class";
        case TC_ENUM:    return t.name ? t.name : "enum";
        case TC_POINTER: {
            static char buf[256];
            snprintf(buf, sizeof(buf), "%s*", type_name_str(*t.pointee));
            return buf;
        }
        case TC_FUNCTION: return "function";
        case TC_UNKNOWN:  return "<unknown>";
        case TC_TYPE_ERROR: return "<error>";
    }
    return "<unknown>";
}

static void tc_error(TCContext *tc, SrcLoc loc, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    /* Use print_error_context for rich formatting (source line + caret) */
    if (error_get_test_mode()) {
        vfprintf(stderr, fmt, args);
        fprintf(stderr, "\n");
    } else {
        /* Build the message string, then pass to print_error_context */
        char msg[1024];
        vsnprintf(msg, sizeof(msg), fmt, args);
        print_error_context(loc, "%s", msg);
    }
    va_end(args);
    tc->error_count++;
}

/* ------------------------------------------------------------------ */
/*  Scope management                                                   */
/* ------------------------------------------------------------------ */

static void scope_push(TCContext *tc) {
    TCScope *scope = calloc(1, sizeof(TCScope));
    scope->parent = tc->current_scope;
    tc->current_scope = scope;
}

static void scope_pop(TCContext *tc) {
    TCScope *old = tc->current_scope;
    if (old) {
        tc->current_scope = old->parent;
        for (size_t i = 0; i < old->count; i++) {
            free(old->vars[i].name);
            free_tc_type(&old->vars[i].type);
        }
        free(old->vars);
        free(old);
    }
}

static int scope_add_var(TCContext *tc, const char *name, TCType type, int is_mut, int is_shared, int line, int col) {
    TCScope *scope = tc->current_scope;
    if (!scope) return 0;

    for (size_t i = 0; i < scope->count; i++) {
        if (strcmp(scope->vars[i].name, name) == 0) {
            return 0;
        }
    }

    if (scope->count >= scope->capacity) {
        scope->capacity = scope->capacity ? scope->capacity * 2 : 16;
        scope->vars = realloc(scope->vars, scope->capacity * sizeof(TCVar));
    }
    scope->vars[scope->count].name = strdup(name);
    scope->vars[scope->count].type = type;
    scope->vars[scope->count].is_mut = is_mut;
    scope->vars[scope->count].is_shared = is_shared;
    scope->vars[scope->count].line = line;
    scope->vars[scope->count].col = col;
    scope->count++;
    return 1;
}

static TCType scope_lookup_var(TCContext *tc, const char *name, int *found) {
    *found = 0;
    TCScope *scope = tc->current_scope;
    while (scope) {
        for (size_t i = scope->count; i > 0; i--) {
            if (strcmp(scope->vars[i - 1].name, name) == 0) {
                *found = 1;
                return scope->vars[i - 1].type;
            }
        }
        scope = scope->parent;
    }
    return make_type(TC_UNKNOWN);
}

__attribute__((unused))
static int scope_var_is_mut(TCContext *tc, const char *name) {
    TCScope *scope = tc->current_scope;
    while (scope) {
        for (size_t i = scope->count; i > 0; i--) {
            if (strcmp(scope->vars[i - 1].name, name) == 0) {
                return scope->vars[i - 1].is_mut;
            }
        }
        scope = scope->parent;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Registries                                                         */
/* ------------------------------------------------------------------ */

static void tc_struct_push(TCContext *tc, const char *name, char **field_names,
                           TCType *field_types, size_t field_count, int line, int col) {
    if (tc->struct_count >= tc->struct_cap) {
        tc->struct_cap = tc->struct_cap ? tc->struct_cap * 2 : 16;
        tc->structs = realloc(tc->structs, tc->struct_cap * sizeof(TCStructInfo));
    }
    TCStructInfo *s = &tc->structs[tc->struct_count++];
    s->name = strdup(name);
    s->parent = NULL;
    s->field_names = field_names;
    s->field_types = field_types;
    s->field_count = field_count;
    s->line = line;
    s->col = col;
}

static TCStructInfo *tc_struct_lookup(TCContext *tc, const char *name) {
    for (size_t i = 0; i < tc->struct_count; i++) {
        if (strcmp(tc->structs[i].name, name) == 0)
            return &tc->structs[i];
    }
    return NULL;
}

static void tc_class_push(TCContext *tc, const char *name, const char *parent,
                          char **field_names, TCType *field_types, size_t field_count,
                          int line, int col) {
    if (tc->class_count >= tc->class_cap) {
        tc->class_cap = tc->class_cap ? tc->class_cap * 2 : 16;
        tc->classes = realloc(tc->classes, tc->class_cap * sizeof(TCClassInfo));
    }
    TCClassInfo *c = &tc->classes[tc->class_count++];
    c->name = strdup(name);
    c->parent = parent ? strdup(parent) : NULL;
    c->field_names = field_names;
    c->field_types = field_types;
    c->field_count = field_count;
    c->line = line;
    c->col = col;
}

static TCClassInfo *tc_class_lookup(TCContext *tc, const char *name) {
    for (size_t i = 0; i < tc->class_count; i++) {
        if (strcmp(tc->classes[i].name, name) == 0)
            return &tc->classes[i];
    }
    return NULL;
}

static void tc_func_push(TCContext *tc, const char *name, TCType ret_type,
                         TCType *param_types, size_t param_count, int line, int col) {
    for (size_t i = 0; i < tc->func_count; i++) {
        if (strcmp(tc->funcs[i].name, name) == 0 &&
            tc->funcs[i].param_count == param_count) {
            int match = 1;
            for (size_t j = 0; j < param_count; j++) {
                if (!types_equal(tc->funcs[i].param_types[j], param_types[j])) {
                    match = 0;
                    break;
                }
            }
            if (match) return;
        }
    }

    if (tc->func_count >= tc->func_cap) {
        tc->func_cap = tc->func_cap ? tc->func_cap * 2 : 16;
        tc->funcs = realloc(tc->funcs, tc->func_cap * sizeof(TCFuncSig));
    }
    TCFuncSig *f = &tc->funcs[tc->func_count++];
    f->name = strdup(name);
    f->ret_type = ret_type;
    f->param_types = param_types;
    f->param_count = param_count;
    f->line = line;
    f->col = col;
}

static TCFuncSig *tc_func_lookup(TCContext *tc, const char *name, size_t param_count) {
    TCFuncSig *fallback = NULL;
    for (size_t i = tc->func_count; i > 0; i--) {
        if (strcmp(tc->funcs[i - 1].name, name) == 0 &&
            tc->funcs[i - 1].param_count == param_count) {
            if (!fallback) fallback = &tc->funcs[i - 1];
        }
    }
    return fallback;
}

static TCFuncSig *tc_func_lookup_best(TCContext *tc, const char *name, TCType *arg_types, size_t argc) {
    TCFuncSig *best = NULL;
    int best_score = -1;
    for (size_t i = 0; i < tc->func_count; i++) {
        if (strcmp(tc->funcs[i].name, name) != 0) continue;
        if (tc->funcs[i].param_count != argc) continue;

        int score = 0;
        int all_match = 1;
        for (size_t j = 0; j < argc; j++) {
            if (tc->funcs[i].param_types[j].kind == TC_TYPE_ERROR ||
                arg_types[j].kind == TC_TYPE_ERROR) {
                score += 1;
                continue;
            }
            if (types_equal(arg_types[j], tc->funcs[i].param_types[j]))
                score += 3;
            else if (is_coercible(arg_types[j], tc->funcs[i].param_types[j]))
                score += 1;
            else
                all_match = 0;
        }
        if (all_match && score > best_score) {
            best_score = score;
            best = &tc->funcs[i];
        }
    }
    return best;
}

static TCFuncSig *tc_func_lookup_any(TCContext *tc, const char *name) {
    for (size_t i = tc->func_count; i > 0; i--) {
        if (strcmp(tc->funcs[i - 1].name, name) == 0) {
            return &tc->funcs[i - 1];
        }
    }
    return NULL;
}

static void tc_enum_push(TCContext *tc, const char *name, long value, int line, int col) {
    if (tc->enum_count >= tc->enum_cap) {
        tc->enum_cap = tc->enum_cap ? tc->enum_cap * 2 : 16;
        tc->enums = realloc(tc->enums, tc->enum_cap * sizeof(TCEnumConst));
    }
    tc->enums[tc->enum_count].name = strdup(name);
    tc->enums[tc->enum_count].value = value;
    tc->enums[tc->enum_count].line = line;
    tc->enums[tc->enum_count].col = col;
    tc->enum_count++;
}

static void tc_typedef_push(TCContext *tc, const char *alias, const char *orig) {
    if (tc->typedef_count >= tc->typedef_cap) {
        tc->typedef_cap = tc->typedef_cap ? tc->typedef_cap * 2 : 16;
        tc->typedefs = realloc(tc->typedefs, tc->typedef_cap * sizeof(char *));
        tc->typedef_origs = realloc(tc->typedef_origs, tc->typedef_cap * sizeof(char *));
    }
    tc->typedefs[tc->typedef_count] = strdup(alias);
    tc->typedef_origs[tc->typedef_count] = strdup(orig);
    tc->typedef_count++;
}

static const char *tc_typedef_resolve(TCContext *tc, const char *name) {
    for (size_t i = tc->typedef_count; i > 0; i--) {
        if (strcmp(tc->typedefs[i - 1], name) == 0)
            return tc->typedef_origs[i - 1];
    }
    return name;
}

/* ------------------------------------------------------------------ */
/*  Type resolution from AST type strings                               */
/* ------------------------------------------------------------------ */

static TCType resolve_tc_type(TCContext *tc, const char *type_str) {
    if (!type_str) return make_type(TC_VOID);

    const char *resolved = tc_typedef_resolve(tc, type_str);

    size_t len = strlen(resolved);
    if (len > 1 && resolved[len - 1] == '*') {
        char base[256];
        snprintf(base, sizeof(base), "%.*s", (int)(len - 1), resolved);
        TCType inner = resolve_tc_type(tc, base);
        return make_pointer_type(inner);
    }

    if (strcmp(resolved, "int") == 0 || strcmp(resolved, "long") == 0)
        return make_type(TC_INT);
    if (strcmp(resolved, "float") == 0)
        return make_type(TC_FLOAT);
    if (strcmp(resolved, "bool") == 0)
        return make_type(TC_BOOL);
    if (strcmp(resolved, "void") == 0)
        return make_type(TC_VOID);
    if (strcmp(resolved, "string") == 0)
        return make_type(TC_STRING);

    if (tc_struct_lookup(tc, resolved))
        return make_struct_type(resolved);
    if (tc_class_lookup(tc, resolved))
        return make_class_type(resolved);

    if (strchr(resolved, '.'))
        return make_type(TC_INT);

    return make_type(TC_UNKNOWN);
}

/* ------------------------------------------------------------------ */
/*  Forward declarations                                               */
/* ------------------------------------------------------------------ */

static TCType tc_expr(TCContext *tc, AstNode *node);
static void tc_stmt(TCContext *tc, AstNode *node);

/* ------------------------------------------------------------------ */
/*  Expression type-checking                                           */
/* ------------------------------------------------------------------ */

static TCType tc_int_lit(TCContext *tc, AstNode *node) {
    (void)tc; (void)node;
    return make_type(TC_INT);
}

static TCType tc_float_lit(TCContext *tc, AstNode *node) {
    (void)tc; (void)node;
    return make_type(TC_FLOAT);
}

static TCType tc_string_lit(TCContext *tc, AstNode *node) {
    (void)tc; (void)node;
    return make_type(TC_STRING);
}

static TCType tc_ident(TCContext *tc, AstNode *node) {
    const char *name = node->as.ident.name;

    if (strcmp(name, "NULL") == 0)
        return make_pointer_type(make_type(TC_VOID));

    int found = 0;
    TCType vt = scope_lookup_var(tc, name, &found);
    if (found) return vt;

    for (size_t i = 0; i < tc->enum_count; i++) {
        if (strcmp(tc->enums[i].name, name) == 0)
            return make_type(TC_INT);
    }

    TCFuncSig *fn = tc_func_lookup_any(tc, name);
    if (fn) {
        TCType ft = {0};
        ft.kind = TC_FUNCTION;
        return ft;
    }

    tc_error(tc, node->loc, "undefined variable '%s'", name);
    return make_type(TC_TYPE_ERROR);
}

static TCType tc_binary(TCContext *tc, AstNode *node) {
    const char *op = node->as.binary.op;

    if (strcmp(op, "&&") == 0 || strcmp(op, "||") == 0) {
        TCType left = tc_expr(tc, node->as.binary.left);
        TCType right = tc_expr(tc, node->as.binary.right);
        if (left.kind != TC_BOOL && left.kind != TC_TYPE_ERROR)
            tc_error(tc, node->as.binary.left->loc,
                "logical operator requires bool, got '%s'", type_name_str(left));
        if (right.kind != TC_BOOL && right.kind != TC_TYPE_ERROR)
            tc_error(tc, node->as.binary.right->loc,
                "logical operator requires bool, got '%s'", type_name_str(right));
        return make_type(TC_BOOL);
    }

    TCType left = tc_expr(tc, node->as.binary.left);
    TCType right = tc_expr(tc, node->as.binary.right);

    if (left.kind == TC_TYPE_ERROR || right.kind == TC_TYPE_ERROR)
        return make_type(TC_TYPE_ERROR);

    if (strcmp(op, "+") == 0 || strcmp(op, "-") == 0 ||
        strcmp(op, "*") == 0 || strcmp(op, "/") == 0 || strcmp(op, "%") == 0) {
        if (left.kind == TC_STRING && right.kind == TC_STRING && strcmp(op, "+") == 0)
            return make_type(TC_STRING);
        if (left.kind == TC_STRING || right.kind == TC_STRING) {
            if (left.kind != TC_TYPE_ERROR && right.kind != TC_TYPE_ERROR)
                tc_error(tc, node->loc,
                    "operator '%s' not defined for string and '%s'",
                    op, type_name_str(right.kind == TC_STRING ? left : right));
            return make_type(TC_TYPE_ERROR);
        }
        if (left.kind == TC_UNKNOWN || right.kind == TC_UNKNOWN)
            return make_type(TC_UNKNOWN);
        if (!is_numeric(left) || !is_numeric(right)) {
            tc_error(tc, node->loc,
                "operator '%s' not defined for '%s' and '%s'",
                op, type_name_str(left), type_name_str(right));
            return make_type(TC_TYPE_ERROR);
        }
        if (left.kind == TC_FLOAT || right.kind == TC_FLOAT)
            return make_type(TC_FLOAT);
        return make_type(TC_INT);
    }

    if (strcmp(op, "**") == 0) {
        if (!is_numeric(left) || !is_numeric(right)) {
            tc_error(tc, node->loc,
                "operator '**' not defined for '%s' and '%s'",
                type_name_str(left), type_name_str(right));
            return make_type(TC_TYPE_ERROR);
        }
        return make_type(TC_FLOAT);
    }

    if (strcmp(op, "<") == 0 || strcmp(op, ">") == 0 ||
        strcmp(op, "<=") == 0 || strcmp(op, ">=") == 0) {
        if (!is_numeric(left) || !is_numeric(right)) {
            tc_error(tc, node->loc,
                "comparison operator '%s' not defined for '%s' and '%s'",
                op, type_name_str(left), type_name_str(right));
            return make_type(TC_TYPE_ERROR);
        }
        return make_type(TC_BOOL);
    }

    if (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0) {
        if (!is_coercible(left, right) && !is_coercible(right, left)) {
            tc_error(tc, node->loc,
                "comparison '%s' not defined for '%s' and '%s'",
                op, type_name_str(left), type_name_str(right));
            return make_type(TC_TYPE_ERROR);
        }
        return make_type(TC_BOOL);
    }

    if (strcmp(op, "&") == 0 || strcmp(op, "|") == 0 || strcmp(op, "^") == 0 ||
        strcmp(op, "<<") == 0 || strcmp(op, ">>") == 0) {
        if (!is_numeric(left) || !is_numeric(right)) {
            tc_error(tc, node->loc,
                "bitwise operator '%s' not defined for '%s' and '%s'",
                op, type_name_str(left), type_name_str(right));
            return make_type(TC_TYPE_ERROR);
        }
        return make_type(TC_INT);
    }

    if (strcmp(op, "in") == 0)
        return make_type(TC_BOOL);

    return make_type(TC_UNKNOWN);
}

static TCType tc_unary(TCContext *tc, AstNode *node) {
    const char *op = node->as.unary.op;

    if (strcmp(op, "++") == 0 || strcmp(op, "--") == 0) {
        TCType operand = tc_expr(tc, node->as.unary.operand);
        if (!is_numeric(operand) && operand.kind != TC_TYPE_ERROR)
            tc_error(tc, node->loc,
                "operator '%s' not defined for '%s'", op, type_name_str(operand));
        return operand;
    }

    TCType operand = tc_expr(tc, node->as.unary.operand);

    if (strcmp(op, "-") == 0) {
        if (!is_numeric(operand) && operand.kind != TC_TYPE_ERROR)
            tc_error(tc, node->loc,
                "unary minus not defined for '%s'", type_name_str(operand));
        return operand;
    }
    if (strcmp(op, "~") == 0) {
        if (operand.kind != TC_INT && operand.kind != TC_TYPE_ERROR)
            tc_error(tc, node->loc,
                "bitwise NOT not defined for '%s'", type_name_str(operand));
        return make_type(TC_INT);
    }
    if (strcmp(op, "*") == 0) {
        if (operand.kind != TC_POINTER && operand.kind != TC_TYPE_ERROR) {
            tc_error(tc, node->loc, "cannot dereference non-pointer type '%s'",
                type_name_str(operand));
            return make_type(TC_TYPE_ERROR);
        }
        if (operand.pointee)
            return *operand.pointee;
        return make_type(TC_UNKNOWN);
    }

    return make_type(TC_UNKNOWN);
}

static TCType tc_assign(TCContext *tc, AstNode *node) {
    TCType target_type = tc_expr(tc, node->as.assign.target);
    TCType value_type = tc_expr(tc, node->as.assign.value);

    const char *op = node->as.assign.op;
    if (strcmp(op, "=") != 0) {
        if (target_type.kind != TC_UNKNOWN && !is_numeric(target_type) && target_type.kind != TC_TYPE_ERROR) {
            tc_error(tc, node->loc,
                "compound assignment '%s' not defined for '%s'",
                op, type_name_str(target_type));
            return make_type(TC_TYPE_ERROR);
        }
        if (value_type.kind != TC_UNKNOWN && !is_numeric(value_type) && value_type.kind != TC_TYPE_ERROR) {
            tc_error(tc, node->loc,
                "compound assignment '%s' not defined for '%s'",
                op, type_name_str(value_type));
            return make_type(TC_TYPE_ERROR);
        }
    }

    /* Check if variable is mutable */
    if (node->as.assign.target->type == NODE_IDENTIFIER) {
        const char *name = node->as.assign.target->as.ident.name;
        int found = 0;
        TCType vt = scope_lookup_var(tc, name, &found);
        if (found && vt.kind != TC_TYPE_ERROR) {
            /* Look up is_mut and is_shared from scope */
            TCScope *scope = tc->current_scope;
            while (scope) {
                for (size_t i = 0; i < scope->count; i++) {
                    if (strcmp(scope->vars[i].name, name) == 0) {
                        if (!scope->vars[i].is_mut) {
                            tc_error(tc, node->loc,
                                "variable '%s' is not mutable, use 'mut %s' to declare as mutable",
                                name, name);
                        }
                        if (scope->vars[i].is_shared && scope->vars[i].is_mut && tc->using_depth == 0) {
                            tc_error(tc, node->loc,
                                "shared mutable variable '%s' must be written inside a 'using' block",
                                name);
                        }
                        goto found_var;
                    }
                }
                scope = scope->parent;
            }
            found_var:;
        }
    }

    if (target_type.kind != TC_TYPE_ERROR && value_type.kind != TC_TYPE_ERROR &&
        target_type.kind != TC_UNKNOWN && value_type.kind != TC_UNKNOWN) {
        if (!is_coercible(value_type, target_type)) {
            tc_error(tc, node->loc,
                "type mismatch: cannot assign '%s' to '%s'",
                type_name_str(value_type), type_name_str(target_type));
            return make_type(TC_TYPE_ERROR);
        }
    }

    return value_type;
}

static TCType tc_call(TCContext *tc, AstNode *node) {
    if (!node->as.call.callee) {
        for (size_t i = 0; i < node->as.call.args.count; i++)
            tc_expr(tc, node->as.call.args.items[i]);
        return make_type(TC_UNKNOWN);
    }

    if (node->as.call.callee->type == NODE_MEMBER) {
        AstNode *obj = node->as.call.callee->as.member.object;
        const char *method = node->as.call.callee->as.member.member;

        if (strcmp(method, "toI") == 0 || strcmp(method, "toF") == 0 ||
            strcmp(method, "toS") == 0 || strcmp(method, "toB") == 0) {
            tc_expr(tc, obj);
            if (strcmp(method, "toI") == 0) return make_type(TC_INT);
            if (strcmp(method, "toF") == 0) return make_type(TC_FLOAT);
            if (strcmp(method, "toS") == 0) return make_type(TC_STRING);
            if (strcmp(method, "toB") == 0) return make_type(TC_BOOL);
        }

        for (size_t i = 0; i < node->as.call.args.count; i++)
            tc_expr(tc, node->as.call.args.items[i]);
        return make_type(TC_UNKNOWN);
    }

    if (node->as.call.callee->type == NODE_IDENTIFIER) {
        const char *name = node->as.call.callee->as.ident.name;
        size_t argc = node->as.call.args.count;

        TCType *arg_types = argc > 0 ? malloc(argc * sizeof(TCType)) : NULL;
        for (size_t i = 0; i < argc; i++)
            arg_types[i] = tc_expr(tc, node->as.call.args.items[i]);

        TCFuncSig *fn = tc_func_lookup_best(tc, name, arg_types, argc);
        if (!fn) fn = tc_func_lookup(tc, name, argc);
        if (fn) {
            for (size_t i = 0; i < argc; i++) {
                if (i < fn->param_count && fn->param_types[i].kind != TC_TYPE_ERROR &&
                    arg_types[i].kind != TC_TYPE_ERROR) {
                    if (!is_coercible(arg_types[i], fn->param_types[i])) {
                        tc_error(tc, node->as.call.args.items[i]->loc,
                            "argument %zu: type mismatch, expected '%s' got '%s'",
                            i + 1, type_name_str(fn->param_types[i]),
                            type_name_str(arg_types[i]));
                    }
                }
            }
            TCType ret = fn->ret_type;
            free(arg_types);
            return ret;
        }

        TCFuncSig *any_fn = tc_func_lookup_any(tc, name);
        if (!any_fn && strcmp(name, "console.print") != 0 &&
            strcmp(name, "console.println") != 0 &&
            strcmp(name, "threads.run") != 0 &&
            strcmp(name, "threads.run1") != 0 &&
            strcmp(name, "arc_alloc") != 0 &&
            strcmp(name, "arc_retain") != 0 &&
            strcmp(name, "arc_release") != 0 &&
            strcmp(name, "arc_wrap_string") != 0 &&
            strcmp(name, "malloc") != 0 &&
            strcmp(name, "free") != 0 &&
            strcmp(name, "pow") != 0 &&
            strcmp(name, "int_to_string") != 0 &&
            strcmp(name, "float_to_string") != 0 &&
            strcmp(name, "bool_to_string") != 0 &&
            strcmp(name, "penguin_str_concat") != 0 &&
            strcmp(name, "parse_int") != 0 &&
            strcmp(name, "parse_float") != 0) {
            tc_error(tc, node->loc, "undefined function '%s'", name);
        }

        free(arg_types);
        return make_type(TC_UNKNOWN);
    }

    for (size_t i = 0; i < node->as.call.args.count; i++)
        tc_expr(tc, node->as.call.args.items[i]);
    return make_type(TC_UNKNOWN);
}

static TCType tc_ternary(TCContext *tc, AstNode *node) {
    TCType cond = tc_expr(tc, node->as.ternary.cond);
    if (cond.kind != TC_BOOL && cond.kind != TC_TYPE_ERROR)
        tc_error(tc, node->loc, "ternary condition must be bool, got '%s'",
            type_name_str(cond));
    TCType then_type = tc_expr(tc, node->as.ternary.then_expr);
    TCType else_type = tc_expr(tc, node->as.ternary.else_expr);
    if (then_type.kind == TC_TYPE_ERROR || else_type.kind == TC_TYPE_ERROR)
        return make_type(TC_TYPE_ERROR);
    if (types_equal(then_type, else_type)) return then_type;
    return make_type(TC_UNKNOWN);
}

static TCType tc_cast(TCContext *tc, AstNode *node) {
    tc_expr(tc, node->as.cast_expr.operand);
    return resolve_tc_type(tc, node->as.cast_expr.type_name);
}

static TCType tc_sizeof(TCContext *tc, AstNode *node) {
    (void)tc;
    (void)node;
    return make_type(TC_INT);
}

static TCType tc_new_expr(TCContext *tc, AstNode *node) {
    const char *type_name = node->as.new_expr.type_name;

    if (strcmp(type_name, "int") == 0 || strcmp(type_name, "long") == 0) {
        for (size_t i = 0; i < node->as.new_expr.args.count; i++)
            tc_expr(tc, node->as.new_expr.args.items[i]);
        return make_pointer_type(make_type(TC_INT));
    }
    if (strcmp(type_name, "float") == 0) {
        for (size_t i = 0; i < node->as.new_expr.args.count; i++)
            tc_expr(tc, node->as.new_expr.args.items[i]);
        return make_pointer_type(make_type(TC_FLOAT));
    }
    if (strcmp(type_name, "bool") == 0) {
        for (size_t i = 0; i < node->as.new_expr.args.count; i++)
            tc_expr(tc, node->as.new_expr.args.items[i]);
        return make_pointer_type(make_type(TC_BOOL));
    }
    if (strcmp(type_name, "string") == 0) {
        for (size_t i = 0; i < node->as.new_expr.args.count; i++)
            tc_expr(tc, node->as.new_expr.args.items[i]);
        return make_pointer_type(make_type(TC_STRING));
    }

    TCClassInfo *cls = tc_class_lookup(tc, type_name);
    TCStructInfo *st = tc_struct_lookup(tc, type_name);

    if (!cls && !st) {
        for (size_t i = 0; i < node->as.new_expr.args.count; i++)
            tc_expr(tc, node->as.new_expr.args.items[i]);
        return make_type(TC_INT);
    }

    for (size_t i = 0; i < node->as.new_expr.args.count; i++)
        tc_expr(tc, node->as.new_expr.args.items[i]);

    if (cls) return make_class_type(type_name);
    return make_struct_type(type_name);
}

static TCType tc_member(TCContext *tc, AstNode *node) {
    TCType obj_type = tc_expr(tc, node->as.member.object);

    /* Handle pointer to struct/class (e.g. class instances accessed via self) */
    if (obj_type.kind == TC_POINTER && obj_type.pointee &&
        (obj_type.pointee->kind == TC_STRUCT || obj_type.pointee->kind == TC_CLASS)) {
        obj_type = *obj_type.pointee;
    }

    if (obj_type.kind == TC_STRUCT || obj_type.kind == TC_CLASS) {
        const char *type_name = obj_type.name;
        TCStructInfo *st = tc_struct_lookup(tc, type_name);
        TCClassInfo *cls = tc_class_lookup(tc, type_name);

        char **field_names = NULL;
        TCType *field_types = NULL;
        size_t field_count = 0;

        if (st) {
            field_names = st->field_names;
            field_types = st->field_types;
            field_count = st->field_count;
        } else if (cls) {
            field_names = cls->field_names;
            field_types = cls->field_types;
            field_count = cls->field_count;
        }

        for (size_t i = 0; i < field_count; i++) {
            if (strcmp(field_names[i], node->as.member.member) == 0) {
                return field_types[i];
            }
        }

        tc_error(tc, node->loc, "type '%s' has no field '%s'",
            type_name, node->as.member.member);
        return make_type(TC_TYPE_ERROR);
    }

    return make_type(TC_UNKNOWN);
}

static TCType tc_range(TCContext *tc, AstNode *node) {
    tc_expr(tc, node->as.range.start);
    tc_expr(tc, node->as.range.end);
    return make_type(TC_INT);
}

static TCType tc_fstring(TCContext *tc, AstNode *node) {
    for (size_t i = 0; i < node->as.fstring.parts.count; i++) {
        AstNode *part = node->as.fstring.parts.items[i];
        if (part->type == NODE_FSTRING_PART && part->as.fstring_part.expr) {
            tc_expr(tc, part->as.fstring_part.expr);
        }
    }
    return make_type(TC_STRING);
}

static TCType tc_super_call(TCContext *tc, AstNode *node) {
    for (size_t i = 0; i < node->as.super_call.args.count; i++)
        tc_expr(tc, node->as.super_call.args.items[i]);
    return make_type(TC_VOID);
}

static TCType tc_drop_expr(TCContext *tc, AstNode *node) {
    tc_expr(tc, node->as.drop_expr.expr);
    return make_type(TC_VOID);
}

static TCType tc_borrow(TCContext *tc, AstNode *node) {
    TCType inner = tc_expr(tc, node->as.borrow_expr.expr);
    return make_pointer_type(inner);
}

static TCType tc_expr(TCContext *tc, AstNode *node) {
    if (!node) return make_type(TC_VOID);
    switch (node->type) {
        case NODE_INT_LIT:      return tc_int_lit(tc, node);
        case NODE_FLOAT_LIT:    return tc_float_lit(tc, node);
        case NODE_STRING_LIT:   return tc_string_lit(tc, node);
        case NODE_IDENTIFIER:   return tc_ident(tc, node);
        case NODE_BINARY:       return tc_binary(tc, node);
        case NODE_UNARY:        return tc_unary(tc, node);
        case NODE_ASSIGN:       return tc_assign(tc, node);
        case NODE_CALL:         return tc_call(tc, node);
        case NODE_TERNARY:      return tc_ternary(tc, node);
        case NODE_CAST:         return tc_cast(tc, node);
        case NODE_SIZEOF_EXPR:  return tc_sizeof(tc, node);
        case NODE_NEW_EXPR:     return tc_new_expr(tc, node);
        case NODE_MEMBER:       return tc_member(tc, node);
        case NODE_RANGE:        return tc_range(tc, node);
        case NODE_SELF_REF:     return make_type(TC_UNKNOWN);
        case NODE_SUPER_CALL:   return tc_super_call(tc, node);
        case NODE_DROP_EXPR:    return tc_drop_expr(tc, node);
        case NODE_BORROW_EXPR:  return tc_borrow(tc, node);
        case NODE_FSTRING:      return tc_fstring(tc, node);
        case NODE_FSTRING_PART: return make_type(TC_STRING);
        case NODE_ALLOC_EXPR:   return make_pointer_type(make_type(TC_VOID));
        case NODE_FREE_EXPR:    return make_type(TC_VOID);
        case NODE_STMT_EXPR:    return tc_expr(tc, node->as.stmt_expr.expr);
        default: return make_type(TC_UNKNOWN);
    }
}

/* ------------------------------------------------------------------ */
/*  Statement type-checking                                            */
/* ------------------------------------------------------------------ */

static void tc_block(TCContext *tc, AstNode *node) {
    for (size_t i = 0; i < node->as.block.stmts.count; i++)
        tc_stmt(tc, node->as.block.stmts.items[i]);
}

static void tc_return(TCContext *tc, AstNode *node) {
    if (node->as.ret.value) {
        TCType ret_type = tc_expr(tc, node->as.ret.value);
        if (tc->current_func_node && tc->current_func_node->type == NODE_FUNC_DECL) {
            const char *expected_str = tc->current_func_node->as.func_decl.ret_type;
            TCType expected = resolve_tc_type(tc, expected_str);
            if (expected.kind != TC_VOID && expected.kind != TC_TYPE_ERROR &&
                ret_type.kind != TC_TYPE_ERROR) {
                if (!is_coercible(ret_type, expected)) {
                    tc_error(tc, node->loc,
                        "return type mismatch: expected '%s' got '%s'",
                        type_name_str(expected), type_name_str(ret_type));
                }
            }
        }
    }
}

static void tc_var_decl(TCContext *tc, AstNode *node) {
    const char *type_str = node->as.var_decl.type;
    const char *var_name = node->as.var_decl.name;
    TCType decl_type = resolve_tc_type(tc, type_str);

    if (node->as.var_decl.init) {
        TCType init_type = tc_expr(tc, node->as.var_decl.init);
        if (decl_type.kind != TC_TYPE_ERROR && init_type.kind != TC_TYPE_ERROR) {
            if (!is_coercible(init_type, decl_type)) {
                tc_error(tc, node->loc,
                    "type mismatch in variable '%s': declared '%s', initialized with '%s'",
                    var_name, type_name_str(decl_type), type_name_str(init_type));
            }
        }
    }

    scope_add_var(tc, var_name, decl_type, node->as.var_decl.is_mut, node->as.var_decl.is_shared,
                  node->loc.line, node->loc.col);
}

static void tc_if(TCContext *tc, AstNode *node) {
    TCType cond = tc_expr(tc, node->as.if_stmt.cond);
    if (cond.kind != TC_BOOL && cond.kind != TC_TYPE_ERROR)
        tc_error(tc, node->loc, "if condition must be bool, got '%s'",
            type_name_str(cond));
    scope_push(tc);
    tc_block(tc, node->as.if_stmt.then_blk);
    scope_pop(tc);
    if (node->as.if_stmt.else_blk) {
        scope_push(tc);
        if (node->as.if_stmt.else_blk->type == NODE_IF)
            tc_if(tc, node->as.if_stmt.else_blk);
        else
            tc_block(tc, node->as.if_stmt.else_blk);
        scope_pop(tc);
    }
}

static void tc_while(TCContext *tc, AstNode *node) {
    TCType cond = tc_expr(tc, node->as.while_stmt.cond);
    if (cond.kind != TC_BOOL && cond.kind != TC_TYPE_ERROR)
        tc_error(tc, node->loc, "while condition must be bool, got '%s'",
            type_name_str(cond));
    scope_push(tc);
    tc_block(tc, node->as.while_stmt.body);
    scope_pop(tc);
}

static void tc_do_while(TCContext *tc, AstNode *node) {
    scope_push(tc);
    tc_block(tc, node->as.do_while_stmt.body);
    scope_pop(tc);
    TCType cond = tc_expr(tc, node->as.do_while_stmt.cond);
    if (cond.kind != TC_BOOL && cond.kind != TC_TYPE_ERROR)
        tc_error(tc, node->loc, "do-while condition must be bool, got '%s'",
            type_name_str(cond));
}

static void tc_for(TCContext *tc, AstNode *node) {
    tc_expr(tc, node->as.for_stmt.iter);
    scope_push(tc);
    scope_add_var(tc, node->as.for_stmt.var, make_type(TC_INT), 1, 0,
                  node->loc.line, node->loc.col);
    tc_block(tc, node->as.for_stmt.body);
    scope_pop(tc);
}

static void tc_c_style_for(TCContext *tc, AstNode *node) {
    scope_push(tc);
    if (node->as.c_style_for.init) tc_stmt(tc, node->as.c_style_for.init);
    if (node->as.c_style_for.cond) {
        TCType cond = tc_expr(tc, node->as.c_style_for.cond);
        if (cond.kind != TC_BOOL && cond.kind != TC_TYPE_ERROR)
            tc_error(tc, node->loc, "for condition must be bool, got '%s'",
                type_name_str(cond));
    }
    tc_block(tc, node->as.c_style_for.body);
    if (node->as.c_style_for.update) tc_expr(tc, node->as.c_style_for.update);
    scope_pop(tc);
}

static void tc_switch(TCContext *tc, AstNode *node) {
    tc_expr(tc, node->as.switch_stmt.expr);
    for (size_t i = 0; i < node->as.switch_stmt.cases.count; i++) {
        AstNode *c = node->as.switch_stmt.cases.items[i];
        if (!c->as.case_stmt.is_default)
            tc_expr(tc, c->as.case_stmt.value);
        scope_push(tc);
        tc_block(tc, c->as.case_stmt.body);
        scope_pop(tc);
    }
}

static void tc_using(TCContext *tc, AstNode *node) {
    // Typecheck the resource expression first
    TCType resource_type = tc_expr(tc, node->as.using_stmt.resource);

    tc->using_depth++;
    scope_push(tc);

    if (node->as.using_stmt.var_name) {
        // Register the variable with the type of the resource
        scope_add_var(tc,
                      node->as.using_stmt.var_name,
                      resource_type,
                      1, 0,
                      node->loc.line,
                      node->loc.col);
    }

    tc_block(tc, node->as.using_stmt.body);
    scope_pop(tc);
    tc->using_depth--;
}


static void tc_match(TCContext *tc, AstNode *node) {
    tc_expr(tc, node->as.match_stmt.expr);
    for (size_t i = 0; i < node->as.match_stmt.cases.count; i++) {
        AstNode *c = node->as.match_stmt.cases.items[i];
        if (!c->as.match_case.is_default)
            tc_expr(tc, c->as.match_case.pattern);
        scope_push(tc);
        tc_block(tc, c->as.match_case.body);
        scope_pop(tc);
    }
}

static void tc_stmt(TCContext *tc, AstNode *node) {
    if (!node) return;
    switch (node->type) {
        case NODE_BLOCK:       tc_block(tc, node); break;
        case NODE_RETURN:      tc_return(tc, node); break;
        case NODE_VAR_DECL:    tc_var_decl(tc, node); break;
        case NODE_IF:          tc_if(tc, node); break;
        case NODE_SWITCH:      tc_switch(tc, node); break;
        case NODE_MATCH:       tc_match(tc, node); break;
        case NODE_WHILE:       tc_while(tc, node); break;
        case NODE_DO_WHILE:    tc_do_while(tc, node); break;
        case NODE_FOR:         tc_for(tc, node); break;
        case NODE_C_STYLE_FOR: tc_c_style_for(tc, node); break;
        case NODE_USING:       tc_using(tc, node); break;
        case NODE_UNSAFE:      tc_block(tc, node->as.unsafe_stmt.body); break;
        case NODE_STMT_EXPR:   tc_expr(tc, node->as.stmt_expr.expr); break;
        case NODE_ASSIGN:      tc_expr(tc, node); break;
        case NODE_CALL:        tc_expr(tc, node); break;
        case NODE_DROP_EXPR:   tc_expr(tc, node); break;
        case NODE_BREAK:
        case NODE_CONTINUE:
        case NODE_GOTO:
        case NODE_LABEL:
            break;
        default:
            tc_expr(tc, node);
            break;
    }
}

/* ------------------------------------------------------------------ */
/*  Declaration type-checking                                          */
/* ------------------------------------------------------------------ */

static int name_in_selected_tc(AstNode *fm, NodeList *selected) {
    if (!fm || fm->type != NODE_FUNC_MAP) return 0;
    const char *orig = fm->as.func_map.orig_name;
    if (!orig) return 0;
    for (size_t i = 0; i < selected->count; i++) {
        AstNode *sel = selected->items[i];
        if (sel && sel->type == NODE_IDENTIFIER &&
            strcmp(sel->as.ident.name, orig) == 0)
            return 1;
    }
    return 0;
}

static void tc_import(TCContext *tc, AstNode *node) {
    const char *mod = node->as.import.module;
    const char *last = strrchr(mod, '.');
    last = last ? last + 1 : mod;

    const char *reg_name = last;
    if (node->as.import.submodule)
        reg_name = node->as.import.submodule;
    else if (node->as.import.alias)
        reg_name = node->as.import.alias;

    /* Local .pc files register under bare names (like wildcard) */
    size_t mod_len = strlen(mod);
    int is_local_pc = (mod_len >= 3 && strcmp(mod + mod_len - 3, ".pc") == 0);

    for (size_t k = 0; k < node->as.import.func_maps.count; k++) {
        AstNode *fm = node->as.import.func_maps.items[k];

        if (node->as.import.selected_names.count > 0) {
            if (fm && fm->type == NODE_FUNC_MAP) {
                if (!name_in_selected_tc(fm, &node->as.import.selected_names))
                    continue;
            }
        }

        if (fm && fm->type == NODE_FUNC_MAP) {
            const char *func_name = fm->as.func_map.orig_name;
            if (!func_name) func_name = fm->as.func_map.pc_name;

            size_t pc = fm->as.func_map.param_count;
            TCType ret_type = resolve_tc_type(tc, fm->as.func_map.ret_type);
            TCType *param_types = pc > 0 ? malloc(pc * sizeof(TCType)) : NULL;
            for (size_t pi = 0; pi < pc; pi++)
                param_types[pi] = resolve_tc_type(tc, fm->as.func_map.param_types[pi]);

            if (node->as.import.wildcard || node->as.import.selected_names.count > 0 || is_local_pc) {
                /* Bare name registration */
                tc_func_push(tc, func_name, ret_type, param_types, pc,
                             node->loc.line, node->loc.col);
                /* If alias, also register alias.funcname */
                if (node->as.import.alias) {
                    TCType *alias_pt = pc > 0 ? malloc(pc * sizeof(TCType)) : NULL;
                    for (size_t pi = 0; pi < pc; pi++)
                        alias_pt[pi] = param_types[pi];
                    char qname[512];
                    snprintf(qname, sizeof(qname), "%s.%s", node->as.import.alias, func_name);
                    tc_func_push(tc, qname, ret_type, alias_pt, pc,
                                 node->loc.line, node->loc.col);
                }
            } else {
                char qname[512];
                snprintf(qname, sizeof(qname), "%s.%s", reg_name, func_name);
                tc_func_push(tc, qname, ret_type, param_types, pc,
                             node->loc.line, node->loc.col);
                if (node->as.import.alias && strcmp(reg_name, last) != 0) {
                    TCType *alias_pt = pc > 0 ? malloc(pc * sizeof(TCType)) : NULL;
                    for (size_t pi = 0; pi < pc; pi++)
                        alias_pt[pi] = param_types[pi];
                    char qname2[512];
                    snprintf(qname2, sizeof(qname2), "%s.%s", last, func_name);
                    tc_func_push(tc, qname2, ret_type, alias_pt, pc,
                                 node->loc.line, node->loc.col);
                }
            }
        }
    }
}

static void tc_struct_decl(TCContext *tc, AstNode *node) {
    const char *name = node->as.struct_decl.name;
    size_t count = node->as.struct_decl.fields.count;
    char **field_names = count > 0 ? malloc(count * sizeof(char *)) : NULL;
    TCType *field_types = count > 0 ? malloc(count * sizeof(TCType)) : NULL;

    for (size_t i = 0; i < count; i++) {
        AstNode *f = node->as.struct_decl.fields.items[i];
        field_names[i] = strdup(f->as.var_decl.name);
        field_types[i] = resolve_tc_type(tc, f->as.var_decl.type);
    }

    tc_struct_push(tc, name, field_names, field_types, count,
                   node->loc.line, node->loc.col);
}

static void tc_func_decl(TCContext *tc, AstNode *node) {
    const char *name = node->as.func_decl.name;
    const char *ret_type_str = node->as.func_decl.ret_type;
    TCType ret_type = resolve_tc_type(tc, ret_type_str);

    size_t param_count = node->as.func_decl.params.count;
    TCType *param_types = param_count > 0 ? malloc(param_count * sizeof(TCType)) : NULL;

    for (size_t i = 0; i < param_count; i++) {
        AstNode *p = node->as.func_decl.params.items[i];
        param_types[i] = resolve_tc_type(tc, p->as.param.type);
    }

    tc_func_push(tc, name, ret_type, param_types, param_count,
                 node->loc.line, node->loc.col);
}

static void tc_class_decl(TCContext *tc, AstNode *node) {
    const char *name = node->as.class_decl.name;
    const char *parent = node->as.class_decl.parent;

    size_t own_count = node->as.class_decl.fields.count;
    size_t parent_field_count = 0;

    TCClassInfo *parent_cls = parent ? tc_class_lookup(tc, parent) : NULL;
    if (parent_cls) parent_field_count = parent_cls->field_count;

    size_t total = parent_field_count + own_count;
    char **field_names = total > 0 ? malloc(total * sizeof(char *)) : NULL;
    TCType *field_types = total > 0 ? malloc(total * sizeof(TCType)) : NULL;

    if (parent_cls) {
        for (size_t i = 0; i < parent_field_count; i++) {
            field_names[i] = strdup(parent_cls->field_names[i]);
            field_types[i] = parent_cls->field_types[i];
        }
    }

    for (size_t i = 0; i < own_count; i++) {
        AstNode *f = node->as.class_decl.fields.items[i];
        field_names[parent_field_count + i] = strdup(f->as.var_decl.name);
        field_types[parent_field_count + i] = resolve_tc_type(tc, f->as.var_decl.type);
    }

    tc_class_push(tc, name, parent, field_names, field_types, total,
                  node->loc.line, node->loc.col);

    for (size_t i = 0; i < node->as.class_decl.methods.count; i++) {
        AstNode *m = node->as.class_decl.methods.items[i];
        if (m && m->type == NODE_FUNC_DECL)
            tc_func_decl(tc, m);
    }
}

static void tc_enum_decl(TCContext *tc, AstNode *node) {
    long next_val = 0;
    for (size_t i = 0; i < node->as.enum_decl.values.count; i++) {
        AstNode *val = node->as.enum_decl.values.items[i];
        if (val->type == NODE_BINARY && strcmp(val->as.binary.op, "=") == 0) {
            tc_expr(tc, val->as.binary.right);
            tc_enum_push(tc, val->as.binary.left->as.ident.name, next_val,
                         val->loc.line, val->loc.col);
            next_val++;
        } else if (val->type == NODE_IDENTIFIER) {
            tc_enum_push(tc, val->as.ident.name, next_val,
                         val->loc.line, val->loc.col);
            next_val++;
        }
    }
}

static void tc_typedef_decl(TCContext *tc, AstNode *node) {
    tc_typedef_push(tc, node->as.typedef_decl.new_name, node->as.typedef_decl.orig_type);
}

static void tc_program(TCContext *tc, AstNode *program) {
    for (size_t i = 0; i < program->as.program.decls.count; i++) {
        AstNode *decl = program->as.program.decls.items[i];
        if (!decl) continue;

        switch (decl->type) {
            case NODE_STRUCT_DECL:  tc_struct_decl(tc, decl); break;
            case NODE_FUNC_DECL:    tc_func_decl(tc, decl); break;
            case NODE_ENUM_DECL:    tc_enum_decl(tc, decl); break;
            case NODE_TYPEDEF_DECL: tc_typedef_decl(tc, decl); break;
            case NODE_CLASS_DECL:   tc_class_decl(tc, decl); break;
            case NODE_VAR_DECL: {
                /* Register global variable */
                const char *type_str = decl->as.var_decl.type;
                const char *var_name = decl->as.var_decl.name;
                TCType decl_type = resolve_tc_type(tc, type_str);
                scope_add_var(tc, var_name, decl_type, decl->as.var_decl.is_mut, decl->as.var_decl.is_shared,
                              decl->loc.line, decl->loc.col);
                break;
            }
            case NODE_IMPORT:       tc_import(tc, decl); break;
            case NODE_LINK:         break;
            default:                break;
        }
    }

    for (size_t i = 0; i < program->as.program.decls.count; i++) {
        AstNode *decl = program->as.program.decls.items[i];
        if (!decl) continue;
        if (decl->type == NODE_FUNC_DECL && decl->as.func_decl.body) {
            scope_push(tc);
            tc->current_func_node = decl;

            for (size_t j = 0; j < decl->as.func_decl.params.count; j++) {
                AstNode *p = decl->as.func_decl.params.items[j];
                TCType pt = resolve_tc_type(tc, p->as.param.type);
                scope_add_var(tc, p->as.param.name, pt, 1, 0,
                              p->loc.line, p->loc.col);
            }

            tc_block(tc, decl->as.func_decl.body);
            tc->current_func_node = NULL;
            scope_pop(tc);
        }

        if (decl->type == NODE_CLASS_DECL) {
            const char *class_name = decl->as.class_decl.name;
            for (size_t j = 0; j < decl->as.class_decl.methods.count; j++) {
                AstNode *m = decl->as.class_decl.methods.items[j];
                if (m && m->type == NODE_FUNC_DECL && m->as.func_decl.body) {
                    scope_push(tc);
                    tc->current_func_node = m;

                    TCType self_type = make_class_type(class_name);
                    scope_add_var(tc, "self", self_type, 1, 0,
                                  m->loc.line, m->loc.col);

                    for (size_t k = 0; k < m->as.func_decl.params.count; k++) {
                        AstNode *p = m->as.func_decl.params.items[k];
                        TCType pt = resolve_tc_type(tc, p->as.param.type);
                        scope_add_var(tc, p->as.param.name, pt, 1, 0,
                                      p->loc.line, p->loc.col);
                    }

                    tc_block(tc, m->as.func_decl.body);
                    tc->current_func_node = NULL;
                    scope_pop(tc);
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

int typecheck(AstNode *program, const char *filename, const char *src) {
    TCContext tc = {0};
    tc.filename = filename;
    tc.src = src;

    scope_push(&tc);
    tc_program(&tc, program);
    scope_pop(&tc);

    if (tc.error_count > 0 && !error_get_test_mode()) {
        fprintf(stderr, "penguinc: %d type error(s) found\n", tc.error_count);
    }

    for (size_t i = 0; i < tc.struct_count; i++) {
        free(tc.structs[i].name);
        for (size_t j = 0; j < tc.structs[i].field_count; j++) {
            free(tc.structs[i].field_names[j]);
            free_tc_type(&tc.structs[i].field_types[j]);
        }
        free(tc.structs[i].field_names);
        free(tc.structs[i].field_types);
    }
    free(tc.structs);

    for (size_t i = 0; i < tc.class_count; i++) {
        free(tc.classes[i].name);
        if (tc.classes[i].parent) free(tc.classes[i].parent);
        for (size_t j = 0; j < tc.classes[i].field_count; j++) {
            free(tc.classes[i].field_names[j]);
            free_tc_type(&tc.classes[i].field_types[j]);
        }
        free(tc.classes[i].field_names);
        free(tc.classes[i].field_types);
    }
    free(tc.classes);

    for (size_t i = 0; i < tc.func_count; i++) {
        free(tc.funcs[i].name);
        free(tc.funcs[i].param_types);
    }
    free(tc.funcs);

    for (size_t i = 0; i < tc.enum_count; i++)
        free(tc.enums[i].name);
    free(tc.enums);

    for (size_t i = 0; i < tc.typedef_count; i++) {
        free(tc.typedefs[i]);
        free(tc.typedef_origs[i]);
    }
    free(tc.typedefs);
    free(tc.typedef_origs);

    return tc.error_count;
}
