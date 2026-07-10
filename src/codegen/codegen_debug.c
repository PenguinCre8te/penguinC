#include "codegen_internal.h"

/* DWARF type encoding constants (DW_ATE_*) */
#define DW_ATE_ADDRESS  0x01
#define DW_ATE_BOOLEAN  0x02
#define DW_ATE_FLOAT    0x04
#define DW_ATE_SIGNED   0x05

static LLVMMetadataRef cached_i64_type;
static LLVMMetadataRef cached_i8ptr_type;
static LLVMMetadataRef cached_double_type;
static LLVMMetadataRef cached_i1_type;
static LLVMMetadataRef cached_void_type;

static void ensure_base_types(CodegenCtx *cg) {
    if (cached_i64_type) return;
    cached_i1_type = LLVMDIBuilderCreateBasicType(
        cg->dibuilder, "bool", 4, 1, DW_ATE_BOOLEAN, LLVMDIFlagZero);
    cached_i64_type = LLVMDIBuilderCreateBasicType(
        cg->dibuilder, "long", 4, 64, DW_ATE_SIGNED, LLVMDIFlagZero);
    cached_i8ptr_type = LLVMDIBuilderCreatePointerType(
        cg->dibuilder, NULL, 64, 64, 0, "string", 6);
    cached_double_type = LLVMDIBuilderCreateBasicType(
        cg->dibuilder, "float", 5, 64, DW_ATE_FLOAT, LLVMDIFlagZero);
    cached_void_type = LLVMDIBuilderCreateBasicType(
        cg->dibuilder, "void", 4, 0, DW_ATE_ADDRESS, LLVMDIFlagZero);
}

LLVMMetadataRef debug_type(CodegenCtx *cg, LLVMTypeRef ty) {
    if (!cg->debug_enabled || !ty) return NULL;
    ensure_base_types(cg);

    switch (LLVMGetTypeKind(ty)) {
        case LLVMIntegerTypeKind:
            return LLVMGetIntTypeWidth(ty) == 1 ? cached_i1_type : cached_i64_type;
        case LLVMDoubleTypeKind:
            return cached_double_type;
        case LLVMVoidTypeKind:
            return cached_void_type;
        case LLVMPointerTypeKind:
            return cached_i8ptr_type;
        default:
            return cached_i64_type;
    }
}

void debug_init(CodegenCtx *cg, const char *filename) {
    if (!cg->debug_enabled) return;

    cg->dibuilder = LLVMCreateDIBuilder(cg->module);

    /* Split filename into directory and basename */
    char dir[512] = ".";
    char file[512];
    const char *last_slash = strrchr(filename, '/');
    if (!last_slash) last_slash = strrchr(filename, '\\');
    if (last_slash) {
        size_t dlen = last_slash - filename;
        if (dlen < sizeof(dir)) {
            memcpy(dir, filename, dlen);
            dir[dlen] = '\0';
        }
        strncpy(file, last_slash + 1, sizeof(file) - 1);
        file[sizeof(file) - 1] = '\0';
    } else {
        strncpy(file, filename, sizeof(file) - 1);
        file[sizeof(file) - 1] = '\0';
    }

    cg->difile = LLVMDIBuilderCreateFile(cg->dibuilder,
        file, strlen(file), dir, strlen(dir));

    cg->dicu = LLVMDIBuilderCreateCompileUnit(cg->dibuilder,
        LLVMDWARFSourceLanguageC,
        cg->difile,
        "penguinc", 8,   /* Producer */
        0,               /* isOptimized */
        "", 0,           /* Flags */
        0,               /* RuntimeVer */
        "", 0,           /* SplitName */
        LLVMDWARFEmissionFull,
        0,               /* DWOId */
        0,               /* SplitDebugInlining */
        0,               /* DebugInfoForProfiling */
        "", 0,           /* SysRoot */
        "", 0);          /* SDK */

    cg->discope = cg->dicu;
}

void debug_finalize(CodegenCtx *cg) {
    if (!cg->debug_enabled) return;
    LLVMDIBuilderFinalize(cg->dibuilder);
}

void debug_set_location(CodegenCtx *cg, SrcLoc loc) {
    if (!cg->debug_enabled || !cg->discope) return;
    if (loc.line <= 0) return;

    LLVMMetadataRef debug_loc = LLVMDIBuilderCreateDebugLocation(
        cg->ctx, (unsigned)loc.line, (unsigned)loc.col,
        cg->discope, NULL);
    LLVMSetCurrentDebugLocation(cg->builder, LLVMMetadataAsValue(cg->ctx, debug_loc));
}

