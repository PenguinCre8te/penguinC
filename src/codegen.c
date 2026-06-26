#include "codegen.h"
#include "error.h"
#include <llvm-c/Core.h>
#include <llvm-c/TargetMachine.h>
#include <llvm-c/Target.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/BitWriter.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    LLVMContextRef ctx;
    LLVMModuleRef  module;
    LLVMBuilderRef builder;
    LLVMValueRef   cur_fn;

    struct { char *name; LLVMTypeRef fn_ty; } *fn_types;
    size_t fn_type_count;
    size_t fn_type_cap;

    struct { char *name; LLVMValueRef val; LLVMTypeRef ty; LLVMTypeRef elem_ty; char *struct_name; } *vars;
    size_t var_count;
    size_t var_cap;

    struct { char *name; LLVMTypeRef ty; } *structs;
    size_t struct_count;
    size_t struct_cap;

    struct { char *struct_name; char **field_names; size_t field_count; } *struct_fields;
    size_t struct_field_count;
    size_t struct_field_cap;

    char **imports;
    size_t import_count;
    size_t import_cap;

    struct { char *name; long value; } *enums;
    size_t enum_count;
    size_t enum_cap;

    struct { char *name; char *parent; } *classes;
    size_t class_count;
    size_t class_cap;

    LLVMBasicBlockRef break_target;
    LLVMBasicBlockRef continue_target;

    struct { char *name; LLVMBasicBlockRef block; } *labels;
    size_t label_count;
    size_t label_cap;

    int loop_depth;
} CodegenCtx;

static void class_push(CodegenCtx *cg, const char *name, const char *parent) {
    if (cg->class_count >= cg->class_cap) {
        cg->class_cap = cg->class_cap ? cg->class_cap * 2 : 16;
        cg->classes = realloc(cg->classes, cg->class_cap * sizeof(*cg->classes));
    }
    cg->classes[cg->class_count].name = strdup(name);
    cg->classes[cg->class_count].parent = parent ? strdup(parent) : NULL;
    cg->class_count++;
}

static const char *class_lookup_parent(CodegenCtx *cg, const char *name) {
    for (size_t i = cg->class_count; i > 0; i--) {
        if (strcmp(cg->classes[i - 1].name, name) == 0)
            return cg->classes[i - 1].parent;
    }
    return NULL;
}

static void enum_push(CodegenCtx *cg, const char *name, long value) {
    if (cg->enum_count >= cg->enum_cap) {
        cg->enum_cap = cg->enum_cap ? cg->enum_cap * 2 : 16;
        cg->enums = realloc(cg->enums, cg->enum_cap * sizeof(*cg->enums));
    }
    cg->enums[cg->enum_count].name = strdup(name);
    cg->enums[cg->enum_count].value = value;
    cg->enum_count++;
}

static long enum_lookup(CodegenCtx *cg, const char *name, int *found) {
    for (size_t i = cg->enum_count; i > 0; i--) {
        if (strcmp(cg->enums[i - 1].name, name) == 0) {
            *found = 1;
            return cg->enums[i - 1].value;
        }
    }
    *found = 0;
    return 0;
}

static void fn_type_push(CodegenCtx *cg, const char *name, LLVMTypeRef fn_ty) {
    if (cg->fn_type_count >= cg->fn_type_cap) {
        cg->fn_type_cap = cg->fn_type_cap ? cg->fn_type_cap * 2 : 16;
        cg->fn_types = realloc(cg->fn_types, cg->fn_type_cap * sizeof(*cg->fn_types));
    }
    cg->fn_types[cg->fn_type_count].name = strdup(name);
    cg->fn_types[cg->fn_type_count].fn_ty = fn_ty;
    cg->fn_type_count++;
}

static LLVMTypeRef fn_type_lookup(CodegenCtx *cg, const char *name) {
    for (size_t i = cg->fn_type_count; i > 0; i--) {
        if (strcmp(cg->fn_types[i - 1].name, name) == 0)
            return cg->fn_types[i - 1].fn_ty;
    }
    return NULL;
}

static void var_push(CodegenCtx *cg, const char *name, LLVMValueRef val, LLVMTypeRef ty) {
    if (cg->var_count >= cg->var_cap) {
        cg->var_cap = cg->var_cap ? cg->var_cap * 2 : 16;
        cg->vars = realloc(cg->vars, cg->var_cap * sizeof(*cg->vars));
    }
    cg->vars[cg->var_count].name = strdup(name);
    cg->vars[cg->var_count].val  = val;
    cg->vars[cg->var_count].ty   = ty;
    cg->vars[cg->var_count].elem_ty = NULL;
    cg->vars[cg->var_count].struct_name = NULL;
    cg->var_count++;
}

static void var_set_elem_type(CodegenCtx *cg, const char *name, LLVMTypeRef elem_ty) {
    for (size_t i = cg->var_count; i > 0; i--) {
        if (strcmp(cg->vars[i - 1].name, name) == 0) {
            cg->vars[i - 1].elem_ty = elem_ty;
            return;
        }
    }
}

static LLVMTypeRef var_lookup_elem_type(CodegenCtx *cg, const char *name) {
    for (size_t i = cg->var_count; i > 0; i--) {
        if (strcmp(cg->vars[i - 1].name, name) == 0)
            return cg->vars[i - 1].elem_ty;
    }
    return NULL;
}

static void var_set_struct_name(CodegenCtx *cg, const char *name, const char *struct_name) {
    for (size_t i = cg->var_count; i > 0; i--) {
        if (strcmp(cg->vars[i - 1].name, name) == 0) {
            cg->vars[i - 1].struct_name = strdup(struct_name);
            return;
        }
    }
}

static const char *var_lookup_struct_name(CodegenCtx *cg, const char *name) {
    for (size_t i = cg->var_count; i > 0; i--) {
        if (strcmp(cg->vars[i - 1].name, name) == 0)
            return cg->vars[i - 1].struct_name;
    }
    return NULL;
}

static LLVMValueRef var_lookup(CodegenCtx *cg, const char *name) {
    for (size_t i = cg->var_count; i > 0; i--) {
        if (strcmp(cg->vars[i - 1].name, name) == 0)
            return cg->vars[i - 1].val;
    }
    return NULL;
}

static LLVMTypeRef var_lookup_type(CodegenCtx *cg, const char *name) {
    for (size_t i = cg->var_count; i > 0; i--) {
        if (strcmp(cg->vars[i - 1].name, name) == 0)
            return cg->vars[i - 1].ty;
    }
    return NULL;
}

static void struct_push(CodegenCtx *cg, const char *name, LLVMTypeRef ty) {
    if (cg->struct_count >= cg->struct_cap) {
        cg->struct_cap = cg->struct_cap ? cg->struct_cap * 2 : 16;
        cg->structs = realloc(cg->structs, cg->struct_cap * sizeof(*cg->structs));
    }
    cg->structs[cg->struct_count].name = strdup(name);
    cg->structs[cg->struct_count].ty   = ty;
    cg->struct_count++;
}

static LLVMTypeRef struct_lookup(CodegenCtx *cg, const char *name) {
    for (size_t i = 0; i < cg->struct_count; i++) {
        if (strcmp(cg->structs[i].name, name) == 0)
            return cg->structs[i].ty;
    }
    return NULL;
}

static void struct_push_fields(CodegenCtx *cg, const char *struct_name, char **field_names, size_t count) {
    if (cg->struct_field_count >= cg->struct_field_cap) {
        cg->struct_field_cap = cg->struct_field_cap ? cg->struct_field_cap * 2 : 16;
        cg->struct_fields = realloc(cg->struct_fields, cg->struct_field_cap * sizeof(*cg->struct_fields));
    }
    cg->struct_fields[cg->struct_field_count].struct_name = strdup(struct_name);
    cg->struct_fields[cg->struct_field_count].field_names = malloc(count * sizeof(char *));
    for (size_t i = 0; i < count; i++)
        cg->struct_fields[cg->struct_field_count].field_names[i] = strdup(field_names[i]);
    cg->struct_fields[cg->struct_field_count].field_count = count;
    cg->struct_field_count++;
}

static int struct_field_index(CodegenCtx *cg, const char *struct_name, const char *field_name) {
    for (size_t i = cg->struct_field_count; i > 0; i--) {
        if (strcmp(cg->struct_fields[i - 1].struct_name, struct_name) == 0) {
            for (size_t j = 0; j < cg->struct_fields[i - 1].field_count; j++) {
                if (strcmp(cg->struct_fields[i - 1].field_names[j], field_name) == 0)
                    return (int)j;
            }
            return -1;
        }
    }
    return -1;
}

static LLVMBasicBlockRef label_lookup(CodegenCtx *cg, const char *name) {
    for (size_t i = 0; i < cg->label_count; i++) {
        if (strcmp(cg->labels[i].name, name) == 0)
            return cg->labels[i].block;
    }
    return NULL;
}

static void label_push(CodegenCtx *cg, const char *name, LLVMBasicBlockRef block) {
    if (cg->label_count >= cg->label_cap) {
        cg->label_cap = cg->label_cap ? cg->label_cap * 2 : 16;
        cg->labels = realloc(cg->labels, cg->label_cap * sizeof(*cg->labels));
    }
    cg->labels[cg->label_count].name  = strdup(name);
    cg->labels[cg->label_count].block = block;
    cg->label_count++;
}

static LLVMValueRef get_or_declare_runtime_fn(CodegenCtx *cg, const char *name,
                                               LLVMTypeRef fn_type);

static LLVMTypeRef resolve_type(CodegenCtx *cg, const char *name) {
    /* Handle pointer types: strip trailing '*' and wrap in pointer */
    size_t len = strlen(name);
    if (len > 1 && name[len - 1] == '*') {
        char base[256];
        snprintf(base, sizeof(base), "%.*s", (int)(len - 1), name);
        return LLVMPointerType(resolve_type(cg, base), 0);
    }
    if (strcmp(name, "int") == 0 || strcmp(name, "long") == 0) return LLVMInt64TypeInContext(cg->ctx);
    if (strcmp(name, "float") == 0)  return LLVMDoubleTypeInContext(cg->ctx);
    if (strcmp(name, "bool") == 0)   return LLVMInt1TypeInContext(cg->ctx);
    if (strcmp(name, "void") == 0)   return LLVMVoidTypeInContext(cg->ctx);
    if (strcmp(name, "string") == 0) return LLVMPointerType(LLVMInt8TypeInContext(cg->ctx), 0);
    LLVMTypeRef st = struct_lookup(cg, name);
    if (st) return st;
    return LLVMPointerType(LLVMInt8TypeInContext(cg->ctx), 0);
}

