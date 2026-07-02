#ifndef PENGUINC_CODEGEN_H
#define PENGUINC_CODEGEN_H

#include "ast.h"

typedef enum {
    OPT_NONE = 0,
    OPT_BASIC,
    OPT_DEFAULT,
    OPT_AGGRESSIVE
} OptLevel;

typedef struct {
    char **paths;
    size_t count;
} LinkPaths;

int codegen(AstNode *program, const char *output_file, OptLevel opt,
            LinkPaths *out_links);

#endif /* PENGUINC_CODEGEN_H */
