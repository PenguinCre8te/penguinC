# Plan: Automatic Reference Counting (ARC) for penguinC

## Goal
Replace manual memory management (`mem.drop`) with compile-time instrumented ARC. All heap objects (class instances, strings) are automatically freed when no longer referenced. The `stdlib/mem` module is removed entirely.

## Memory Layout

Every heap-allocated object gets a refcount header:

```
+------------------+---------------------+
| refcount: i64    | object data...      |
+------------------+---------------------+
^                  ^
|                  user pointer points here
arc_alloc returns this
```

- User-visible pointer points to data (after header)
- Refcount is at `pointer - 8`
- Initial refcount: 1

## Phase 1: Runtime (`runtime/arc.c`)

Create `runtime/arc.c` with three functions:

```c
#include <stdlib.h>
#include <stdint.h>

// Allocate with refcount header, refcount = 1
void *arc_alloc(size_t size) {
    uint64_t *p = malloc(sizeof(uint64_t) + size);
    *p = 1;
    return p + 1;
}

// Increment refcount, return same pointer
void *arc_retain(void *ptr) {
    if (ptr) { ((uint64_t *)ptr)[-1]++; }
    return ptr;
}

// Decrement refcount, free entire block if reaches 0
void arc_release(void *ptr) {
    if (ptr) {
        uint64_t *header = (uint64_t *)ptr - 1;
        if (--(*header) == 0) {
            free(header);
        }
    }
}
```

Compile: `gcc -c -o runtime/arc.o runtime/arc.c`

Every compiled program links with `runtime/arc.o`.

## Phase 2: Remove stdlib/mem

- Delete `stdlib/mem/mem.c` and `stdlib/mem/mem.ph`
- Remove `$(STDLIB)/mem/mem.o` from Makefile
- Remove `#import std.mem;` from `test_22_new_free.pc` and `example.pc`
- Remove `mem.drop(p)` calls from `test_22_new_free.pc` (update test expected output if needed)
- Update `run_tests.sh` to not look for `mem.o`

## Phase 3: Codegen Instrumentation

### 3a. ARC helper declarations in codegen.c

Add static functions that declare and call the ARC runtime:

```c
static LLVMValueRef call_arc_retain(CodegenCtx *cg, LLVMValueRef ptr);
static LLVMValueRef call_arc_release(CodegenCtx *cg, LLVMValueRef ptr);
static LLVMValueRef call_arc_alloc(CodegenCtx *cg, LLVMValueRef size);
static int is_arc_type(CodegenCtx *cg, const char *type_name);
```

`is_arc_type` returns true for:
- `string` type
- Any class/struct type (has elem_ty or struct_name in var_map)
- Any pointer type (int*, float*, etc.)

### 3b. `new` expression — use `arc_alloc`

In `codegen_new_expr` (line ~881), replace `call_malloc(size)` with `call_arc_alloc(size)`. The refcount starts at 1 automatically.

### 3c. String allocation — use `arc_alloc`

In `stdlib/io/io.c`, replace all `malloc` calls with `arc_alloc`:
- `penguin_str_concat`: `arc_alloc(la + lb + 1)` instead of `malloc`
- `int_to_string`: `arc_alloc(len + 1)` instead of `malloc`
- `float_to_string`: `arc_alloc(n + 1)` instead of `malloc`
- `bool_to_string`: `arc_alloc(len + 1)` instead of `malloc`

These functions need to `#include "../runtime/arc.h"` (or declare `arc_alloc` as extern).

### 3d. Variable declaration with heap init

In `codegen_var_decl`, after storing the init value:
```c
if (init && is_arc_type(cg, type_name)) {
    call_arc_retain(cg, init_val);  // init is already refcount=1 for new, retain for the variable
}
```

Wait — for `new`, `arc_alloc` already sets refcount=1. The variable "owns" that reference. So we do NOT retain on declaration for `new` expressions. But for string concatenation results (which also get refcount=1 from `arc_alloc` in the C runtime), same logic applies.

**Rule**: `arc_alloc` returns a value with refcount=1 (owned by caller). No retain needed when storing into a variable. The variable takes ownership.

### 3e. Assignment: release old, store new

In `codegen_assign` (or wherever `x = expr` is handled), before storing:

```c
// If x currently holds a heap pointer, release it
if (is_arc_type(cg, var_name)) {
    LLVMValueRef old_val = LLVMBuildLoad(cg->builder, alloca, "old");
    call_arc_release(cg, old_val);
}
// Evaluate new value and store
LLVMValueRef new_val = codegen_expr(cg, rhs);
LLVMBuildStore(cg->builder, new_val, alloca);
// Note: new_val already has refcount=1 from its creation site. No retain needed.
```

### 3f. Scope exit — release all heap variables

In `codegen_func_decl`, after `codegen_block` and before the implicit return, release all heap-typed variables that were declared in this function's scope:

