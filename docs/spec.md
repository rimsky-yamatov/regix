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

## Implementation status (unreleased, after v0.1.0)

Implemented:

* Types: `i32`, structs of `i32` fields and nested structs (inline), arrays
  `[i32; N]` / `[u8; N]`, slice parameters, string literals.
* Variables: `reg`, `stack`, `heap`; `if`/`else`, `while`, assignment, calls,
  guaranteed tail calls.
* `stack` values live in frame slots. `heap` values come from a tiny runtime
  and are released at scope end, before every `return`, and before a tail
  call jumps away.
* `drop(x)` zero-fills and releases; use after drop, double drop, and
  dropping in one branch of an `if` or inside a loop are compile errors.
* Parameters and returns: `reg`, `stack` (copied) and `heap` (ownership moves).
* `import` is enforced for the `std::io` functions above.

Known limits (planned, not implemented):

* Struct `heap` fields (recursive types), whole-struct copy and move by
  assignment, `copy(x)`, the `secret` modifier.
* Arrays inside structs, arrays as parameters or return values, `&mut [T]`,
  slices of slices other than `main`'s arguments, slice variables.
* The rest of the standard library; importing user packages from other files.
* Each heap value takes a whole page on Linux (`mmap`/`munmap`) and the
  size you ask for from the process heap on Windows; a real allocator comes later.
* A `reg` struct or array is rejected.
