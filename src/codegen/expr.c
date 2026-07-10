#include "codegen_internal.h"

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
    return codegen_expr(cg, node);
}

static LLVMValueRef codegen_assign(CodegenCtx *cg, AstNode *node) {
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
        node->as.call.args.count == 0) {
        AstNode *obj = node->as.call.callee->as.member.object;
        const char *method = node->as.call.callee->as.member.member;
        if (strcmp(method, "toI") == 0 || strcmp(method, "toF") == 0 ||
            strcmp(method, "toS") == 0 || strcmp(method, "toB") == 0) {
            LLVMValueRef val = codegen_expr(cg, obj);
            if (!val) return NULL;

            const char *src_type = NULL;
            if (obj->type == NODE_IDENTIFIER)
                src_type = var_lookup_type_name(cg, obj->as.ident.name);

            LLVMTypeRef val_ty = LLVMTypeOf(val);
            LLVMTypeRef i64_ty = LLVMInt64TypeInContext(cg->ctx);
            LLVMTypeRef f64_ty = LLVMDoubleTypeInContext(cg->ctx);
            LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(cg->ctx), 0);

            if (strcmp(method, "toI") == 0) {
                if (src_type && strcmp(src_type, "int") == 0) return val;
                if (LLVMGetTypeKind(val_ty) == LLVMIntegerTypeKind)
                    return LLVMBuildSExtOrBitCast(cg->builder, val, i64_ty, "toi");
                if (LLVMGetTypeKind(val_ty) == LLVMDoubleTypeKind)
                    return LLVMBuildFPToSI(cg->builder, val, i64_ty, "toi");
                LLVMTypeRef fn_ty = LLVMFunctionType(i64_ty, (LLVMTypeRef[]){i8ptr}, 1, 0);
                LLVMValueRef fn = get_or_declare_runtime_fn(cg, "parse_int", fn_ty);
                return LLVMBuildCall2(cg->builder, fn_ty, fn, &val, 1, "toi");
            }
            if (strcmp(method, "toF") == 0) {
                if (src_type && strcmp(src_type, "float") == 0) return val;
                if (LLVMGetTypeKind(val_ty) == LLVMDoubleTypeKind) return val;
                if (LLVMGetTypeKind(val_ty) == LLVMIntegerTypeKind)
                    return LLVMBuildSIToFP(cg->builder, val, f64_ty, "tof");
                LLVMTypeRef fn_ty = LLVMFunctionType(f64_ty, (LLVMTypeRef[]){i8ptr}, 1, 0);
                LLVMValueRef fn = get_or_declare_runtime_fn(cg, "parse_float", fn_ty);
                return LLVMBuildCall2(cg->builder, fn_ty, fn, &val, 1, "tof");
            }
            if (strcmp(method, "toS") == 0) {
                if (src_type && strcmp(src_type, "string") == 0) return val;
                if (LLVMGetTypeKind(val_ty) == LLVMPointerTypeKind) return val;
                if (LLVMGetTypeKind(val_ty) == LLVMIntegerTypeKind) {
                    if (LLVMGetIntTypeWidth(val_ty) == 1) {
                        LLVMTypeRef fn_ty = LLVMFunctionType(i8ptr, (LLVMTypeRef[]){i64_ty}, 1, 0);
                        LLVMValueRef fn = get_or_declare_runtime_fn(cg, "bool_to_string", fn_ty);
                        LLVMValueRef ext = LLVMBuildZExt(cg->builder, val, i64_ty, "bext");
                        return LLVMBuildCall2(cg->builder, fn_ty, fn, &ext, 1, "tos");
                    }
                    LLVMTypeRef fn_ty = LLVMFunctionType(i8ptr, (LLVMTypeRef[]){i64_ty}, 1, 0);
                    LLVMValueRef fn = get_or_declare_runtime_fn(cg, "int_to_string", fn_ty);
                    return LLVMBuildCall2(cg->builder, fn_ty, fn, &val, 1, "tos");
                }
                if (LLVMGetTypeKind(val_ty) == LLVMDoubleTypeKind) {
                    LLVMTypeRef fn_ty = LLVMFunctionType(i8ptr, (LLVMTypeRef[]){f64_ty}, 1, 0);
                    LLVMValueRef fn = get_or_declare_runtime_fn(cg, "float_to_string", fn_ty);
                    return LLVMBuildCall2(cg->builder, fn_ty, fn, &val, 1, "tos");
                }
                return val;
            }
            if (strcmp(method, "toB") == 0) {
                if (src_type && strcmp(src_type, "bool") == 0) return val;
                if (LLVMGetTypeKind(val_ty) == LLVMIntegerTypeKind && LLVMGetIntTypeWidth(val_ty) == 1)
                    return val;
                if (LLVMGetTypeKind(val_ty) == LLVMIntegerTypeKind)
                    return LLVMBuildICmp(cg->builder, LLVMIntNE, val, LLVMConstInt(val_ty, 0, 0), "tob");
                if (LLVMGetTypeKind(val_ty) == LLVMDoubleTypeKind)
                    return LLVMBuildFCmp(cg->builder, LLVMRealUNE, val, LLVMConstReal(f64_ty, 0.0), "tob");
                return LLVMConstInt(LLVMInt1TypeInContext(cg->ctx), 1, 0);
            }
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
            if (!c_name) c_name = func_map_lookup(cg, qualified);

            if (c_name) {
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
            static const char *type_chars[] = {"i", "f", "s", "b"};
            size_t argc = node->as.call.args.count;
            for (size_t try_idx = 0; try_idx < 4 && !callee; try_idx++) {
                char try_name[512];
                size_t p = 0;
                p += snprintf(try_name + p, sizeof(try_name) - p, "_pC%s", ident_name);
                for (size_t a = 0; a < argc; a++)
                    p += snprintf(try_name + p, sizeof(try_name) - p, "%s", type_chars[try_idx]);
                callee = LLVMGetNamedFunction(cg->module, try_name);
                if (callee) callee_fn_type = fn_type_lookup(cg, try_name);
            }
            if (!callee) {
                callee = LLVMGetNamedFunction(cg->module, ident_name);
                if (callee) callee_fn_type = fn_type_lookup(cg, ident_name);
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

    size_t argc = node->as.call.args.count;
    LLVMValueRef *args = argc > 0 ? malloc(argc * sizeof(LLVMValueRef)) : NULL;
    for (size_t i = 0; i < argc; i++)
        args[i] = codegen_expr(cg, node->as.call.args.items[i]);

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
    LLVMTypeRef ty = resolve_type(cg, node->as.new_expr.type_name);
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
        case NODE_STMT_EXPR:    return codegen_expr(cg, node->as.stmt_expr.expr);
        default:
            error_at(node->loc, ERR_SEMANTIC,
                     "cannot codegen expression node type %d", node->type);
            return NULL;
    }
}
