#include "codegen_internal.h"

static void codegen_func_decl(CodegenCtx *cg, AstNode *node);
static void codegen_struct_decl(CodegenCtx *cg, AstNode *node);

static void codegen_func_decl(CodegenCtx *cg, AstNode *node) {
    const char *name = node->as.func_decl.name;
    const char *ret_type_name = node->as.func_decl.ret_type;
    LLVMTypeRef ret_ty = resolve_type(cg, ret_type_name);
    if (LLVMGetTypeKind(ret_ty) == LLVMStructTypeKind)
        ret_ty = LLVMPointerType(ret_ty, 0);

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
    } else if (strcmp(name, "main") == 0) {
        snprintf(mangled, sizeof(mangled), "main");
    } else {
        mangle_name(mangled, sizeof(mangled), name,
                    &node->as.func_decl.params, user_param_count);
    }

    LLVMValueRef fn = LLVMGetNamedFunction(cg->module, mangled);
    if (!fn) {
        fn = LLVMAddFunction(cg->module, mangled, fn_type);
        fn_type_push(cg, mangled, fn_type);
    }

    if (!node->as.func_decl.body) {
        if (param_types) free(param_types);
        return;
    }

    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(cg->ctx, fn, "entry");
    LLVMPositionBuilderAtEnd(cg->builder, entry);

    LLVMValueRef prev_fn = cg->cur_fn;
    cg->cur_fn = fn;

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
        if (!node->as.func_decl.is_method || i != 0) {
            size_t pidx = node->as.func_decl.is_method ? i - 1 : i;
            if (pidx < node->as.func_decl.params.count) {
                const char *pty_name = node->as.func_decl.params.items[pidx]->as.param.type;
                if (pty_name) var_set_type_name(cg, param_name, pty_name);
            }
        }
        if (node->as.func_decl.is_method && i == 0 && node->as.func_decl.class_name) {
            LLVMTypeRef class_ty = struct_lookup(cg, node->as.func_decl.class_name);
            if (class_ty) var_set_elem_type(cg, param_name, class_ty);
            var_set_struct_name(cg, param_name, node->as.func_decl.class_name);
        }
    }

    size_t prev_var_count = cg->var_count;
    cg->scope_base = prev_var_count;

    codegen_block(cg, node->as.func_decl.body);

    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) {
        for (size_t i = cg->var_count; i > prev_var_count; i--) {
            codegen_arc_release_var(cg, i - 1);
        }
        if (LLVMGetTypeKind(ret_ty) == LLVMVoidTypeKind)
            LLVMBuildRetVoid(cg->builder);
        else
            LLVMBuildRet(cg->builder, LLVMConstNull(ret_ty));
    }

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
    typedef_push(cg, node->as.typedef_decl.new_name, node->as.typedef_decl.orig_type);
}

static void codegen_class_decl(CodegenCtx *cg, AstNode *decl) {
    class_push(cg, decl->as.class_decl.name, decl->as.class_decl.parent);

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

    for (size_t j = 0; j < decl->as.class_decl.methods.count; j++)
        codegen_func_decl(cg, decl->as.class_decl.methods.items[j]);
}

void codegen_program(CodegenCtx *cg, AstNode *program) {
    for (size_t i = 0; i < program->as.program.decls.count; i++) {
        AstNode *decl = program->as.program.decls.items[i];
        if (!decl) continue;

        switch (decl->type) {
            case NODE_STRUCT_DECL:  codegen_struct_decl(cg, decl); break;
            case NODE_FUNC_DECL:    codegen_func_decl(cg, decl); break;
            case NODE_ENUM_DECL:    codegen_enum_decl(cg, decl); break;
            case NODE_TYPEDEF_DECL: codegen_typedef_decl(cg, decl); break;
            case NODE_CLASS_DECL:   codegen_class_decl(cg, decl); break;
            case NODE_IMPORT:       codegen_import(cg, decl); break;
            case NODE_LINK:         break;
            default:                break;
        }
    }
}
