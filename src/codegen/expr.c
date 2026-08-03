#include "codegen_internal.h"
#include <stdio.h>

static int is_integer_type_name(const char *name) {
    return strcmp(name, "int") == 0 || strcmp(name, "long") == 0 ||
           strcmp(name, "i8") == 0 || strcmp(name, "i16") == 0 ||
           strcmp(name, "i32") == 0 || strcmp(name, "i64") == 0 ||
           strcmp(name, "i128") == 0 || strcmp(name, "u8") == 0 ||
           strcmp(name, "u16") == 0 || strcmp(name, "u32") == 0 ||
           strcmp(name, "u64") == 0 || strcmp(name, "u128") == 0 ||
           strcmp(name, "isize") == 0 || strcmp(name, "usize") == 0 ||
           strcmp(name, "char") == 0;
}

static int is_float_type_name(const char *name) {
    return strcmp(name, "float") == 0 || strcmp(name, "f32") == 0 || strcmp(name, "f64") == 0;
}

static const char *llvm_type_name(LLVMTypeRef ty) {
    if (!ty) return "?";
    switch (LLVMGetTypeKind(ty)) {
        case LLVMIntegerTypeKind:
            return LLVMGetIntTypeWidth(ty) == 1 ? "bool" :
                   LLVMGetIntTypeWidth(ty) == 64 ? "long" : "int";
        case LLVMDoubleTypeKind:    return "float";
        case LLVMVoidTypeKind:      return "void";
        case LLVMPointerTypeKind:   return "string";
        default:                    return "?";
    }
}

static int validate_call_args(CodegenCtx *cg, AstNode *node,
                              const char *fn_name, LLVMTypeRef fn_type) {
    if (!fn_type) return 0;
    unsigned expected = LLVMCountParamTypes(fn_type);
    unsigned got = (unsigned)node->as.call.args.count;
    if (expected != got) {
        error_at(node->loc, ERR_SEMANTIC,
            "'%s' expects %u argument(s), got %u", fn_name, expected, got);
        return 1;
    }
    LLVMTypeRef *param_tys = malloc(expected * sizeof(LLVMTypeRef));
    LLVMGetParamTypes(fn_type, param_tys);
    for (unsigned i = 0; i < expected; i++) {
        AstNode *arg = node->as.call.args.items[i];
        LLVMValueRef arg_val = codegen_expr(cg, arg);
        if (!arg_val) continue;
        LLVMTypeRef actual = LLVMTypeOf(arg_val);
        if (actual != param_tys[i]) {
            error_at(arg->loc, ERR_SEMANTIC,
                "argument %u of '%s' expects %s, got %s",
                i + 1, fn_name,
                llvm_type_name(param_tys[i]),
                llvm_type_name(actual));
            free(param_tys);
            return 1;
        }
    }
    free(param_tys);
    return 0;
}

LLVMValueRef get_or_declare_runtime_fn(CodegenCtx *cg, const char *name,
                                       LLVMTypeRef fn_type) {
    LLVMValueRef fn = LLVMGetNamedFunction(cg->module, name);
    if (!fn) {
        fn = LLVMAddFunction(cg->module, name, fn_type);
        fn_type_push(cg, name, fn_type);
    }
    return fn;
}

static LLVMValueRef codegen_string_lit(CodegenCtx *cg, AstNode *node) {
    return LLVMBuildGlobalStringPtr(cg->builder, node->as.string_lit.value, "str");
}

LLVMValueRef wrap_string_literal(CodegenCtx *cg, LLVMValueRef val) {
    if (!val) return NULL;
    LLVMTypeRef val_ty = LLVMTypeOf(val);
    if (LLVMGetTypeKind(val_ty) != LLVMPointerTypeKind) return val;
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(cg->ctx), 0);
    LLVMTypeRef fn_ty = LLVMFunctionType(i8ptr, (LLVMTypeRef[]){i8ptr}, 1, 0);
    LLVMValueRef fn = get_or_declare_runtime_fn(cg, "arc_wrap_string", fn_ty);
    return LLVMBuildCall2(cg->builder, fn_ty, fn, (LLVMValueRef[]){val}, 1, "wrapped");
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
    {
        char prefix[260];
        snprintf(prefix, sizeof(prefix), "_pC%s", name);
        size_t plen = strlen(prefix);
        for (LLVMValueRef f = LLVMGetFirstFunction(cg->module); f; f = LLVMGetNextFunction(f)) {
            const char *fname = LLVMGetValueName(f);
            if (strncmp(fname, prefix, plen) == 0)
                return f;
        }
    }
    if (strcmp(name, "NULL") == 0)
        return LLVMConstNull(LLVMPointerType(LLVMInt8TypeInContext(cg->ctx), 0));
    int found = 0;
    long eval = enum_lookup(cg, name, &found);
    if (found)
        return LLVMConstInt(LLVMInt64TypeInContext(cg->ctx), (unsigned long long)eval, 0);
    error_at(node->loc, ERR_SEMANTIC, "undefined variable '%s'", name);
    return NULL;
}

static LLVMValueRef codegen_unary(CodegenCtx *cg, AstNode *node);
static LLVMValueRef get_lvalue(CodegenCtx *cg, AstNode *node);

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

    if (strcmp(op, "+") == 0 &&
        LLVMGetTypeKind(LLVMTypeOf(left)) == LLVMPointerTypeKind &&
        LLVMGetTypeKind(LLVMTypeOf(right)) == LLVMPointerTypeKind) {
        LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(cg->ctx), 0);
        LLVMTypeRef fn_ty = LLVMFunctionType(i8ptr, (LLVMTypeRef[]){i8ptr, i8ptr}, 2, 0);
        const char *c_name = func_map_lookup(cg, "console._pCstr_concatss");
        if (!c_name) c_name = func_map_lookup(cg, "console.str_concat");
        if (!c_name) c_name = "penguin_str_concat";
        LLVMValueRef fn = get_or_declare_runtime_fn(cg, c_name, fn_ty);
        return LLVMBuildCall2(cg->builder, fn_ty, fn, (LLVMValueRef[]){left, right}, 2, "strcat");
    }

    /* Coerce int to float when the other operand is float */
    int left_flt  = is_float_type(LLVMTypeOf(left));
    int right_flt = is_float_type(LLVMTypeOf(right));
    LLVMTypeRef f64 = LLVMDoubleTypeInContext(cg->ctx);
    if (left_flt && !right_flt)
        right = LLVMBuildSIToFP(cg->builder, right, f64, "coerce");
    else if (!left_flt && right_flt)
        left = LLVMBuildSIToFP(cg->builder, left, f64, "coerce");
    int flt = left_flt || right_flt;

    if (strcmp(op, "+") == 0)  return flt ? LLVMBuildFAdd(cg->builder, left, right, "fadd") : LLVMBuildAdd(cg->builder, left, right, "add");
    if (strcmp(op, "-") == 0)  return flt ? LLVMBuildFSub(cg->builder, left, right, "fsub") : LLVMBuildSub(cg->builder, left, right, "sub");
    if (strcmp(op, "*") == 0)  return flt ? LLVMBuildFMul(cg->builder, left, right, "fmul") : LLVMBuildMul(cg->builder, left, right, "mul");
    if (strcmp(op, "**") == 0) {
        LLVMTypeRef f64_ty = LLVMDoubleTypeInContext(cg->ctx);
        LLVMValueRef l = LLVMBuildSIToFP(cg->builder, left, f64_ty, "pow.l");
        LLVMValueRef r = LLVMBuildSIToFP(cg->builder, right, f64_ty, "pow.r");
        LLVMTypeRef fn_ty = LLVMFunctionType(f64_ty, (LLVMTypeRef[]){f64_ty, f64_ty}, 2, 0);
        LLVMValueRef pow_fn = get_or_declare_runtime_fn(cg, "pow", fn_ty);
        LLVMValueRef result = LLVMBuildCall2(cg->builder, fn_ty, pow_fn,
            (LLVMValueRef[]){l, r}, 2, "pow");
        if (flt)
            return LLVMBuildFPToSI(cg->builder, result,
                LLVMTypeOf(left), "pow.result");
        return LLVMBuildFPToSI(cg->builder, result,
            LLVMInt64TypeInContext(cg->ctx), "pow.result");
    }
    if (strcmp(op, "/") == 0)  return flt ? LLVMBuildFDiv(cg->builder, left, right, "fdiv") : LLVMBuildSDiv(cg->builder, left, right, "sdiv");
    if (strcmp(op, "%") == 0)  return flt ? LLVMBuildFRem(cg->builder, left, right, "frem") : LLVMBuildSRem(cg->builder, left, right, "srem");
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
    const char *op = node->as.unary.op;
    int is_prefix = node->as.unary.is_prefix;

    if (strcmp(op, "++") == 0 || strcmp(op, "--") == 0) {
        LLVMValueRef ptr = get_lvalue(cg, node->as.unary.operand);
        if (!ptr) {
            error_at(node->loc, ERR_SEMANTIC, "invalid operand for '%s'", op);
            return NULL;
        }
        LLVMTypeRef val_ty = LLVMInt64TypeInContext(cg->ctx);
        if (node->as.unary.operand->type == NODE_IDENTIFIER) {
            LLVMTypeRef tracked = var_lookup_type(cg, node->as.unary.operand->as.ident.name);
            if (tracked) val_ty = tracked;
        }
        LLVMValueRef old_val = LLVMBuildLoad2(cg->builder, val_ty, ptr, "oldval");
        LLVMValueRef one = LLVMConstInt(val_ty, 1, 0);
        LLVMValueRef new_val;
        if (strcmp(op, "++") == 0)
            new_val = LLVMBuildAdd(cg->builder, old_val, one, "inc");
        else
            new_val = LLVMBuildSub(cg->builder, old_val, one, "dec");
        LLVMBuildStore(cg->builder, new_val, ptr);
        return is_prefix ? new_val : old_val;
    }

    LLVMValueRef operand = codegen_expr(cg, node->as.unary.operand);
    if (!operand) return NULL;
    if (strcmp(op, "-") == 0)
        return is_float_type(LLVMTypeOf(operand))
            ? LLVMBuildFNeg(cg->builder, operand, "fneg")
            : LLVMBuildNeg(cg->builder, operand, "neg");
    if (strcmp(op, "~") == 0)
        return LLVMBuildXor(cg->builder, operand,
            LLVMConstAllOnes(LLVMInt64TypeInContext(cg->ctx)), "not");
    if (strcmp(op, "*") == 0) {
        LLVMTypeRef operand_ty = LLVMTypeOf(operand);
        if (LLVMGetTypeKind(operand_ty) != LLVMPointerTypeKind) {
            error_at(node->loc, ERR_SEMANTIC,
                "cannot dereference non-pointer type");
            return NULL;
        }
        return LLVMBuildLoad2(cg->builder, LLVMInt64TypeInContext(cg->ctx),
                              operand, "deref");
    }
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
    if (node->type == NODE_INDEX) {
        LLVMValueRef obj = codegen_expr(cg, node->as.index.object);
        LLVMValueRef idx = codegen_expr(cg, node->as.index.index);
        if (!obj || !idx) return NULL;
        /* Determine element type from variable info */
        LLVMTypeRef elem_ty = NULL;
        if (node->as.index.object->type == NODE_IDENTIFIER)
            elem_ty = var_lookup_elem_type(cg, node->as.index.object->as.ident.name);
        if (!elem_ty || LLVMGetTypeKind(elem_ty) == LLVMVoidTypeKind)
            elem_ty = LLVMInt64TypeInContext(cg->ctx);
        return LLVMBuildGEP2(cg->builder, elem_ty, obj,
            (LLVMValueRef[]){idx}, 1, "idx.ptr");
    }
    if (node->type == NODE_TUPLE_FIELD) {
        LLVMValueRef obj = codegen_expr(cg, node->as.tuple_field.object);
        if (!obj) return NULL;
        /* For tuple field lvalue, extract-then-insert approach is needed;
         * we return the extractvalue result; assignment handled specially */
        return LLVMBuildExtractValue(cg->builder, obj,
            (unsigned)node->as.tuple_field.index, "tuplefield");
    }
    return codegen_expr(cg, node);
}

