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

## Implementation status (v0.1.0)

Implemented:

* Types: `i32` and C-style structs of `i32` fields and nested structs
  (stored inline). Struct literals `P { x: 1, y: 2 }` initialize `stack` or
  `heap` variables; fields are read and assigned with `.`.
* Variables: `reg`, `stack`, `heap`; `if`/`else`, `while`, assignment, calls,
  guaranteed tail calls.
* `stack` values live in frame slots (a stack struct takes consecutive slots).
* `heap` values are allocated by a tiny runtime and released at scope end,
  before every `return`, and before a tail call jumps away.
* `drop(x)` zero-fills and releases (all fields of a struct). Use after drop,
  double drop, and dropping in only one branch of an `if` or inside a loop are
  compile errors; a branch ending in `return` does not count for the merge.
* Parameters and return values: `reg`, `stack` (copied into the callee) and
  `heap` (the owner moves: the caller's variable cannot be used afterwards;
  `-> heap T` hands ownership to the caller). Struct parameters and returns
  must be `heap`.
* `import` is enforced for `std::io::print_i32(n)` (prints a decimal and a
  newline). This is a stand-in until slices exist.
* `package` names mangle into assembler symbols.

Known limits (planned, not implemented):

* Struct fields cannot have `heap` (recursive types); whole-struct copy,
  move-by-assignment and `copy(x)`; the `secret` modifier.
* Arrays, strings and slices; `main` arguments; the rest of the standard
  library.
* Importing user packages from other files (only the standard stub resolves).
* Each heap value takes a whole page on Linux (`mmap`/`munmap`) and 16 bytes
  from the process heap on Windows; a real allocator comes later.
* A `reg` struct is rejected.
