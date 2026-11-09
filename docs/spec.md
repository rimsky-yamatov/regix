# Regix language specification (draft)

## Storage classes

Every variable, parameter and return value names where it lives:

    reg   x: i32
    stack y: i32
    heap  z: i32

* `reg` and `stack` values are released at the end of their lexical scope.
* `heap` values are also released at scope end, unless dropped earlier.
* A `reg` value has no address: `&` cannot be applied to it.

## `reg` allocation

* There is no spilling. If more `reg` variables are live than the target
  provides, compilation fails and the error names the target's limit.
* The limit is target dependent and comes from the ABI table in `rgc`.
* Tail calls `return(f(...))` are guaranteed to compile to a jump, so
  tail-recursive functions run in registers only.

## `drop`

`drop(x)` works for all storage classes: zero-fill, release, and `x` can no
longer be used (checked at compile time). The zero-fill is a side effect the
compiler must never remove. Scope exit releases without clearing.

## Moves and copies

* Primitives are copied. Structs and `heap` values are moved.
* Explicit duplication is `copy(x)`. Function arguments move by default;
  `&x` and `&mut x` borrow.

## Functions

Parameter storage classes and the return type are mandatory:

    fn name(reg a: i32) -> reg i32
    fn name() -> void

`main` is `fn main() -> reg i32`, `fn main() -> void`, or takes
`stack args: &[&[u8]]`.

## Structs and arrays

C-style structs. A field without a storage class is stored inline in its
parent; a `heap` field owns a separate heap block. Strings are fixed-length
arrays (`heap s: [u8; 5] = "Hello"`) with borrowed slices `&[u8]`.

## Packages

Java-style packages with `::` as separator:

    package com::example::project;
    import std::io;

Only imported names are usable. `pub` marks items visible outside the package.

## Arrays, slices and strings

    stack a: [i32; 4] = [1, 2, 3, 4]      // literal
    stack z: [i32; 16] = [0; 16]          // repeat
    heap  s: [u8; 5] = "Hello"            // string literal (length must match)

* Element types are `i32` and `u8`. `u8` exists only as an element type: a
  read yields an `i32`, a write truncates to 8 bits.
* `a[i]` reads and writes; `a.len` is the length. Every index is bounds
  checked: a constant index is a compile error, any other failure exits the
  program with code 101.
* An array cannot be `reg`. It can be `stack` or `heap`, and `drop(a)` zero
  fills every byte.
* A slice `&[T]` is a borrowed, read-only (pointer, length) view. It exists as
  a `stack` parameter: `fn sum(stack s: &[i32]) -> reg i32`. Pass `&a`, a
  slice variable, or (for `&[u8]`) a string literal. A slice uses two argument
  registers.
* A call that borrows an array living in the current frame is an ordinary call,
  not a tail jump, because the array dies with the frame. Borrowing a slice
  parameter keeps the tail-call guarantee.
* Strings are `[u8]` data; there is no special string type.

## Standard library (stub)

With `import std::io;`:

    std::io::print_i32(n)       // decimal and newline
    std::io::print(&[u8])       // bytes, no newline
    std::io::println(&[u8])     // bytes and newline

## `main` arguments

    fn main(stack args: &[&[u8]]) -> reg i32

`args[0]` is the program name. `args.len` is the count. An element can be
passed on as an argument (`std::io::println(args[1])`) when the index is a
constant or a variable. On Windows the command line is split on spaces and
tabs with double quotes grouping; there are no backslash rules, and at most
128 arguments are kept.

## Packages across files

* A file may start with `package a::b;`, then `import` lines. `rgc` takes any
  number of files: `rgc main.rgx src/a/b/x.rgx -o app`.
* Names in the current package resolve without a prefix, even across files.
  A function in another package is called with its full name,
  `a::b::f(1)`, and needs `import a::b;` plus `pub fn f`.
* Structs are visible inside their own package only (cross-package struct
  types are not implemented yet).
* Exactly one `main` must exist. Two functions with the same name are fine in
  different packages.
* `rgpm build` compiles every `.rgx` under `src/`. The directory must match
  the package: `src/com/example/x.rgx` declares `package com::example;`, and
  files directly in `src/` declare no package.

