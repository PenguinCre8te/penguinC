#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "parser.h"
#include "error.h"
#include "codegen.h"

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        error_fatal(ERR_PARSER, "cannot open file '%s'", path);
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(len + 1);
    size_t n = fread(buf, 1, len, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

static void print_indent(int depth) {
    for (int i = 0; i < depth; i++) printf("  ");
}

__attribute__((unused)) static void print_ast(AstNode *node, int depth) {
    if (!node) return;
    switch (node->type) {
        case NODE_PROGRAM:
            printf("Program\n");
            for (size_t i = 0; i < node->as.program.decls.count; i++)
                print_ast(node->as.program.decls.items[i], depth + 1);
            break;
        case NODE_IMPORT:
            print_indent(depth);
            printf("Import(\"%s\"%s)\n", node->as.import.module,
                   node->as.import.is_header ? ", header" : "");
            break;
        case NODE_LINK:
            print_indent(depth);
            printf("Link(\"%s\")\n", node->as.link.path);
            break;
        case NODE_FUNC_MAP:
            print_indent(depth);
            printf("FuncMap(%s => %s)\n", node->as.func_map.pc_name,
                   node->as.func_map.c_name);
            break;
        case NODE_STRUCT_DECL:
            print_indent(depth);
            printf("Struct(%s)\n", node->as.struct_decl.name);
            for (size_t i = 0; i < node->as.struct_decl.fields.count; i++)
                print_ast(node->as.struct_decl.fields.items[i], depth + 1);
            break;
        case NODE_CLASS_DECL:
            print_indent(depth);
            printf("Class(%s%s)\n", node->as.class_decl.name,
                   node->as.class_decl.parent ? node->as.class_decl.parent : "");
            for (size_t i = 0; i < node->as.class_decl.fields.count; i++)
                print_ast(node->as.class_decl.fields.items[i], depth + 1);
            for (size_t i = 0; i < node->as.class_decl.methods.count; i++)
                print_ast(node->as.class_decl.methods.items[i], depth + 1);
            break;
        case NODE_FUNC_DECL:
            print_indent(depth);
            printf("Func(%s %s, params=%zu)\n", node->as.func_decl.ret_type,
                   node->as.func_decl.name, node->as.func_decl.params.count);
            for (size_t i = 0; i < node->as.func_decl.params.count; i++)
                print_ast(node->as.func_decl.params.items[i], depth + 1);
            if (node->as.func_decl.body)
                print_ast(node->as.func_decl.body, depth + 1);
            break;
        case NODE_PARAM:
            print_indent(depth);
            printf("Param(%s %s%s%s)\n", node->as.param.type, node->as.param.name,
                   node->as.param.is_borrow ? ", borrow" : "",
                   node->as.param.is_lock ? ", lock" : "");
            break;
        case NODE_BLOCK:
            print_indent(depth);
            printf("Block(%zu stmts)\n", node->as.block.stmts.count);
            for (size_t i = 0; i < node->as.block.stmts.count; i++)
                print_ast(node->as.block.stmts.items[i], depth + 1);
            break;
        case NODE_VAR_DECL:
            print_indent(depth);
            printf("VarDecl(%s %s%s)\n", node->as.var_decl.type,
                   node->as.var_decl.name,
                   node->as.var_decl.is_mut ? ", mut" : "");
            if (node->as.var_decl.init)
                print_ast(node->as.var_decl.init, depth + 1);
            break;
        case NODE_RETURN:
            print_indent(depth);
            printf("Return\n");
            if (node->as.ret.value)
                print_ast(node->as.ret.value, depth + 1);
            break;
        case NODE_IF:
            print_indent(depth);
            printf("If\n");
            print_ast(node->as.if_stmt.cond, depth + 1);
            print_ast(node->as.if_stmt.then_blk, depth + 1);
            if (node->as.if_stmt.else_blk)
                print_ast(node->as.if_stmt.else_blk, depth + 1);
            break;
        case NODE_SWITCH:
            print_indent(depth);
            printf("Switch\n");
            print_ast(node->as.switch_stmt.expr, depth + 1);
            for (size_t i = 0; i < node->as.switch_stmt.cases.count; i++)
                print_ast(node->as.switch_stmt.cases.items[i], depth + 1);
            break;
        case NODE_CASE:
            print_indent(depth);
            printf("Case%s\n", node->as.case_stmt.is_default ? "(default)" : "");
            if (node->as.case_stmt.value)
                print_ast(node->as.case_stmt.value, depth + 1);
            print_ast(node->as.case_stmt.body, depth + 1);
            break;
        case NODE_MATCH:
            print_indent(depth);
            printf("Match\n");
            print_ast(node->as.match_stmt.expr, depth + 1);
            for (size_t i = 0; i < node->as.match_stmt.cases.count; i++)
                print_ast(node->as.match_stmt.cases.items[i], depth + 1);
            break;
        case NODE_MATCH_CASE:
            print_indent(depth);
            printf("MatchCase%s\n", node->as.match_case.is_default ? "(default)" : "");
            if (node->as.match_case.pattern)
                print_ast(node->as.match_case.pattern, depth + 1);
            print_ast(node->as.match_case.body, depth + 1);
            break;
        case NODE_FOR:
            print_indent(depth);
            printf("For(%s)\n", node->as.for_stmt.var);
            print_ast(node->as.for_stmt.iter, depth + 1);
            print_ast(node->as.for_stmt.body, depth + 1);
            break;
        case NODE_WHILE:
            print_indent(depth);
            printf("While\n");
            print_ast(node->as.while_stmt.cond, depth + 1);
            print_ast(node->as.while_stmt.body, depth + 1);
            break;
        case NODE_USING:
            print_indent(depth);
            printf("Using\n");
            print_ast(node->as.using_stmt.resource, depth + 1);
            print_ast(node->as.using_stmt.body, depth + 1);
            break;
        case NODE_UNSAFE:
            print_indent(depth);
            printf("Unsafe\n");
            print_ast(node->as.unsafe_stmt.body, depth + 1);
            break;
        case NODE_ASSIGN:
            print_indent(depth);
            printf("Assign(%s)\n", node->as.assign.op);
            print_ast(node->as.assign.target, depth + 1);
            print_ast(node->as.assign.value, depth + 1);
            break;
        case NODE_BINARY:
            print_indent(depth);
            printf("Binary(%s)\n", node->as.binary.op);
            print_ast(node->as.binary.left, depth + 1);
            print_ast(node->as.binary.right, depth + 1);
            break;
        case NODE_UNARY:
            print_indent(depth);
            printf("Unary(%s, %s)\n", node->as.unary.op,
                   node->as.unary.is_prefix ? "prefix" : "postfix");
            print_ast(node->as.unary.operand, depth + 1);
            break;
        case NODE_CALL:
            print_indent(depth);
            printf("Call(%zu args)\n", node->as.call.args.count);
            print_ast(node->as.call.callee, depth + 1);
            for (size_t i = 0; i < node->as.call.args.count; i++)
                print_ast(node->as.call.args.items[i], depth + 1);
            break;
        case NODE_MEMBER:
            print_indent(depth);
            printf("Member(.%s)\n", node->as.member.member);
            print_ast(node->as.member.object, depth + 1);
            break;
        case NODE_IDENTIFIER:
            print_indent(depth);
            printf("Ident(%s)\n", node->as.ident.name);
            break;
        case NODE_INT_LIT:
            print_indent(depth);
            printf("IntLit(%ld)\n", node->as.int_lit.value);
            break;
        case NODE_FLOAT_LIT:
            print_indent(depth);
            printf("FloatLit(%f)\n", node->as.float_lit.value);
            break;
        case NODE_STRING_LIT:
            print_indent(depth);
            printf("StringLit(\"%s\")\n", node->as.string_lit.value);
            break;
        case NODE_RANGE:
            print_indent(depth);
            printf("Range\n");
            print_ast(node->as.range.start, depth + 1);
            print_ast(node->as.range.end, depth + 1);
            break;
        case NODE_NEW_EXPR:
            print_indent(depth);
            printf("New(%s)\n", node->as.new_expr.type_name);
            for (size_t i = 0; i < node->as.new_expr.args.count; i++)
                print_ast(node->as.new_expr.args.items[i], depth + 1);
            break;
        case NODE_SELF_REF:
            print_indent(depth);
            printf("Self\n");
            break;
        case NODE_SUPER_CALL:
            print_indent(depth);
            printf("Super.%s(%zu args)\n", node->as.super_call.method,
                   node->as.super_call.args.count);
            break;
        case NODEPointerType:
            print_indent(depth);
            printf("PtrType(%s*)\n", node->as.ptr_type.base_type);
            break;
        case NODE_DO_WHILE:
            print_indent(depth);
            printf("DoWhile\n");
            print_ast(node->as.do_while_stmt.body, depth + 1);
            print_ast(node->as.do_while_stmt.cond, depth + 1);
            break;
        case NODE_C_STYLE_FOR:
            print_indent(depth);
            printf("CStyleFor\n");
            if (node->as.c_style_for.init)
                print_ast(node->as.c_style_for.init, depth + 1);
            if (node->as.c_style_for.cond)
                print_ast(node->as.c_style_for.cond, depth + 1);
            if (node->as.c_style_for.update)
                print_ast(node->as.c_style_for.update, depth + 1);
            print_ast(node->as.c_style_for.body, depth + 1);
            break;
        case NODE_BREAK:
            print_indent(depth);
            printf("Break\n");
            break;
        case NODE_CONTINUE:
            print_indent(depth);
            printf("Continue\n");
            break;
        case NODE_GOTO:
            print_indent(depth);
            printf("Goto(%s)\n", node->as.goto_stmt.label);
            break;
        case NODE_TERNARY:
            print_indent(depth);
            printf("Ternary\n");
            print_ast(node->as.ternary.cond, depth + 1);
            print_ast(node->as.ternary.then_expr, depth + 1);
            print_ast(node->as.ternary.else_expr, depth + 1);
            break;
        case NODE_CAST:
            print_indent(depth);
            printf("Cast(%s)\n", node->as.cast_expr.type_name);
            print_ast(node->as.cast_expr.operand, depth + 1);
            break;
        case NODE_ENUM_DECL:
            print_indent(depth);
            printf("Enum(%s)\n", node->as.enum_decl.name);
            for (size_t i = 0; i < node->as.enum_decl.values.count; i++)
                print_ast(node->as.enum_decl.values.items[i], depth + 1);
            break;
        case NODE_UNION_DECL:
            print_indent(depth);
            printf("Union(%s)\n", node->as.union_decl.name);
            for (size_t i = 0; i < node->as.union_decl.fields.count; i++)
                print_ast(node->as.union_decl.fields.items[i], depth + 1);
            break;
        case NODE_TYPEDEF_DECL:
            print_indent(depth);
            printf("Typedef(%s -> %s)\n", node->as.typedef_decl.orig_type,
                   node->as.typedef_decl.new_name);
            break;
        default:
            print_indent(depth);
            printf("Node(%d)\n", node->type);
            break;
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: penguinc [-o output] <file.pc>\n");
        return 1;
    }

    const char *input_file = NULL;
    const char *output_file = NULL;
    int explicit_output = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output_file = argv[++i];
            explicit_output = 1;
        } else if (argv[i][0] != '-') {
            input_file = argv[i];
        }
    }

    if (!input_file) {
        fprintf(stderr, "penguinc: no input file specified\n");
        return 1;
    }

    if (!output_file) {
        /* Derive output name from input: foo.pc -> foo */
        static char default_out[512];
        strncpy(default_out, input_file, sizeof(default_out) - 1);
        default_out[sizeof(default_out) - 1] = '\0';
        char *dot = strrchr(default_out, '.');
        if (dot) *dot = '\0';
        output_file = default_out;
    }

    char *src = read_file(input_file);
    error_set_source(input_file, src);
    AstNode *ast = parse_file(input_file, src);

    codegen(ast, output_file);

    /* Auto-link when no explicit -o: read .imports and invoke gcc */
    if (!explicit_output) {
        char imports_path[1024];
        snprintf(imports_path, sizeof(imports_path), "%s.imports", output_file);
        FILE *f = fopen(imports_path, "r");
        if (f) {
            /* Find stdlib directory */
            const char *stdlib = getenv("STDLIB");
            if (!stdlib) stdlib = "stdlib";

            /* The object file has .o appended by codegen */
            char obj_file[512];
            snprintf(obj_file, sizeof(obj_file), "%s.o", output_file);

            /* Build gcc command */
            char cmd[4096];
            snprintf(cmd, sizeof(cmd), "gcc \"%s\"", obj_file);

            char line[256];
            while (fgets(line, sizeof(line), f)) {
                /* Trim newline */
                size_t len = strlen(line);
                while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
                    line[--len] = '\0';
                if (len == 0) continue;

                char obj_path[1024];
                snprintf(obj_path, sizeof(obj_path), "%s/%s/%s.o", stdlib, line, line);
                if (access(obj_path, F_OK) == 0) {
                    char append[1200];
                    snprintf(append, sizeof(append), " \"%s\"", obj_path);
                    strncat(cmd, append, sizeof(cmd) - strlen(cmd) - 1);
                }
            }
            fclose(f);

            char append_out[600];
            snprintf(append_out, sizeof(append_out), " -o \"%s\"", output_file);
            strncat(cmd, append_out, sizeof(cmd) - strlen(cmd) - 1);

            /* Link */
            int ret = system(cmd);
            if (ret != 0) {
                fprintf(stderr, "penguinc: linking failed (exit code %d)\n", ret);
            }
        }
    }

    free(src);
    return 0;
}
