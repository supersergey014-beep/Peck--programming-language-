# Peck Compiler

`pk` is a small native compiler for the Peck `.pk` language. It uses LLVM to emit
native object files and `clang++` to link executables.

## Requirements

- CMake 3.20 or newer
- C++17 compiler
- LLVM development package with CMake config files
- `clang++` on `PATH` (needed to link executables; not needed with `-c`)

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

If LLVM is installed in a nonstandard location, set `LLVM_DIR` to the directory
containing `LLVMConfig.cmake`.

## Compile

```sh
build/pk examples/hello.pk -o hello
./hello
```

Use `-c` (or `--emit-obj`) to emit a native object file instead of linking:

```sh
build/pk examples/hello.pk -c -o hello.o
```

Functions use `Str` or `func`; an optional suffix after the parameter list sets
the return type. Parameters take a mutability keyword and type suffix, and
`ret` returns a value:

```pk
Str add(var left!N, var right!N) !N {
	ret left + right
	End
}

Str main() {
	change total!N = add(2, 3)
	Output.string("Functions are supported")
	End
}
```

Parameters declared with `var` or `mut` are immutable; `change`/`chn` and
`alwchang`/`alwchn` parameters can be reassigned. Void functions omit the return
suffix. Function bodies use `End`, with `Output.string("text")` and `//` line
comments supported.

Top-level structs and relative source imports are supported. `pack "file.pk"`
and `import "file.pk"` are equivalent. Pointer expressions include `&value`,
`*pointer`, `^pointer`, and `@value`; `select(StructName)` allocates struct
storage with `malloc`, and `free(pointer)` releases it.

`numer` declares ordinal enum variants, and `respon` lowers pattern arms to an
LLVM switch. `change-method` registers compile-time modifier sequences; the
built-in update/edit/control modifiers are accepted as no-op annotations.
`input()` and `line()` read one line from stdin (up to 4095 bytes), while
`panic("reason")` prints a diagnostic and aborts the compiled program.