static LLVMValueRef codegen_assign(CodegenCtx *cg, AstNode *node) {
    /* Handle tuple field assignment: t.0 = value */
    if (node->as.assign.target->type == NODE_TUPLE_FIELD) {
        AstNode *target = node->as.assign.target;
        const char *var_name = target->as.tuple_field.object->as.ident.name;
        long field_idx = target->as.tuple_field.index;
        LLVMValueRef var_ptr = var_lookup(cg, var_name);
        if (!var_ptr) {
            error_at(node->loc, ERR_SEMANTIC,
                "tuple assignment: undefined variable '%s'", var_name);
            return NULL;
        }
        LLVMTypeRef var_ty = var_lookup_type(cg, var_name);
        LLVMValueRef old_tuple = LLVMBuildLoad2(cg->builder, var_ty, var_ptr, "old.tuple");
        LLVMValueRef new_val = codegen_expr(cg, node->as.assign.value);
        LLVMValueRef new_tuple = LLVMBuildInsertValue(cg->builder, old_tuple, new_val, (unsigned)field_idx, "new.tuple");
        LLVMBuildStore(cg->builder, new_tuple, var_ptr);
        return new_val;
    }
    LLVMValueRef target = get_lvalue(cg, node->as.assign.target);
    LLVMValueRef value  = codegen_expr(cg, node->as.assign.value);
    if (!target || !value) return NULL;
    const char *op = node->as.assign.op;
    if (strcmp(op, "=") == 0) {
        if (node->as.assign.target->type == NODE_IDENTIFIER) {
            const char *var_name = node->as.assign.target->as.ident.name;
            size_t vi;
            if (var_lookup_index(cg, var_name, &vi) && is_arc_type_for_var(cg, vi)) {
                LLVMValueRef old_val = LLVMBuildLoad2(cg->builder, cg->vars[vi].ty, cg->vars[vi].val, "arc.old");
                if (cg->vars[vi].is_shared)
                    call_arc_release_shared(cg, old_val);
                else
                    call_arc_release(cg, old_val);
            }
        }
        if (node->as.assign.value->type == NODE_STRING_LIT &&
            node->as.assign.target->type == NODE_IDENTIFIER) {
            const char *vn = node->as.assign.target->as.ident.name;
            const char *tn = var_lookup_type_name(cg, vn);
            if (tn && strcmp(tn, "string") == 0)
                value = wrap_string_literal(cg, value);
        }
        LLVMBuildStore(cg->builder, value, target);
        return value;
    }
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

static LLVMValueRef codegen_call(CodegenCtx *cg, AstNode *node);
static LLVMValueRef codegen_self_ref(CodegenCtx *cg, AstNode *node);

static LLVMValueRef codegen_call(CodegenCtx *cg, AstNode *node) {
    if (!node->as.call.callee)
        return LLVMConstNull(LLVMPointerType(LLVMInt8TypeInContext(cg->ctx), 0));

    if (node->as.call.callee->type == NODE_MEMBER &&
        node->as.call.args.count == 1 &&
        node->as.call.args.items[0]->type == NODE_GENERIC_INST) {
        AstNode *obj = node->as.call.callee->as.member.object;
        const char *method = node->as.call.callee->as.member.member;
        if (strcmp(method, "to") == 0) {
            const char *target = node->as.call.args.items[0]->as.generic_inst.base_name;

            const char *src_type = NULL;
            if (obj->type == NODE_IDENTIFIER)
                src_type = var_lookup_type_name(cg, obj->as.ident.name);

            /* Skip built-in conversion for struct/class types — dispatch to class method instead */
            {
                int is_class = 0;
                const char *resolved_name = NULL;
                if (src_type && struct_lookup(cg, src_type)) {
                    is_class = 1;
                    resolved_name = src_type;
                }
                /* For generic types like "Foo<int>", build the monomorphized name */
                if (!is_class && src_type) {
                    const char *lt = strchr(src_type, '<');
                    if (lt) {
                        char spec[512];
                        size_t sp = 0;
                        size_t base_len = lt - src_type;
                        if (base_len < sizeof(spec)) {
                            memcpy(spec, src_type, base_len);
                            sp = base_len;
                            const char *p = lt + 1;
                            while (*p && *p != '>') {
                                while (*p == ' ') p++;
                                if (*p == ',') { p++; continue; }
                                if (*p == '>') break;
                                const char *start = p;
                                while (*p && *p != ',' && *p != '>' && *p != ' ') p++;
                                size_t arg_len = p - start;
                                char arg[256];
                                if (arg_len < sizeof(arg)) {
                                    memcpy(arg, start, arg_len);
                                    arg[arg_len] = '\0';
                                    if (strcmp(arg, "int") == 0 || strcmp(arg, "long") == 0)
                                        sp += snprintf(spec + sp, sizeof(spec) - sp, "_i64");
                                    else if (strcmp(arg, "float") == 0)
                                        sp += snprintf(spec + sp, sizeof(spec) - sp, "_f64");
                                    else if (strcmp(arg, "bool") == 0)
                                        sp += snprintf(spec + sp, sizeof(spec) - sp, "_i1");
                                    else if (strcmp(arg, "string") == 0)
                                        sp += snprintf(spec + sp, sizeof(spec) - sp, "_str");
                                    else
                                        sp += snprintf(spec + sp, sizeof(spec) - sp, "_%s", arg);
                                }
                                while (*p && *p != ',' && *p != '>') p++;
                            }
                            spec[sp] = '\0';
                            if (struct_lookup(cg, spec)) {
                                is_class = 1;
                                resolved_name = struct_lookup(cg, spec) ? spec : src_type;
                            }
                        }
                    }
                }
                if (is_class && resolved_name) {
                    char method_mangled[512];
                    snprintf(method_mangled, sizeof(method_mangled), "%s.to", resolved_name);
                    LLVMValueRef callee_fn = LLVMGetNamedFunction(cg->module, method_mangled);
                    if (callee_fn) {
                        LLVMTypeRef callee_ft = fn_type_lookup(cg, method_mangled);
                        LLVMValueRef self_val = codegen_expr(cg, obj);
                        if (!self_val) return NULL;
                        if (!callee_ft) {
                            LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(cg->ctx), 0);
                            callee_ft = LLVMFunctionType(i8ptr, (LLVMTypeRef[]){i8ptr}, 1, 0);
                        }
                        return LLVMBuildCall2(cg->builder, callee_ft, callee_fn,
                            (LLVMValueRef[]){self_val}, 1, "method.call");
                    }
                }
            }

            LLVMValueRef val = codegen_expr(cg, obj);
            if (!val) return NULL;

            LLVMTypeRef val_ty = LLVMTypeOf(val);
            LLVMTypeRef target_ty = resolve_type(cg, target);
            LLVMTypeRef i64_ty = LLVMInt64TypeInContext(cg->ctx);
            LLVMTypeRef f64_ty = LLVMDoubleTypeInContext(cg->ctx);
            LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(cg->ctx), 0);

            /* string target */
            if (strcmp(target, "string") == 0) {
                if (src_type && strcmp(src_type, "string") == 0) return val;
                if (LLVMGetTypeKind(val_ty) == LLVMPointerTypeKind) return val;
                if (LLVMGetTypeKind(val_ty) == LLVMIntegerTypeKind) {
                    if (LLVMGetIntTypeWidth(val_ty) == 1) {
                        LLVMTypeRef fn_ty = LLVMFunctionType(i8ptr, (LLVMTypeRef[]){i64_ty}, 1, 0);
                        LLVMValueRef fn = get_or_declare_runtime_fn(cg, "bool_to_string", fn_ty);
                        LLVMValueRef ext = LLVMBuildZExt(cg->builder, val, i64_ty, "bext");
                        return LLVMBuildCall2(cg->builder, fn_ty, fn, &ext, 1, "tos");
                    }
                    /* Widen integer to i64 for int_to_string */
                    LLVMValueRef wide = LLVMBuildSExtOrBitCast(cg->builder, val, i64_ty, "widened");
                    LLVMTypeRef fn_ty = LLVMFunctionType(i8ptr, (LLVMTypeRef[]){i64_ty}, 1, 0);
                    LLVMValueRef fn = get_or_declare_runtime_fn(cg, "int_to_string", fn_ty);
                    return LLVMBuildCall2(cg->builder, fn_ty, fn, &wide, 1, "tos");
                }
                if (LLVMGetTypeKind(val_ty) == LLVMDoubleTypeKind ||
                    LLVMGetTypeKind(val_ty) == LLVMFloatTypeKind) {
                    LLVMValueRef wide = LLVMBuildFPExt(cg->builder, val, f64_ty, "widened");
                    LLVMTypeRef fn_ty = LLVMFunctionType(i8ptr, (LLVMTypeRef[]){f64_ty}, 1, 0);
                    LLVMValueRef fn = get_or_declare_runtime_fn(cg, "float_to_string", fn_ty);
                    return LLVMBuildCall2(cg->builder, fn_ty, fn, &wide, 1, "tos");
                }
                return val;
            }

            /* bool target */
            if (strcmp(target, "bool") == 0) {
                if (src_type && strcmp(src_type, "bool") == 0) return val;
                if (LLVMGetTypeKind(val_ty) == LLVMIntegerTypeKind && LLVMGetIntTypeWidth(val_ty) == 1)
                    return val;
                if (LLVMGetTypeKind(val_ty) == LLVMIntegerTypeKind)
                    return LLVMBuildICmp(cg->builder, LLVMIntNE, val, LLVMConstInt(val_ty, 0, 0), "tob");
                if (LLVMGetTypeKind(val_ty) == LLVMDoubleTypeKind || LLVMGetTypeKind(val_ty) == LLVMFloatTypeKind)
                    return LLVMBuildFCmp(cg->builder, LLVMRealUNE, val, LLVMConstReal(f64_ty, 0.0), "tob");
                return LLVMConstInt(LLVMInt1TypeInContext(cg->ctx), 1, 0);
            }

            /* int target family */
            if (is_integer_type_name(target)) {
                if (LLVMGetTypeKind(val_ty) == LLVMIntegerTypeKind) {
                    unsigned src_w = LLVMGetIntTypeWidth(val_ty);
                    unsigned dst_w = LLVMGetIntTypeWidth(target_ty);
                    if (src_w == dst_w) return val;
                    if (src_w < dst_w) return LLVMBuildSExtOrBitCast(cg->builder, val, target_ty, "toi");
                    return LLVMBuildTrunc(cg->builder, val, target_ty, "toi");
                }
                if (LLVMGetTypeKind(val_ty) == LLVMDoubleTypeKind || LLVMGetTypeKind(val_ty) == LLVMFloatTypeKind)
                    return LLVMBuildFPToSI(cg->builder, val, target_ty, "toi");
                LLVMTypeRef fn_ty = LLVMFunctionType(i64_ty, (LLVMTypeRef[]){i8ptr}, 1, 0);
                LLVMValueRef fn = get_or_declare_runtime_fn(cg, "parse_int", fn_ty);
                LLVMValueRef parsed = LLVMBuildCall2(cg->builder, fn_ty, fn, &val, 1, "toi");
                return LLVMBuildTruncOrBitCast(cg->builder, parsed, target_ty, "toi");
            }

            /* float target family */
            if (is_float_type_name(target)) {
                if (LLVMGetTypeKind(val_ty) == LLVMDoubleTypeKind && strcmp(target, "f64") == 0) return val;
                if (LLVMGetTypeKind(val_ty) == LLVMFloatTypeKind && strcmp(target, "f32") == 0) return val;
                if (LLVMGetTypeKind(val_ty) == LLVMDoubleTypeKind && strcmp(target, "f32") == 0)
                    return LLVMBuildFPTrunc(cg->builder, val, LLVMFloatTypeInContext(cg->ctx), "tof");
                if (LLVMGetTypeKind(val_ty) == LLVMFloatTypeKind && strcmp(target, "f64") == 0)
                    return LLVMBuildFPExt(cg->builder, val, f64_ty, "tof");
                if (LLVMGetTypeKind(val_ty) == LLVMIntegerTypeKind) {
                    if (strcmp(target, "f32") == 0)
                        return LLVMBuildSIToFP(cg->builder, val, LLVMFloatTypeInContext(cg->ctx), "tof");
                    return LLVMBuildSIToFP(cg->builder, val, f64_ty, "tof");
                }
                LLVMTypeRef fn_ty = LLVMFunctionType(f64_ty, (LLVMTypeRef[]){i8ptr}, 1, 0);
                LLVMValueRef fn = get_or_declare_runtime_fn(cg, "parse_float", fn_ty);
                LLVMValueRef parsed = LLVMBuildCall2(cg->builder, fn_ty, fn, &val, 1, "tof");
                if (strcmp(target, "f32") == 0)
                    return LLVMBuildFPTrunc(cg->builder, parsed, LLVMFloatTypeInContext(cg->ctx), "tof");
                return parsed;
            }

            /* char target */
            if (strcmp(target, "char") == 0) {
                if (LLVMGetTypeKind(val_ty) == LLVMIntegerTypeKind) {
                    if (LLVMGetIntTypeWidth(val_ty) == 8) return val;
                    return LLVMBuildTrunc(cg->builder, val, LLVMInt8TypeInContext(cg->ctx), "toch");
                }
                return val;
            }

            /* Fallback: bitcast */
            return LLVMBuildBitCast(cg->builder, val, target_ty, "tobit");
        }
    }

    if (node->as.call.callee->type == NODE_MEMBER) {
        AstNode *obj = node->as.call.callee->as.member.object;
        const char *member = node->as.call.callee->as.member.member;

        /* Resolve module.class.method calls (nested member access like files.file.writeStr) */
        if (obj->type == NODE_MEMBER &&
            obj->as.member.object->type == NODE_IDENTIFIER &&
            obj->as.member.object->as.ident.name) {
            const char *mod_name = obj->as.member.object->as.ident.name;
            const char *class_name = obj->as.member.member;

            char qualified[512];
            snprintf(qualified, sizeof(qualified), "%s.%s.%s", mod_name, class_name, member);
            const char *c_name = func_map_lookup(cg, qualified);

            if (c_name) {
                LLVMValueRef callee_fn = LLVMGetNamedFunction(cg->module, c_name);
                LLVMTypeRef callee_ft = fn_type_lookup(cg, c_name);
                if (!callee_fn) {
                    if (!callee_ft) {
                        size_t ac = node->as.call.args.count;
                        LLVMTypeRef i64_ty = LLVMInt64TypeInContext(cg->ctx);
                        LLVMTypeRef *argt = malloc(ac * sizeof(LLVMTypeRef));
                        for (size_t i = 0; i < ac; i++) argt[i] = i64_ty;
                        callee_ft = LLVMFunctionType(i64_ty, argt, (unsigned)ac, 0);
                        free(argt);
                    }
                    callee_fn = get_or_declare_runtime_fn(cg, c_name, callee_ft);
                }
                size_t user_argc = node->as.call.args.count;
                LLVMValueRef *args = user_argc > 0 ? malloc(user_argc * sizeof(LLVMValueRef)) : NULL;
                for (size_t i = 0; i < user_argc; i++)
                    args[i] = codegen_expr(cg, node->as.call.args.items[i]);
                LLVMValueRef result = LLVMBuildCall2(cg->builder, callee_ft, callee_fn,
                                                     args ? args : NULL, (unsigned)user_argc, "call");
                if (args) free(args);
                return result;
            }
        }

        if (obj->type == NODE_IDENTIFIER) {
            const char *type_name = var_lookup_type_name(cg, obj->as.ident.name);
            if (type_name && strchr(type_name, '.')) {
                const char *dot = strchr(type_name, '.');
                size_t mod_len = dot - type_name;
                const char *class_name = dot + 1;

                char simple[512];
                snprintf(simple, sizeof(simple), "%.*s.%s.%s", (int)mod_len, type_name, class_name, member);
                const char *c_name = func_map_lookup(cg, simple);

                char fn_mangled[256];
                mangle_call_name(fn_mangled, sizeof(fn_mangled), member, cg,
                                 &node->as.call.args, node->as.call.args.count);
                if (!c_name) {
                    char mangled_qualified[512];
                    snprintf(mangled_qualified, sizeof(mangled_qualified), "%.*s.%s.%s",
                             (int)mod_len, type_name, class_name, fn_mangled);
                    c_name = func_map_lookup(cg, mangled_qualified);
                }

                if (c_name) {
                    LLVMValueRef callee_fn = LLVMGetNamedFunction(cg->module, c_name);
                    LLVMTypeRef callee_ft = fn_type_lookup(cg, c_name);
                    if (!callee_fn) {
                        if (!callee_ft) {
                            LLVMTypeRef i64_ty = LLVMInt64TypeInContext(cg->ctx);
                            size_t total_ac = node->as.call.args.count + 1;
                            LLVMTypeRef *argt = malloc(total_ac * sizeof(LLVMTypeRef));
                            argt[0] = i64_ty;
                            for (size_t i = 1; i < total_ac; i++) argt[i] = i64_ty;
                            callee_ft = LLVMFunctionType(i64_ty, argt, (unsigned)total_ac, 0);
                            free(argt);
                        }
                        callee_fn = get_or_declare_runtime_fn(cg, c_name, callee_ft);
                    }
                    size_t user_argc = node->as.call.args.count;
                    size_t total_argc = user_argc + 1;
                    LLVMValueRef *args = malloc(total_argc * sizeof(LLVMValueRef));
                    args[0] = codegen_expr(cg, obj);
                    for (size_t i = 0; i < user_argc; i++)
                        args[i + 1] = codegen_expr(cg, node->as.call.args.items[i]);
                    LLVMValueRef result = LLVMBuildCall2(cg->builder, callee_ft, callee_fn,
                                                         args, (unsigned)total_argc, "method.call");
                    free(args);
                    return result;
                }
            }
        }
    }

    LLVMValueRef callee = NULL;
    LLVMTypeRef callee_fn_type = NULL;
    int pre_evaluated_args = 0;
    size_t argc = node->as.call.args.count;
    LLVMValueRef *args = NULL;

    if (node->as.call.callee->type == NODE_MEMBER) {
        AstNode *obj = node->as.call.callee->as.member.object;
        const char *member = node->as.call.callee->as.member.member;
        if (obj->type == NODE_IDENTIFIER) {
            LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(cg->ctx), 0);

            char fn_mangled[256];
            mangle_call_name(fn_mangled, sizeof(fn_mangled), member, cg,
                             &node->as.call.args, node->as.call.args.count);

            char qualified_mangled[512];
            snprintf(qualified_mangled, sizeof(qualified_mangled), "%s.%s",
                     obj->as.ident.name, fn_mangled);

            char qualified[256];
            snprintf(qualified, sizeof(qualified), "%s.%s",
                     obj->as.ident.name, member);

            const char *c_name = func_map_lookup(cg, qualified_mangled);
            int mangled_match = (c_name != NULL);
            if (!c_name) c_name = func_map_lookup(cg, qualified);

            if (c_name) {
                /* If unmangled matched but mangled didn't, args may be wrong */
                if (!mangled_match) {
                    LLVMTypeRef check_ft = fn_type_lookup(cg, c_name);
                    if (check_ft && validate_call_args(cg, node, qualified, check_ft))
                        return NULL;
                }
                callee = LLVMGetNamedFunction(cg->module, c_name);
                callee_fn_type = fn_type_lookup(cg, c_name);
                if (!callee_fn_type) {
                    size_t ac = node->as.call.args.count;
                    LLVMTypeRef *argt = ac > 0 ? malloc(ac * sizeof(LLVMTypeRef)) : NULL;
                    for (size_t i = 0; i < ac; i++) argt[i] = i8ptr;
                    callee_fn_type = LLVMFunctionType(i8ptr, argt, (unsigned)ac, 0);
                    if (argt) free(argt);
                }
                if (!callee) {
                    callee = get_or_declare_runtime_fn(cg, c_name, callee_fn_type);
                }
            } else {
                callee = LLVMGetNamedFunction(cg->module, qualified_mangled);
                if (!callee) callee = LLVMGetNamedFunction(cg->module, qualified);
                if (callee) {
                    callee_fn_type = fn_type_lookup(cg, qualified_mangled);
                    if (!callee_fn_type) callee_fn_type = fn_type_lookup(cg, qualified);
                    if (!callee_fn_type) {
                        size_t ac = node->as.call.args.count;
                        LLVMTypeRef *argt = ac > 0 ? malloc(ac * sizeof(LLVMTypeRef)) : NULL;
                        for (size_t i = 0; i < ac; i++) argt[i] = i8ptr;
                        callee_fn_type = LLVMFunctionType(i8ptr, argt, (unsigned)ac, 0);
                        if (argt) free(argt);
                    }
                } else {
                    /* Function not found in func_map or LLVM module — undefined */
                    error_at(node->loc, ERR_SEMANTIC,
                        "undefined function '%s.%s'", obj->as.ident.name, member);
                    return NULL;
                }
            }
        }
        if (!callee) {
            callee = LLVMGetNamedFunction(cg->module, member);
            if (callee) callee_fn_type = fn_type_lookup(cg, member);
        }
    } else if (node->as.call.callee->type == NODE_IDENTIFIER) {
        const char *ident_name = node->as.call.callee->as.ident.name;
        char mangled[512];
        mangle_call_name(mangled, sizeof(mangled), ident_name, cg,
                         &node->as.call.args, node->as.call.args.count);
        callee = LLVMGetNamedFunction(cg->module, mangled);
        if (callee) {
            callee_fn_type = fn_type_lookup(cg, mangled);
        } else {
            static const char *type_chars[] = {"i", "f", "s", "b", "c"};
            size_t argc = node->as.call.args.count;

            /* Evaluate arguments early so we can match overload types */
            LLVMValueRef *arg_vals = argc > 0 ? malloc(argc * sizeof(LLVMValueRef)) : NULL;
            for (size_t i = 0; i < argc; i++)
                arg_vals[i] = codegen_expr(cg, node->as.call.args.items[i]);

            /* Try all mangled overloads, pick the best type-compatible one */
            {
                int best_score = -1;
                for (size_t try_idx = 0; try_idx < 5; try_idx++) {
                    char try_name[512];
                    size_t p = 0;
                    p += snprintf(try_name + p, sizeof(try_name) - p, "_pC%s", ident_name);
                    for (size_t a = 0; a < argc; a++)
                        p += snprintf(try_name + p, sizeof(try_name) - p, "%s", type_chars[try_idx]);
                    LLVMValueRef fn = LLVMGetNamedFunction(cg->module, try_name);
                    if (!fn) continue;
                    LLVMTypeRef fn_ty = fn_type_lookup(cg, try_name);
                    if (!fn_ty) continue;
                    unsigned param_count = LLVMCountParamTypes(fn_ty);
                    if (param_count != argc) continue;
                    LLVMTypeRef *param_tys = malloc(param_count * sizeof(LLVMTypeRef));
                    LLVMGetParamTypes(fn_ty, param_tys);
                    int score = 0;
                    int compatible = 1;
                    for (unsigned a = 0; a < argc; a++) {
                        LLVMTypeRef actual = arg_vals ? LLVMTypeOf(arg_vals[a]) : NULL;
                        if (!actual || param_tys[a] == actual)
                            score += 3;
                        else if (LLVMGetTypeKind(param_tys[a]) == LLVMPointerTypeKind &&
                                 LLVMGetTypeKind(actual) == LLVMPointerTypeKind)
                            score += 2;
                        else if (LLVMGetTypeKind(param_tys[a]) == LLVMIntegerTypeKind &&
                                 LLVMGetTypeKind(actual) == LLVMIntegerTypeKind &&
                                 LLVMGetIntTypeWidth(actual) < LLVMGetIntTypeWidth(param_tys[a]))
                            score += 1;  /* integer promotion: i8 → i64 */
                        else
                            compatible = 0;
                    }
                    free(param_tys);
                    if (compatible && score > best_score) {
                        best_score = score;
                        callee = fn;
                        callee_fn_type = fn_ty;
                    }
                }
            }

            if (!callee) {
                callee = LLVMGetNamedFunction(cg->module, ident_name);
                if (callee) {
                    callee_fn_type = fn_type_lookup(cg, ident_name);
                    if (callee_fn_type && validate_call_args(cg, node, ident_name, callee_fn_type)) {
                        if (arg_vals) free(arg_vals);
                        return NULL;
                    }
                }
            }
            if (!callee) {
                const char *c_name = func_map_lookup(cg, mangled);
                if (!c_name) c_name = func_map_lookup(cg, ident_name);
                if (c_name) {
                    callee_fn_type = fn_type_lookup(cg, c_name);
                    if (!callee_fn_type) {
                        LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(cg->ctx), 0);
                        LLVMTypeRef *argt = argc > 0 ? malloc(argc * sizeof(LLVMTypeRef)) : NULL;
                        for (size_t i = 0; i < argc; i++) argt[i] = i8ptr;
                        callee_fn_type = LLVMFunctionType(i8ptr, argt, (unsigned)argc, 0);
                        if (argt) free(argt);
                    }
                    callee = get_or_declare_runtime_fn(cg, c_name, callee_fn_type);
                }
            }

            /* Try to find a generic function template and call it directly */
            if (!callee) {
                AstNode *tmpl = generic_func_lookup(cg, ident_name);
                if (tmpl) {
                    if (tmpl->as.func_decl.generic_dispatch == 0) {
                        /* Static dispatch: call the i8* version with inttoptr/ptrtoint casts */
                        char gen_mangled[512];
                        mangle_name(gen_mangled, sizeof(gen_mangled), ident_name,
                                    &tmpl->as.func_decl.params, tmpl->as.func_decl.params.count);
                        callee = LLVMGetNamedFunction(cg->module, gen_mangled);
                        if (callee) {
                            callee_fn_type = fn_type_lookup(cg, gen_mangled);
                            if (!callee_fn_type) {
                                LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(cg->ctx), 0);
                                LLVMTypeRef *argt = argc > 0 ? malloc(argc * sizeof(LLVMTypeRef)) : NULL;
                                for (size_t i = 0; i < argc; i++) argt[i] = i8ptr;
                                callee_fn_type = LLVMFunctionType(i8ptr, argt, (unsigned)argc, 0);
                                if (argt) free(argt);
                            }
                            /* Evaluate args and cast to i8* for static generic function */
                            args = argc > 0 ? malloc(argc * sizeof(LLVMValueRef)) : NULL;
                            for (size_t i = 0; i < argc; i++)
                                args[i] = codegen_expr(cg, node->as.call.args.items[i]);
                            pre_evaluated_args = 1;
                            LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(cg->ctx), 0);
                            unsigned param_count = LLVMCountParams(callee);
                            LLVMTypeRef *param_tys = param_count > 0 ? malloc(param_count * sizeof(LLVMTypeRef)) : NULL;
                            if (param_tys) LLVMGetParamTypes(callee_fn_type, param_tys);
                            for (size_t i = 0; i < argc && i < param_count; i++) {
                                if (!param_tys) break;
                                LLVMTypeRef actual_ty = LLVMTypeOf(args[i]);
                                if (LLVMGetTypeKind(actual_ty) == LLVMIntegerTypeKind &&
                                    LLVMGetTypeKind(param_tys[i]) == LLVMPointerTypeKind) {
                                    args[i] = LLVMBuildIntToPtr(cg->builder, args[i], i8ptr, "gen.inttoptr");
                                } else if (LLVMGetTypeKind(actual_ty) == LLVMDoubleTypeKind &&
                                           LLVMGetTypeKind(param_tys[i]) == LLVMPointerTypeKind) {
                                    LLVMTypeRef i64 = LLVMInt64TypeInContext(cg->ctx);
                                    args[i] = LLVMBuildBitCast(cg->builder, args[i], i64, "gen.f2i");
                                    args[i] = LLVMBuildIntToPtr(cg->builder, args[i], i8ptr, "gen.inttoptr");
                                } else if (LLVMGetTypeKind(actual_ty) == LLVMPointerTypeKind &&
                                           LLVMGetTypeKind(param_tys[i]) == LLVMPointerTypeKind) {
                                    /* pointer to pointer — ok */
                                }
                            }
                            if (param_tys) free(param_tys);
                            /* Free pre-evaluated args since we built our own; null to skip fallthrough */
                            free(arg_vals);
                            arg_vals = NULL;
                        }
                    } else {
                        /* Dynamic dispatch: monomorphize with concrete types */
                        /* Determine concrete types from argument values */
                        args = argc > 0 ? malloc(argc * sizeof(LLVMValueRef)) : NULL;
                        for (size_t i = 0; i < argc; i++)
                            args[i] = codegen_expr(cg, node->as.call.args.items[i]);
                        pre_evaluated_args = 1;

                        size_t gp_count = tmpl->as.func_decl.generic_count;
                        LLVMTypeRef *concrete_types = gp_count > 0 ? malloc(gp_count * sizeof(LLVMTypeRef)) : NULL;
                        for (size_t g = 0; g < gp_count; g++)
                            concrete_types[g] = NULL;

                        /* Map argument types to generic parameters */
                        for (size_t a = 0; a < argc && a < tmpl->as.func_decl.params.count; a++) {
                            const char *param_type = tmpl->as.func_decl.params.items[a]->as.param.type;
                            for (size_t g = 0; g < gp_count; g++) {
                                if (strcmp(param_type, tmpl->as.func_decl.generic_params[g]) == 0) {
                                    if (concrete_types[g] == NULL && a < argc)
                                        concrete_types[g] = LLVMTypeOf(args[a]);
                                    break;
                                }
                            }
                        }

                        /* Build specialized function name */
                        char spec_name[512];
                        size_t sp = 0;
                        sp += snprintf(spec_name + sp, sizeof(spec_name) - sp, "_pC%s", ident_name);
                        for (size_t g = 0; g < gp_count; g++) {
                            if (concrete_types[g]) {
                                if (LLVMGetTypeKind(concrete_types[g]) == LLVMIntegerTypeKind) {
                                    unsigned w = LLVMGetIntTypeWidth(concrete_types[g]);
                                    if (w == 1) sp += snprintf(spec_name + sp, sizeof(spec_name) - sp, "b");
                                    else sp += snprintf(spec_name + sp, sizeof(spec_name) - sp, "i");
                                } else if (LLVMGetTypeKind(concrete_types[g]) == LLVMDoubleTypeKind) {
                                    sp += snprintf(spec_name + sp, sizeof(spec_name) - sp, "f");
                                } else if (LLVMGetTypeKind(concrete_types[g]) == LLVMPointerTypeKind) {
                                    sp += snprintf(spec_name + sp, sizeof(spec_name) - sp, "s");
                                } else {
                                    sp += snprintf(spec_name + sp, sizeof(spec_name) - sp, "p");
                                }
                            } else {
                                sp += snprintf(spec_name + sp, sizeof(spec_name) - sp, "p");
                            }
                        }

                        /* Check if already instantiated */
                        callee = LLVMGetNamedFunction(cg->module, spec_name);
                        if (!callee) {
                            /* Build specialized function type with concrete types */
                            size_t param_count = tmpl->as.func_decl.params.count;
                            LLVMTypeRef *spec_param_types = param_count > 0 ? malloc(param_count * sizeof(LLVMTypeRef)) : NULL;
                            for (size_t p = 0; p < param_count; p++) {
                                const char *ptype = tmpl->as.func_decl.params.items[p]->as.param.type;
                                spec_param_types[p] = NULL;
                                for (size_t g = 0; g < gp_count; g++) {
                                    if (strcmp(ptype, tmpl->as.func_decl.generic_params[g]) == 0) {
                                        spec_param_types[p] = concrete_types[g];
                                        break;
                                    }
                                }
                                if (!spec_param_types[p])
                                    spec_param_types[p] = resolve_type(cg, ptype);
                            }

                            /* Resolve return type */
                            const char *ret_type = tmpl->as.func_decl.ret_type;
                            LLVMTypeRef spec_ret_type = NULL;
                            for (size_t g = 0; g < gp_count; g++) {
                                if (strcmp(ret_type, tmpl->as.func_decl.generic_params[g]) == 0) {
                                    spec_ret_type = concrete_types[g];
                                    break;
                                }
                            }
                            if (!spec_ret_type)
                                spec_ret_type = resolve_type(cg, ret_type);

                            LLVMTypeRef spec_fn_type = LLVMFunctionType(spec_ret_type, spec_param_types,
                                (unsigned)param_count, 0);

                            /* Create the specialized function */
                            callee = LLVMAddFunction(cg->module, spec_name, spec_fn_type);
                            fn_type_push(cg, spec_name, spec_fn_type);
                            callee_fn_type = spec_fn_type;

                            /* Save current builder state */
                            LLVMValueRef prev_fn = cg->cur_fn;
                            LLVMBasicBlockRef prev_insert = LLVMGetInsertBlock(cg->builder);
                            size_t saved_var_count = cg->var_count;
                            size_t saved_scope_base = cg->scope_base;

                            /* Create entry block */
                            LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(cg->ctx, callee, "entry");
                            LLVMPositionBuilderAtEnd(cg->builder, entry);

                            cg->cur_fn = callee;
                            cg->scope_base = saved_var_count;

                            /* Create allocas for parameters */
                            for (unsigned i = 0; i < (unsigned)param_count; i++) {
                                LLVMValueRef param = LLVMGetParam(callee, i);
                                const char *pname = tmpl->as.func_decl.params.items[i]->as.param.name;
                                LLVMSetValueName2(param, pname, strlen(pname));
                                LLVMTypeRef pty = LLVMTypeOf(param);
                                LLVMValueRef alloca_inst = LLVMBuildAlloca(cg->builder, pty, pname);
                                LLVMBuildStore(cg->builder, param, alloca_inst);
                                var_push(cg, pname, alloca_inst, pty);
                                var_set_type_name(cg, pname, tmpl->as.func_decl.params.items[i]->as.param.type);
                            }

                            /* Generate the body */
                            codegen_block(cg, tmpl->as.func_decl.body);

                            /* Add implicit return if needed */
                            if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) {
                                if (LLVMGetTypeKind(spec_ret_type) == LLVMVoidTypeKind) {
                                    LLVMBuildRetVoid(cg->builder);
                                } else {
                                    LLVMBuildRet(cg->builder, LLVMConstNull(spec_ret_type));
                                }
                            }

                            /* Restore state */
                            cg->var_count = saved_var_count;
                            cg->scope_base = saved_scope_base;
                            cg->cur_fn = prev_fn;
                            if (prev_insert)
                                LLVMPositionBuilderAtEnd(cg->builder, prev_insert);

                            if (spec_param_types) free(spec_param_types);
                        } else {
                            callee_fn_type = fn_type_lookup(cg, spec_name);
                        }
                        if (concrete_types) free(concrete_types);
                        free(arg_vals);
                        arg_vals = NULL;
                    }
                }
            }

            /* If args were pre-evaluated, use them (skip re-evaluation below) */
            if (arg_vals) {
                if (args) free(args);
                args = arg_vals;
                pre_evaluated_args = 1;
            }
        }
    } else {
        callee = codegen_expr(cg, node->as.call.callee);
        if (callee) callee_fn_type = fn_type_lookup(cg, "unknown");
    }

    if (!callee || !callee_fn_type) {
        error_at(node->loc, ERR_SEMANTIC, "call to undefined function");
        return NULL;
    }

    if (!pre_evaluated_args) {
        args = argc > 0 ? malloc(argc * sizeof(LLVMValueRef)) : NULL;
        for (size_t i = 0; i < argc; i++)
            args[i] = codegen_expr(cg, node->as.call.args.items[i]);
    }

    /* Implicit i8→i64 promotion for integer arguments smaller than the parameter type */
    if (callee_fn_type && argc > 0) {
        unsigned param_count = LLVMCountParamTypes(callee_fn_type);
        LLVMTypeRef *param_tys = param_count > 0 ? malloc(param_count * sizeof(LLVMTypeRef)) : NULL;
        if (param_tys) LLVMGetParamTypes(callee_fn_type, param_tys);
        for (unsigned i = 0; i < argc && i < param_count; i++) {
            LLVMTypeRef arg_ty = LLVMTypeOf(args[i]);
            LLVMTypeRef param_ty = param_tys[i];
            if (LLVMGetTypeKind(arg_ty) == LLVMIntegerTypeKind &&
                LLVMGetTypeKind(param_ty) == LLVMIntegerTypeKind) {
                unsigned arg_w = LLVMGetIntTypeWidth(arg_ty);
                unsigned param_w = LLVMGetIntTypeWidth(param_ty);
                if (arg_w < param_w)
                    args[i] = LLVMBuildSExt(cg->builder, args[i], param_ty, "arg.ext");
            }
        }
        if (param_tys) free(param_tys);
    }

    {
        const char *cname = callee ? LLVMGetValueName(callee) : NULL;
        if (cname && (strcmp(cname, "thread_run") == 0 || strcmp(cname, "thread_run1") == 0) &&
            argc >= 1 && node->as.call.args.items[0]->type == NODE_IDENTIFIER) {
            LLVMTypeRef i64_ty = LLVMInt64TypeInContext(cg->ctx);
            args[0] = LLVMBuildPtrToInt(cg->builder, args[0], i64_ty, "fn.int");
            if (argc == 2) {
                callee_fn_type = LLVMFunctionType(i64_ty,
                    (LLVMTypeRef[]){i64_ty, i64_ty}, 2, 0);
            } else {
                callee_fn_type = LLVMFunctionType(i64_ty,
                    (LLVMTypeRef[]){i64_ty}, 1, 0);
            }
        }
    }

    LLVMValueRef result = LLVMBuildCall2(cg->builder, callee_fn_type, callee, args,
                                          (unsigned)argc, argc > 0 ? "call" : "");
    for (size_t i = 0; i < argc; i++) {
        if (node->as.call.args.items[i]->type == NODE_FSTRING)
            call_arc_release(cg, args[i]);
    }
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
    const char *type_name = node->as.new_expr.type_name;
    int has_type_args = node->as.new_expr.type_args.count > 0;

    /* Handle generic class instantiation: new Foo<T>(args) */
    if (has_type_args) {
        AstNode *tmpl = generic_class_lookup(cg, type_name);
        if (tmpl) {
            /* Build monomorphized struct name: Foo_i64 for Foo<int> */
            char spec_name[512];
            size_t sp = 0;
            sp += snprintf(spec_name + sp, sizeof(spec_name) - sp, "%s", type_name);
            for (size_t t = 0; t < node->as.new_expr.type_args.count; t++) {
                AstNode *ta = node->as.new_expr.type_args.items[t];
                if (ta->type == NODE_IDENTIFIER) {
                    const char *tn = ta->as.ident.name;
                    if (strcmp(tn, "int") == 0 || strcmp(tn, "long") == 0)
                        sp += snprintf(spec_name + sp, sizeof(spec_name) - sp, "_i64");
                    else if (strcmp(tn, "float") == 0)
                        sp += snprintf(spec_name + sp, sizeof(spec_name) - sp, "_f64");
                    else if (strcmp(tn, "bool") == 0)
                        sp += snprintf(spec_name + sp, sizeof(spec_name) - sp, "_i1");
                    else if (strcmp(tn, "string") == 0)
                        sp += snprintf(spec_name + sp, sizeof(spec_name) - sp, "_str");
                    else
                        sp += snprintf(spec_name + sp, sizeof(spec_name) - sp, "_%s", tn);
                }
            }

            /* Check if already monomorphized */
            LLVMTypeRef struct_ty = struct_lookup(cg, spec_name);
            if (!struct_ty) {
                /* Monomorphize: build struct with concrete field types */
                size_t field_count = tmpl->as.class_decl.fields.count;
                LLVMTypeRef *field_types = field_count > 0 ? malloc(field_count * sizeof(LLVMTypeRef)) : NULL;
                char **field_names = field_count > 0 ? malloc(field_count * sizeof(char *)) : NULL;

                for (size_t f = 0; f < field_count; f++) {
                    AstNode *field = tmpl->as.class_decl.fields.items[f];
                    field_names[f] = field->as.var_decl.name;
                    /* Substitute generic type params with concrete types */
                    const char *ftype = field->as.var_decl.type;
                    field_types[f] = NULL;
                    for (size_t t = 0; t < tmpl->as.class_decl.generic_count; t++) {
                        if (strcmp(ftype, tmpl->as.class_decl.generic_params[t]) == 0) {
                            /* Resolve concrete type from type_args */
                            AstNode *ta = node->as.new_expr.type_args.items[t];
                            if (ta->type == NODE_IDENTIFIER) {
                                if (strcmp(ta->as.ident.name, "int") == 0 ||
                                    strcmp(ta->as.ident.name, "long") == 0)
                                    field_types[f] = LLVMInt64TypeInContext(cg->ctx);
                                else if (strcmp(ta->as.ident.name, "float") == 0)
                                    field_types[f] = LLVMDoubleTypeInContext(cg->ctx);
                                else if (strcmp(ta->as.ident.name, "bool") == 0)
                                    field_types[f] = LLVMInt1TypeInContext(cg->ctx);
                                else if (strcmp(ta->as.ident.name, "string") == 0)
                                    field_types[f] = LLVMPointerType(LLVMInt8TypeInContext(cg->ctx), 0);
                                else {
                                    /* Try as struct/class */
                                    LLVMTypeRef st = struct_lookup(cg, ta->as.ident.name);
                                    if (st) field_types[f] = st;
                                    else field_types[f] = LLVMPointerType(LLVMInt8TypeInContext(cg->ctx), 0);
                                }
                            }
                            break;
                        }
                    }
                    if (!field_types[f])
                        field_types[f] = resolve_type(cg, ftype);
                }

                struct_ty = LLVMStructTypeInContext(cg->ctx, field_types,
                    (unsigned)field_count, 0);
                struct_push(cg, spec_name, struct_ty);
                if (field_count > 0)
                    struct_push_fields(cg, spec_name, field_names, field_count);

                if (field_types) free(field_types);
                if (field_names) free(field_names);

                /* Monomorphize methods */
                for (size_t m = 0; m < tmpl->as.class_decl.methods.count; m++) {
                    AstNode *method = tmpl->as.class_decl.methods.items[m];
                    const char *mname = method->as.func_decl.name;

                    /* Build monomorphized method name */
                    char mangled[512];
                    snprintf(mangled, sizeof(mangled), "%s.%s", spec_name, mname);

                    /* Skip if already exists */
                    if (LLVMGetNamedFunction(cg->module, mangled))
                        continue;

                    /* Resolve return type with substitutions */
                    const char *ret_type_name = method->as.func_decl.ret_type;
                    LLVMTypeRef ret_ty = NULL;
                    for (size_t t = 0; t < tmpl->as.class_decl.generic_count; t++) {
                        if (strcmp(ret_type_name, tmpl->as.class_decl.generic_params[t]) == 0) {
                            AstNode *ta = node->as.new_expr.type_args.items[t];
                            if (ta->type == NODE_IDENTIFIER) {
                                if (strcmp(ta->as.ident.name, "int") == 0 ||
                                    strcmp(ta->as.ident.name, "long") == 0)
                                    ret_ty = LLVMInt64TypeInContext(cg->ctx);
                                else if (strcmp(ta->as.ident.name, "float") == 0)
                                    ret_ty = LLVMDoubleTypeInContext(cg->ctx);
                                else if (strcmp(ta->as.ident.name, "bool") == 0)
                                    ret_ty = LLVMInt1TypeInContext(cg->ctx);
                                else if (strcmp(ta->as.ident.name, "string") == 0)
                                    ret_ty = LLVMPointerType(LLVMInt8TypeInContext(cg->ctx), 0);
                                else {
                                    LLVMTypeRef st = struct_lookup(cg, ta->as.ident.name);
                                    if (st) ret_ty = st;
                                    else ret_ty = LLVMPointerType(LLVMInt8TypeInContext(cg->ctx), 0);
                                }
                            }
                            break;
                        }
                    }
                    if (!ret_ty) ret_ty = resolve_type(cg, ret_type_name);
                    if (LLVMGetTypeKind(ret_ty) == LLVMStructTypeKind)
                        ret_ty = LLVMPointerType(ret_ty, 0);

                    /* Build param types: self is always ptr + user params */
                    size_t user_param_count = method->as.func_decl.params.count;
                    int method_has_self = (user_param_count > 0 &&
                                           method->as.func_decl.params.items[0]->as.param.is_self);
                    size_t total_params = user_param_count + (method_has_self ? 0 : 1);
                    LLVMTypeRef *param_types = malloc(total_params * sizeof(LLVMTypeRef));
                    size_t pti = 0;
                    if (!method_has_self)
                        param_types[pti++] = LLVMPointerType(struct_ty, 0); /* self */

                    for (size_t p = 0; p < user_param_count; p++) {
                        AstNode *pm = method->as.func_decl.params.items[p];
                        if (pm->as.param.is_self) {
                            param_types[pti++] = LLVMPointerType(struct_ty, 0);
                            continue;
                        }
                        const char *ptype = pm->as.param.type;
                        LLVMTypeRef pty = NULL;
                        for (size_t t = 0; t < tmpl->as.class_decl.generic_count; t++) {
                            if (strcmp(ptype, tmpl->as.class_decl.generic_params[t]) == 0) {
                                AstNode *ta = node->as.new_expr.type_args.items[t];
                                if (ta->type == NODE_IDENTIFIER) {
                                    if (strcmp(ta->as.ident.name, "int") == 0 ||
                                        strcmp(ta->as.ident.name, "long") == 0)
                                        pty = LLVMInt64TypeInContext(cg->ctx);
                                    else if (strcmp(ta->as.ident.name, "float") == 0)
                                        pty = LLVMDoubleTypeInContext(cg->ctx);
                                    else if (strcmp(ta->as.ident.name, "bool") == 0)
                                        pty = LLVMInt1TypeInContext(cg->ctx);
                                    else if (strcmp(ta->as.ident.name, "string") == 0)
                                        pty = LLVMPointerType(LLVMInt8TypeInContext(cg->ctx), 0);
                                    else {
                                        LLVMTypeRef st = struct_lookup(cg, ta->as.ident.name);
                                        if (st) pty = st;
                                        else pty = LLVMPointerType(LLVMInt8TypeInContext(cg->ctx), 0);
                                    }
                                }
                                break;
                            }
                        }
                        if (!pty) pty = resolve_type(cg, ptype);
                        param_types[pti++] = pty;
                    }

                    LLVMTypeRef fn_type = LLVMFunctionType(ret_ty, param_types,
                        (unsigned)total_params, 0);
                    LLVMValueRef fn = LLVMAddFunction(cg->module, mangled, fn_type);
                    fn_type_push(cg, mangled, fn_type);

                    /* Generate method body */
                    LLVMValueRef prev_fn = cg->cur_fn;
                    LLVMBasicBlockRef prev_insert = LLVMGetInsertBlock(cg->builder);
                    size_t saved_var_count = cg->var_count;
                    size_t saved_scope_base = cg->scope_base;

                    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(cg->ctx, fn, "entry");
                    LLVMPositionBuilderAtEnd(cg->builder, entry);
                    cg->cur_fn = fn;

                    /* Create allocas for params */
                    for (unsigned i = 0; i < (unsigned)total_params; i++) {
                        LLVMValueRef param = LLVMGetParam(fn, i);
                        const char *pname;
                        if (i == 0 && !method_has_self)
                            pname = "self";
                        else if (method_has_self && i == 0)
                            pname = "self";
                        else {
                            size_t pidx = method_has_self ? i : i - 1;
                            pname = method->as.func_decl.params.items[pidx]->as.param.name;
                        }
                        LLVMSetValueName2(param, pname, strlen(pname));
                        LLVMTypeRef pty = LLVMTypeOf(param);
                        LLVMValueRef alloca_inst = LLVMBuildAlloca(cg->builder, pty, pname);
                        LLVMBuildStore(cg->builder, param, alloca_inst);
                        var_push(cg, pname, alloca_inst, pty);
                    }

                    /* Set scope_base AFTER params — params are borrowed, not ARC-owned */
                    cg->scope_base = cg->var_count;

                    /* Mark self as the struct type */
                    var_set_struct_name(cg, "self", spec_name);
                    var_set_elem_type(cg, "self", struct_ty);

                    /* Generate body */
                    codegen_block(cg, method->as.func_decl.body);

                    /* Implicit return */
                    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) {
                        if (LLVMGetTypeKind(ret_ty) == LLVMVoidTypeKind)
                            LLVMBuildRetVoid(cg->builder);
                        else
                            LLVMBuildRet(cg->builder, LLVMConstNull(ret_ty));
                    }

                    /* Restore state */
                    cg->var_count = saved_var_count;
                    cg->scope_base = saved_scope_base;
                    cg->cur_fn = prev_fn;
                    if (prev_insert)
                        LLVMPositionBuilderAtEnd(cg->builder, prev_insert);

                    free(param_types);
                }
            }

            /* Now create the instance */
            LLVMValueRef size = LLVMSizeOf(struct_ty);
            LLVMValueRef ptr = call_arc_alloc(cg, size);
            LLVMValueRef typed = LLVMBuildBitCast(cg->builder, ptr,
                LLVMPointerType(struct_ty, 0), "new.typed");

            /* Call constructor: SpecName.new(self, args...) */
            if (node->as.new_expr.args.count > 0) {
                char ctor_name[512];
                snprintf(ctor_name, sizeof(ctor_name), "%s.new", spec_name);
                LLVMValueRef ctor = LLVMGetNamedFunction(cg->module, ctor_name);
                if (ctor) {
                    size_t argc = node->as.new_expr.args.count + 1;
                    LLVMValueRef *args = malloc(argc * sizeof(LLVMValueRef));
                    args[0] = typed;
                    for (size_t i = 0; i < node->as.new_expr.args.count; i++)
                        args[i + 1] = codegen_expr(cg, node->as.new_expr.args.items[i]);
                    LLVMTypeRef ctor_ft = fn_type_lookup(cg, ctor_name);
                    LLVMBuildCall2(cg->builder, ctor_ft, ctor, args, (unsigned)argc, "");
                    free(args);
                }
            }
            return typed;
        }
    }

    /* Non-generic new */
    LLVMTypeRef ty = resolve_type(cg, type_name);
    LLVMValueRef size = LLVMSizeOf(ty);
    LLVMValueRef ptr = call_arc_alloc(cg, size);
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
        LLVMValueRef ptr = LLVMBuildGEP2(cg->builder, inner, obj_ptr,
            (LLVMValueRef[]){
                LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), 0, 0),
                LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), (unsigned)idx, 0)
            }, 2, "field.ptr");
        return LLVMBuildLoad2(cg->builder,
            LLVMStructGetTypeAtIndex(inner, (unsigned)idx), ptr, field);
    }

    LLVMValueRef obj = codegen_expr(cg, node->as.member.object);
    return obj;
}