__attribute__((unused))
static LLVMTypeRef resolve_type_node(CodegenCtx *cg, AstNode *node) {
    (void)cg;
    if (!node) return LLVMVoidTypeInContext(cg->ctx);
    switch (node->type) {
        case NODE_IDENTIFIER: return resolve_type(cg, node->as.ident.name);
        case NODEPointerType: return LLVMPointerType(resolve_type(cg, node->as.ptr_type.base_type), 0);
        case NODE_BORROW_EXPR: return LLVMPointerType(resolve_type_node(cg, node->as.borrow_expr.expr), 0);
        default: return resolve_type(cg, "void");
    }
}

static int is_float_type(LLVMTypeRef ty) {
    return LLVMGetTypeKind(ty) == LLVMDoubleTypeKind;
}

static LLVMValueRef codegen_expr(CodegenCtx *cg, AstNode *node);
static void codegen_stmt(CodegenCtx *cg, AstNode *node);

static LLVMTypeRef malloc_fn_type;
static LLVMTypeRef free_fn_type;

static LLVMValueRef get_malloc(CodegenCtx *cg) {
    LLVMValueRef fn = LLVMGetNamedFunction(cg->module, "malloc");
    if (!fn) {
        malloc_fn_type = LLVMFunctionType(
            LLVMPointerType(LLVMInt8TypeInContext(cg->ctx), 0),
            (LLVMTypeRef[]){ LLVMInt64TypeInContext(cg->ctx) }, 1, 0);
        fn = LLVMAddFunction(cg->module, "malloc", malloc_fn_type);
        fn_type_push(cg, "malloc", malloc_fn_type);
    }
    return fn;
}

static LLVMValueRef get_free(CodegenCtx *cg) {
    LLVMValueRef fn = LLVMGetNamedFunction(cg->module, "free");
    if (!fn) {
        free_fn_type = LLVMFunctionType(
            LLVMVoidTypeInContext(cg->ctx),
            (LLVMTypeRef[]){ LLVMPointerType(LLVMInt8TypeInContext(cg->ctx), 0) }, 1, 0);
        fn = LLVMAddFunction(cg->module, "free", free_fn_type);
        fn_type_push(cg, "free", free_fn_type);
    }
    return fn;
}

static LLVMValueRef call_malloc(CodegenCtx *cg, LLVMValueRef size) {
    LLVMValueRef fn = get_malloc(cg);
    return LLVMBuildCall2(cg->builder, malloc_fn_type, fn, (LLVMValueRef[]){ size }, 1, "alloc");
}

static LLVMValueRef call_free(CodegenCtx *cg, LLVMValueRef ptr) {
    LLVMValueRef fn = get_free(cg);
    LLVMValueRef casted = LLVMBuildBitCast(cg->builder, ptr,
        LLVMPointerType(LLVMInt8TypeInContext(cg->ctx), 0), "free.cast");
    return LLVMBuildCall2(cg->builder, free_fn_type, fn, (LLVMValueRef[]){ casted }, 1, "");
}

static void codegen_block(CodegenCtx *cg, AstNode *node) {
    for (size_t i = 0; i < node->as.block.stmts.count; i++) {
        codegen_stmt(cg, node->as.block.stmts.items[i]);
    }
}

static LLVMValueRef codegen_string_lit(CodegenCtx *cg, AstNode *node) {
    return LLVMBuildGlobalStringPtr(cg->builder, node->as.string_lit.value, "str");
}

static LLVMValueRef codegen_int_lit(CodegenCtx *cg, AstNode *node) {
    return LLVMConstInt(LLVMInt64TypeInContext(cg->ctx),
                        (unsigned long long)node->as.int_lit.value, 1);
}

static LLVMValueRef codegen_float_lit(CodegenCtx *cg, AstNode *node) {
    return LLVMConstReal(LLVMDoubleTypeInContext(cg->ctx), node->as.float_lit.value);
}

static LLVMValueRef codegen_ident(CodegenCtx *cg, AstNode *node) {
    const char *name = node->as.ident.name;
    LLVMValueRef val = var_lookup(cg, name);
    if (val) {
        LLVMTypeRef var_ty = var_lookup_type(cg, name);
        if (var_ty && LLVMGetTypeKind(var_ty) == LLVMFunctionTypeKind)
            return val;
        if (var_ty)
            return LLVMBuildLoad2(cg->builder, var_ty, val, name);
        return val;
    }
    val = LLVMGetNamedFunction(cg->module, name);
    if (val) return val;
    if (strcmp(name, "NULL") == 0)
        return LLVMConstNull(LLVMPointerType(LLVMInt8TypeInContext(cg->ctx), 0));
    /* Check enum constants */
    int found = 0;
    long eval = enum_lookup(cg, name, &found);
    if (found)
        return LLVMConstInt(LLVMInt64TypeInContext(cg->ctx), (unsigned long long)eval, 0);
    error_at(node->loc, ERR_SEMANTIC, "undefined variable '%s'", name);
    return NULL;
}

static LLVMValueRef codegen_binary(CodegenCtx *cg, AstNode *node) {
    const char *op = node->as.binary.op;

    if (strcmp(op, "&&") == 0 || strcmp(op, "||") == 0) {
        LLVMValueRef left = codegen_expr(cg, node->as.binary.left);
        LLVMBasicBlockRef start = LLVMGetInsertBlock(cg->builder);
        LLVMValueRef fn = LLVMGetBasicBlockParent(start);
        LLVMBasicBlockRef rhs_bb = LLVMAppendBasicBlockInContext(cg->ctx, fn, "sc.rhs");
        LLVMBasicBlockRef merge  = LLVMAppendBasicBlockInContext(cg->ctx, fn, "sc.merge");
        if (strcmp(op, "&&") == 0)
            LLVMBuildCondBr(cg->builder, left, rhs_bb, merge);
        else
            LLVMBuildCondBr(cg->builder, left, merge, rhs_bb);
        LLVMPositionBuilderAtEnd(cg->builder, rhs_bb);
        LLVMValueRef right = codegen_expr(cg, node->as.binary.right);
        LLVMBuildBr(cg->builder, merge);
        LLVMBasicBlockRef rhs_end = LLVMGetInsertBlock(cg->builder);
        LLVMPositionBuilderAtEnd(cg->builder, merge);
        LLVMValueRef phi = LLVMBuildPhi(cg->builder, LLVMInt1TypeInContext(cg->ctx), "sc.phi");
        LLVMValueRef lv = (strcmp(op, "&&") == 0)
            ? LLVMConstNull(LLVMInt1TypeInContext(cg->ctx))
            : LLVMConstInt(LLVMInt1TypeInContext(cg->ctx), 1, 0);
        LLVMAddIncoming(phi, &lv, &start, 1);
        LLVMAddIncoming(phi, &right, &rhs_end, 1);
        return phi;
    }

    LLVMValueRef left  = codegen_expr(cg, node->as.binary.left);
    LLVMValueRef right = codegen_expr(cg, node->as.binary.right);
    if (!left || !right) return NULL;

    /* String concatenation: ptr + ptr -> str_concat(left, right) */
    if (strcmp(op, "+") == 0 &&
        LLVMGetTypeKind(LLVMTypeOf(left)) == LLVMPointerTypeKind &&
        LLVMGetTypeKind(LLVMTypeOf(right)) == LLVMPointerTypeKind) {
        LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(cg->ctx), 0);
        LLVMTypeRef fn_ty = LLVMFunctionType(i8ptr, (LLVMTypeRef[]){i8ptr, i8ptr}, 2, 0);
        LLVMValueRef fn = get_or_declare_runtime_fn(cg, "str_concat", fn_ty);
        return LLVMBuildCall2(cg->builder, fn_ty, fn, (LLVMValueRef[]){left, right}, 2, "strcat");
    }

    int flt = is_float_type(LLVMTypeOf(left));

    if (strcmp(op, "+") == 0)  return flt ? LLVMBuildFAdd(cg->builder, left, right, "fadd") : LLVMBuildAdd(cg->builder, left, right, "add");
    if (strcmp(op, "-") == 0)  return flt ? LLVMBuildFSub(cg->builder, left, right, "fsub") : LLVMBuildSub(cg->builder, left, right, "sub");
    if (strcmp(op, "*") == 0)  return flt ? LLVMBuildFMul(cg->builder, left, right, "fmul") : LLVMBuildMul(cg->builder, left, right, "mul");
    if (strcmp(op, "/") == 0)  return flt ? LLVMBuildFDiv(cg->builder, left, right, "fdiv") : LLVMBuildSDiv(cg->builder, left, right, "sdiv");
    if (strcmp(op, "%") == 0)  return LLVMBuildSRem(cg->builder, left, right, "srem");
    if (strcmp(op, "<") == 0)  return flt ? LLVMBuildFCmp(cg->builder, LLVMRealOLT, left, right, "flt") : LLVMBuildICmp(cg->builder, LLVMIntSLT, left, right, "slt");
    if (strcmp(op, ">") == 0)  return flt ? LLVMBuildFCmp(cg->builder, LLVMRealOGT, left, right, "fgt") : LLVMBuildICmp(cg->builder, LLVMIntSGT, left, right, "sgt");
    if (strcmp(op, "<=") == 0) return flt ? LLVMBuildFCmp(cg->builder, LLVMRealOLE, left, right, "fle") : LLVMBuildICmp(cg->builder, LLVMIntSLE, left, right, "sle");
    if (strcmp(op, ">=") == 0) return flt ? LLVMBuildFCmp(cg->builder, LLVMRealOGE, left, right, "fge") : LLVMBuildICmp(cg->builder, LLVMIntSGE, left, right, "sge");
    if (strcmp(op, "==") == 0) return flt ? LLVMBuildFCmp(cg->builder, LLVMRealOEQ, left, right, "feq") : LLVMBuildICmp(cg->builder, LLVMIntEQ, left, right, "eq");
    if (strcmp(op, "!=") == 0) return flt ? LLVMBuildFCmp(cg->builder, LLVMRealONE, left, right, "fne") : LLVMBuildICmp(cg->builder, LLVMIntNE, left, right, "ne");
    if (strcmp(op, "&") == 0)  return LLVMBuildAnd(cg->builder, left, right, "and");
    if (strcmp(op, "|") == 0)  return LLVMBuildOr(cg->builder, left, right, "or");
    if (strcmp(op, "^") == 0)  return LLVMBuildXor(cg->builder, left, right, "xor");
    if (strcmp(op, "<<") == 0) return LLVMBuildShl(cg->builder, left, right, "shl");
    if (strcmp(op, ">>") == 0) return LLVMBuildAShr(cg->builder, left, right, "ashr");
    if (strcmp(op, "in") == 0) return LLVMConstInt(LLVMInt1TypeInContext(cg->ctx), 1, 0);

    error_at(node->loc, ERR_SEMANTIC, "unsupported binary operator '%s'", op);
    return NULL;
}

