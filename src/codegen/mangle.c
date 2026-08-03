#include "codegen_internal.h"

void mangle_name(char *buf, size_t buflen, const char *name,
                 NodeList *params, size_t param_count) {
    size_t pos = 0;
    pos += snprintf(buf + pos, buflen - pos, "_pC%s", name);
    for (size_t i = 0; i < param_count && pos < buflen - 1; i++) {
        const char *t = params->items[i]->as.param.type;
        if (strchr(t, '*')) { buf[pos++] = 'p'; }
        else if (strcmp(t, "int") == 0 || strcmp(t, "long") == 0 || strcmp(t, "i64") == 0 || strcmp(t, "u64") == 0) buf[pos++] = '2';
        else if (strcmp(t, "float") == 0 || strcmp(t, "f64") == 0) buf[pos++] = 'f';
        else if (strcmp(t, "bool") == 0)  buf[pos++] = 'b';
        else if (strcmp(t, "string") == 0) buf[pos++] = 's';
        else if (strcmp(t, "void") == 0)  buf[pos++] = 'v';
        else if (strcmp(t, "char") == 0)  buf[pos++] = 'c';
        else if (strcmp(t, "i8") == 0 || strcmp(t, "u8") == 0)   buf[pos++] = '8';
        else if (strcmp(t, "i16") == 0 || strcmp(t, "u16") == 0) buf[pos++] = '6';
        else if (strcmp(t, "i32") == 0 || strcmp(t, "u32") == 0) buf[pos++] = '4';
        else if (strcmp(t, "i128") == 0 || strcmp(t, "u128") == 0) buf[pos++] = 'Q';
        else if (strcmp(t, "isize") == 0) buf[pos++] = 'S';
        else if (strcmp(t, "usize") == 0) buf[pos++] = 'U';
        else if (strcmp(t, "f32") == 0)   buf[pos++] = 'F';
        else {
            size_t tlen = strlen(t);
            buf[pos++] = 'p';
            if (tlen > 0 && pos < buflen - 1)
                buf[pos++] = t[0];
        }
    }
    buf[pos] = '\0';
}

