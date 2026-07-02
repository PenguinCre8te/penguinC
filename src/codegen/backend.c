#include "codegen_internal.h"

static LLVMTargetMachineRef create_target_machine(LLVMModuleRef module, OptLevel opt) {
    char *triple = LLVMGetDefaultTargetTriple();
    LLVMTargetRef target;
    char *err = NULL;

    if (LLVMGetTargetFromTriple(triple, &target, &err) != 0) {
        fprintf(stderr, "penguinc: failed to get target: %s\n", err);
        LLVMDisposeMessage(err);
        LLVMDisposeMessage(triple);
        return NULL;
    }

    LLVMCodeGenOptLevel codegen_opt;
    switch (opt) {
        case OPT_NONE:       codegen_opt = LLVMCodeGenLevelNone; break;
        case OPT_BASIC:      codegen_opt = LLVMCodeGenLevelLess; break;
        case OPT_DEFAULT:    codegen_opt = LLVMCodeGenLevelDefault; break;
        case OPT_AGGRESSIVE: codegen_opt = LLVMCodeGenLevelAggressive; break;
    }

    LLVMTargetMachineRef tm = LLVMCreateTargetMachine(
        target, triple, "generic", "", codegen_opt,
        LLVMRelocDefault, LLVMCodeModelDefault);

    LLVMTargetDataRef td = LLVMCreateTargetDataLayout(tm);
    LLVMSetDataLayout(module, LLVMCopyStringRepOfTargetData(td));
    LLVMDisposeTargetData(td);

    free(triple);
    return tm;
}

static void emit_object_file(LLVMTargetMachineRef tm, LLVMModuleRef module, const char *output_file) {
    char *obj_file = malloc(strlen(output_file) + 5);
    strcpy(obj_file, output_file);
    char *ext = strrchr(obj_file, '.');
    if (ext && strcmp(ext, ".o") == 0)
        strcpy(ext, ".o");
    else
        strcat(obj_file, ".o");

    char *emit_err = NULL;
    if (LLVMTargetMachineEmitToFile(tm, module, obj_file,
                                     LLVMObjectFile, &emit_err) != 0) {
        fprintf(stderr, "penguinc: failed to emit object file: %s\n", emit_err);
        LLVMDisposeMessage(emit_err);
    } else {
        fprintf(stderr, "penguinc: wrote %s\n", obj_file);
    }

    free(obj_file);
}

int codegen(AstNode *program, const char *output_file, OptLevel opt,
            LinkPaths *out_links) {
    LLVMInitializeNativeTarget();
    LLVMInitializeNativeAsmParser();
    LLVMInitializeNativeAsmPrinter();

    LLVMContextRef ctx = LLVMContextCreate();
    LLVMModuleRef module = LLVMModuleCreateWithNameInContext("penguinc", ctx);
    LLVMBuilderRef builder = LLVMCreateBuilderInContext(ctx);

    CodegenCtx cg = {
        .ctx = ctx,
        .module = module,
        .builder = builder,
        .cur_fn = NULL,
        .vars = NULL, .var_count = 0, .var_cap = 0,
        .fn_types = NULL, .fn_type_count = 0, .fn_type_cap = 0,
        .structs = NULL, .struct_count = 0, .struct_cap = 0,
        .imports = NULL, .import_count = 0, .import_cap = 0,
        .break_target = NULL, .continue_target = NULL,
        .labels = NULL, .label_count = 0, .label_cap = 0,
        .loop_depth = 0,
        .scope_base = 0,
    };

    init_arc_types(&cg);

    codegen_program(&cg, program);

    codegen_arc_optimize(module);

    char *verify_err = NULL;
    LLVMVerifyModule(module, LLVMReturnStatusAction, &verify_err);
    if (verify_err && verify_err[0]) {
        fprintf(stderr, "penguinc: module verification failed:\n%s\n", verify_err);
        LLVMDisposeMessage(verify_err);
    } else if (verify_err) {
        LLVMDisposeMessage(verify_err);
    }

    LLVMTargetMachineRef tm = create_target_machine(module, opt);

    if (tm && opt > OPT_NONE) {
        const char *pipeline;
        switch (opt) {
            case OPT_BASIC:      pipeline = "default<O1>"; break;
            case OPT_DEFAULT:    pipeline = "default<O2>"; break;
            case OPT_AGGRESSIVE: pipeline = "default<O3>"; break;
            default:             pipeline = "default<O0>"; break;
        }
        LLVMPassBuilderOptionsRef opts = LLVMCreatePassBuilderOptions();
        LLVMErrorRef err = LLVMRunPasses(module, pipeline, tm, opts);
        if (err) {
            char *msg = LLVMGetErrorMessage(err);
            fprintf(stderr, "penguinc: optimization failed: %s\n", msg);
            LLVMDisposeErrorMessage(msg);
        }
        LLVMDisposePassBuilderOptions(opts);
    }

    if (tm) {
        emit_object_file(tm, module, output_file);
        LLVMDisposeTargetMachine(tm);
    }

    if (out_links) {
        out_links->count = cg.import_count;
        out_links->paths = cg.import_count > 0
            ? malloc(cg.import_count * sizeof(char *))
            : NULL;
        for (size_t i = 0; i < cg.import_count; i++)
            out_links->paths[i] = cg.imports[i];
    } else {
        for (size_t i = 0; i < cg.import_count; i++)
            free(cg.imports[i]);
    }
    free(cg.imports);

    LLVMDisposeBuilder(builder);
    LLVMDisposeModule(module);
    LLVMContextDispose(ctx);

    for (size_t i = 0; i < cg.var_count; i++) free(cg.vars[i].name);
    free(cg.vars);
    for (size_t i = 0; i < cg.fn_type_count; i++) free(cg.fn_types[i].name);
    free(cg.fn_types);
    for (size_t i = 0; i < cg.struct_count; i++) free(cg.structs[i].name);
    free(cg.structs);
    for (size_t i = 0; i < cg.label_count; i++) free(cg.labels[i].name);
    free(cg.labels);

    return 0;
}