static LLVMValueRef codegen_unary(CodegenCtx *cg, AstNode *node) {
    LLVMValueRef operand = codegen_expr(cg, node->as.unary.operand);
    if (!operand) return NULL;
    const char *op = node->as.unary.op;
    if (strcmp(op, "-") == 0)
        return is_float_type(LLVMTypeOf(operand))
            ? LLVMBuildFNeg(cg->builder, operand, "fneg")
            : LLVMBuildNeg(cg->builder, operand, "neg");
    if (strcmp(op, "~") == 0)
        return LLVMBuildXor(cg->builder, operand,
            LLVMConstAllOnes(LLVMInt64TypeInContext(cg->ctx)), "not");
    if (strcmp(op, "*") == 0)
        return LLVMBuildLoad2(cg->builder, LLVMInt64TypeInContext(cg->ctx),
                              operand, "deref");
    error_at(node->loc, ERR_SEMANTIC, "unsupported unary operator '%s'", op);
    return NULL;
}

static LLVMValueRef get_lvalue(CodegenCtx *cg, AstNode *node) {
    if (node->type == NODE_IDENTIFIER) {
        LLVMValueRef v = var_lookup(cg, node->as.ident.name);
        if (v) return v;
    }
    if (node->type == NODE_MEMBER) {
        const char *field = node->as.member.member;
        LLVMTypeRef inner = NULL;
        LLVMValueRef obj_ptr = NULL;
        const char *struct_name = NULL;
        const char *obj_name = NULL;
        if (node->as.member.object->type == NODE_IDENTIFIER) {
            obj_name = node->as.member.object->as.ident.name;
        } else if (node->as.member.object->type == NODE_SELF_REF) {
            obj_name = "self";
        }
        if (obj_name) {
            obj_ptr = var_lookup(cg, obj_name);
            struct_name = var_lookup_struct_name(cg, obj_name);
            /* Check if this is a class instance (has elem_ty) */
            LLVMTypeRef elem = var_lookup_elem_type(cg, obj_name);
            if (elem && LLVMGetTypeKind(elem) == LLVMStructTypeKind) {
                inner = elem;
                obj_ptr = LLVMBuildLoad2(cg->builder,
                    LLVMPointerType(inner, 0), obj_ptr, "deref");
            } else {
                inner = var_lookup_type(cg, obj_name);
            }
        }
        if (inner && LLVMGetTypeKind(inner) == LLVMStructTypeKind) {
            int idx = -1;
            if (struct_name)
                idx = struct_field_index(cg, struct_name, field);
            if (idx < 0) {
                unsigned count = LLVMCountStructElementTypes(inner);
                unsigned h = 0;
                for (const char *p = field; *p; p++) h = h * 31 + (unsigned char)*p;
                idx = (int)(h % count);
            }
            return LLVMBuildGEP2(cg->builder, inner, obj_ptr,
                (LLVMValueRef[]){
                    LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0),
                    LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), (unsigned)idx, 0)
                }, 2, "field.ptr");
        }
    }
    if (node->type == NODE_UNARY && strcmp(node->as.unary.op, "*") == 0) {
        return codegen_expr(cg, node->as.unary.operand);
    }
    return codegen_expr(cg, node);
}

static LLVMValueRef codegen_assign(CodegenCtx *cg, AstNode *node) {
    LLVMValueRef target = get_lvalue(cg, node->as.assign.target);
    LLVMValueRef value  = codegen_expr(cg, node->as.assign.value);
    if (!target || !value) return NULL;
    const char *op = node->as.assign.op;
    if (strcmp(op, "=") == 0) {
        LLVMBuildStore(cg->builder, value, target);
        return value;
    }
    /* Use the RHS type for the load since LHS is an alloca (opaque ptr in LLVM 20) */
    LLVMValueRef loaded = LLVMBuildLoad2(cg->builder, LLVMTypeOf(value), target, "cmp.tmp");
    LLVMValueRef result = NULL;
    if (strcmp(op, "+=") == 0)  result = LLVMBuildAdd(cg->builder, loaded, value, "add.tmp");
    if (strcmp(op, "-=") == 0)  result = LLVMBuildSub(cg->builder, loaded, value, "sub.tmp");
    if (strcmp(op, "*=") == 0)  result = LLVMBuildMul(cg->builder, loaded, value, "mul.tmp");
    if (strcmp(op, "/=") == 0)  result = LLVMBuildSDiv(cg->builder, loaded, value, "div.tmp");
    if (result) {
        LLVMBuildStore(cg->builder, result, target);
        return result;
    }
    error_at(node->loc, ERR_SEMANTIC, "unsupported assignment operator '%s'", op);
    return NULL;
}

static LLVMValueRef get_or_declare_runtime_fn(CodegenCtx *cg, const char *name,
                                               LLVMTypeRef fn_type) {
    LLVMValueRef fn = LLVMGetNamedFunction(cg->module, name);
    if (!fn) {
        fn = LLVMAddFunction(cg->module, name, fn_type);
        fn_type_push(cg, name, fn_type);
    }
    return fn;
}

static LLVMValueRef codegen_call(CodegenCtx *cg, AstNode *node) {
    if (!node->as.call.callee)
        return LLVMConstNull(LLVMPointerType(LLVMInt8TypeInContext(cg->ctx), 0));

    LLVMValueRef callee = NULL;
    LLVMTypeRef callee_fn_type = NULL;

    if (node->as.call.callee->type == NODE_MEMBER) {
        AstNode *obj = node->as.call.callee->as.member.object;
        const char *member = node->as.call.callee->as.member.member;
        if (obj->type == NODE_IDENTIFIER) {
            char buf[256];
            snprintf(buf, sizeof(buf), "%s.%s", obj->as.ident.name, member);

            LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(cg->ctx), 0);
            LLVMTypeRef i64ty = LLVMInt64TypeInContext(cg->ctx);

            if (strcmp(buf, "io.print") == 0 || strcmp(buf, "io.println") == 0) {
                callee_fn_type = LLVMFunctionType(i64ty, (LLVMTypeRef[]){i8ptr, i64ty}, 2, 0);
                callee = get_or_declare_runtime_fn(cg, buf, callee_fn_type);
            } else if (strcmp(buf, "io.print_int") == 0 || strcmp(buf, "io.println_int") == 0) {
                callee_fn_type = LLVMFunctionType(i64ty, (LLVMTypeRef[]){i64ty}, 1, 0);
                callee = get_or_declare_runtime_fn(cg, buf, callee_fn_type);
            } else {
                callee = LLVMGetNamedFunction(cg->module, buf);
                if (callee) {
                    callee_fn_type = fn_type_lookup(cg, buf);
                    if (!callee_fn_type) {
                        /* Fallback: assume all-ptr args, ptr return */
                        size_t ac = node->as.call.args.count;
                        LLVMTypeRef *argt = ac > 0 ? malloc(ac * sizeof(LLVMTypeRef)) : NULL;
                        for (size_t i = 0; i < ac; i++) argt[i] = i8ptr;
                        callee_fn_type = LLVMFunctionType(i8ptr, argt, (unsigned)ac, 0);
                        if (argt) free(argt);
                    }
                } else {
                    /* Declare with all-ptr args as fallback */
                    size_t ac = node->as.call.args.count;
                    LLVMTypeRef *argt = ac > 0 ? malloc(ac * sizeof(LLVMTypeRef)) : NULL;
                    for (size_t i = 0; i < ac; i++) argt[i] = i8ptr;
                    callee_fn_type = LLVMFunctionType(i8ptr, argt, (unsigned)ac, 0);
                    callee = get_or_declare_runtime_fn(cg, buf, callee_fn_type);
                    if (argt) free(argt);
                }
            }
        }
        if (!callee) {
            callee = LLVMGetNamedFunction(cg->module, member);
            if (callee) callee_fn_type = fn_type_lookup(cg, member);
        }
    } else if (node->as.call.callee->type == NODE_IDENTIFIER) {
        const char *ident_name = node->as.call.callee->as.ident.name;
        callee = LLVMGetNamedFunction(cg->module, ident_name);
        if (callee) callee_fn_type = fn_type_lookup(cg, ident_name);
    } else {
        callee = codegen_expr(cg, node->as.call.callee);
        if (callee) callee_fn_type = fn_type_lookup(cg, "unknown");
    }

    if (!callee || !callee_fn_type) {
        error_at(node->loc, ERR_SEMANTIC, "call to undefined function");
        return NULL;
    }

    size_t argc = node->as.call.args.count;
    LLVMValueRef *args = argc > 0 ? malloc(argc * sizeof(LLVMValueRef)) : NULL;
    for (size_t i = 0; i < argc; i++)
        args[i] = codegen_expr(cg, node->as.call.args.items[i]);

    /* Auto-insert string length for io.print / io.println called with one string arg */
    int is_io_print = 0;
    if (node->as.call.callee->type == NODE_MEMBER) {
        AstNode *obj = node->as.call.callee->as.member.object;
        const char *m = node->as.call.callee->as.member.member;
        if (obj->type == NODE_IDENTIFIER &&
            strcmp(obj->as.ident.name, "io") == 0 &&
            (strcmp(m, "print") == 0 || strcmp(m, "println") == 0) &&
            argc == 1) {
            is_io_print = 1;
        }
    }
    if (is_io_print && args) {
        LLVMValueRef str_arg = args[0];
        LLVMTypeRef i64ty = LLVMInt64TypeInContext(cg->ctx);
        LLVMValueRef len;
        if (node->as.call.args.items[0]->type == NODE_STRING_LIT) {
            const char *s = node->as.call.args.items[0]->as.string_lit.value;
            len = LLVMConstInt(i64ty, (unsigned long long)strlen(s), 0);
        } else {
            LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(cg->ctx), 0);
            LLVMTypeRef strty = LLVMFunctionType(i64ty, (LLVMTypeRef[]){i8ptr}, 1, 0);
            LLVMValueRef sfn = get_or_declare_runtime_fn(cg, "strlen", strty);
            len = LLVMBuildCall2(cg->builder, strty, sfn, (LLVMValueRef[]){str_arg}, 1, "strlen");
        }
        args = realloc(args, 2 * sizeof(LLVMValueRef));
        args[1] = len;
        argc = 2;
    }

    LLVMValueRef result = LLVMBuildCall2(cg->builder, callee_fn_type, callee, args,
                                          (unsigned)argc, argc > 0 ? "call" : "");
    if (args) free(args);
    return result;
}