static LLVMValueRef codegen_self_ref(CodegenCtx *cg, AstNode *node) {
    (void)node;
    if (cg->cur_fn) return LLVMGetParam(cg->cur_fn, 0);
    return LLVMConstNull(LLVMPointerType(LLVMInt8TypeInContext(cg->ctx), 0));
}

static LLVMValueRef codegen_super_call(CodegenCtx *cg, AstNode *node) {
    const char *parent = NULL;
    if (cg->cur_fn) {
        const char *fn_name = LLVMGetValueName(cg->cur_fn);
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

LLVMValueRef codegen_auto_tostring(CodegenCtx *cg, LLVMValueRef val) {
    LLVMTypeRef val_ty = LLVMTypeOf(val);
    LLVMTypeRef i64_ty = LLVMInt64TypeInContext(cg->ctx);
    LLVMTypeRef f64_ty = LLVMDoubleTypeInContext(cg->ctx);
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(cg->ctx), 0);

    if (LLVMGetTypeKind(val_ty) == LLVMPointerTypeKind)
        return val;

    if (LLVMGetTypeKind(val_ty) == LLVMIntegerTypeKind) {
        if (LLVMGetIntTypeWidth(val_ty) == 1) {
            LLVMTypeRef fn_ty = LLVMFunctionType(i8ptr, (LLVMTypeRef[]){i64_ty}, 1, 0);
            LLVMValueRef fn = get_or_declare_runtime_fn(cg, "bool_to_string", fn_ty);
            LLVMValueRef ext = LLVMBuildZExt(cg->builder, val, i64_ty, "bext");
            return LLVMBuildCall2(cg->builder, fn_ty, fn, &ext, 1, "tos");
        }
        LLVMValueRef ext = LLVMBuildSExtOrBitCast(cg->builder, val, i64_ty, "iext");
        LLVMTypeRef fn_ty = LLVMFunctionType(i8ptr, (LLVMTypeRef[]){i64_ty}, 1, 0);
        LLVMValueRef fn = get_or_declare_runtime_fn(cg, "int_to_string", fn_ty);
        return LLVMBuildCall2(cg->builder, fn_ty, fn, &ext, 1, "tos");
    }

    if (LLVMGetTypeKind(val_ty) == LLVMDoubleTypeKind) {
        LLVMTypeRef fn_ty = LLVMFunctionType(i8ptr, (LLVMTypeRef[]){f64_ty}, 1, 0);
        LLVMValueRef fn = get_or_declare_runtime_fn(cg, "float_to_string", fn_ty);
        return LLVMBuildCall2(cg->builder, fn_ty, fn, &val, 1, "tos");
    }

    return val;
}

static LLVMValueRef codegen_fstring(CodegenCtx *cg, AstNode *node) {
    LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(cg->ctx), 0);
    LLVMTypeRef str_concat_ty = LLVMFunctionType(i8ptr, (LLVMTypeRef[]){i8ptr, i8ptr}, 2, 0);
    LLVMValueRef str_concat_fn = get_or_declare_runtime_fn(cg,
        func_map_lookup(cg, "console.str_concat") ? func_map_lookup(cg, "console.str_concat") : "penguin_str_concat",
        str_concat_ty);

    size_t count = node->as.fstring.parts.count;
    if (count == 0)
        return LLVMBuildGlobalStringPtr(cg->builder, "", "fstr.empty");

    LLVMValueRef result = NULL;
    int result_is_heap = 0;
    AstNode *first = node->as.fstring.parts.items[0];
    if (first->type == NODE_FSTRING_PART) {
        if (first->as.fstring_part.text) {
            result = LLVMBuildGlobalStringPtr(cg->builder, first->as.fstring_part.text, "fstr");
            } else if (first->as.fstring_part.expr) {
                result = codegen_expr(cg, first->as.fstring_part.expr);
                LLVMTypeRef prev_ty = LLVMTypeOf(result);
                result = codegen_auto_tostring(cg, result);
                result_is_heap = (LLVMGetTypeKind(prev_ty) != LLVMPointerTypeKind);
        }
    }
    if (!result)
        result = LLVMBuildGlobalStringPtr(cg->builder, "", "fstr.empty");

    for (size_t i = 1; i < count; i++) {
        AstNode *part = node->as.fstring.parts.items[i];
        LLVMValueRef part_val = NULL;
        int part_is_heap = 0;
        if (part->type == NODE_FSTRING_PART) {
            if (part->as.fstring_part.text) {
                part_val = LLVMBuildGlobalStringPtr(cg->builder, part->as.fstring_part.text, "fstr");
            } else if (part->as.fstring_part.expr) {
                part_val = codegen_expr(cg, part->as.fstring_part.expr);
                LLVMTypeRef prev_ty = LLVMTypeOf(part_val);
                part_val = codegen_auto_tostring(cg, part_val);
                part_is_heap = (LLVMGetTypeKind(prev_ty) != LLVMPointerTypeKind);
            }
        }
        if (!part_val) {
            part_val = LLVMBuildGlobalStringPtr(cg->builder, "", "fstr.empty");
            part_is_heap = 0;
        }
        LLVMValueRef old_result = result;
        int old_is_heap = result_is_heap;
        result = LLVMBuildCall2(cg->builder, str_concat_ty, str_concat_fn,
            (LLVMValueRef[]){result, part_val}, 2, "fstr.cat");
        if (old_is_heap) call_arc_release(cg, old_result);
        if (part_is_heap) call_arc_release(cg, part_val);
        result_is_heap = 1;
    }

    return result;
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

static LLVMValueRef codegen_array_lit(CodegenCtx *cg, AstNode *node) {
    size_t count = node->as.array_lit.elements.count;
    LLVMTypeRef i64_ty = LLVMInt64TypeInContext(cg->ctx);
    LLVMTypeRef i32_ty = LLVMInt32TypeInContext(cg->ctx);

    if (count == 0) {
        /* Empty array: allocate i64 header only, return ptr past it */
        LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(cg->ctx), 0);
        LLVMValueRef raw = call_arc_alloc(cg, LLVMSizeOf(i64_ty));
        LLVMValueRef hdr = LLVMBuildBitCast(cg->builder, raw, LLVMPointerType(i64_ty, 0), "arr.hdr");
        LLVMBuildStore(cg->builder, LLVMConstInt(i64_ty, 0, 0), hdr);
        LLVMValueRef data = LLVMBuildGEP2(cg->builder, i64_ty, hdr,
            (LLVMValueRef[]){LLVMConstInt(i32_ty, 1, 0)}, 1, "arr.data");
        return LLVMBuildBitCast(cg->builder, data, i8ptr, "arr.ptr");
    }

    /* Determine element type from first element */
    LLVMTypeRef elem_ty = NULL;
    AstNode *first = node->as.array_lit.elements.items[0];
    if (first->type == NODE_INT_LIT || first->type == NODE_FLOAT_LIT) {
        if (first->type == NODE_FLOAT_LIT)
            elem_ty = LLVMDoubleTypeInContext(cg->ctx);
        else
            elem_ty = i64_ty;
    } else if (first->type == NODE_STRING_LIT) {
        elem_ty = LLVMPointerType(LLVMInt8TypeInContext(cg->ctx), 0);
    } else {
        LLVMValueRef fv = codegen_expr(cg, first);
        if (fv) elem_ty = LLVMTypeOf(fv);
        else elem_ty = i64_ty;
    }

    /* Layout: [i64 length | elem0 elem1 ...]
     * Single allocation: sizeof(i64) + count * sizeof(elem_ty)
     * Data pointer returned to user points past the i64 header. */
    LLVMTypeRef ptr_ty = LLVMPointerType(elem_ty, 0);
    LLVMValueRef hdr_size = LLVMSizeOf(i64_ty);
    LLVMValueRef data_size = LLVMBuildMul(cg->builder,
        LLVMSizeOf(elem_ty), LLVMConstInt(i64_ty, count, 0), "arr.dsize");
    LLVMValueRef total = LLVMBuildAdd(cg->builder, hdr_size, data_size, "arr.total");
    LLVMValueRef raw = call_arc_alloc(cg, total);
    LLVMValueRef hdr = LLVMBuildBitCast(cg->builder, raw, LLVMPointerType(i64_ty, 0), "arr.hdr");

    /* Store length at header[0] */
    LLVMBuildStore(cg->builder, LLVMConstInt(i64_ty, count, 0), hdr);

    /* Data pointer = header + 1 (skip the i64 header) */
    LLVMValueRef data_i8 = LLVMBuildGEP2(cg->builder, i64_ty, hdr,
        (LLVMValueRef[]){LLVMConstInt(i32_ty, 1, 0)}, 1, "arr.data");
    LLVMValueRef arr = LLVMBuildBitCast(cg->builder, data_i8, ptr_ty, "arr.ptr");

    /* Fill elements */
    for (size_t i = 0; i < count; i++) {
        LLVMValueRef val = codegen_expr(cg, node->as.array_lit.elements.items[i]);
        if (!val) continue;
        LLVMValueRef ptr = LLVMBuildGEP2(cg->builder, elem_ty, arr,
            (LLVMValueRef[]){
                LLVMConstInt(i32_ty, (unsigned)i, 0)
            }, 1, "arr.elem");
        LLVMBuildStore(cg->builder, val, ptr);
    }

    return arr;
}

