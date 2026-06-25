#ifndef PENGUINC_CODEGEN_H
#define PENGUINC_CODEGEN_H

#include "ast.h"

/* Generate LLVM IR and emit an object file for the given AST.
   Returns 0 on success, 1 on failure. */
int codegen(AstNode *program, const char *output_file);

#endif /* PENGUINC_CODEGEN_H */