## Copy and move of structs and arrays

    stack b: Point = copy(a)     // duplicates the bytes; `a` stays usable
    stack c: Point = a           // moves; `a` can no longer be used

Both work between `stack` and `heap` (a heap source that moves to the stack is
freed; heap to heap just transfers the block). Types must match exactly.
Whole-value assignment `a = b` is still rejected.

## `secret`

    secret reg   key: i32 = 3
    secret stack buf: [u8; 16] = [0; 16]

A secret is zero-filled at the end of its scope and before every `return` or
tail jump (a `heap` secret is zeroed and then freed). `drop(x)` clears it
earlier. Direct leaks are compile errors: copying a secret into a non-secret
variable (or field or element), passing it to a function, borrowing it,
returning it, or `copy`-ing it. A secret may be moved only into another
secret. There is no taint tracking: `reg y: i32 = k + 0` is accepted.

## Heap allocator

Linux: a size-class allocator. Each block has a 16-byte header; block sizes
are powers of two from 32 to 8192 bytes, carved from 1 MiB `mmap` chunks, and
freed blocks go on per-class lists for reuse (not cleared; use `drop` or
`secret` to clear). Windows: `HeapAlloc`/`HeapFree` on the process heap. The
largest heap value is 4096 bytes.

## References (`&T`, `&mut T`)

    fn inc(stack n: &mut i32) -> void { *n = *n + 1 }
    fn len2(stack p: &Point) -> reg i32 { return(p.x + p.y) }

    stack a: i32 = 41
    inc(&mut a)                    // a is 42
    stack p: Point = Point { x: 1, y: 2 }
    heap  h: Point = Point { x: 3, y: 4 }
    len2(&p) + len2(&h)

* `T` is `i32` or a struct. A reference exists only as a `stack` parameter, so
  it lives exactly as long as the call and cannot dangle. References cannot be
  variables, struct fields or return values.
* The caller spells the borrow: `&x` (shared), `&mut x` (mutable), or a field
  such as `&mut p.pos.y`. A `&mut x` may be passed to a `&T` parameter; a `&x`
  cannot be passed to `&mut T`.
* Through a reference: `*r` reads and `*r = v` writes an `i32`; `r.field`
  reads and `r.field = v` writes a struct field (no `*` needed). Writing
  through a shared reference is a compile error.
* A reference parameter can be passed on (`g(r)`); a shared reference cannot
  be passed where `&mut` is needed.
* A `reg` variable has no address and cannot be borrowed; use `stack` or
  `heap`. Arrays and slices are borrowed as slices (`&[T]`), not as references.
* No aliasing: a variable borrowed with `&mut` cannot appear anywhere else in
  the same call (not as `&x`, not by value, not as another field of it).
* `copy(r)` copies a struct out of a reference; moving out of a reference is
  an error. A secret cannot be borrowed.
* A call that borrows a local of the current frame is an ordinary call, not a
  tail jump; passing a reference parameter on keeps the tail-call guarantee.
* A `*` at the start of a line begins a statement (`*r = v`) rather than a
  multiplication, so write a product's `*` on the same line as its left side.

## Implementation status (unreleased, after v0.1.0)

Implemented:

* Types: `i32`, structs of `i32` fields and nested structs (inline), arrays
  `[i32; N]` / `[u8; N]`, slice and reference parameters, string literals.
* Variables: `reg`, `stack`, `heap`, optional `secret`; `if`/`else`, `while`,
  assignment, calls, guaranteed tail calls.
* `drop`, `copy`, moves, ownership transfer through `heap` parameters and
  returns, compile-time use-after-drop/move checks.
* Borrowed references `&T` / `&mut T` as parameters, with aliasing checks.
* Multi-file packages with `import` and `pub`; `rgpm init/build/run`.
* `std::io::print_i32`, `print`, `println`; `main` arguments.

Known limits (planned, not implemented):

* Struct `heap` fields (recursive types) still need a null value and a way to
  hold a reference across a loop iteration (a reference that is a local
  variable); both are next on the list.
* Whole-value assignment `a = b`; passing structs or arrays by value;
  arrays inside structs; `&mut [T]`; slice variables; references to array
  elements.
* Cross-package structs; the rest of the standard library.
* A `reg` struct or array is rejected.