static LLVMValueRef codegen_ternary(CodegenCtx *cg, AstNode *node) {
    LLVMValueRef cond = codegen_expr(cg, node->as.ternary.cond);
    LLVMBasicBlockRef start = LLVMGetInsertBlock(cg->builder);
    LLVMValueRef fn = LLVMGetBasicBlockParent(start);
    LLVMBasicBlockRef then_bb  = LLVMAppendBasicBlockInContext(cg->ctx, fn, "tern.then");
    LLVMBasicBlockRef else_bb  = LLVMAppendBasicBlockInContext(cg->ctx, fn, "tern.else");
    LLVMBasicBlockRef merge_bb = LLVMAppendBasicBlockInContext(cg->ctx, fn, "tern.merge");
    LLVMBuildCondBr(cg->builder, cond, then_bb, else_bb);
    LLVMPositionBuilderAtEnd(cg->builder, then_bb);
    LLVMValueRef tv = codegen_expr(cg, node->as.ternary.then_expr);
    LLVMBuildBr(cg->builder, merge_bb);
    LLVMBasicBlockRef te = LLVMGetInsertBlock(cg->builder);
    LLVMPositionBuilderAtEnd(cg->builder, else_bb);
    LLVMValueRef ev = codegen_expr(cg, node->as.ternary.else_expr);
    LLVMBuildBr(cg->builder, merge_bb);
    LLVMBasicBlockRef ee = LLVMGetInsertBlock(cg->builder);
    LLVMPositionBuilderAtEnd(cg->builder, merge_bb);
    LLVMValueRef phi = LLVMBuildPhi(cg->builder, LLVMTypeOf(tv), "tern.phi");
    LLVMAddIncoming(phi, &tv, &te, 1);
    LLVMAddIncoming(phi, &ev, &ee, 1);
    return phi;
}

static LLVMValueRef codegen_cast(CodegenCtx *cg, AstNode *node) {
    LLVMValueRef operand = codegen_expr(cg, node->as.cast_expr.operand);
    LLVMTypeRef target_ty = resolve_type(cg, node->as.cast_expr.type_name);
    LLVMTypeRef src_ty = LLVMTypeOf(operand);
    if (LLVMGetTypeKind(src_ty) == LLVMIntegerTypeKind &&
        LLVMGetTypeKind(target_ty) == LLVMIntegerTypeKind)
        return LLVMBuildIntCast2(cg->builder, operand, target_ty, 1, "cast");
    if (LLVMGetTypeKind(src_ty) == LLVMIntegerTypeKind &&
        LLVMGetTypeKind(target_ty) == LLVMDoubleTypeKind)
        return LLVMBuildSIToFP(cg->builder, operand, target_ty, "cast");
    if (LLVMGetTypeKind(src_ty) == LLVMDoubleTypeKind &&
        LLVMGetTypeKind(target_ty) == LLVMIntegerTypeKind)
        return LLVMBuildFPToSI(cg->builder, operand, target_ty, "cast");
    if (LLVMGetTypeKind(target_ty) == LLVMPointerTypeKind)
        return LLVMBuildBitCast(cg->builder, operand, target_ty, "cast");
    return operand;
}

static LLVMValueRef codegen_sizeof(CodegenCtx *cg, AstNode *node) {
    LLVMTypeRef ty = resolve_type(cg, node->as.sizeof_expr.type_name);
    LLVMValueRef sz = LLVMSizeOf(ty);
    return LLVMBuildBitCast(cg->builder, sz, LLVMInt64TypeInContext(cg->ctx), "sizeof");
}

static LLVMValueRef codegen_new_expr(CodegenCtx *cg, AstNode *node) {
    LLVMTypeRef ty = resolve_type(cg, node->as.new_expr.type_name);
    LLVMValueRef size = LLVMSizeOf(ty);
    LLVMValueRef ptr = call_malloc(cg, size);
    LLVMValueRef typed = LLVMBuildBitCast(cg->builder, ptr,
        LLVMPointerType(ty, 0), "new.typed");
    if (node->as.new_expr.args.count > 0) {
        char ctor_name[256];
        snprintf(ctor_name, sizeof(ctor_name), "%s.new", node->as.new_expr.type_name);
        LLVMValueRef ctor = LLVMGetNamedFunction(cg->module, ctor_name);
        if (ctor) {
            size_t argc = node->as.new_expr.args.count + 1;
            LLVMValueRef *args = malloc(argc * sizeof(LLVMValueRef));
            args[0] = typed;
            for (size_t i = 0; i < node->as.new_expr.args.count; i++)
                args[i + 1] = codegen_expr(cg, node->as.new_expr.args.items[i]);
            LLVMTypeRef ctor_fn_type = fn_type_lookup(cg, ctor_name);
            if (!ctor_fn_type) ctor_fn_type = fn_type_lookup(cg, node->as.new_expr.type_name);
            LLVMBuildCall2(cg->builder, ctor_fn_type, ctor, args, (unsigned)argc, "");
            free(args);
        }
    }
    return typed;
}

static LLVMValueRef codegen_alloc(CodegenCtx *cg, AstNode *node) {
    LLVMValueRef size = codegen_expr(cg, node->as.alloc_expr.size);
    return call_malloc(cg, size);
}

static LLVMValueRef codegen_free(CodegenCtx *cg, AstNode *node) {
    LLVMValueRef expr = codegen_expr(cg, node->as.free_expr.expr);
    return call_free(cg, expr);
}

static LLVMValueRef codegen_member(CodegenCtx *cg, AstNode *node) {
    const char *field = node->as.member.member;

    /* Try to get the struct type from our type map */
    LLVMTypeRef inner = NULL;
    LLVMValueRef obj_ptr = NULL;
    const char *struct_name = NULL;
    const char *obj_name = NULL;
    if (node->as.member.object->type == NODE_IDENTIFIER) {
        obj_name = node->as.member.object->as.ident.name;
    } else if (node->as.member.object->type == NODE_SELF_REF) {
        obj_name = "self";
    }
    if (obj_name) {
        obj_ptr = var_lookup(cg, obj_name);
        struct_name = var_lookup_struct_name(cg, obj_name);
        /* Check if this is a class instance (has elem_ty) */
        LLVMTypeRef elem = var_lookup_elem_type(cg, obj_name);
        if (elem && LLVMGetTypeKind(elem) == LLVMStructTypeKind) {
            inner = elem;
            /* Load the pointer from the alloca */
            obj_ptr = LLVMBuildLoad2(cg->builder,
                LLVMPointerType(inner, 0), obj_ptr, "deref");
        } else {
            inner = var_lookup_type(cg, obj_name);
        }
    }

    if (inner && LLVMGetTypeKind(inner) == LLVMStructTypeKind) {
        int idx = -1;
        if (struct_name)
            idx = struct_field_index(cg, struct_name, field);
        if (idx < 0) {
            /* Fallback: hash-based lookup */
            unsigned count = LLVMCountStructElementTypes(inner);
            unsigned h = 0;
            for (const char *p = field; *p; p++) h = h * 31 + (unsigned char)*p;
            idx = (int)(h % count);
        }
        LLVMValueRef ptr = LLVMBuildGEP2(cg->builder, inner, obj_ptr,
            (LLVMValueRef[]){
                LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0),
                LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), (unsigned)idx, 0)
            }, 2, "field.ptr");
        return LLVMBuildLoad2(cg->builder,
            LLVMStructGetTypeAtIndex(inner, (unsigned)idx), ptr, field);
    }

    /* Fallback: load the object and return it */
    LLVMValueRef obj = codegen_expr(cg, node->as.member.object);
    return obj;
}

static LLVMValueRef codegen_self_ref(CodegenCtx *cg, AstNode *node) {
    (void)node;
    if (cg->cur_fn) return LLVMGetParam(cg->cur_fn, 0);
    return LLVMConstNull(LLVMPointerType(LLVMInt8TypeInContext(cg->ctx), 0));
}

static LLVMValueRef codegen_super_call(CodegenCtx *cg, AstNode *node) {
    /* Find the parent class from the current function's class_name */
    const char *parent = NULL;
    if (cg->cur_fn) {
        const char *fn_name = LLVMGetValueName(cg->cur_fn);
        /* Strip ".method" suffix to get class name */
        char class_name[256];
        const char *dot = strrchr(fn_name, '.');
        if (dot) {
            size_t len = dot - fn_name;
            if (len < sizeof(class_name)) {
                memcpy(class_name, fn_name, len);
                class_name[len] = '\0';
                parent = class_lookup_parent(cg, class_name);
            }
        }
    }

    char name[256];
    if (parent) {
        snprintf(name, sizeof(name), "%s.%s", parent, node->as.super_call.method);
    } else {
        snprintf(name, sizeof(name), "%s.new", node->as.super_call.method);
    }
    LLVMValueRef callee = LLVMGetNamedFunction(cg->module, name);
    if (!callee) {
        LLVMTypeRef fn_ty = LLVMFunctionType(LLVMVoidTypeInContext(cg->ctx), NULL, 0, 0);
        callee = LLVMAddFunction(cg->module, name, fn_ty);
        fn_type_push(cg, name, fn_ty);
    }
    size_t argc = node->as.super_call.args.count + 1;
    LLVMValueRef *args = malloc(argc * sizeof(LLVMValueRef));
    args[0] = codegen_self_ref(cg, node);
    for (size_t i = 0; i < node->as.super_call.args.count; i++)
        args[i + 1] = codegen_expr(cg, node->as.super_call.args.items[i]);
    LLVMTypeRef callee_ft = fn_type_lookup(cg, name);
    LLVMBuildCall2(cg->builder, callee_ft, callee, args, (unsigned)argc, "");
    free(args);
    return LLVMConstNull(LLVMInt1TypeInContext(cg->ctx));
}

static LLVMValueRef codegen_range(CodegenCtx *cg, AstNode *node) {
    return codegen_expr(cg, node->as.range.start);
}

static LLVMValueRef codegen_fstring(CodegenCtx *cg, AstNode *node) {
    if (node->as.fstring.parts.count > 0) {
        AstNode *first = node->as.fstring.parts.items[0];
        if (first->type == NODE_FSTRING_PART && first->as.fstring_part.text)
            return LLVMBuildGlobalStringPtr(cg->builder, first->as.fstring_part.text, "fstr");
    }
    return LLVMBuildGlobalStringPtr(cg->builder, "", "fstr.empty");
}

