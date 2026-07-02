#include "codegen_internal.h"

void class_push(CodegenCtx *cg, const char *name, const char *parent) {
    if (cg->class_count >= cg->class_cap) {
        cg->class_cap = cg->class_cap ? cg->class_cap * 2 : 16;
        cg->classes = realloc(cg->classes, cg->class_cap * sizeof(*cg->classes));
    }
    cg->classes[cg->class_count].name = strdup(name);
    cg->classes[cg->class_count].parent = parent ? strdup(parent) : NULL;
    cg->class_count++;
}

const char *class_lookup_parent(CodegenCtx *cg, const char *name) {
    for (size_t i = cg->class_count; i > 0; i--) {
        if (strcmp(cg->classes[i - 1].name, name) == 0)
            return cg->classes[i - 1].parent;
    }
    return NULL;
}

void enum_push(CodegenCtx *cg, const char *name, long value) {
    if (cg->enum_count >= cg->enum_cap) {
        cg->enum_cap = cg->enum_cap ? cg->enum_cap * 2 : 16;
        cg->enums = realloc(cg->enums, cg->enum_cap * sizeof(*cg->enums));
    }
    cg->enums[cg->enum_count].name = strdup(name);
    cg->enums[cg->enum_count].value = value;
    cg->enum_count++;
}

long enum_lookup(CodegenCtx *cg, const char *name, int *found) {
    for (size_t i = cg->enum_count; i > 0; i--) {
        if (strcmp(cg->enums[i - 1].name, name) == 0) {
            *found = 1;
            return cg->enums[i - 1].value;
        }
    }
    *found = 0;
    return 0;
}

void fn_type_push(CodegenCtx *cg, const char *name, LLVMTypeRef fn_ty) {
    if (cg->fn_type_count >= cg->fn_type_cap) {
        cg->fn_type_cap = cg->fn_type_cap ? cg->fn_type_cap * 2 : 16;
        cg->fn_types = realloc(cg->fn_types, cg->fn_type_cap * sizeof(*cg->fn_types));
    }
    cg->fn_types[cg->fn_type_count].name = strdup(name);
    cg->fn_types[cg->fn_type_count].fn_ty = fn_ty;
    cg->fn_type_count++;
}

LLVMTypeRef fn_type_lookup(CodegenCtx *cg, const char *name) {
    for (size_t i = cg->fn_type_count; i > 0; i--) {
        if (strcmp(cg->fn_types[i - 1].name, name) == 0)
            return cg->fn_types[i - 1].fn_ty;
    }
    return NULL;
}

void var_push(CodegenCtx *cg, const char *name, LLVMValueRef val, LLVMTypeRef ty) {
    if (cg->var_count >= cg->var_cap) {
        cg->var_cap = cg->var_cap ? cg->var_cap * 2 : 16;
        cg->vars = realloc(cg->vars, cg->var_cap * sizeof(*cg->vars));
    }
    cg->vars[cg->var_count].name = strdup(name);
    cg->vars[cg->var_count].val  = val;
    cg->vars[cg->var_count].ty   = ty;
    cg->vars[cg->var_count].elem_ty = NULL;
    cg->vars[cg->var_count].struct_name = NULL;
    cg->vars[cg->var_count].type_name = NULL;
    cg->var_count++;
}

void var_set_elem_type(CodegenCtx *cg, const char *name, LLVMTypeRef elem_ty) {
    for (size_t i = cg->var_count; i > 0; i--) {
        if (strcmp(cg->vars[i - 1].name, name) == 0) {
            cg->vars[i - 1].elem_ty = elem_ty;
            return;
        }
    }
}

LLVMTypeRef var_lookup_elem_type(CodegenCtx *cg, const char *name) {
    for (size_t i = cg->var_count; i > 0; i--) {
        if (strcmp(cg->vars[i - 1].name, name) == 0)
            return cg->vars[i - 1].elem_ty;
    }
    return NULL;
}

void var_set_struct_name(CodegenCtx *cg, const char *name, const char *struct_name) {
    for (size_t i = cg->var_count; i > 0; i--) {
        if (strcmp(cg->vars[i - 1].name, name) == 0) {
            cg->vars[i - 1].struct_name = strdup(struct_name);
            return;
        }
    }
}

const char *var_lookup_struct_name(CodegenCtx *cg, const char *name) {
    for (size_t i = cg->var_count; i > 0; i--) {
        if (strcmp(cg->vars[i - 1].name, name) == 0)
            return cg->vars[i - 1].struct_name;
    }
    return NULL;
}

void var_set_type_name(CodegenCtx *cg, const char *name, const char *type_name) {
    for (size_t i = cg->var_count; i > 0; i--) {
        if (strcmp(cg->vars[i - 1].name, name) == 0) {
            if (cg->vars[i - 1].type_name) free(cg->vars[i - 1].type_name);
            cg->vars[i - 1].type_name = strdup(type_name);
            return;
        }
    }
}

const char *var_lookup_type_name(CodegenCtx *cg, const char *name) {
    for (size_t i = cg->var_count; i > 0; i--) {
        if (strcmp(cg->vars[i - 1].name, name) == 0)
            return cg->vars[i - 1].type_name;
    }
    return NULL;
}

