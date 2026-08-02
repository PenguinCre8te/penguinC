#include "codegen_internal.h"

void codegen_block(CodegenCtx *cg, AstNode *node) {
    for (size_t i = 0; i < node->as.block.stmts.count; i++) {
        codegen_stmt(cg, node->as.block.stmts.items[i]);
    }
}

static void codegen_return(CodegenCtx *cg, AstNode *node) {
    if (node->as.ret.value) {
        LLVMValueRef val = codegen_expr(cg, node->as.ret.value);
        for (size_t i = cg->var_count; i > cg->scope_base; i--) {
            codegen_arc_release_var(cg, i - 1);
        }
        if (node->as.ret.value->type == NODE_IDENTIFIER) {
            const char *vn = node->as.ret.value->as.ident.name;
            size_t vi;
            if (var_lookup_index(cg, vn, &vi) && is_arc_type_for_var(cg, vi)) {
                if (cg->vars[vi].is_shared)
                    val = call_arc_retain_shared(cg, val);
                else
                    val = call_arc_retain(cg, val);
            }
        }
        LLVMBuildRet(cg->builder, val);
    } else {
        LLVMBuildRetVoid(cg->builder);
    }
}

static void codegen_var_decl(CodegenCtx *cg, AstNode *node) {
    const char *type_name = node->as.var_decl.type;
    const char *var_name  = node->as.var_decl.name;
    LLVMTypeRef ty = resolve_type(cg, type_name);

    /* For generic types like "Foo<int>", resolve the monomorphized struct name */
    const char *struct_name = type_name;
    const char *lt = strchr(type_name, '<');
    if (lt && (LLVMGetTypeKind(ty) == LLVMPointerTypeKind)) {
        /* Build the monomorphized name from the type args */
        char spec[512];
        size_t sp = 0;
        size_t base_len = lt - type_name;
        if (base_len < sizeof(spec)) {
            memcpy(spec, type_name, base_len);
            sp = base_len;
            /* Parse the type args from the string: Foo<T1,T2> */
            const char *p = lt + 1;
            while (*p && *p != '>') {
                /* Skip whitespace */
                while (*p == ' ') p++;
                if (*p == ',') { p++; continue; }
                if (*p == '>') break;
                /* Read type arg name */
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
            /* Check if this monomorphized struct exists */
            LLVMTypeRef spec_ty = struct_lookup(cg, spec);
            if (spec_ty) {
                struct_name = spec;
                ty = spec_ty;
            }
        }
    }

    if (node->as.var_decl.init && node->as.var_decl.init->type == NODE_NEW_EXPR &&
        (LLVMGetTypeKind(ty) == LLVMStructTypeKind || LLVMGetTypeKind(ty) == LLVMPointerTypeKind)) {
        LLVMTypeRef ptr_ty = LLVMPointerType(ty, 0);
        LLVMValueRef alloca_inst = LLVMBuildAlloca(cg->builder, ptr_ty, var_name);
        var_push(cg, var_name, alloca_inst, ptr_ty);
        var_set_elem_type(cg, var_name, ty);
        var_set_struct_name(cg, var_name, struct_name);
        var_set_type_name(cg, var_name, struct_name);
        var_set_is_shared(cg, var_name, node->as.var_decl.is_shared);

        /* Create debug info for this local variable */
        debug_create_variable(cg, var_name, node->loc, ptr_ty, alloca_inst);

        LLVMValueRef init_val = codegen_expr(cg, node->as.var_decl.init);
        if (init_val) {
            /* After evaluating init, re-resolve struct_name from the actual pointer element type */
            LLVMTypeRef actual_ptr_ty = LLVMTypeOf(init_val);
            if (LLVMGetTypeKind(actual_ptr_ty) == LLVMPointerTypeKind) {
                LLVMTypeRef elem_ty = LLVMGetElementType(actual_ptr_ty);
                if (LLVMGetTypeKind(elem_ty) == LLVMStructTypeKind) {
                    /* Find the struct name from the struct registry */
                    for (size_t si = 0; si < cg->struct_count; si++) {
                        if (cg->structs[si].ty == elem_ty) {
                            var_set_struct_name(cg, var_name, cg->structs[si].name);
                            var_set_type_name(cg, var_name, cg->structs[si].name);
                            var_set_elem_type(cg, var_name, elem_ty);
                            break;
                        }
                    }
                }
            }
            LLVMBuildStore(cg->builder, init_val, alloca_inst);
        }
        return;
    }

    LLVMValueRef alloca_inst = LLVMBuildAlloca(cg->builder, ty, var_name);
    var_push(cg, var_name, alloca_inst, ty);
    var_set_type_name(cg, var_name, type_name);
    var_set_is_shared(cg, var_name, node->as.var_decl.is_shared);
    if (LLVMGetTypeKind(ty) == LLVMStructTypeKind)
        var_set_struct_name(cg, var_name, type_name);

    /* For array<T> types, set elem_type to the element type */
    if (strncmp(type_name, "array<", 6) == 0 && LLVMGetTypeKind(ty) == LLVMPointerTypeKind) {
        const char *elem_start = type_name + 6;
        const char *elem_end = strrchr(type_name, '>');
        if (elem_end && elem_end > elem_start) {
            size_t elem_len = elem_end - elem_start;
            char elem_type_name[256];
            if (elem_len < sizeof(elem_type_name)) {
                memcpy(elem_type_name, elem_start, elem_len);
                elem_type_name[elem_len] = '\0';
                LLVMTypeRef elem_ty = resolve_type(cg, elem_type_name);
                var_set_elem_type(cg, var_name, elem_ty);
            }
        }
    }

    /* Create debug info for this local variable */
    debug_create_variable(cg, var_name, node->loc, ty, alloca_inst);

    if (node->as.var_decl.init) {
        AstNode *init_node = node->as.var_decl.init;
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
            } else if (LLVMGetTypeKind(init_ty) == LLVMPointerTypeKind &&
                       LLVMGetTypeKind(ty) == LLVMIntegerTypeKind) {
                init_val = LLVMBuildPtrToInt(cg->builder, init_val, ty, "ptrtoint");
            } else if (LLVMGetTypeKind(init_ty) == LLVMIntegerTypeKind &&
                       LLVMGetTypeKind(ty) == LLVMIntegerTypeKind &&
                       LLVMGetIntTypeWidth(init_ty) != LLVMGetIntTypeWidth(ty)) {
                init_val = LLVMBuildIntCast2(cg->builder, init_val, ty, 1, "coerce");
            } else if (LLVMGetTypeKind(init_ty) == LLVMDoubleTypeKind &&
                       LLVMGetTypeKind(ty) == LLVMIntegerTypeKind) {
                init_val = LLVMBuildFPToSI(cg->builder, init_val, ty, "coerce");
            } else if (LLVMGetTypeKind(init_ty) == LLVMIntegerTypeKind &&
                       LLVMGetTypeKind(ty) == LLVMDoubleTypeKind) {
                init_val = LLVMBuildSIToFP(cg->builder, init_val, ty, "coerce");
            }
            if (init_node->type == NODE_STRING_LIT &&
                strcmp(type_name, "string") == 0) {
                init_val = wrap_string_literal(cg, init_val);
            }
            LLVMBuildStore(cg->builder, init_val, alloca_inst);

            /* For array literal init, set elem_type on the variable */
            if (init_node->type == NODE_ARRAY_LIT && init_node->as.array_lit.elements.count > 0) {
                AstNode *first = init_node->as.array_lit.elements.items[0];
                LLVMTypeRef e_ty = NULL;
                if (first->type == NODE_FLOAT_LIT)
                    e_ty = LLVMDoubleTypeInContext(cg->ctx);
                else if (first->type == NODE_INT_LIT)
                    e_ty = LLVMInt64TypeInContext(cg->ctx);
                else {
                    LLVMValueRef fv = codegen_expr(cg, first);
                    if (fv) e_ty = LLVMTypeOf(fv);
                    else e_ty = LLVMInt64TypeInContext(cg->ctx);
                }
                var_set_elem_type(cg, var_name, e_ty);
            }
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
    LLVMTypeRef i32 = LLVMInt32TypeInContext(cg->ctx);

    /* Check if iter is an array (identifier with elem_type set, or array<T> type) */
    int is_array_iter = 0;
    LLVMTypeRef elem_ty = NULL;
    LLVMValueRef arr_ptr = NULL;
    if (node->as.for_stmt.iter) {
        if (node->as.for_stmt.iter->type == NODE_IDENTIFIER) {
            const char *vn = node->as.for_stmt.iter->as.ident.name;
            /* Check elem_type (set by var_decl for array<T> or array literal) */
            elem_ty = var_lookup_elem_type(cg, vn);
            if (elem_ty) {
                is_array_iter = 1;
                arr_ptr = var_lookup(cg, vn);
                if (arr_ptr) {
                    /* arr_ptr is an alloca containing a pointer to data past the i64 header.
                     * Load the actual data pointer from the alloca. */
                    LLVMTypeRef ptr_ty = LLVMPointerType(elem_ty, 0);
                    arr_ptr = LLVMBuildLoad2(cg->builder, ptr_ty, arr_ptr, "arr.deref");
                }
            }
            /* Fallback: check type_name for array<T> */
            if (!is_array_iter) {
                const char *tn = var_lookup_type_name(cg, vn);
                if (tn && strncmp(tn, "array<", 6) == 0) {
                    is_array_iter = 1;
                    elem_ty = var_lookup_elem_type(cg, vn);
                    if (!elem_ty) elem_ty = i64;
                    arr_ptr = var_lookup(cg, vn);
                    if (arr_ptr) {
                        LLVMTypeRef ptr_ty = LLVMPointerType(elem_ty, 0);
                        arr_ptr = LLVMBuildLoad2(cg->builder, ptr_ty, arr_ptr, "arr.deref");
                    }
                }
            }
        } else if (node->as.for_stmt.iter->type == NODE_ARRAY_LIT) {
            is_array_iter = 1;
            arr_ptr = codegen_expr(cg, node->as.for_stmt.iter);
            if (!elem_ty && node->as.for_stmt.iter->as.array_lit.elements.count > 0) {
                AstNode *first = node->as.for_stmt.iter->as.array_lit.elements.items[0];
                if (first->type == NODE_FLOAT_LIT)
                    elem_ty = LLVMDoubleTypeInContext(cg->ctx);
                else
                    elem_ty = i64;
            }
            if (!elem_ty) elem_ty = i64;
        }
    }

    if (!elem_ty) elem_ty = i64;

    if (is_array_iter && arr_ptr) {
        /* Array iteration: for item in arr
         * Layout: [i64 length | elem0 elem1 ...]
         * Data pointer is past the i64 header, so length is at data_ptr[-1]. */
        LLVMTypeRef i8ptr = LLVMPointerType(LLVMInt8TypeInContext(cg->ctx), 0);
        LLVMTypeRef i64ptr = LLVMPointerType(i64, 0);

        /* Get length: go back 1 i64 from data pointer */
        LLVMValueRef hdr_ptr = LLVMBuildBitCast(cg->builder, arr_ptr, i64ptr, "arr.hdr.ptr");
        LLVMValueRef len_ptr = LLVMBuildGEP2(cg->builder, i64, hdr_ptr,
            (LLVMValueRef[]){LLVMConstInt(LLVMInt32TypeInContext(cg->ctx), -1, 1)}, 1, "len.ptr");
        LLVMValueRef arr_len = LLVMBuildLoad2(cg->builder, i64, len_ptr, "arr.len");

        /* Index variable: i = 0 */
        LLVMValueRef idx_alloca = LLVMBuildAlloca(cg->builder, i64, "for.idx");
        var_push(cg, "__for_idx", idx_alloca, i64);
        LLVMBuildStore(cg->builder, LLVMConstInt(i64, 0, 0), idx_alloca);

        /* Loop variable: item (typed as elem_ty) */
        LLVMValueRef item_alloca = LLVMBuildAlloca(cg->builder, elem_ty, node->as.for_stmt.var);
        var_push(cg, node->as.for_stmt.var, item_alloca, elem_ty);
        debug_create_variable(cg, node->as.for_stmt.var, node->loc, elem_ty, item_alloca);

        LLVMBuildBr(cg->builder, cond_bb);

        /* Condition: i < arr_len */
        LLVMPositionBuilderAtEnd(cg->builder, cond_bb);
        LLVMValueRef cur_idx = LLVMBuildLoad2(cg->builder, i64, idx_alloca, "for.i");
        LLVMValueRef cmp = LLVMBuildICmp(cg->builder, LLVMIntSLT, cur_idx, arr_len, "for.cmp");
        LLVMBuildCondBr(cg->builder, cmp, body_bb, end_bb);

        /* Body: load arr[i] into item */
        LLVMPositionBuilderAtEnd(cg->builder, body_bb);
        LLVMValueRef elem_ptr = LLVMBuildGEP2(cg->builder, elem_ty, arr_ptr,
            (LLVMValueRef[]){cur_idx}, 1, "elem.ptr");
        LLVMValueRef elem_val = LLVMBuildLoad2(cg->builder, elem_ty, elem_ptr, "elem.val");
        LLVMBuildStore(cg->builder, elem_val, item_alloca);

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
        LLVMValueRef cur_idx2 = LLVMBuildLoad2(cg->builder, i64, idx_alloca, "for.i2");
        LLVMValueRef next = LLVMBuildAdd(cg->builder, cur_idx2, LLVMConstInt(i64, 1, 0), "for.next");
        LLVMBuildStore(cg->builder, next, idx_alloca);
        LLVMBuildBr(cg->builder, cond_bb);

        LLVMPositionBuilderAtEnd(cg->builder, end_bb);
        return;
    }

    /* String iteration: for char in str / for char in "literal" */
    int is_string_iter = 0;
    LLVMValueRef str_ptr = NULL;
    if (node->as.for_stmt.iter) {
        if (node->as.for_stmt.iter->type == NODE_IDENTIFIER) {
            const char *vn = node->as.for_stmt.iter->as.ident.name;
            const char *tn = var_lookup_type_name(cg, vn);
            if (tn && strcmp(tn, "string") == 0) {
                is_string_iter = 1;
                str_ptr = var_lookup(cg, vn);
                if (str_ptr) {
                    LLVMTypeRef str_ty = var_lookup_type(cg, vn);
                    if (str_ty)
                        str_ptr = LLVMBuildLoad2(cg->builder, str_ty, str_ptr, "str.deref");
                }
            }
        } else if (node->as.for_stmt.iter->type == NODE_STRING_LIT) {
            is_string_iter = 1;
            str_ptr = codegen_expr(cg, node->as.for_stmt.iter);
        }
    }

    if (is_string_iter && str_ptr) {
        LLVMTypeRef i8 = LLVMInt8TypeInContext(cg->ctx);
        LLVMTypeRef i8ptr = LLVMPointerType(i8, 0);
        LLVMValueRef zero8 = LLVMConstInt(i8, 0, 0);
        LLVMValueRef one64 = LLVMConstInt(i64, 1, 0);

        /* Index variable: i = 0 */
        LLVMValueRef idx_alloca = LLVMBuildAlloca(cg->builder, i64, "for.idx");
        var_push(cg, "__for_idx", idx_alloca, i64);
        LLVMBuildStore(cg->builder, LLVMConstInt(i64, 0, 0), idx_alloca);

        /* Loop variable: char (typed as i8) */
        LLVMValueRef char_alloca = LLVMBuildAlloca(cg->builder, i8, node->as.for_stmt.var);
        var_push(cg, node->as.for_stmt.var, char_alloca, i8);
        var_set_type_name(cg, node->as.for_stmt.var, "char");
        debug_create_variable(cg, node->as.for_stmt.var, node->loc, i8, char_alloca);

        LLVMBuildBr(cg->builder, cond_bb);

        /* Condition: str[i] != '\0' */
        LLVMPositionBuilderAtEnd(cg->builder, cond_bb);
        LLVMValueRef cur_idx = LLVMBuildLoad2(cg->builder, i64, idx_alloca, "for.i");
        LLVMValueRef elem_ptr = LLVMBuildGEP2(cg->builder, i8, str_ptr,
            (LLVMValueRef[]){cur_idx}, 1, "ch.ptr");
        LLVMValueRef ch = LLVMBuildLoad2(cg->builder, i8, elem_ptr, "ch");
        LLVMValueRef cmp = LLVMBuildICmp(cg->builder, LLVMIntNE, ch, zero8, "for.cmp");
        LLVMBuildCondBr(cg->builder, cmp, body_bb, end_bb);

        /* Body: store char, execute body */
        LLVMPositionBuilderAtEnd(cg->builder, body_bb);
        LLVMBuildStore(cg->builder, ch, char_alloca);

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
        LLVMValueRef cur_idx2 = LLVMBuildLoad2(cg->builder, i64, idx_alloca, "for.i2");
        LLVMValueRef next = LLVMBuildAdd(cg->builder, cur_idx2, one64, "for.next");
        LLVMBuildStore(cg->builder, next, idx_alloca);
        LLVMBuildBr(cg->builder, cond_bb);

        LLVMPositionBuilderAtEnd(cg->builder, end_bb);
        return;
    }

    /* Range/integer iteration (original code) */
    LLVMValueRef range_start = NULL, range_end = NULL;
    if (node->as.for_stmt.iter && node->as.for_stmt.iter->type == NODE_RANGE) {
        range_start = codegen_expr(cg, node->as.for_stmt.iter->as.range.start);
        range_end   = codegen_expr(cg, node->as.for_stmt.iter->as.range.end);
    } else {
        range_start = LLVMConstInt(i64, 0, 0);
        range_end = codegen_expr(cg, node->as.for_stmt.iter);
    }

    LLVMValueRef alloca_inst = LLVMBuildAlloca(cg->builder, i64, node->as.for_stmt.var);
    var_push(cg, node->as.for_stmt.var, alloca_inst, i64);
    LLVMBuildStore(cg->builder, range_start, alloca_inst);

    /* Create debug info for for-loop variable */
    debug_create_variable(cg, node->as.for_stmt.var, node->loc,
                          i64, alloca_inst);

    LLVMBuildBr(cg->builder, cond_bb);

    LLVMPositionBuilderAtEnd(cg->builder, cond_bb);
    LLVMValueRef cur = LLVMBuildLoad2(cg->builder, i64, alloca_inst, "for.i");
    LLVMValueRef cmp2 = LLVMBuildICmp(cg->builder, LLVMIntSLT, cur, range_end, "for.cmp");
    LLVMBuildCondBr(cg->builder, cmp2, body_bb, end_bb);

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

    LLVMPositionBuilderAtEnd(cg->builder, incr_bb);
    LLVMValueRef cur2 = LLVMBuildLoad2(cg->builder, i64, alloca_inst, "for.i2");
    LLVMValueRef next2 = LLVMBuildAdd(cg->builder, cur2, LLVMConstInt(i64, 1, 0), "for.next");
    LLVMBuildStore(cg->builder, next2, alloca_inst);
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

    LLVMBasicBlockRef cur_check_bb = start;
    for (size_t i = 0; i < case_count; i++) {
        AstNode *c = node->as.match_stmt.cases.items[i];
        if (c->as.match_case.is_default) continue;

        LLVMPositionBuilderAtEnd(cg->builder, cur_check_bb);
        AstNode *pattern = c->as.match_case.pattern;

        LLVMValueRef cond = NULL;
        if (pattern->type == NODE_BINARY && strcmp(pattern->as.binary.op, "in") == 0) {
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
            LLVMValueRef rhs = codegen_expr(cg, pattern->as.binary.right);
            const char *op = pattern->as.binary.op;
            if (strcmp(op, ">") == 0)   cond = LLVMBuildICmp(cg->builder, LLVMIntSGT, expr_val, rhs, "match.gt");
            if (strcmp(op, "<") == 0)   cond = LLVMBuildICmp(cg->builder, LLVMIntSLT, expr_val, rhs, "match.lt");
            if (strcmp(op, ">=") == 0)  cond = LLVMBuildICmp(cg->builder, LLVMIntSGE, expr_val, rhs, "match.ge");
            if (strcmp(op, "<=") == 0)  cond = LLVMBuildICmp(cg->builder, LLVMIntSLE, expr_val, rhs, "match.le");
            if (strcmp(op, "==") == 0)  cond = LLVMBuildICmp(cg->builder, LLVMIntEQ,  expr_val, rhs, "match.eq");
            if (strcmp(op, "!=") == 0)  cond = LLVMBuildICmp(cg->builder, LLVMIntNE,  expr_val, rhs, "match.ne");
        } else {
            LLVMValueRef val = codegen_expr(cg, pattern);
            cond = LLVMBuildICmp(cg->builder, LLVMIntEQ, expr_val, val, "match.eq");
        }

        LLVMBasicBlockRef next_check = LLVMAppendBasicBlockInContext(cg->ctx, fn, "match.next");
        LLVMBuildCondBr(cg->builder, cond, case_bbs[i], next_check);
        cur_check_bb = next_check;
    }

    LLVMPositionBuilderAtEnd(cg->builder, cur_check_bb);
    if (default_bb) {
        LLVMBuildBr(cg->builder, default_bb);
    } else {
        LLVMBuildBr(cg->builder, merge_bb);
    }

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
    LLVMValueRef resource = codegen_expr(cg, node->as.using_stmt.resource);
    if (!resource) {
        codegen_block(cg, node->as.using_stmt.body);
        return;
    }

    /* Determine the type name of the resource for method dispatch */
    const char *type_name = NULL;
    AstNode *res = node->as.using_stmt.resource;

    /* Unwrap call: files.open(...) → callee is Member(Ident("files"), "open") */
    AstNode *unwrap = res;
    if (unwrap->type == NODE_CALL && unwrap->as.call.callee)
        unwrap = unwrap->as.call.callee;

    if (unwrap->type == NODE_IDENTIFIER) {
        type_name = var_lookup_type_name(cg, unwrap->as.ident.name);
    } else if (unwrap->type == NODE_MEMBER) {
        AstNode *obj = unwrap->as.member.object;
        if (obj->type == NODE_IDENTIFIER) {
            const char *mod_name = obj->as.ident.name;
            for (size_t i = 0; i < cg->func_map_count; i++) {
                const char *key = cg->func_maps[i].pc_name;
                if (strncmp(key, mod_name, strlen(mod_name)) == 0 &&
                    key[strlen(mod_name)] == '.') {
                    const char *rest = key + strlen(mod_name) + 1;
                    const char *dot2 = strchr(rest, '.');
                    if (dot2) {
                        size_t class_len = dot2 - rest;
                        char class_type[512];
                        snprintf(class_type, sizeof(class_type), "%s.%.*s",
                                 mod_name, (int)class_len, rest);
                        type_name = strdup(class_type);
                        break;
                    }
                }
            }
        }
    }

    LLVMTypeRef i64_ty = LLVMInt64TypeInContext(cg->ctx);
    LLVMTypeRef void_ty = LLVMVoidTypeInContext(cg->ctx);

    /* If var_name is set, store the resource handle in a named variable */
    if (node->as.using_stmt.var_name) {
        LLVMValueRef alloca = LLVMBuildAlloca(cg->builder, i64_ty, node->as.using_stmt.var_name);
        LLVMBuildStore(cg->builder, resource, alloca);
        var_push(cg, node->as.using_stmt.var_name, alloca, i64_ty);
        if (type_name)
            var_set_type_name(cg, node->as.using_stmt.var_name, type_name);
    }

    /* Call enter() on the resource if type info available */
    if (type_name && strchr(type_name, '.')) {
        const char *dot = strchr(type_name, '.');
        size_t mod_len = dot - type_name;
        const char *class_name = dot + 1;

        char enter_key[512];
        snprintf(enter_key, sizeof(enter_key), "%.*s.%s.enter", (int)mod_len, type_name, class_name);
        const char *enter_c = func_map_lookup(cg, enter_key);

        if (enter_c) {
            LLVMValueRef enter_fn = LLVMGetNamedFunction(cg->module, enter_c);
            LLVMTypeRef enter_ft = fn_type_lookup(cg, enter_c);
            if (!enter_ft)
                enter_ft = LLVMFunctionType(void_ty, (LLVMTypeRef[]){i64_ty}, 1, 0);
            if (!enter_fn)
                enter_fn = get_or_declare_runtime_fn(cg, enter_c, enter_ft);
            LLVMBuildCall2(cg->builder, enter_ft, enter_fn, &resource, 1, "");
        }
    }

    /* Execute the body */
    codegen_block(cg, node->as.using_stmt.body);

    /* Call exit() on the resource if type info available */
    if (type_name && strchr(type_name, '.')) {
        const char *dot = strchr(type_name, '.');
        size_t mod_len = dot - type_name;
        const char *class_name = dot + 1;

        char exit_key[512];
        snprintf(exit_key, sizeof(exit_key), "%.*s.%s.exit", (int)mod_len, type_name, class_name);
        const char *exit_c = func_map_lookup(cg, exit_key);

        if (exit_c) {
            LLVMValueRef exit_fn = LLVMGetNamedFunction(cg->module, exit_c);
            LLVMTypeRef exit_ft = fn_type_lookup(cg, exit_c);
            if (!exit_ft)
                exit_ft = LLVMFunctionType(void_ty, (LLVMTypeRef[]){i64_ty}, 1, 0);
            if (!exit_fn)
                exit_fn = get_or_declare_runtime_fn(cg, exit_c, exit_ft);
            LLVMBuildCall2(cg->builder, exit_ft, exit_fn, &resource, 1, "");
        }
    }
}

static void codegen_unsafe(CodegenCtx *cg, AstNode *node) {
    codegen_block(cg, node->as.unsafe_stmt.body);
}

void codegen_stmt(CodegenCtx *cg, AstNode *node) {
    if (!node) return;

    /* Set debug location for this statement */
    debug_set_location(cg, node->loc);

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