void debug_create_function(CodegenCtx *cg, const char *name,
                           const char *linkage_name, SrcLoc loc,
                           LLVMTypeRef fn_type, LLVMValueRef fn) {
    if (!cg->debug_enabled) return;
    ensure_base_types(cg);

    /* Build subroutine type: return type + param types */
    unsigned param_count = LLVMCountParamTypes(fn_type);
    LLVMMetadataRef *param_tys = malloc((param_count + 1) * sizeof(LLVMMetadataRef));

    /* Index 0 is return type */
    LLVMTypeRef ret_ty = LLVMGetReturnType(fn_type);
    param_tys[0] = debug_type(cg, ret_ty);
    if (!param_tys[0]) param_tys[0] = cached_void_type;

    /* LLVMGetParamTypes fills an array of LLVMTypeRef */
    LLVMTypeRef *llvm_params = param_count > 0
        ? malloc(param_count * sizeof(LLVMTypeRef)) : NULL;
    if (param_count > 0)
        LLVMGetParamTypes(fn_type, llvm_params);

    for (unsigned i = 0; i < param_count; i++) {
        param_tys[i + 1] = debug_type(cg, llvm_params[i]);
        if (!param_tys[i + 1]) param_tys[i + 1] = cached_i64_type;
    }
    if (llvm_params) free(llvm_params);

    LLVMMetadataRef subr_type = LLVMDIBuilderCreateSubroutineType(
        cg->dibuilder, cg->difile, param_tys, param_count + 1, LLVMDIFlagZero);

    if (param_tys) free(param_tys);

    LLVMMetadataRef sp = LLVMDIBuilderCreateFunction(
        cg->dibuilder,
        cg->dicu,                   /* Scope */
        name, strlen(name),         /* Name */
        linkage_name, strlen(linkage_name), /* LinkageName */
        cg->difile,                 /* File */
        (unsigned)loc.line,         /* LineNo */
        subr_type,                  /* Type */
        0,                          /* IsLocalToUnit */
        1,                          /* IsDefinition */
        (unsigned)loc.line,         /* ScopeLine */
        LLVMDIFlagZero,             /* Flags */
        0);                         /* IsOptimized */

    LLVMSetSubprogram(fn, sp);
    cg->discope = sp;
}

void debug_create_param(CodegenCtx *cg, const char *name, unsigned argno,
                        SrcLoc loc, LLVMTypeRef llvm_ty, LLVMValueRef alloca) {
    if (!cg->debug_enabled || !cg->discope) return;

    LLVMMetadataRef di_ty = debug_type(cg, llvm_ty);
    if (!di_ty) di_ty = cached_i64_type;

    LLVMMetadataRef var = LLVMDIBuilderCreateParameterVariable(
        cg->dibuilder,
        cg->discope,           /* Scope */
        name, strlen(name),    /* Name */
        argno,                 /* ArgNo (1-based) */
        cg->difile,            /* File */
        (unsigned)loc.line,    /* LineNo */
        di_ty,                 /* Type */
        0,                     /* AlwaysPreserve */
        LLVMDIFlagZero);       /* Flags */

    LLVMMetadataRef expr = LLVMDIBuilderCreateExpression(cg->dibuilder, NULL, 0);
    LLVMMetadataRef debug_loc = LLVMDIBuilderCreateDebugLocation(
        cg->ctx, (unsigned)loc.line, (unsigned)loc.col,
        cg->discope, NULL);

    LLVMDIBuilderInsertDeclareRecordAtEnd(
        cg->dibuilder, alloca, var, expr, debug_loc,
        LLVMGetInsertBlock(cg->builder));
}

void debug_create_variable(CodegenCtx *cg, const char *name, SrcLoc loc,
                           LLVMTypeRef llvm_ty, LLVMValueRef alloca) {
    if (!cg->debug_enabled || !cg->discope) return;
    if (loc.line <= 0) return;

    LLVMMetadataRef di_ty = debug_type(cg, llvm_ty);
    if (!di_ty) di_ty = cached_i64_type;

    LLVMMetadataRef var = LLVMDIBuilderCreateAutoVariable(
        cg->dibuilder,
        cg->discope,           /* Scope */
        name, strlen(name),    /* Name */
        cg->difile,            /* File */
        (unsigned)loc.line,    /* LineNo */
        di_ty,                 /* Type */
        0,                     /* AlwaysPreserve */
        LLVMDIFlagZero,        /* Flags */
        0);                    /* AlignInBits */

    LLVMMetadataRef expr = LLVMDIBuilderCreateExpression(cg->dibuilder, NULL, 0);
    LLVMMetadataRef debug_loc = LLVMDIBuilderCreateDebugLocation(
        cg->ctx, (unsigned)loc.line, (unsigned)loc.col,
        cg->discope, NULL);

    LLVMDIBuilderInsertDeclareRecordAtEnd(
        cg->dibuilder, alloca, var, expr, debug_loc,
        LLVMGetInsertBlock(cg->builder));
}
