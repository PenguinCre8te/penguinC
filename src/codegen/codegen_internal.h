#ifndef PENGUINC_CODEGEN_INTERNAL_H
#define PENGUINC_CODEGEN_INTERNAL_H

#include "codegen.h"
#include "frontend/ast.h"
#include "error.h"
#include <llvm-c/Core.h>
#include <llvm-c/TargetMachine.h>
#include <llvm-c/Target.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/BitWriter.h>
#include <llvm-c/Transforms/PassBuilder.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  CodegenCtx — central compiler state                                */
/* ------------------------------------------------------------------ */
typedef struct {
    LLVMContextRef ctx;
    LLVMModuleRef  module;
    LLVMBuilderRef builder;
    LLVMValueRef   cur_fn;

    struct { char *name; LLVMTypeRef fn_ty; } *fn_types;
    size_t fn_type_count;
    size_t fn_type_cap;

    struct { char *name; LLVMValueRef val; LLVMTypeRef ty; LLVMTypeRef elem_ty; char *struct_name; char *type_name; int needs_release; } *vars;
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

    struct { char *pc_name; char *c_name; } *func_maps;
    size_t func_map_count;
    size_t func_map_cap;

    struct { char *alias; char *orig; } *typedefs;
    size_t typedef_count;
    size_t typedef_cap;

    LLVMBasicBlockRef break_target;
    LLVMBasicBlockRef continue_target;

    struct { char *name; LLVMBasicBlockRef block; } *labels;
    size_t label_count;
    size_t label_cap;

    int loop_depth;
    size_t scope_base;
} CodegenCtx;

/* ------------------------------------------------------------------ */
/*  codegen_ctx.c — symbol registries                                  */
/* ------------------------------------------------------------------ */
void class_push(CodegenCtx *cg, const char *name, const char *parent);
const char *class_lookup_parent(CodegenCtx *cg, const char *name);
void enum_push(CodegenCtx *cg, const char *name, long value);
long enum_lookup(CodegenCtx *cg, const char *name, int *found);
void fn_type_push(CodegenCtx *cg, const char *name, LLVMTypeRef fn_ty);
LLVMTypeRef fn_type_lookup(CodegenCtx *cg, const char *name);
void var_push(CodegenCtx *cg, const char *name, LLVMValueRef val, LLVMTypeRef ty);
void var_set_elem_type(CodegenCtx *cg, const char *name, LLVMTypeRef elem_ty);
LLVMTypeRef var_lookup_elem_type(CodegenCtx *cg, const char *name);
void var_set_struct_name(CodegenCtx *cg, const char *name, const char *struct_name);
const char *var_lookup_struct_name(CodegenCtx *cg, const char *name);
void var_set_type_name(CodegenCtx *cg, const char *name, const char *type_name);
const char *var_lookup_type_name(CodegenCtx *cg, const char *name);
void typedef_push(CodegenCtx *cg, const char *alias, const char *orig);
const char *typedef_resolve(CodegenCtx *cg, const char *name);
LLVMValueRef var_lookup(CodegenCtx *cg, const char *name);
int var_lookup_index(CodegenCtx *cg, const char *name, size_t *out_index);
LLVMTypeRef var_lookup_type(CodegenCtx *cg, const char *name);
void struct_push(CodegenCtx *cg, const char *name, LLVMTypeRef ty);
LLVMTypeRef struct_lookup(CodegenCtx *cg, const char *name);
void struct_push_fields(CodegenCtx *cg, const char *struct_name, char **field_names, size_t count);
int struct_field_index(CodegenCtx *cg, const char *struct_name, const char *field_name);
void func_map_push(CodegenCtx *cg, const char *pc_name, const char *c_name);
const char *func_map_lookup(CodegenCtx *cg, const char *pc_name);
LLVMBasicBlockRef label_lookup(CodegenCtx *cg, const char *name);
void label_push(CodegenCtx *cg, const char *name, LLVMBasicBlockRef block);

/* ------------------------------------------------------------------ */
/*  codegen_type.c — type resolution                                   */
/* ------------------------------------------------------------------ */
LLVMTypeRef resolve_type(CodegenCtx *cg, const char *name);
int is_float_type(LLVMTypeRef ty);

/* ------------------------------------------------------------------ */
/*  codegen_arc.c — ARC helpers and optimization                       */
/* ------------------------------------------------------------------ */
extern LLVMTypeRef arc_i8ptr;
extern LLVMTypeRef malloc_fn_type;
extern LLVMTypeRef free_fn_type;

void init_arc_types(CodegenCtx *cg);
LLVMValueRef get_or_declare_arc_fn(CodegenCtx *cg, const char *name, LLVMTypeRef fn_ty);
LLVMValueRef call_arc_alloc(CodegenCtx *cg, LLVMValueRef size);
LLVMValueRef call_arc_retain(CodegenCtx *cg, LLVMValueRef ptr);
void call_arc_release(CodegenCtx *cg, LLVMValueRef ptr);
int is_arc_type(CodegenCtx *cg, const char *type_name);
int is_arc_type_for_var(CodegenCtx *cg, size_t var_index);
void codegen_arc_release_var(CodegenCtx *cg, size_t var_index);
LLVMValueRef get_malloc(CodegenCtx *cg);
LLVMValueRef get_free(CodegenCtx *cg);
LLVMValueRef call_malloc(CodegenCtx *cg, LLVMValueRef size);
LLVMValueRef call_free(CodegenCtx *cg, LLVMValueRef ptr);
void codegen_arc_optimize(LLVMModuleRef module);

/* ------------------------------------------------------------------ */
/*  codegen_mangle.c — name mangling                                  */
/* ------------------------------------------------------------------ */
void mangle_name(char *buf, size_t buflen, const char *name,
                 NodeList *params, size_t param_count);
void mangle_call_name(char *buf, size_t buflen, const char *name,
                      CodegenCtx *cg, NodeList *args, size_t arg_count);

/* ------------------------------------------------------------------ */
/*  codegen_expr.c — expression codegen                                */
/* ------------------------------------------------------------------ */
LLVMValueRef get_or_declare_runtime_fn(CodegenCtx *cg, const char *name, LLVMTypeRef fn_type);
LLVMValueRef codegen_expr(CodegenCtx *cg, AstNode *node);
LLVMValueRef wrap_string_literal(CodegenCtx *cg, LLVMValueRef val);
LLVMValueRef codegen_auto_tostring(CodegenCtx *cg, LLVMValueRef val);

/* ------------------------------------------------------------------ */
/*  codegen_stmt.c — statement codegen                                 */
/* ------------------------------------------------------------------ */
void codegen_block(CodegenCtx *cg, AstNode *node);
void codegen_stmt(CodegenCtx *cg, AstNode *node);

/* ------------------------------------------------------------------ */
/*  codegen_import.c — import/link processing                          */
/* ------------------------------------------------------------------ */
void codegen_import(CodegenCtx *cg, AstNode *decl);

/* ------------------------------------------------------------------ */
/*  codegen_decl.c — declaration codegen                               */
/* ------------------------------------------------------------------ */
void codegen_program(CodegenCtx *cg, AstNode *program);

#endif /* PENGUINC_CODEGEN_INTERNAL_H */