```c
// Release heap variables in reverse order
for (size_t i = cg->var_count; i > prev_var_count; i--) {
    if (is_arc_type_for_var(cg, i-1)) {
        LLVMValueRef val = LLVMBuildLoad(cg->builder, cg->vars[i-1].val, "cleanup");
        call_arc_release(cg, val);
    }
}
```

### 3g. Function return — retain return value

When returning a heap-typed value, retain it so the caller can own it:

```c
// return expr
LLVMValueRef retVal = codegen_expr(cg, return_expr);
if (is_heap_return_type) {
    retVal = call_arc_retain(cg, retVal);
}
LLVMBuildRet(cg->builder, retVal);
```

### 3h. Function call arguments — no special handling needed

Arguments are passed by value (the pointer). The caller retains its own reference. The callee gets a new reference (the parameter variable). The callee releases its parameter on scope exit.

**Issue**: This means double-release. The caller releases its variable on its own scope exit, AND the callee releases the parameter on its scope exit. But the refcount was only incremented once (by arc_alloc).

**Fix**: Parameters are NOT retained on call. The callee borrows the reference. If the callee wants to keep it, it must retain it. For scope exit, the callee only releases variables it created (not parameters).

**Revised rule**: Only release variables with `prev_var_count` offset (locals created in this function). Parameters are released by the caller.

### 3i. `mem.drop` — remove or make no-op

Since we're removing the mem module, `mem.drop` calls will cause a parse error (unknown member). This is fine — users should remove them. The compiler can emit a helpful error message if it sees `mem.drop`.

## Phase 4: Optimization

### 4a. Release elision for temporaries

When a value is created (via `new`, `str_concat`, etc.) and immediately consumed in the same expression (e.g., `new Foo().doSomething()`), skip the retain/release pair.

Implementation: In `codegen_call`, if the callee argument is a `NODE_NEW_EXPR` or a string-producing expression, emit `arc_alloc` with refcount=1 but no retain on the result. The called function will release its parameter (decrement to 0, free). Wait — this is wrong because the called function doesn't retain.

**Better approach**: Don't retain parameters. Only retain when storing into a variable. So:
- `new Foo()` → arc_alloc returns refcount=1
- `new Foo().doSomething()` → pass the pointer directly, no retain
- Inside `doSomething(self)`, self is a parameter, NOT retained, NOT released on scope exit
- After the call, the caller releases the temporary if it stored it in a variable

**This means parameters are borrowed references, not owned.**

### 4b. Redundant retain/release elimination

In the generated IR, detect patterns like:
```
%ret = call ptr @arc_retain(ptr %val)
call void @arc_release(ptr %ret)
```
And remove both calls. This can be done as a simple peephole pass.

### 4c. Move on last use (future optimization)

If a variable is used exactly once more before scope exit, transfer ownership instead of retain+release:
- On assignment: skip retain (give ownership to variable)
- On last use: skip retain (give ownership to the use)
- On scope exit: skip release (ownership was transferred)

This requires use-count analysis. Defer to future work.

## Phase 5: Test Updates

### test_22_new_free.pc
Remove `mem.drop(p)` call and `#import std.mem`. Expected output stays `42`.

### example.pc
Remove `#import std.mem`, `mem.drop(penguin)`, and lock/using lines that reference mem.

### All other tests
No changes needed — they don't use the mem module.

## Files to Change

| File | Change |
|------|--------|
| `runtime/arc.c` | **NEW** — ARC runtime functions |
| `Makefile` | Add `runtime/arc.o` build, remove `mem.o`, link `arc.o` with test binaries |
| `stdlib/mem/mem.c` | **DELETE** |
| `stdlib/mem/mem.ph` | **DELETE** |
| `stdlib/io/io.c` | Replace `malloc` with `arc_alloc` in string functions |
| `src/codegen.c` | Add `call_arc_retain/release/alloc`, `is_arc_type`; instrument `new`, assignments, scope exits, returns |
| `tests/test_22_new_free.pc` | Remove `mem.drop`, remove `#import std.mem` |
| `tests/run_tests.sh` | Link `runtime/arc.o` with test binaries, remove `mem.o` from linking |
| `example.pc` | Remove mem-related code |
| `AGENTS.md` | Update memory management documentation |

## Implementation Order

1. Create `runtime/arc.c` and update Makefile
2. Delete `stdlib/mem/` 
3. Modify `stdlib/io/io.c` to use `arc_alloc`
4. Add ARC helpers to `codegen.c`
5. Instrument `new` expressions
6. Instrument assignments
7. Instrument scope exits
8. Instrument function returns
9. Update tests (test_22, test_25, example.pc)
10. Run full test suite
11. Add optimization passes
12. Final test run

## Verification

After implementation:
- `make test` should pass all 31 tests
- `test_22_new_free.pc` should work without `mem.drop`
- No memory leaks (verified by running under valgrind if available)
- String concatenation in `test.pc` should work with ARC
