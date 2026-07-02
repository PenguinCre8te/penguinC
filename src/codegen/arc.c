#include "codegen_internal.h"

LLVMTypeRef arc_i8ptr;
LLVMTypeRef malloc_fn_type;
LLVMTypeRef free_fn_type;

void init_arc_types(CodegenCtx *cg) {
    arc_i8ptr = LLVMPointerType(LLVMInt8TypeInContext(cg->ctx), 0);
}

LLVMValueRef get_or_declare_arc_fn(CodegenCtx *cg, const char *name, LLVMTypeRef fn_ty) {
    LLVMValueRef fn = LLVMGetNamedFunction(cg->module, name);
    if (!fn) {
        fn = LLVMAddFunction(cg->module, name, fn_ty);
        fn_type_push(cg, name, fn_ty);
    }
    return fn;
}

LLVMValueRef call_arc_alloc(CodegenCtx *cg, LLVMValueRef size) {
    LLVMTypeRef fn_ty = LLVMFunctionType(arc_i8ptr, (LLVMTypeRef[]){LLVMInt64TypeInContext(cg->ctx)}, 1, 0);
    LLVMValueRef fn = get_or_declare_arc_fn(cg, "arc_alloc", fn_ty);
    return LLVMBuildCall2(cg->builder, fn_ty, fn, &size, 1, "arc.alloc");
}

LLVMValueRef call_arc_retain(CodegenCtx *cg, LLVMValueRef ptr) {
    LLVMTypeRef fn_ty = LLVMFunctionType(arc_i8ptr, (LLVMTypeRef[]){arc_i8ptr}, 1, 0);
    LLVMValueRef fn = get_or_declare_arc_fn(cg, "arc_retain", fn_ty);
    return LLVMBuildCall2(cg->builder, fn_ty, fn, &ptr, 1, "arc.retain");
}

void call_arc_release(CodegenCtx *cg, LLVMValueRef ptr) {
    LLVMTypeRef fn_ty = LLVMFunctionType(LLVMVoidTypeInContext(cg->ctx), (LLVMTypeRef[]){arc_i8ptr}, 1, 0);
    LLVMValueRef fn = get_or_declare_arc_fn(cg, "arc_release", fn_ty);
    LLVMBuildCall2(cg->builder, fn_ty, fn, &ptr, 1, "");
}

int is_arc_type(CodegenCtx *cg, const char *type_name) {
    if (!type_name) return 0;
    if (strcmp(type_name, "string") == 0) return 1;
    if (struct_lookup(cg, type_name)) return 1;
    size_t len = strlen(type_name);
    if (len > 1 && type_name[len - 1] == '*') return 1;
    return 0;
}

int is_arc_type_for_var(CodegenCtx *cg, size_t var_index) {
    const char *tn = cg->vars[var_index].type_name;
    if (is_arc_type(cg, tn)) return 1;
    if (cg->vars[var_index].elem_ty) return 1;
    return 0;
}

void codegen_arc_release_var(CodegenCtx *cg, size_t var_index) {
    if (!is_arc_type_for_var(cg, var_index)) return;
    if (LLVMGetTypeKind(cg->vars[var_index].ty) != LLVMPointerTypeKind) return;
    LLVMValueRef val = LLVMBuildLoad2(cg->builder,
        cg->vars[var_index].ty, cg->vars[var_index].val, "arc.cleanup");
    call_arc_release(cg, val);
}

LLVMValueRef get_malloc(CodegenCtx *cg) {
    LLVMValueRef fn = LLVMGetNamedFunction(cg->module, "malloc");
    if (!fn) {
        malloc_fn_type = LLVMFunctionType(
            LLVMPointerType(LLVMInt8TypeInContext(cg->ctx), 0),
            (LLVMTypeRef[]){ LLVMInt64TypeInContext(cg->ctx) }, 1, 0);
        fn = LLVMAddFunction(cg->module, "malloc", malloc_fn_type);
        fn_type_push(cg, "malloc", malloc_fn_type);
    }
    return fn;
}

LLVMValueRef get_free(CodegenCtx *cg) {
    LLVMValueRef fn = LLVMGetNamedFunction(cg->module, "free");
    if (!fn) {
        free_fn_type = LLVMFunctionType(
            LLVMVoidTypeInContext(cg->ctx),
            (LLVMTypeRef[]){ LLVMPointerType(LLVMInt8TypeInContext(cg->ctx), 0) }, 1, 0);
        fn = LLVMAddFunction(cg->module, "free", free_fn_type);
        fn_type_push(cg, "free", free_fn_type);
    }
    return fn;
}

LLVMValueRef call_malloc(CodegenCtx *cg, LLVMValueRef size) {
    LLVMValueRef fn = get_malloc(cg);
    return LLVMBuildCall2(cg->builder, malloc_fn_type, fn, (LLVMValueRef[]){ size }, 1, "alloc");
}

LLVMValueRef call_free(CodegenCtx *cg, LLVMValueRef ptr) {
    LLVMValueRef fn = get_free(cg);
    LLVMValueRef casted = LLVMBuildBitCast(cg->builder, ptr,
        LLVMPointerType(LLVMInt8TypeInContext(cg->ctx), 0), "free.cast");
    return LLVMBuildCall2(cg->builder, free_fn_type, fn, (LLVMValueRef[]){ casted }, 1, "");
}

void codegen_arc_optimize(LLVMModuleRef module) {
    LLVMValueRef func = LLVMGetFirstFunction(module);
    while (func) {
        if (LLVMCountBasicBlocks(func) > 0) {
            LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(func);
            while (bb) {
                LLVMValueRef inst = LLVMGetFirstInstruction(bb);
                while (inst) {
                    LLVMValueRef next = LLVMGetNextInstruction(inst);
                    if (LLVMGetInstructionOpcode(inst) == LLVMCall) {
                        LLVMValueRef called_fn = LLVMGetCalledValue(inst);
                        if (LLVMGetValueKind(called_fn) == LLVMFunctionValueKind) {
                            const char *fn_name = LLVMGetValueName(called_fn);
                            if (fn_name && strcmp(fn_name, "arc_retain") == 0 && next) {
                                LLVMValueRef next_inst = next;
                                if (LLVMGetInstructionOpcode(next_inst) == LLVMCall) {
                                    LLVMValueRef next_fn = LLVMGetCalledValue(next_inst);
                                    if (LLVMGetValueKind(next_fn) == LLVMFunctionValueKind) {
                                        const char *next_name = LLVMGetValueName(next_fn);
                                        if (next_name && strcmp(next_name, "arc_release") == 0) {
                                            LLVMValueRef retain_arg = LLVMGetOperand(inst, 0);
                                            LLVMValueRef release_arg = LLVMGetOperand(next_inst, 0);
                                            if (retain_arg == release_arg) {
                                                LLVMInstructionEraseFromParent(next_inst);
                                                LLVMInstructionEraseFromParent(inst);
                                                inst = next;
                                                continue;
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                    inst = next;
                }
                bb = LLVMGetNextBasicBlock(bb);
            }
        }
        func = LLVMGetNextFunction(func);
    }
}
