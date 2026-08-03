#ifndef PENGUINC_TYPECHECK_H
#define PENGUINC_TYPECHECK_H

#include "ast.h"

typedef enum {
    TC_INT,
    TC_FLOAT,
    TC_BOOL,
    TC_VOID,
    TC_STRING,
    TC_CHAR,
    TC_I8, TC_I16, TC_I32, TC_I64, TC_I128,
    TC_U8, TC_U16, TC_U32, TC_U64, TC_U128,
    TC_ISIZE, TC_USIZE,
    TC_F32, TC_F64,
    TC_STRUCT,
    TC_CLASS,
    TC_ENUM,
    TC_POINTER,
    TC_FUNCTION,
    TC_TUPLE,
    TC_UNKNOWN,
    TC_TYPE_ERROR,
} TCTypeKind;

typedef struct TCType {
    TCTypeKind kind;
    char *name;
    struct TCType *pointee;
    struct {
        TCTypeKind *param_kinds;
        TCTypeKind ret_kind;
        size_t param_count;
    } fn;
    struct {
        struct TCType *elems;
        size_t count;
    } tuple;
} TCType;

typedef struct {
    char *name;
    TCType type;
    int is_mut;
    int is_shared;
    int line;
    int col;
} TCVar;

typedef struct TCScope {
    TCVar *vars;
    size_t count;
    size_t capacity;
    struct TCScope *parent;
} TCScope;

typedef struct {
    char *name;
    TCType ret_type;
    TCType *param_types;
    size_t param_count;
    int line;
    int col;
} TCFuncSig;

typedef struct {
    char *name;
    char *parent;
    char **field_names;
    TCType *field_types;
    size_t field_count;
    int line;
    int col;
} TCStructInfo;

typedef struct {
    char *name;
    char *parent;
    char **field_names;
    TCType *field_types;
    size_t field_count;
    int line;
    int col;
} TCClassInfo;

typedef struct {
    char *name;
    long value;
    int line;
    int col;
} TCEnumConst;

typedef struct {
    TCScope *current_scope;
    TCStructInfo *structs;
    size_t struct_count;
    size_t struct_cap;
    TCClassInfo *classes;
    size_t class_count;
    size_t class_cap;
    TCFuncSig *funcs;
    size_t func_count;
    size_t func_cap;
    TCEnumConst *enums;
    size_t enum_count;
    size_t enum_cap;
    char **typedefs;
    char **typedef_origs;
    size_t typedef_count;
    size_t typedef_cap;
    int error_count;
    const char *filename;
    const char *src;
    AstNode *current_func_node;
    int using_depth;
} TCContext;

int typecheck(AstNode *program, const char *filename, const char *src);

#endif /* PENGUINC_TYPECHECK_H */
