#include "codegen_internal.h"

static void register_class_func_maps(CodegenCtx *cg, AstNode *fm, const char *mod_name) {
    class_push(cg, fm->as.class_decl.name, fm->as.class_decl.parent);

    for (size_t mj = 0; mj < fm->as.class_decl.methods.count; mj++) {
        AstNode *meth = fm->as.class_decl.methods.items[mj];
        if (!meth || meth->type != NODE_FUNC_MAP) continue;

        char qualified[512];
        snprintf(qualified, sizeof(qualified), "%s.%s.%s", mod_name,
                 fm->as.class_decl.name, meth->as.func_map.pc_name);
        func_map_push(cg, qualified, meth->as.func_map.c_name);

        char simple[512];
        const char *mn = meth->as.func_map.pc_name;
        const char *start = mn;
        if (strncmp(start, "_pC", 3) == 0) start += 3;
        size_t nlen;
        if (meth->as.func_map.param_count == 0) {
            nlen = strlen(start);
        } else {
            /* Find the end of the base name: stop at a type-suffix char
               ONLY if the next char is NOT an identifier char (letter/digit/underscore).
               This prevents truncating names like "writeStr" at the 'i' in "write". */
            const char *name_end = start;
            while (*name_end) {
                char c = *name_end;
                int is_suffix = (c == 'i' || c == 'f' || c == 'b' || c == 's' || c == 'v' || c == 'p');
                if (is_suffix) {
                    /* Check if next char is identifier char — if so, this is part of the name */
                    char next = name_end[1];
                    if (next == '\0' || (!(next >= 'a' && next <= 'z') &&
                                         !(next >= 'A' && next <= 'Z') &&
                                         !(next >= '0' && next <= '9') &&
                                         next != '_'))
                        break; /* real suffix */
                }
                name_end++;
            }
            nlen = name_end - start;
        }
        if (nlen > 0 && nlen < 256) {
            char name_buf[256];
            memcpy(name_buf, start, nlen);
            name_buf[nlen] = '\0';
            snprintf(simple, sizeof(simple), "%s.%s.%s", mod_name,
                     fm->as.class_decl.name, name_buf);
            func_map_push(cg, simple, meth->as.func_map.c_name);
        }

        /* Also register using the original (unmangled) method name */
        if (meth->as.func_map.orig_name) {
            char orig_key[512];
            snprintf(orig_key, sizeof(orig_key), "%s.%s.%s", mod_name,
                     fm->as.class_decl.name, meth->as.func_map.orig_name);
            func_map_push(cg, orig_key, meth->as.func_map.c_name);
        }

        if (meth->as.func_map.ret_type) {
            LLVMTypeRef ret_ty = resolve_type(cg, meth->as.func_map.ret_type);
            size_t pc = meth->as.func_map.param_count;
            LLVMTypeRef *arg_tys = pc > 0 ? malloc(pc * sizeof(LLVMTypeRef)) : NULL;
            for (size_t pi = 0; pi < pc; pi++)
                arg_tys[pi] = resolve_type(cg, meth->as.func_map.param_types[pi]);
            LLVMTypeRef fn_ty = LLVMFunctionType(ret_ty, arg_tys, (unsigned)pc, 0);
            fn_type_push(cg, meth->as.func_map.c_name, fn_ty);
            if (arg_tys) free(arg_tys);
        }
    }
}

static void register_func_map(CodegenCtx *cg, AstNode *fm, const char *mod_name) {
    char qualified[512];
    snprintf(qualified, sizeof(qualified), "%s.%s", mod_name, fm->as.func_map.pc_name);
    func_map_push(cg, qualified, fm->as.func_map.c_name);

    /* Register simple-name lookup: module.funcname */
    if (fm->as.func_map.orig_name) {
        char simple[512];
        snprintf(simple, sizeof(simple), "%s.%s", mod_name, fm->as.func_map.orig_name);
        func_map_push(cg, simple, fm->as.func_map.c_name);
    }

    if (fm->as.func_map.ret_type) {
        LLVMTypeRef ret_ty = resolve_type(cg, fm->as.func_map.ret_type);
        size_t pc = fm->as.func_map.param_count;
        LLVMTypeRef *arg_tys = pc > 0 ? malloc(pc * sizeof(LLVMTypeRef)) : NULL;
        for (size_t pi = 0; pi < pc; pi++)
            arg_tys[pi] = resolve_type(cg, fm->as.func_map.param_types[pi]);
        LLVMTypeRef fn_ty = LLVMFunctionType(ret_ty, arg_tys, (unsigned)pc, 0);
        fn_type_push(cg, fm->as.func_map.c_name, fn_ty);
        if (arg_tys) free(arg_tys);
    }
}

static void collect_link_paths(CodegenCtx *cg, AstNode *decl) {
    for (size_t k = 0; k < decl->as.import.links.count; k++) {
        AstNode *lnk = decl->as.import.links.items[k];
        if (lnk && lnk->type == NODE_LINK) {
            int dup = 0;
            for (size_t m = 0; m < cg->import_count; m++) {
                if (strcmp(cg->imports[m], lnk->as.link.path) == 0) { dup = 1; break; }
            }
            if (!dup) {
                if (cg->import_count >= cg->import_cap) {
                    cg->import_cap = cg->import_cap ? cg->import_cap * 2 : 8;
                    cg->imports = realloc(cg->imports, cg->import_cap * sizeof(char *));
                }
                cg->imports[cg->import_count++] = strdup(lnk->as.link.path);
            }
        }
    }
}

void codegen_import(CodegenCtx *cg, AstNode *decl) {
    const char *mod = decl->as.import.module;
    const char *last = strrchr(mod, '.');
    last = last ? last + 1 : mod;

    for (size_t k = 0; k < decl->as.import.func_maps.count; k++) {
        AstNode *fm = decl->as.import.func_maps.items[k];
        if (fm && fm->type == NODE_CLASS_DECL) {
            register_class_func_maps(cg, fm, last);
            continue;
        }
        if (fm && fm->type == NODE_FUNC_MAP) {
            register_func_map(cg, fm, last);
        }
    }

    collect_link_paths(cg, decl);
}
