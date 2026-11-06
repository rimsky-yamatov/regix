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

## Implementation status (v0.1)

Implemented: `i32`, `reg` variables, `if`/`else`, `while`, assignment,
calls, guaranteed tail calls, `package`/`import` parsing.
Not yet: `stack`, `heap`, `drop`, structs, arrays, slices, imports resolution,
`main` arguments, standard library.