void mangle_call_name(char *buf, size_t buflen, const char *name,
                      CodegenCtx *cg, NodeList *args, size_t arg_count) {
    size_t pos = 0;
    pos += snprintf(buf + pos, buflen - pos, "_pC%s", name);
    for (size_t i = 0; i < arg_count && pos < buflen - 1; i++) {
        AstNode *a = args->items[i];
        if (a->type == NODE_INT_LIT)       { buf[pos++] = '2'; }
        else if (a->type == NODE_FLOAT_LIT) { buf[pos++] = 'f'; }
        else if (a->type == NODE_STRING_LIT) { buf[pos++] = 's'; }
        else if (a->type == NODE_IDENTIFIER) {
            LLVMTypeRef ty = var_lookup_type(cg, a->as.ident.name);
            const char *tn = var_lookup_type_name(cg, a->as.ident.name);
            const char *resolved = tn ? typedef_resolve(cg, tn) : tn;
            if (resolved && strcmp(resolved, "string") == 0)              buf[pos++] = 's';
            else if (resolved && (strcmp(resolved, "int") == 0 || strcmp(resolved, "i64") == 0 || strcmp(resolved, "u64") == 0))
                buf[pos++] = '2';
            else if (resolved && (strcmp(resolved, "float") == 0 || strcmp(resolved, "f64") == 0))
                buf[pos++] = 'f';
            else if (resolved && strcmp(resolved, "bool") == 0)          buf[pos++] = 'b';
            else if (resolved && strcmp(resolved, "char") == 0)          buf[pos++] = 'c';
            else if (resolved && (strcmp(resolved, "i8") == 0 || strcmp(resolved, "u8") == 0))
                buf[pos++] = '8';
            else if (resolved && (strcmp(resolved, "i16") == 0 || strcmp(resolved, "u16") == 0))
                buf[pos++] = '6';
            else if (resolved && (strcmp(resolved, "i32") == 0 || strcmp(resolved, "u32") == 0))
                buf[pos++] = '4';
            else if (resolved && (strcmp(resolved, "i128") == 0 || strcmp(resolved, "u128") == 0))
                buf[pos++] = 'Q';
            else if (resolved && strcmp(resolved, "isize") == 0)         buf[pos++] = 'S';
            else if (resolved && strcmp(resolved, "usize") == 0)         buf[pos++] = 'U';
            else if (resolved && strcmp(resolved, "f32") == 0)           buf[pos++] = 'F';
            else if (tn && strcmp(tn, "string") == 0)                    buf[pos++] = 's';
            else if (ty == LLVMInt64TypeInContext(cg->ctx))              buf[pos++] = '2';
            else if (ty == LLVMDoubleTypeInContext(cg->ctx))             buf[pos++] = 'f';
            else if (ty == LLVMInt1TypeInContext(cg->ctx))               buf[pos++] = 'b';
            else if (ty && LLVMGetTypeKind(ty) == LLVMIntegerTypeKind) {
                unsigned w = LLVMGetIntTypeWidth(ty);
                if (w == 8)   buf[pos++] = '8';
                else if (w == 16)  buf[pos++] = '6';
                else if (w == 32)  buf[pos++] = '4';
                else if (w == 128) buf[pos++] = 'Q';
                else buf[pos++] = '2';
            }
            else if (ty && LLVMGetTypeKind(ty) == LLVMFloatTypeKind)    buf[pos++] = 'F';
            else if (!ty) {
                buf[pos++] = '2';
            } else                                                         buf[pos++] = 'p';
        } else if (a->type == NODE_BINARY || a->type == NODE_UNARY) {
            if (a->type == NODE_BINARY && strcmp(a->as.binary.op, "+") == 0) {
                if (a->as.binary.left->type == NODE_STRING_LIT ||
                    a->as.binary.right->type == NODE_STRING_LIT) {
                    buf[pos++] = 's'; continue;
                }
                if (a->as.binary.left->type == NODE_IDENTIFIER) {
                    const char *tn = var_lookup_type_name(cg, a->as.binary.left->as.ident.name);
                    if (tn && strcmp(tn, "string") == 0) { buf[pos++] = 's'; continue; }
                }
                if (a->as.binary.left->type == NODE_CALL &&
                    a->as.binary.left->as.call.callee->type == NODE_MEMBER) {
                    const char *m = a->as.binary.left->as.call.callee->as.member.member;
                    if (strcmp(m, "to") == 0) { buf[pos++] = 's'; continue; }
                }
            }
            AstNode *inner = (a->type == NODE_BINARY) ? a->as.binary.left : a->as.unary.operand;
            if (inner->type == NODE_INT_LIT || inner->type == NODE_IDENTIFIER) {
                if (inner->type == NODE_IDENTIFIER) {
                    LLVMTypeRef ty = var_lookup_type(cg, inner->as.ident.name);
                    if (ty && LLVMGetTypeKind(ty) == LLVMDoubleTypeKind) { buf[pos++] = 'f'; continue; }
                    if (ty && LLVMGetTypeKind(ty) == LLVMFloatTypeKind)  { buf[pos++] = 'F'; continue; }
                }
                buf[pos++] = '2';
            } else { buf[pos++] = '2'; }
        } else if (a->type == NODE_CALL) {
            if (a->as.call.callee->type == NODE_MEMBER) {
                const char *m = a->as.call.callee->as.member.member;
                if (strcmp(m, "to") == 0 && a->as.call.args.count == 1 &&
                    a->as.call.args.items[0]->type == NODE_GENERIC_INST) {
                    const char *tgt = a->as.call.args.items[0]->as.generic_inst.base_name;
                    if (strcmp(tgt, "string") == 0)  { buf[pos++] = 's'; continue; }
                    if (strcmp(tgt, "int") == 0 || strcmp(tgt, "i64") == 0) { buf[pos++] = '2'; continue; }
                    if (strcmp(tgt, "float") == 0 || strcmp(tgt, "f64") == 0) { buf[pos++] = 'f'; continue; }
                    if (strcmp(tgt, "bool") == 0)    { buf[pos++] = 'b'; continue; }
                    if (strcmp(tgt, "char") == 0)    { buf[pos++] = 'c'; continue; }
                    if (strcmp(tgt, "i8") == 0 || strcmp(tgt, "u8") == 0)   { buf[pos++] = '8'; continue; }
                    if (strcmp(tgt, "i16") == 0 || strcmp(tgt, "u16") == 0) { buf[pos++] = '6'; continue; }
                    if (strcmp(tgt, "i32") == 0 || strcmp(tgt, "u32") == 0) { buf[pos++] = '4'; continue; }
                    if (strcmp(tgt, "f32") == 0)     { buf[pos++] = 'F'; continue; }
                    if (strcmp(tgt, "f64") == 0)     { buf[pos++] = 'f'; continue; }
                    buf[pos++] = '2'; continue;
                }
            }
            if (a->as.call.callee->type == NODE_IDENTIFIER) {
                const char *cname = func_map_lookup(cg, a->as.call.callee->as.ident.name);
                if (cname) {
                    LLVMTypeRef fty = fn_type_lookup(cg, cname);
                    if (fty) {
                        LLVMTypeRef ret = LLVMGetReturnType(fty);
                        switch (LLVMGetTypeKind(ret)) {
                            case LLVMIntegerTypeKind:
                                if (LLVMGetIntTypeWidth(ret) == 1) { buf[pos++] = 'b'; continue; }
                                if (LLVMGetIntTypeWidth(ret) == 8) { buf[pos++] = '8'; continue; }
                                if (LLVMGetIntTypeWidth(ret) == 16) { buf[pos++] = '6'; continue; }
                                if (LLVMGetIntTypeWidth(ret) == 32) { buf[pos++] = '4'; continue; }
                                if (LLVMGetIntTypeWidth(ret) == 128) { buf[pos++] = 'Q'; continue; }
                                buf[pos++] = '2'; continue;
                            case LLVMDoubleTypeKind:  buf[pos++] = 'f'; continue;
                            case LLVMFloatTypeKind:   buf[pos++] = 'F'; continue;
                            case LLVMPointerTypeKind: buf[pos++] = 's'; continue;
                            default: break;
                        }
                    }
                }
            }
            buf[pos++] = '2';
        } else if (a->type == NODE_FSTRING) {
            buf[pos++] = 's';
        } else { buf[pos++] = '2'; }
    }
    buf[pos] = '\0';
}