void typedef_push(CodegenCtx *cg, const char *alias, const char *orig) {
    if (cg->typedef_count >= cg->typedef_cap) {
        cg->typedef_cap = cg->typedef_cap ? cg->typedef_cap * 2 : 16;
        cg->typedefs = realloc(cg->typedefs, cg->typedef_cap * sizeof(*cg->typedefs));
    }
    cg->typedefs[cg->typedef_count].alias = strdup(alias);
    cg->typedefs[cg->typedef_count].orig = strdup(orig);
    cg->typedef_count++;
}

const char *typedef_resolve(CodegenCtx *cg, const char *name) {
    for (size_t i = cg->typedef_count; i > 0; i--) {
        if (strcmp(cg->typedefs[i - 1].alias, name) == 0)
            return cg->typedefs[i - 1].orig;
    }
    return name;
}

LLVMValueRef var_lookup(CodegenCtx *cg, const char *name) {
    for (size_t i = cg->var_count; i > 0; i--) {
        if (strcmp(cg->vars[i - 1].name, name) == 0)
            return cg->vars[i - 1].val;
    }
    return NULL;
}

int var_lookup_index(CodegenCtx *cg, const char *name, size_t *out_index) {
    for (size_t i = cg->var_count; i > 0; i--) {
        if (strcmp(cg->vars[i - 1].name, name) == 0) {
            *out_index = i - 1;
            return 1;
        }
    }
    return 0;
}

LLVMTypeRef var_lookup_type(CodegenCtx *cg, const char *name) {
    for (size_t i = cg->var_count; i > 0; i--) {
        if (strcmp(cg->vars[i - 1].name, name) == 0)
            return cg->vars[i - 1].ty;
    }
    return NULL;
}

void struct_push(CodegenCtx *cg, const char *name, LLVMTypeRef ty) {
    if (cg->struct_count >= cg->struct_cap) {
        cg->struct_cap = cg->struct_cap ? cg->struct_cap * 2 : 16;
        cg->structs = realloc(cg->structs, cg->struct_cap * sizeof(*cg->structs));
    }
    cg->structs[cg->struct_count].name = strdup(name);
    cg->structs[cg->struct_count].ty   = ty;
    cg->struct_count++;
}

LLVMTypeRef struct_lookup(CodegenCtx *cg, const char *name) {
    for (size_t i = 0; i < cg->struct_count; i++) {
        if (strcmp(cg->structs[i].name, name) == 0)
            return cg->structs[i].ty;
    }
    return NULL;
}

void struct_push_fields(CodegenCtx *cg, const char *struct_name, char **field_names, size_t count) {
    if (cg->struct_field_count >= cg->struct_field_cap) {
        cg->struct_field_cap = cg->struct_field_cap ? cg->struct_field_cap * 2 : 16;
        cg->struct_fields = realloc(cg->struct_fields, cg->struct_field_cap * sizeof(*cg->struct_fields));
    }
    cg->struct_fields[cg->struct_field_count].struct_name = strdup(struct_name);
    cg->struct_fields[cg->struct_field_count].field_names = malloc(count * sizeof(char *));
    for (size_t i = 0; i < count; i++)
        cg->struct_fields[cg->struct_field_count].field_names[i] = strdup(field_names[i]);
    cg->struct_fields[cg->struct_field_count].field_count = count;
    cg->struct_field_count++;
}

int struct_field_index(CodegenCtx *cg, const char *struct_name, const char *field_name) {
    for (size_t i = cg->struct_field_count; i > 0; i--) {
        if (strcmp(cg->struct_fields[i - 1].struct_name, struct_name) == 0) {
            for (size_t j = 0; j < cg->struct_fields[i - 1].field_count; j++) {
                if (strcmp(cg->struct_fields[i - 1].field_names[j], field_name) == 0)
                    return (int)j;
            }
            return -1;
        }
    }
    return -1;
}

void func_map_push(CodegenCtx *cg, const char *pc_name, const char *c_name) {
    if (cg->func_map_count >= cg->func_map_cap) {
        cg->func_map_cap = cg->func_map_cap ? cg->func_map_cap * 2 : 16;
        cg->func_maps = realloc(cg->func_maps, cg->func_map_cap * sizeof(*cg->func_maps));
    }
    cg->func_maps[cg->func_map_count].pc_name = strdup(pc_name);
    cg->func_maps[cg->func_map_count].c_name  = strdup(c_name);
    cg->func_map_count++;
}

const char *func_map_lookup(CodegenCtx *cg, const char *pc_name) {
    for (size_t i = cg->func_map_count; i > 0; i--) {
        if (strcmp(cg->func_maps[i - 1].pc_name, pc_name) == 0)
            return cg->func_maps[i - 1].c_name;
    }
    return NULL;
}

LLVMBasicBlockRef label_lookup(CodegenCtx *cg, const char *name) {
    for (size_t i = 0; i < cg->label_count; i++) {
        if (strcmp(cg->labels[i].name, name) == 0)
            return cg->labels[i].block;
    }
    return NULL;
}

void label_push(CodegenCtx *cg, const char *name, LLVMBasicBlockRef block) {
    if (cg->label_count >= cg->label_cap) {
        cg->label_cap = cg->label_cap ? cg->label_cap * 2 : 16;
        cg->labels = realloc(cg->labels, cg->label_cap * sizeof(*cg->labels));
    }
    cg->labels[cg->label_count].name  = strdup(name);
    cg->labels[cg->label_count].block = block;
    cg->label_count++;
}
