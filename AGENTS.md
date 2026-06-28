# AGENTS.md — penguinC

## Overview

penguinC is a systems programming language with C-like syntax, Rust-inspired ownership semantics, and an LLVM-based backend. It compiles `.pc` source files to native binaries on aarch64 Linux.

The compiler is written in C11, uses a hand-written recursive descent parser, and targets LLVM 20 via the C API (not C++).

## Project Structure

```
penguinC/
├── src/                    # Compiler source
│   ├── main.c              # Entry point, CLI arg parsing, file I/O
│   ├── lexer.c/h           # Tokenizer — keywords, operators, f-strings, escape sequences
│   ├── parser.c/h          # Recursive descent parser — produces AST
│   ├── ast.c/h             # AST node constructors and data structures
│   ├── codegen.c/h         # LLVM IR codegen — all lowering lives here
│   └── error.c/h           # Error reporting with source location spans
├── runtime/                # ARC runtime
│   └── arc.c               # arc_alloc, arc_retain, arc_release, arc_refcount
├── stdlib/                 # Standard library (C implementations + .ph headers)
│   └── io/
│       ├── io.c            # C implementation: io_print, io_println, str_concat, etc.
│       └── io.ph           # #link directives and => function mappings
├── tests/                  # Integration tests (input → expected output)
│   ├── run_tests.sh        # Test runner — compiles, links, runs, compares output
│   ├── test_*.pc            # penguinC source files
│   └── test_*.expected      # Expected stdout for each test
├── Makefile                # Builds compiler, stdlib, and runs tests
└── example.pc              # Language feature showcase
```

## Building

```bash
make              # Build compiler + stdlib + runtime
make test         # Build and run all tests (31 tests)
make test-5       # Run a single test by number
make clean        # Remove build artifacts
```

Requires: `gcc`, `llvm-20-dev` (for `llvm-config`), `lli`/`llc` not needed — the compiler emits `.o` files directly via `LLVMTargetMachineEmitToFile`.

## Architecture

### Compilation Pipeline

1. **Lexer** (`lexer.c`) — Tokenizes source into a flat token array. Handles f-strings (`f"..."`) with embedded expressions, escape sequences (`\n`, `\t`, `\\`, `\"`), and all keyword/operator tokens.

2. **Parser** (`parser.c`) — Recursive descent, single-pass. Produces an AST. No symbol resolution happens here. Parses types inline (e.g., `int*`, `struct Name`), handles operator precedence via Pratt parsing for expressions. Enforces camelCase naming conventions with warnings.

3. **AST** (`ast.c/h`) — Tagged union nodes (`AstNode`). Node types include: `NODE_PROGRAM`, `NODE_FUNC_DECL`, `NODE_STRUCT_DECL`, `NODE_CLASS_DECL`, `NODE_VAR_DECL`, `NODE_ASSIGN`, `NODE_BINARY`, `NODE_CALL`, `NODE_MEMBER`, `NODE_NEW_EXPR`, `NODE_IF`, `NODE_WHILE`, `NODE_FOR`, `NODE_SWITCH`, `NODE_MATCH`, `NODE_GOTO`, `NODE_LABEL`, `NODE_FUNC_MAP`, etc.

4. **Codegen** (`codegen.c`) — Single-pass LLVM IR generation with ARC instrumentation. Uses LLVM C API exclusively. Emits object files via target machine.

### Memory Management: Automatic Reference Counting (ARC)

penguinC uses **compile-time instrumented ARC** for memory safety. No manual memory management required.

#### Memory Layout

Every heap-allocated object has a refcount header:
```
+------------------+---------------------+
| refcount: i64    | object data...      |
+------------------+---------------------+
^                  ^
|                  user pointer points here
arc_alloc returns this
```

#### ARC Runtime (`runtime/arc.c`)

| Function | Description |
|----------|-------------|
| `arc_alloc(size)` | Allocate with refcount header, refcount=1 |
| `arc_retain(ptr)` | Increment refcount, return ptr |
| `arc_release(ptr)` | Decrement refcount, free if 0 |
| `arc_refcount(ptr)` | Return current refcount |

#### What Gets ARC

- `new Type(...)` — allocated via `arc_alloc`, refcount starts at 1
- String concatenation (`+`) — `penguin_str_concat` uses `arc_alloc`
- `int_to_string`, `float_to_string`, `bool_to_string` — all use `arc_alloc`
- String literals — NOT refcounted (global constants)

#### Instrumentation Points

The compiler automatically inserts `arc_retain`/`arc_release` calls at:

1. **Assignment** (`x = val`): releases old value, stores new (no retain needed — caller owns the ref)
2. **Scope exit**: releases all heap-typed locals when leaving a function body
3. **Function return**: retains return value so caller takes ownership
4. **`using(expr) { body }`**: evaluates expr, executes body, releases expr

#### Parameters Are Borrowed

Function parameters are NOT retained on call and NOT released on scope exit. The caller retains its own reference. The callee borrows it.

#### ARC Optimization Pass

A peephole pass eliminates redundant `arc_retain(x)` + `arc_release(x)` pairs where both operate on the same value.

