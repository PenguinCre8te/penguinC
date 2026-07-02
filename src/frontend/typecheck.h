#ifndef PENGUINC_TYPECHECK_H
#define PENGUINC_TYPECHECK_H

#include "ast.h"

typedef enum {
    TC_INT,
    TC_FLOAT,
    TC_BOOL,
    TC_VOID,
    TC_STRING,
    TC_STRUCT,
    TC_CLASS,
    TC_ENUM,
    TC_POINTER,
    TC_FUNCTION,
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
} TCType;

typedef struct {
    char *name;
    TCType type;
    int is_mut;
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
} TCContext;

int typecheck(AstNode *program, const char *filename, const char *src);

#endif /* PENGUINC_TYPECHECK_H */