static LLVMValueRef codegen_fstring_part(CodegenCtx *cg, AstNode *node) {
    if (node->as.fstring_part.expr)
        return codegen_expr(cg, node->as.fstring_part.expr);
    return LLVMBuildGlobalStringPtr(cg->builder,
        node->as.fstring_part.text ? node->as.fstring_part.text : "", "fstpart");
}

static LLVMValueRef codegen_drop_expr(CodegenCtx *cg, AstNode *node) {
    AstNode *expr = node->as.drop_expr.expr;
    if (expr->type == NODE_CALL && expr->as.call.callee &&
        expr->as.call.callee->type == NODE_MEMBER) {
        AstNode *obj = expr->as.call.callee->as.member.object;
        if (obj->type == NODE_IDENTIFIER &&
            strcmp(obj->as.ident.name, "mem") == 0 &&
            expr->as.call.args.count > 0) {
            return codegen_free(cg, ast_new_free_expr(node->loc, expr->as.call.args.items[0]));
        }
    }
    return LLVMConstNull(LLVMInt1TypeInContext(cg->ctx));
}

static LLVMValueRef codegen_borrow(CodegenCtx *cg, AstNode *node) {
    return codegen_expr(cg, node->as.borrow_expr.expr);
}

static LLVMValueRef codegen_expr(CodegenCtx *cg, AstNode *node) {
    if (!node) return NULL;
    switch (node->type) {
        case NODE_INT_LIT:      return codegen_int_lit(cg, node);
        case NODE_FLOAT_LIT:    return codegen_float_lit(cg, node);
        case NODE_STRING_LIT:   return codegen_string_lit(cg, node);
        case NODE_IDENTIFIER:   return codegen_ident(cg, node);
        case NODE_BINARY:       return codegen_binary(cg, node);
        case NODE_UNARY:        return codegen_unary(cg, node);
        case NODE_ASSIGN:       return codegen_assign(cg, node);
        case NODE_CALL:         return codegen_call(cg, node);
        case NODE_TERNARY:      return codegen_ternary(cg, node);
        case NODE_CAST:         return codegen_cast(cg, node);
        case NODE_SIZEOF_EXPR:  return codegen_sizeof(cg, node);
        case NODE_NEW_EXPR:     return codegen_new_expr(cg, node);
        case NODE_ALLOC_EXPR:   return codegen_alloc(cg, node);
        case NODE_FREE_EXPR:    return codegen_free(cg, node);
        case NODE_MEMBER:       return codegen_member(cg, node);
        case NODE_RANGE:        return codegen_range(cg, node);
        case NODE_SELF_REF:     return codegen_self_ref(cg, node);
        case NODE_SUPER_CALL:   return codegen_super_call(cg, node);
        case NODE_DROP_EXPR:    return codegen_drop_expr(cg, node);
        case NODE_BORROW_EXPR:  return codegen_borrow(cg, node);
        case NODE_FSTRING:      return codegen_fstring(cg, node);
        case NODE_FSTRING_PART: return codegen_fstring_part(cg, node);
        case NODE_STMT_EXPR:    return codegen_expr(cg, node->as.stmt_expr.expr);
        default:
            error_at(node->loc, ERR_SEMANTIC,
                     "cannot codegen expression node type %d", node->type);
            return NULL;
    }
}

static void codegen_return(CodegenCtx *cg, AstNode *node) {
    if (node->as.ret.value) {
        LLVMValueRef val = codegen_expr(cg, node->as.ret.value);
        LLVMBuildRet(cg->builder, val);
    } else {
        LLVMBuildRetVoid(cg->builder);
    }
}

static void codegen_var_decl(CodegenCtx *cg, AstNode *node) {
    const char *type_name = node->as.var_decl.type;
    const char *var_name  = node->as.var_decl.name;
    LLVMTypeRef ty = resolve_type(cg, type_name);

    /* If init is a new expression, the variable holds a pointer to the type */
    if (node->as.var_decl.init && node->as.var_decl.init->type == NODE_NEW_EXPR &&
        LLVMGetTypeKind(ty) == LLVMStructTypeKind) {
        LLVMTypeRef ptr_ty = LLVMPointerType(ty, 0);
        LLVMValueRef alloca_inst = LLVMBuildAlloca(cg->builder, ptr_ty, var_name);
        var_push(cg, var_name, alloca_inst, ptr_ty);
        var_set_elem_type(cg, var_name, ty);
        var_set_struct_name(cg, var_name, type_name);
        /* Store the init value */
        LLVMValueRef init_val = codegen_expr(cg, node->as.var_decl.init);
        if (init_val) LLVMBuildStore(cg->builder, init_val, alloca_inst);
        return;
    }

    LLVMValueRef alloca_inst = LLVMBuildAlloca(cg->builder, ty, var_name);
    var_push(cg, var_name, alloca_inst, ty);
    if (LLVMGetTypeKind(ty) == LLVMStructTypeKind)
        var_set_struct_name(cg, var_name, type_name);

    if (node->as.var_decl.init) {
        AstNode *init_node = node->as.var_decl.init;
        /* Handle struct literal init: { val1, val2, ... } */
        if (init_node->type == NODE_CALL && !init_node->as.call.callee &&
            LLVMGetTypeKind(ty) == LLVMStructTypeKind) {
            unsigned count = LLVMCountStructElementTypes(ty);
            for (unsigned i = 0; i < count && i < init_node->as.call.args.count; i++) {
                LLVMValueRef field_val = codegen_expr(cg, init_node->as.call.args.items[i]);
                LLVMValueRef ptr = LLVMBuildGEP2(cg->builder, ty, alloca_inst,
                    (LLVMValueRef[]){
                        LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0),
                        LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), i, 0)
                    }, 2, "struct.init");
                LLVMBuildStore(cg->builder, field_val, ptr);
            }
            return;
        }
        LLVMValueRef init_val = codegen_expr(cg, init_node);
        if (init_val) {
            LLVMTypeRef init_ty = LLVMTypeOf(init_val);
            if (LLVMGetTypeKind(init_ty) == LLVMIntegerTypeKind &&
                LLVMGetTypeKind(ty) == LLVMPointerTypeKind) {
                init_val = LLVMBuildIntToPtr(cg->builder, init_val, ty, "inttoptr");
            }
            LLVMBuildStore(cg->builder, init_val, alloca_inst);
        }
    }
}

static void codegen_if(CodegenCtx *cg, AstNode *node) {
    LLVMValueRef cond = codegen_expr(cg, node->as.if_stmt.cond);
    LLVMBasicBlockRef start = LLVMGetInsertBlock(cg->builder);
    LLVMValueRef fn = LLVMGetBasicBlockParent(start);
    LLVMBasicBlockRef then_bb  = LLVMAppendBasicBlockInContext(cg->ctx, fn, "if.then");
    LLVMBasicBlockRef merge_bb = LLVMAppendBasicBlockInContext(cg->ctx, fn, "if.end");
    LLVMBasicBlockRef else_bb = NULL;
    if (node->as.if_stmt.else_blk) {
        else_bb = LLVMAppendBasicBlockInContext(cg->ctx, fn, "if.else");
        LLVMBuildCondBr(cg->builder, cond, then_bb, else_bb);
    } else {
        LLVMBuildCondBr(cg->builder, cond, then_bb, merge_bb);
    }
    LLVMPositionBuilderAtEnd(cg->builder, then_bb);
    codegen_block(cg, node->as.if_stmt.then_blk);
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder)))
        LLVMBuildBr(cg->builder, merge_bb);
    if (else_bb) {
        LLVMPositionBuilderAtEnd(cg->builder, else_bb);
        if (node->as.if_stmt.else_blk->type == NODE_IF)
            codegen_if(cg, node->as.if_stmt.else_blk);
        else
            codegen_block(cg, node->as.if_stmt.else_blk);
        if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder)))
            LLVMBuildBr(cg->builder, merge_bb);
    }
    LLVMPositionBuilderAtEnd(cg->builder, merge_bb);
}

static void codegen_switch(CodegenCtx *cg, AstNode *node) {
    LLVMValueRef expr = codegen_expr(cg, node->as.switch_stmt.expr);
    LLVMBasicBlockRef start = LLVMGetInsertBlock(cg->builder);
    LLVMValueRef fn = LLVMGetBasicBlockParent(start);
    LLVMBasicBlockRef merge_bb = LLVMAppendBasicBlockInContext(cg->ctx, fn, "switch.end");
    LLVMBasicBlockRef default_bb = NULL;
    size_t case_count = node->as.switch_stmt.cases.count;

    LLVMBasicBlockRef *case_bbs = case_count > 0 ? malloc(case_count * sizeof(LLVMBasicBlockRef)) : NULL;
    LLVMValueRef *case_vals = case_count > 0 ? malloc(case_count * sizeof(LLVMValueRef)) : NULL;
    int *is_default = case_count > 0 ? malloc(case_count * sizeof(int)) : NULL;
    unsigned real_count = 0;

    for (size_t i = 0; i < case_count; i++) {
        AstNode *c = node->as.switch_stmt.cases.items[i];
        case_bbs[i] = LLVMAppendBasicBlockInContext(cg->ctx, fn, "case");
        is_default[i] = c->as.case_stmt.is_default;
        if (is_default[i]) {
            default_bb = case_bbs[i];
        } else {
            case_vals[real_count] = codegen_expr(cg, c->as.case_stmt.value);
            real_count++;
        }
    }

    LLVMPositionBuilderAtEnd(cg->builder, start);
    LLVMValueRef sw = LLVMBuildSwitch(cg->builder, expr,
        default_bb ? default_bb : merge_bb, real_count);

    unsigned ci = 0;
    for (size_t i = 0; i < case_count; i++) {
        if (!is_default[i]) {
            LLVMAddCase(sw, case_vals[ci], case_bbs[i]);
            ci++;
        }
    }

    for (size_t i = 0; i < case_count; i++) {
        AstNode *c = node->as.switch_stmt.cases.items[i];
        LLVMPositionBuilderAtEnd(cg->builder, case_bbs[i]);
        codegen_block(cg, c->as.case_stmt.body);
        if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder)))
            LLVMBuildBr(cg->builder, merge_bb);
    }

    if (case_bbs) free(case_bbs);
    if (case_vals) free(case_vals);
    if (is_default) free(is_default);
    LLVMPositionBuilderAtEnd(cg->builder, merge_bb);
}

