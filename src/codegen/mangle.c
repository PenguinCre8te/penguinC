#include "codegen_internal.h"

void mangle_name(char *buf, size_t buflen, const char *name,
                 NodeList *params, size_t param_count) {
    size_t pos = 0;
    pos += snprintf(buf + pos, buflen - pos, "_pC%s", name);
    for (size_t i = 0; i < param_count && pos < buflen - 1; i++) {
        const char *t = params->items[i]->as.param.type;
        if (strchr(t, '*')) {
            buf[pos++] = 'p';
        } else if (strcmp(t, "int") == 0 || strcmp(t, "long") == 0) buf[pos++] = 'i';
        else if (strcmp(t, "float") == 0) buf[pos++] = 'f';
        else if (strcmp(t, "bool") == 0)  buf[pos++] = 'b';
        else if (strcmp(t, "string") == 0) buf[pos++] = 's';
        else if (strcmp(t, "void") == 0)  buf[pos++] = 'v';
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
        if (a->type == NODE_INT_LIT)       { buf[pos++] = 'i'; }
        else if (a->type == NODE_FLOAT_LIT) { buf[pos++] = 'f'; }
        else if (a->type == NODE_STRING_LIT) { buf[pos++] = 's'; }
        else if (a->type == NODE_IDENTIFIER) {
            LLVMTypeRef ty = var_lookup_type(cg, a->as.ident.name);
            const char *tn = var_lookup_type_name(cg, a->as.ident.name);
            const char *resolved = tn ? typedef_resolve(cg, tn) : tn;
            if (resolved && strcmp(resolved, "string") == 0)              buf[pos++] = 's';
            else if (resolved && (strcmp(resolved, "int") == 0 || strcmp(resolved, "long") == 0))  buf[pos++] = 'i';
            else if (resolved && strcmp(resolved, "float") == 0)         buf[pos++] = 'f';
            else if (resolved && strcmp(resolved, "bool") == 0)          buf[pos++] = 'b';
            else if (tn && strcmp(tn, "string") == 0)                    buf[pos++] = 's';
            else if (ty == LLVMInt64TypeInContext(cg->ctx))              buf[pos++] = 'i';
            else if (ty == LLVMDoubleTypeInContext(cg->ctx))             buf[pos++] = 'f';
            else if (ty == LLVMInt1TypeInContext(cg->ctx))               buf[pos++] = 'b';
            else if (!ty) {
                buf[pos++] = 'i';
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
                    if (strcmp(m, "toS") == 0) { buf[pos++] = 's'; continue; }
                }
            }
            AstNode *inner = (a->type == NODE_BINARY) ? a->as.binary.left : a->as.unary.operand;
            if (inner->type == NODE_INT_LIT || inner->type == NODE_IDENTIFIER) {
                if (inner->type == NODE_IDENTIFIER) {
                    LLVMTypeRef ty = var_lookup_type(cg, inner->as.ident.name);
                    if (ty == LLVMDoubleTypeInContext(cg->ctx)) { buf[pos++] = 'f'; continue; }
                }
                buf[pos++] = 'i';
            } else { buf[pos++] = 'i'; }
        } else if (a->type == NODE_CALL) {
            if (a->as.call.callee->type == NODE_MEMBER) {
                const char *m = a->as.call.callee->as.member.member;
                if (strcmp(m, "toS") == 0) { buf[pos++] = 's'; continue;
                } else if (strcmp(m, "toI") == 0) { buf[pos++] = 'i'; continue;
                } else if (strcmp(m, "toF") == 0) { buf[pos++] = 'f'; continue;
                } else if (strcmp(m, "toB") == 0) { buf[pos++] = 'b'; continue; }
            }
            /* Determine return type of the called function for mangling */
            if (a->as.call.callee->type == NODE_IDENTIFIER) {
                const char *cname = func_map_lookup(cg, a->as.call.callee->as.ident.name);
                if (cname) {
                    LLVMTypeRef fty = fn_type_lookup(cg, cname);
                    if (fty) {
                        LLVMTypeRef ret = LLVMGetReturnType(fty);
                        switch (LLVMGetTypeKind(ret)) {
                            case LLVMIntegerTypeKind:
                                if (LLVMGetIntTypeWidth(ret) == 1) { buf[pos++] = 'b'; continue; }
                                buf[pos++] = 'i'; continue;
                            case LLVMDoubleTypeKind:  buf[pos++] = 'f'; continue;
                            case LLVMPointerTypeKind: buf[pos++] = 's'; continue;
                            default: break;
                        }
                    }
                }
            }
            buf[pos++] = 'i';
        } else if (a->type == NODE_FSTRING) {
            buf[pos++] = 's';
        } else { buf[pos++] = 'i'; }
    }
    buf[pos] = '\0';
}