static LLVMValueRef codegen_index(CodegenCtx *cg, AstNode *node) {
    LLVMValueRef obj = codegen_expr(cg, node->as.index.object);
    LLVMValueRef idx = codegen_expr(cg, node->as.index.index);
    if (!obj || !idx) return NULL;

    /* Determine element type from variable info or type annotation */
    LLVMTypeRef elem_ty = NULL;
    if (node->as.index.object->type == NODE_IDENTIFIER) {
        elem_ty = var_lookup_elem_type(cg, node->as.index.object->as.ident.name);
    }
    if (!elem_ty || LLVMGetTypeKind(elem_ty) == LLVMVoidTypeKind) {
        elem_ty = LLVMInt64TypeInContext(cg->ctx);
    }

    LLVMValueRef ptr = LLVMBuildGEP2(cg->builder, elem_ty, obj,
        (LLVMValueRef[]){idx}, 1, "idx.ptr");
    return LLVMBuildLoad2(cg->builder, elem_ty, ptr, "idx.val");
}

static LLVMValueRef codegen_tuple_lit(CodegenCtx *cg, AstNode *node) {
    size_t count = node->as.tuple_lit.elements.count;
    LLVMTypeRef *elem_types = malloc(count * sizeof(LLVMTypeRef));
    LLVMValueRef *elem_vals = malloc(count * sizeof(LLVMValueRef));
    for (size_t i = 0; i < count; i++) {
        elem_vals[i] = codegen_expr(cg, node->as.tuple_lit.elements.items[i]);
        elem_types[i] = LLVMTypeOf(elem_vals[i]);
    }
    LLVMTypeRef tuple_type = LLVMStructTypeInContext(cg->ctx, elem_types, count, 1);
    LLVMValueRef tuple = LLVMGetUndef(tuple_type);
    for (size_t i = 0; i < count; i++) {
        tuple = LLVMBuildInsertValue(cg->builder, tuple, elem_vals[i], i, "tupleinsert");
    }
    free(elem_types);
    free(elem_vals);
    return tuple;
}

static LLVMValueRef codegen_tuple_field(CodegenCtx *cg, AstNode *node) {
    LLVMValueRef obj = codegen_expr(cg, node->as.tuple_field.object);
    long idx = node->as.tuple_field.index;
    return LLVMBuildExtractValue(cg->builder, obj, (unsigned)idx, "tuplefield");
}

LLVMValueRef codegen_expr(CodegenCtx *cg, AstNode *node) {
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
        case NODE_ARRAY_LIT:    return codegen_array_lit(cg, node);
        case NODE_INDEX:        return codegen_index(cg, node);
        case NODE_TUPLE_LIT:    return codegen_tuple_lit(cg, node);
        case NODE_TUPLE_FIELD:  return codegen_tuple_field(cg, node);
        case NODE_STMT_EXPR:    return codegen_expr(cg, node->as.stmt_expr.expr);
        default:
            error_at(node->loc, ERR_SEMANTIC,
                     "cannot codegen expression node type %d", node->type);
            return NULL;
    }
}