static void codegen_while(CodegenCtx *cg, AstNode *node) {
    LLVMBasicBlockRef start = LLVMGetInsertBlock(cg->builder);
    LLVMValueRef fn = LLVMGetBasicBlockParent(start);
    LLVMBasicBlockRef cond_bb = LLVMAppendBasicBlockInContext(cg->ctx, fn, "while.cond");
    LLVMBasicBlockRef body_bb = LLVMAppendBasicBlockInContext(cg->ctx, fn, "while.body");
    LLVMBasicBlockRef end_bb  = LLVMAppendBasicBlockInContext(cg->ctx, fn, "while.end");
    LLVMBuildBr(cg->builder, cond_bb);

    LLVMPositionBuilderAtEnd(cg->builder, cond_bb);
    LLVMValueRef cond = codegen_expr(cg, node->as.while_stmt.cond);
    LLVMBuildCondBr(cg->builder, cond, body_bb, end_bb);

    LLVMPositionBuilderAtEnd(cg->builder, body_bb);
    int saved_loop = cg->loop_depth;
    LLVMBasicBlockRef saved_brk = cg->break_target;
    LLVMBasicBlockRef saved_cont = cg->continue_target;
    cg->loop_depth++;
    cg->break_target = end_bb;
    cg->continue_target = cond_bb;
    codegen_block(cg, node->as.while_stmt.body);
    cg->loop_depth = saved_loop;
    cg->break_target = saved_brk;
    cg->continue_target = saved_cont;
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder)))
        LLVMBuildBr(cg->builder, cond_bb);

    LLVMPositionBuilderAtEnd(cg->builder, end_bb);
}

static void codegen_do_while(CodegenCtx *cg, AstNode *node) {
    LLVMBasicBlockRef start = LLVMGetInsertBlock(cg->builder);
    LLVMValueRef fn = LLVMGetBasicBlockParent(start);
    LLVMBasicBlockRef body_bb = LLVMAppendBasicBlockInContext(cg->ctx, fn, "dowhile.body");
    LLVMBasicBlockRef cond_bb = LLVMAppendBasicBlockInContext(cg->ctx, fn, "dowhile.cond");
    LLVMBasicBlockRef end_bb  = LLVMAppendBasicBlockInContext(cg->ctx, fn, "dowhile.end");
    (void)start;
    LLVMBuildBr(cg->builder, body_bb);

    LLVMPositionBuilderAtEnd(cg->builder, body_bb);
    int saved_loop = cg->loop_depth;
    LLVMBasicBlockRef saved_brk = cg->break_target;
    LLVMBasicBlockRef saved_cont = cg->continue_target;
    cg->loop_depth++;
    cg->break_target = end_bb;
    cg->continue_target = cond_bb;
    codegen_block(cg, node->as.do_while_stmt.body);
    cg->loop_depth = saved_loop;
    cg->break_target = saved_brk;
    cg->continue_target = saved_cont;
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder)))
        LLVMBuildBr(cg->builder, cond_bb);

    LLVMPositionBuilderAtEnd(cg->builder, cond_bb);
    LLVMValueRef cond = codegen_expr(cg, node->as.do_while_stmt.cond);
    LLVMBuildCondBr(cg->builder, cond, body_bb, end_bb);

    LLVMPositionBuilderAtEnd(cg->builder, end_bb);
}

static void codegen_for(CodegenCtx *cg, AstNode *node) {
    LLVMBasicBlockRef start_bb = LLVMGetInsertBlock(cg->builder);
    LLVMValueRef fn = LLVMGetBasicBlockParent(start_bb);
    LLVMBasicBlockRef cond_bb = LLVMAppendBasicBlockInContext(cg->ctx, fn, "for.cond");
    LLVMBasicBlockRef body_bb = LLVMAppendBasicBlockInContext(cg->ctx, fn, "for.body");
    LLVMBasicBlockRef incr_bb = LLVMAppendBasicBlockInContext(cg->ctx, fn, "for.incr");
    LLVMBasicBlockRef end_bb  = LLVMAppendBasicBlockInContext(cg->ctx, fn, "for.end");

    LLVMTypeRef i64 = LLVMInt64TypeInContext(cg->ctx);

    /* Evaluate range: iter should be NODE_RANGE */
    LLVMValueRef range_start = NULL, range_end = NULL;
    if (node->as.for_stmt.iter && node->as.for_stmt.iter->type == NODE_RANGE) {
        range_start = codegen_expr(cg, node->as.for_stmt.iter->as.range.start);
        range_end   = codegen_expr(cg, node->as.for_stmt.iter->as.range.end);
    } else {
        /* fallback: evaluate as 0..iter */
        range_start = LLVMConstInt(i64, 0, 0);
        range_end = codegen_expr(cg, node->as.for_stmt.iter);
    }

    /* Create loop variable, store start value */
    LLVMValueRef alloca_inst = LLVMBuildAlloca(cg->builder, i64, node->as.for_stmt.var);
    var_push(cg, node->as.for_stmt.var, alloca_inst, i64);
    LLVMBuildStore(cg->builder, range_start, alloca_inst);
    LLVMBuildBr(cg->builder, cond_bb);

    /* Condition: i < end */
    LLVMPositionBuilderAtEnd(cg->builder, cond_bb);
    LLVMValueRef cur = LLVMBuildLoad2(cg->builder, i64, alloca_inst, "for.i");
    LLVMValueRef cmp = LLVMBuildICmp(cg->builder, LLVMIntSLT, cur, range_end, "for.cmp");
    LLVMBuildCondBr(cg->builder, cmp, body_bb, end_bb);

    /* Body */
    LLVMPositionBuilderAtEnd(cg->builder, body_bb);
    int saved_loop = cg->loop_depth;
    LLVMBasicBlockRef saved_brk = cg->break_target;
    LLVMBasicBlockRef saved_cont = cg->continue_target;
    cg->loop_depth++;
    cg->break_target = end_bb;
    cg->continue_target = incr_bb;
    codegen_block(cg, node->as.for_stmt.body);
    cg->loop_depth = saved_loop;
    cg->break_target = saved_brk;
    cg->continue_target = saved_cont;
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder)))
        LLVMBuildBr(cg->builder, incr_bb);

    /* Increment: i++ */
    LLVMPositionBuilderAtEnd(cg->builder, incr_bb);
    LLVMValueRef cur2 = LLVMBuildLoad2(cg->builder, i64, alloca_inst, "for.i2");
    LLVMValueRef next = LLVMBuildAdd(cg->builder, cur2, LLVMConstInt(i64, 1, 0), "for.next");
    LLVMBuildStore(cg->builder, next, alloca_inst);
    LLVMBuildBr(cg->builder, cond_bb);

    LLVMPositionBuilderAtEnd(cg->builder, end_bb);
}

static void codegen_c_style_for(CodegenCtx *cg, AstNode *node) {
    LLVMBasicBlockRef start = LLVMGetInsertBlock(cg->builder);
    LLVMValueRef fn = LLVMGetBasicBlockParent(start);
    LLVMBasicBlockRef cond_bb = LLVMAppendBasicBlockInContext(cg->ctx, fn, "for.cond");
    LLVMBasicBlockRef body_bb = LLVMAppendBasicBlockInContext(cg->ctx, fn, "for.body");
    LLVMBasicBlockRef update_bb = LLVMAppendBasicBlockInContext(cg->ctx, fn, "for.update");
    LLVMBasicBlockRef end_bb  = LLVMAppendBasicBlockInContext(cg->ctx, fn, "for.end");
    (void)start;

    if (node->as.c_style_for.init) codegen_stmt(cg, node->as.c_style_for.init);
    LLVMBuildBr(cg->builder, cond_bb);

    LLVMPositionBuilderAtEnd(cg->builder, cond_bb);
    if (node->as.c_style_for.cond) {
        LLVMValueRef cond = codegen_expr(cg, node->as.c_style_for.cond);
        LLVMBuildCondBr(cg->builder, cond, body_bb, end_bb);
    } else {
        LLVMBuildBr(cg->builder, body_bb);
    }

    LLVMPositionBuilderAtEnd(cg->builder, body_bb);
    int saved_loop = cg->loop_depth;
    LLVMBasicBlockRef saved_brk = cg->break_target;
    LLVMBasicBlockRef saved_cont = cg->continue_target;
    cg->loop_depth++;
    cg->break_target = end_bb;
    cg->continue_target = update_bb;
    codegen_block(cg, node->as.c_style_for.body);
    cg->loop_depth = saved_loop;
    cg->break_target = saved_brk;
    cg->continue_target = saved_cont;
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder)))
        LLVMBuildBr(cg->builder, update_bb);

    LLVMPositionBuilderAtEnd(cg->builder, update_bb);
    if (node->as.c_style_for.update) codegen_expr(cg, node->as.c_style_for.update);
    LLVMBuildBr(cg->builder, cond_bb);

    LLVMPositionBuilderAtEnd(cg->builder, end_bb);
}