### Stdlib Architecture

Standard library modules are written in C, compiled to `.o` files, and linked selectively. Each module is a directory containing:
- `.c` — C implementation (e.g., `io/io.c` defines `io_print`, `io_print_int`)
- `.ph` — penguinC header with `#link` directives and `=>` function mappings

```
stdlib/
└── io/
    ├── io.c      # C implementation (uses arc_alloc for string allocation)
    └── io.ph     # #link "./io.o"; void print(...) => io_print;
```

The `.ph` file format:
- `#link "./io.o";` — Specifies which `.o` file to link when this module is imported
- `ret_type pc_name(args) => c_name;` — Maps a penguinC function name to a C symbol name

The compiler resolves `=>` function mappings at codegen time. When `io.print(s, n)` is called, the compiler looks up `"io.print"` in the `func_map` registry and uses the mapped C name (`io_print`) for the LLVM function declaration. No assembly trampolines needed.

The compiler writes a `.imports` file listing which stdlib modules were imported. The test runner and Makefile read this to link only the needed `.o` files, avoiding unnecessary symbol conflicts. The module name (e.g., `io`) is extracted from the dotted import path (e.g., `std.io`), and the linked object is found at `stdlib/io/io.o`.

### Key Codegen Patterns

- **Function type registry**: Every function's `LLVMTypeRef` is stored via `fn_type_push()` when declared and retrieved via `fn_type_lookup()` when called. This is mandatory — LLVM 20 uses opaque pointers so `LLVMTypeOf(callee)` returns `ptr`, not the function type.

- **Function mapping registry**: penguinC function names (e.g., `io.print`) are mapped to C symbol names (e.g., `io_print`) via `func_map_push()`/`func_map_lookup()`. This eliminates the need for assembly trampolines and allows the compiler to resolve foreign function calls at codegen time.

- **Variable type tracking**: Each variable stores its LLVM type, an element type (for pointer-to-struct class instances), a struct name (for field index lookup), and a type_name (for ARC type inference and camelCase enforcement).

- **Struct field lookup**: Field indices are resolved by name via `struct_field_index()` against stored field name arrays, not by hash or position assumption.

- **Class inheritance**: Derived class struct types include parent fields at the beginning. `super.new()` dispatches via the class parent registry.

- **Self parameter**: Methods receive `self` as an opaque `ptr` (i8*). The var map stores the struct element type alongside it so `self.field` resolves through GEP on the correct struct type.

- **Class return types**: Functions returning class types use `ptr` (pointer-to-struct) as the LLVM return type, not the raw struct type.

## Language Features

- **Types**: `int` (i64), `float` (f64), `bool` (i1), `void`, `string` (i8*), `struct`, `class`, `enum`, `typedef`
- **Pointers**: `int*` syntax, `new` for heap allocation, `*ptr` dereference, `&borrow` syntax
- **Classes**: Single inheritance via `extends`, `self` keyword, `super.new()` for constructor chaining, methods as standalone functions with `self` as first param
- **Control flow**: `if`/`else`, `while`, `do`/`while`, `for`/`in` ranges, C-style `for`, `switch`/`case`, `match` (with `in`, comparison, and literal patterns), `break`, `continue`, `goto`/labels
- **Expressions**: Ternary (`? :`), cast (`(type)expr`), `sizeof(type)`, f-strings (`f"..."`), string concatenation (`+`), compound assignment (`+=`, `-=`, `*=`, `/=`), type methods (`.toI()`, `.toF()`, `.toS()`, `.toB()`)
- **Memory**: `new Type(args)` allocates and calls constructor, ARC handles deallocation automatically
- **Ownership**: `using(expr) { body }` for scoped resource management, `borrow` for references
- **Modules**: `#import std.io;` for stdlib, `#import "./file.pc"` for local, `#link "./file.o"` for precompiled objects

## Testing

Tests are integration tests — each test is a `.pc` file compiled, linked, executed, and compared against a `.expected` file containing exact stdout. Run with `make test` or `./tests/run_tests.sh`. The runner reads `.imports` files for selective stdlib linking and links `runtime/arc.o` with every binary.

Current status: **31/31 tests passing**.

## Conventions

- Source files in `src/`, stdlib in `stdlib/`, runtime in `runtime/`, tests in `tests/`
- C11 standard, `-Wall -Wextra` enabled
- Verbose error messages with source location (filename, line, column, colored spans)
- camelCase warnings for variables/functions, PascalCase for classes/structs
- AST node constructors prefixed with `ast_new_*`
- Codegen functions prefixed with `codegen_*`
- Parser functions prefixed with `parse_*`
- No comments in code unless explicitly requested
- Mangled names: `ClassName.method` for methods, `_pC<name><types>` for overloaded functions

## Known Limitations

- Single inheritance only (no traits/interfaces yet)
- No closures or function pointers
- No generics/templates
- aarch64 Linux only (ARM64 syscalls in runtime, LLVM target hardcoded)
- ARC overhead: retain/release calls on every assignment and scope exit (optimizable with LLVM O1+)
