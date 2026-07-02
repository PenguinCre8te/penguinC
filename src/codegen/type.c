#include "codegen_internal.h"

LLVMTypeRef resolve_type(CodegenCtx *cg, const char *name) {
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