static void codegen_match(CodegenCtx *cg, AstNode *node) {
    LLVMValueRef expr_val = codegen_expr(cg, node->as.match_stmt.expr);
    LLVMBasicBlockRef start = LLVMGetInsertBlock(cg->builder);
    LLVMValueRef fn = LLVMGetBasicBlockParent(start);
    LLVMBasicBlockRef merge_bb = LLVMAppendBasicBlockInContext(cg->ctx, fn, "match.end");
    LLVMBasicBlockRef default_bb = NULL;

    size_t case_count = node->as.match_stmt.cases.count;
    LLVMBasicBlockRef *case_bbs = case_count > 0 ? malloc(case_count * sizeof(LLVMBasicBlockRef)) : NULL;

    for (size_t i = 0; i < case_count; i++) {
        case_bbs[i] = LLVMAppendBasicBlockInContext(cg->ctx, fn, "match.case");
        AstNode *c = node->as.match_stmt.cases.items[i];
        if (c->as.match_case.is_default)
            default_bb = case_bbs[i];
    }

    /* Generate condition chains */
    LLVMBasicBlockRef cur_check_bb = start;
    for (size_t i = 0; i < case_count; i++) {
        AstNode *c = node->as.match_stmt.cases.items[i];
        if (c->as.match_case.is_default) continue;

        LLVMPositionBuilderAtEnd(cg->builder, cur_check_bb);
        AstNode *pattern = c->as.match_case.pattern;

        LLVMValueRef cond = NULL;
        if (pattern->type == NODE_BINARY && strcmp(pattern->as.binary.op, "in") == 0) {
            /* case result in 0..50: the right side is the range */
            LLVMValueRef r_start = codegen_expr(cg, pattern->as.binary.right->as.range.start);
            LLVMValueRef r_end   = codegen_expr(cg, pattern->as.binary.right->as.range.end);
            LLVMValueRef ge = LLVMBuildICmp(cg->builder, LLVMIntSGE, expr_val, r_start, "match.ge");
            LLVMValueRef lt = LLVMBuildICmp(cg->builder, LLVMIntSLT, expr_val, r_end,   "match.lt");
            cond = LLVMBuildAnd(cg->builder, ge, lt, "match.in");
        } else if (pattern->type == NODE_BINARY &&
                   (strcmp(pattern->as.binary.op, ">") == 0 ||
                    strcmp(pattern->as.binary.op, "<") == 0 ||
                    strcmp(pattern->as.binary.op, ">=") == 0 ||
                    strcmp(pattern->as.binary.op, "<=") == 0 ||
                    strcmp(pattern->as.binary.op, "==") == 0 ||
                    strcmp(pattern->as.binary.op, "!=") == 0)) {
            /* case result > 50: evaluate as binary comparison */
            LLVMValueRef rhs = codegen_expr(cg, pattern->as.binary.right);
            const char *op = pattern->as.binary.op;
            if (strcmp(op, ">") == 0)   cond = LLVMBuildICmp(cg->builder, LLVMIntSGT, expr_val, rhs, "match.gt");
            if (strcmp(op, "<") == 0)   cond = LLVMBuildICmp(cg->builder, LLVMIntSLT, expr_val, rhs, "match.lt");
            if (strcmp(op, ">=") == 0)  cond = LLVMBuildICmp(cg->builder, LLVMIntSGE, expr_val, rhs, "match.ge");
            if (strcmp(op, "<=") == 0)  cond = LLVMBuildICmp(cg->builder, LLVMIntSLE, expr_val, rhs, "match.le");
            if (strcmp(op, "==") == 0)  cond = LLVMBuildICmp(cg->builder, LLVMIntEQ,  expr_val, rhs, "match.eq");
            if (strcmp(op, "!=") == 0)  cond = LLVMBuildICmp(cg->builder, LLVMIntNE,  expr_val, rhs, "match.ne");
        } else {
            /* Literal comparison */
            LLVMValueRef val = codegen_expr(cg, pattern);
            cond = LLVMBuildICmp(cg->builder, LLVMIntEQ, expr_val, val, "match.eq");
        }

        LLVMBasicBlockRef next_check = LLVMAppendBasicBlockInContext(cg->ctx, fn, "match.next");
        LLVMBuildCondBr(cg->builder, cond, case_bbs[i], next_check);
        cur_check_bb = next_check;
    }

    /* Fall through to default or end */
    LLVMPositionBuilderAtEnd(cg->builder, cur_check_bb);
    if (default_bb) {
        LLVMBuildBr(cg->builder, default_bb);
    } else {
        LLVMBuildBr(cg->builder, merge_bb);
    }

    /* Generate case bodies */
    for (size_t i = 0; i < case_count; i++) {
        AstNode *c = node->as.match_stmt.cases.items[i];
        LLVMPositionBuilderAtEnd(cg->builder, case_bbs[i]);
        codegen_block(cg, c->as.match_case.body);
        if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder)))
            LLVMBuildBr(cg->builder, merge_bb);
    }

    if (case_bbs) free(case_bbs);
    LLVMPositionBuilderAtEnd(cg->builder, merge_bb);
}

static void codegen_using(CodegenCtx *cg, AstNode *node) {
    codegen_block(cg, node->as.using_stmt.body);
}

static void codegen_unsafe(CodegenCtx *cg, AstNode *node) {
    codegen_block(cg, node->as.unsafe_stmt.body);
}

static void codegen_stmt(CodegenCtx *cg, AstNode *node) {
    if (!node) return;

    /* Labels and gotos must be processed even in terminated blocks */
    if (node->type == NODE_LABEL || node->type == NODE_GOTO ||
        node->type == NODE_BREAK || node->type == NODE_CONTINUE) {
        switch (node->type) {
            case NODE_LABEL: {
                LLVMBasicBlockRef target = label_lookup(cg, node->as.label_stmt.name);
                if (!target) {
                    target = LLVMAppendBasicBlockInContext(cg->ctx, cg->cur_fn,
                        node->as.label_stmt.name);
                    label_push(cg, node->as.label_stmt.name, target);
                }
                if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder)))
                    LLVMBuildBr(cg->builder, target);
                LLVMPositionBuilderAtEnd(cg->builder, target);
                break;
            }
            case NODE_GOTO: {
                LLVMBasicBlockRef target = label_lookup(cg, node->as.goto_stmt.label);
                if (!target) {
                    target = LLVMAppendBasicBlockInContext(cg->ctx, cg->cur_fn,
                        node->as.goto_stmt.label);
                    label_push(cg, node->as.goto_stmt.label, target);
                }
                LLVMBuildBr(cg->builder, target);
                break;
            }
            case NODE_BREAK:
                if (cg->break_target)
                    LLVMBuildBr(cg->builder, cg->break_target);
                break;
            case NODE_CONTINUE:
                if (cg->continue_target)
                    LLVMBuildBr(cg->builder, cg->continue_target);
                break;
            default: break;
        }
        return;
    }

    if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) return;

    switch (node->type) {
        case NODE_BLOCK:       codegen_block(cg, node); break;
        case NODE_RETURN:      codegen_return(cg, node); break;
        case NODE_VAR_DECL:    codegen_var_decl(cg, node); break;
        case NODE_IF:          codegen_if(cg, node); break;
        case NODE_SWITCH:      codegen_switch(cg, node); break;
        case NODE_MATCH:       codegen_match(cg, node); break;
        case NODE_WHILE:       codegen_while(cg, node); break;
        case NODE_DO_WHILE:    codegen_do_while(cg, node); break;
        case NODE_FOR:         codegen_for(cg, node); break;
        case NODE_C_STYLE_FOR: codegen_c_style_for(cg, node); break;
        case NODE_USING:       codegen_using(cg, node); break;
        case NODE_UNSAFE:      codegen_unsafe(cg, node); break;
        case NODE_STMT_EXPR:
            codegen_expr(cg, node->as.stmt_expr.expr);
            break;
        case NODE_ASSIGN:
            codegen_expr(cg, node);
            break;
        case NODE_CALL:
            codegen_expr(cg, node);
            break;
        case NODE_DROP_EXPR:
            codegen_expr(cg, node);
            break;
        default:
            codegen_expr(cg, node);
            break;
    }
}

static void codegen_func_decl(CodegenCtx *cg, AstNode *node) {
    const char *name = node->as.func_decl.name;
    const char *ret_type_name = node->as.func_decl.ret_type;
    LLVMTypeRef ret_ty = resolve_type(cg, ret_type_name);

    int is_method = node->as.func_decl.is_method;
    size_t user_param_count = node->as.func_decl.params.count;
    size_t total_params = user_param_count + (is_method ? 1 : 0);
    LLVMTypeRef *param_types = total_params > 0
        ? malloc(total_params * sizeof(LLVMTypeRef)) : NULL;

    size_t pi = 0;
    if (is_method) {
        param_types[pi++] = LLVMPointerType(LLVMInt8TypeInContext(cg->ctx), 0);
    }
    for (size_t i = 0; i < user_param_count; i++) {
        AstNode *p = node->as.func_decl.params.items[i];
        param_types[pi++] = resolve_type(cg, p->as.param.type);
    }

    LLVMTypeRef fn_type = LLVMFunctionType(ret_ty, param_types,
        (unsigned)total_params, 0);

    char mangled[512];
    if (is_method && node->as.func_decl.class_name) {
        snprintf(mangled, sizeof(mangled), "%s.%s", node->as.func_decl.class_name, name);
    } else if (is_method) {
        snprintf(mangled, sizeof(mangled), "%s.new", name);
    } else {
        snprintf(mangled, sizeof(mangled), "%s", name);
    }

    LLVMValueRef fn = LLVMGetNamedFunction(cg->module, mangled);
    if (!fn) {
        fn = LLVMAddFunction(cg->module, mangled, fn_type);
        fn_type_push(cg, mangled, fn_type);
    }

    if (!node->as.func_decl.body) {
        /* External declaration only */
        if (param_types) free(param_types);
        return;
    }

    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(cg->ctx, fn, "entry");
    LLVMPositionBuilderAtEnd(cg->builder, entry);

    LLVMValueRef prev_fn = cg->cur_fn;
    cg->cur_fn = fn;

    /* Save and reset variable scope */
    size_t prev_var_count = cg->var_count;

    /* Name parameters */
    unsigned actual_params = LLVMCountParams(fn);
    for (unsigned i = 0; i < actual_params; i++) {
        LLVMValueRef param = LLVMGetParam(fn, i);
        const char *param_name;
        if (node->as.func_decl.is_method && i == 0) {
            param_name = "self";
        } else {
            size_t pidx = node->as.func_decl.is_method ? i - 1 : i;
            if (pidx < node->as.func_decl.params.count) {
                param_name = node->as.func_decl.params.items[pidx]->as.param.name;
            } else {
                param_name = "arg";
            }
        }
        LLVMSetValueName2(param, param_name, strlen(param_name));
        LLVMTypeRef pty = LLVMTypeOf(param);
        LLVMValueRef alloca_inst = LLVMBuildAlloca(cg->builder, pty, param_name);
        LLVMBuildStore(cg->builder, param, alloca_inst);
        var_push(cg, param_name, alloca_inst, pty);
        /* For the self parameter of a class method, set elem_ty for member access */
        if (node->as.func_decl.is_method && i == 0 && node->as.func_decl.class_name) {
            LLVMTypeRef class_ty = struct_lookup(cg, node->as.func_decl.class_name);
            if (class_ty) var_set_elem_type(cg, param_name, class_ty);
            var_set_struct_name(cg, param_name, node->as.func_decl.class_name);
        }
    }

    codegen_block(cg, node->as.func_decl.body);

    /* Add implicit return if no terminator */
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) {
        if (LLVMGetTypeKind(ret_ty) == LLVMVoidTypeKind)
            LLVMBuildRetVoid(cg->builder);
        else
            LLVMBuildRet(cg->builder, LLVMConstNull(ret_ty));
    }

    /* Restore variable scope */
    cg->var_count = prev_var_count;
    cg->cur_fn = prev_fn;

    if (param_types) free(param_types);
}

