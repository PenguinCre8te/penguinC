#include "codegen_internal.h"

LLVMTypeRef resolve_type(CodegenCtx *cg, const char *name) {
    size_t len = strlen(name);
    if (len > 1 && name[len - 1] == '*') {
        char base[256];
        snprintf(base, sizeof(base), "%.*s", (int)(len - 1), name);
        return LLVMPointerType(resolve_type(cg, base), 0);
    }

    /* Generic type: Base<T1,T2,...> — resolve the base name */
    const char *lt = strchr(name, '<');
    if (lt) {
        size_t base_len = lt - name;
        char base_name[256];
        if (base_len < sizeof(base_name)) {
            memcpy(base_name, name, base_len);
            base_name[base_len] = '\0';

            /* array<T> → pointer to element type (null-terminated) */
            if (strcmp(base_name, "array") == 0) {
                /* Extract the element type from inside < > */
                const char *elem_start = lt + 1;
                const char *elem_end = strrchr(name, '>');
                if (elem_end && elem_end > elem_start) {
                    size_t elem_len = elem_end - elem_start;
                    char elem_type[256];
                    if (elem_len < sizeof(elem_type)) {
                        memcpy(elem_type, elem_start, elem_len);
                        elem_type[elem_len] = '\0';
                        return LLVMPointerType(resolve_type(cg, elem_type), 0);
                    }
                }
                /* fallback: array of unknown = i8** */
                return LLVMPointerType(LLVMPointerType(LLVMInt8TypeInContext(cg->ctx), 0), 0);
            }

            LLVMTypeRef st = struct_lookup(cg, base_name);
            if (st) return st;
            /* Not yet monomorphized — return i8* as fallback */
            return LLVMPointerType(LLVMInt8TypeInContext(cg->ctx), 0);
        }
    }

    if (strcmp(name, "int") == 0 || strcmp(name, "long") == 0) return LLVMInt64TypeInContext(cg->ctx);
    if (strcmp(name, "float") == 0)  return LLVMDoubleTypeInContext(cg->ctx);
    if (strcmp(name, "bool") == 0)   return LLVMInt1TypeInContext(cg->ctx);
    if (strcmp(name, "void") == 0)   return LLVMVoidTypeInContext(cg->ctx);
    if (strcmp(name, "char") == 0)   return LLVMInt8TypeInContext(cg->ctx);
    if (strcmp(name, "string") == 0) return LLVMPointerType(LLVMInt8TypeInContext(cg->ctx), 0);
    if (strchr(name, '.')) {
        return LLVMInt64TypeInContext(cg->ctx);
    }
    LLVMTypeRef st = struct_lookup(cg, name);
    if (st) return st;
    return LLVMPointerType(LLVMInt8TypeInContext(cg->ctx), 0);
}

int is_float_type(LLVMTypeRef ty) {
    return LLVMGetTypeKind(ty) == LLVMDoubleTypeKind;
}