static void codegen_struct_decl(CodegenCtx *cg, AstNode *node) {
    const char *name = node->as.struct_decl.name;
    size_t field_count = node->as.struct_decl.fields.count;
    LLVMTypeRef *field_types = field_count > 0
        ? malloc(field_count * sizeof(LLVMTypeRef)) : NULL;
    char **field_names = field_count > 0
        ? malloc(field_count * sizeof(char *)) : NULL;

    for (size_t i = 0; i < field_count; i++) {
        AstNode *f = node->as.struct_decl.fields.items[i];
        field_types[i] = resolve_type(cg, f->as.var_decl.type);
        field_names[i] = f->as.var_decl.name;
    }

    LLVMTypeRef struct_ty = LLVMStructTypeInContext(cg->ctx, field_types,
        (unsigned)field_count, 0);
    struct_push(cg, name, struct_ty);
    if (field_count > 0)
        struct_push_fields(cg, name, field_names, field_count);

    if (field_types) free(field_types);
    if (field_names) free(field_names);
}

static void codegen_enum_decl(CodegenCtx *cg, AstNode *node) {
    long next_val = 0;
    for (size_t i = 0; i < node->as.enum_decl.values.count; i++) {
        AstNode *val = node->as.enum_decl.values.items[i];
        if (val->type == NODE_BINARY && strcmp(val->as.binary.op, "=") == 0) {
            const char *name = val->as.binary.left->as.ident.name;
            LLVMValueRef cv = codegen_expr(cg, val->as.binary.right);
            next_val = LLVMConstIntGetZExtValue(cv);
            enum_push(cg, name, next_val);
            next_val++;
        } else if (val->type == NODE_IDENTIFIER) {
            enum_push(cg, val->as.ident.name, next_val);
            next_val++;
        }
    }
}

static void codegen_typedef_decl(CodegenCtx *cg, AstNode *node) {
    /* Typedefs are just aliases in our type resolver - no LLVM IR needed */
    (void)cg; (void)node;
}

static void codegen_program(CodegenCtx *cg, AstNode *program) {
    for (size_t i = 0; i < program->as.program.decls.count; i++) {
        AstNode *decl = program->as.program.decls.items[i];
        if (!decl) continue;

        switch (decl->type) {
            case NODE_STRUCT_DECL:  codegen_struct_decl(cg, decl); break;
            case NODE_FUNC_DECL:    codegen_func_decl(cg, decl); break;
            case NODE_ENUM_DECL:    codegen_enum_decl(cg, decl); break;
            case NODE_TYPEDEF_DECL: codegen_typedef_decl(cg, decl); break;
            case NODE_CLASS_DECL:
                /* Register class with parent tracking */
                class_push(cg, decl->as.class_decl.name, decl->as.class_decl.parent);
                /* Register class fields as a struct type, including parent fields */
                {
                    /* Collect parent fields first if this class extends another */
                    size_t parent_field_count = 0;
                    LLVMTypeRef *all_field_types = NULL;
                    if (decl->as.class_decl.parent) {
                        LLVMTypeRef parent_ty = struct_lookup(cg, decl->as.class_decl.parent);
                        if (parent_ty && LLVMGetTypeKind(parent_ty) == LLVMStructTypeKind) {
                            parent_field_count = LLVMCountStructElementTypes(parent_ty);
                            all_field_types = malloc((parent_field_count + decl->as.class_decl.fields.count) * sizeof(LLVMTypeRef));
                            for (size_t j = 0; j < parent_field_count; j++)
                                all_field_types[j] = LLVMStructGetTypeAtIndex(parent_ty, j);
                        }
                    }
                    size_t own_field_count = decl->as.class_decl.fields.count;
                    size_t total = parent_field_count + own_field_count;
                    if (total > 0) {
                        if (!all_field_types)
                            all_field_types = malloc(total * sizeof(LLVMTypeRef));
                        char **all_field_names = malloc(total * sizeof(char *));
                        /* Copy parent field names */
                        if (parent_field_count > 0) {
                            for (size_t i = cg->struct_field_count; i > 0; i--) {
                                if (strcmp(cg->struct_fields[i - 1].struct_name, decl->as.class_decl.parent) == 0) {
                                    for (size_t j = 0; j < parent_field_count && j < cg->struct_fields[i - 1].field_count; j++)
                                        all_field_names[j] = cg->struct_fields[i - 1].field_names[j];
                                    break;
                                }
                            }
                        }
                        for (size_t j = 0; j < own_field_count; j++) {
                            AstNode *f = decl->as.class_decl.fields.items[j];
                            all_field_types[parent_field_count + j] = resolve_type(cg, f->as.var_decl.type);
                            all_field_names[parent_field_count + j] = f->as.var_decl.name;
                        }
                        LLVMTypeRef struct_ty = LLVMStructTypeInContext(cg->ctx, all_field_types,
                            (unsigned)total, 0);
                        struct_push(cg, decl->as.class_decl.name, struct_ty);
                        struct_push_fields(cg, decl->as.class_decl.name, all_field_names, total);
                        free(all_field_names);
                    }
                    if (all_field_types) free(all_field_types);
                }
                /* Generate methods as standalone functions */
                for (size_t j = 0; j < decl->as.class_decl.methods.count; j++)
                    codegen_func_decl(cg, decl->as.class_decl.methods.items[j]);
                break;
            case NODE_IMPORT: {
                /* Record the module name for selective linking */
                const char *mod = decl->as.import.module;
                /* Extract last segment: "std.io" -> "io" */
                const char *last = strrchr(mod, '.');
                last = last ? last + 1 : mod;
                /* Check for duplicates */
                int dup = 0;
                for (size_t k = 0; k < cg->import_count; k++) {
                    if (strcmp(cg->imports[k], last) == 0) { dup = 1; break; }
                }
                if (!dup) {
                    if (cg->import_count >= cg->import_cap) {
                        cg->import_cap = cg->import_cap ? cg->import_cap * 2 : 8;
                        cg->imports = realloc(cg->imports, cg->import_cap * sizeof(char *));
                    }
                    cg->imports[cg->import_count++] = strdup(last);
                }
                break;
            }
            case NODE_LINK:
                break;
            default:
                break;
        }
    }
}

static void emit_object_file(LLVMModuleRef module, const char *output_file) {
    char *triple = LLVMGetDefaultTargetTriple();
    LLVMTargetRef target;
    char *err = NULL;

    if (LLVMGetTargetFromTriple(triple, &target, &err) != 0) {
        fprintf(stderr, "penguinc: failed to get target: %s\n", err);
        LLVMDisposeMessage(err);
        LLVMDisposeMessage(triple);
        return;
    }

    LLVMTargetMachineRef tm = LLVMCreateTargetMachine(
        target, triple, "generic", "", LLVMCodeGenLevelDefault,
        LLVMRelocDefault, LLVMCodeModelDefault);

    LLVMTargetDataRef td = LLVMCreateTargetDataLayout(tm);
    LLVMSetDataLayout(module, LLVMCopyStringRepOfTargetData(td));
    LLVMDisposeTargetData(td);

    char *obj_file = malloc(strlen(output_file) + 5);
    strcpy(obj_file, output_file);
    char *ext = strrchr(obj_file, '.');
    if (ext && strcmp(ext, ".o") == 0)
        strcpy(ext, ".o");
    else
        strcat(obj_file, ".o");

    char *emit_err = NULL;
    if (LLVMTargetMachineEmitToFile(tm, module, obj_file,
                                     LLVMObjectFile, &emit_err) != 0) {
        fprintf(stderr, "penguinc: failed to emit object file: %s\n", emit_err);
        LLVMDisposeMessage(emit_err);
    } else {
        fprintf(stderr, "penguinc: wrote %s\n", obj_file);
    }

    free(obj_file);
    LLVMDisposeMessage(triple);
    LLVMDisposeTargetMachine(tm);
}

int codegen(AstNode *program, const char *output_file) {
    LLVMInitializeNativeTarget();
    LLVMInitializeNativeAsmParser();
    LLVMInitializeNativeAsmPrinter();

    LLVMContextRef ctx = LLVMContextCreate();
    LLVMModuleRef module = LLVMModuleCreateWithNameInContext("penguinc", ctx);
    LLVMBuilderRef builder = LLVMCreateBuilderInContext(ctx);

    CodegenCtx cg = {
        .ctx = ctx,
        .module = module,
        .builder = builder,
        .cur_fn = NULL,
        .vars = NULL, .var_count = 0, .var_cap = 0,
        .fn_types = NULL, .fn_type_count = 0, .fn_type_cap = 0,
        .structs = NULL, .struct_count = 0, .struct_cap = 0,
        .imports = NULL, .import_count = 0, .import_cap = 0,
        .break_target = NULL, .continue_target = NULL,
        .labels = NULL, .label_count = 0, .label_cap = 0,
        .loop_depth = 0,
    };

    /* Pre-register struct types */
    codegen_program(&cg, program);

    /* Verify module */
    char *verify_err = NULL;
    LLVMVerifyModule(module, LLVMReturnStatusAction, &verify_err);
    if (verify_err && verify_err[0]) {
        fprintf(stderr, "penguinc: module verification failed:\n%s\n", verify_err);
        LLVMDisposeMessage(verify_err);
    } else if (verify_err) {
        LLVMDisposeMessage(verify_err);
    }

    /* Emit object file */
    emit_object_file(module, output_file);

    /* Write .imports file for selective linking */
    {
        char imports_path[1024];
        snprintf(imports_path, sizeof(imports_path), "%s.imports", output_file);
        FILE *f = fopen(imports_path, "w");
        if (f) {
            for (size_t i = 0; i < cg.import_count; i++)
                fprintf(f, "%s\n", cg.imports[i]);
            fclose(f);
        }
    }

    /* Cleanup */
    LLVMDisposeBuilder(builder);
    LLVMDisposeModule(module);
    LLVMContextDispose(ctx);

    for (size_t i = 0; i < cg.var_count; i++) free(cg.vars[i].name);
    free(cg.vars);
    for (size_t i = 0; i < cg.fn_type_count; i++) free(cg.fn_types[i].name);
    free(cg.fn_types);
    for (size_t i = 0; i < cg.struct_count; i++) free(cg.structs[i].name);
    free(cg.structs);
    for (size_t i = 0; i < cg.import_count; i++) free(cg.imports[i]);
    free(cg.imports);
    for (size_t i = 0; i < cg.label_count; i++) free(cg.labels[i].name);
    free(cg.labels);

    return 0;
}
