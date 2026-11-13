# Regix standard library

Two kinds of modules:

* **Runtime modules**, implemented in assembly by the compiler: `std::io` and
  `std::process`.
* **Source modules**, written in Regix and shipped in the `std/` directory:
  `std::math`, `std::str`, `std::num`, `std::list`.

Every name is called with its full path and the module must be imported:

    import std::io;
    import std::math;

    fn main() -> reg i32 {
        std::io::print_i32(std::math::pow(2, 10))
        return(0)
    }

`rgc` finds `std/` through `$RGX_STD`, or next to the compiler executable
(`<dir>/std` or `<dir>/../std`). A source module is compiled together with
your program; modules that import other modules pull them in too.

Strings are `[u8]` slices (`&[u8]`) without a terminator. A function that fills
a buffer takes a `&mut [u8]` and returns how much it wrote.

## std::io (runtime)

| function | |
|---|---|
| `print_i32(reg n: i32)` | prints `n` in decimal and a newline |
| `print(stack s: &[u8])` | writes the bytes of `s` to standard output |
| `println(stack s: &[u8])` | the same, plus a newline |
| `eprint(stack s: &[u8])`, `eprintln(stack s: &[u8])` | the same for standard error |
| `read_line(stack buf: &mut [u8]) -> reg i32` | reads a line from standard input into `buf` without the line break (a carriage return is ignored, bytes beyond the buffer are dropped). Returns the number of bytes stored, or -1 at end of input |

`print` writes the whole slice, so printing a partly filled buffer also prints
the unused bytes; keep the buffer exactly as long as the text when you need that.

## std::process (runtime)

| function | |
|---|---|
| `exit(reg code: i32)` | ends the program with the exit code `code` |

## std::math

All values are `i32`; arithmetic wraps on overflow.

| function | result |
|---|---|
| `abs(x)` | absolute value (the smallest i32 stays negative) |
| `min(a, b)`, `max(a, b)` | smaller / larger |
| `clamp(x, lo, hi)` | `x` limited to `lo..=hi` |
| `sign(x)` | -1, 0 or 1 |
| `is_even(x)`, `is_odd(x)` | 1 or 0 |
| `pow(base, exp)` | `base` to the power `exp`; 0 if `exp` is negative |
| `factorial(n)` | `n!`; wraps for `n > 12` |
| `gcd(a, b)` | greatest common divisor, never negative; `gcd(0, 0)` is 0 |
| `isqrt(n)` | floor of the square root; 0 for `n <= 0` |

## std::str

Functions return 1 or 0 for yes or no. `a`, `b`, `s` are `&[u8]`.

| function | result |
|---|---|
| `len(s)` | length |
| `eq(a, b)` | same bytes |
| `starts_with(s, prefix)`, `ends_with(s, suffix)` | |
| `find(hay, needle)` | index of the first match or -1 (an empty needle is found at 0) |
| `contains(hay, needle)` | |
| `find_byte(s, c)` | index of the first byte equal to `c`, or -1 |
| `count_byte(s, c)` | how many bytes equal `c` |
| `copy_into(dst, src)` | copies as many bytes as fit, returns the count (`dst` is `&mut [u8]`) |
| `fill(dst, c)` | sets every byte of `dst` |
| `reverse(buf)` | reverses `buf` in place |
| `is_digit(c)`, `is_upper(c)`, `is_lower(c)`, `is_alpha(c)`, `is_alnum(c)`, `is_space(c)` | ASCII classes of one byte value |
| `to_upper(c)`, `to_lower(c)` | ASCII case conversion of one byte value |
| `to_upper_into(dst, src)`, `to_lower_into(dst, src)` | convert while copying, return the count |

## std::num

| function | result |
|---|---|
| `parse_i32(s: &[u8], out: &mut i32) -> i32` | parses an optionally signed decimal number: returns 1 and stores the value in `*out`; returns 0 and leaves `*out` alone if the text is empty, has other characters, or does not fit (`-2147483648` is not accepted) |
| `format_i32(n, buf: &mut [u8]) -> i32` | writes `n` in decimal into `buf`, returns the byte count, or 0 if it does not fit (an i32 needs at most 11 bytes) |
| `digits(n)` | number of decimal digits, sign not counted; 1 for 0 |

`parse_i32` takes the whole slice, so pass an array of exactly the text's length
(or a string literal).

## std::list

A singly linked list of `i32` built on `heap` fields. The empty list is `null`.

    import std::list;

    heap l: std::list::Node = std::list::empty()
    l = std::list::push(l, 3)
    l = std::list::push(l, 4)
    if (l != null) {
        std::io::print_i32(std::list::sum(&l))
    }

| function | result |
|---|---|
| `empty() -> heap Node` | the empty list (`null`) |
| `push(heap list, v) -> heap Node` | adds `v` at the front |
| `pop(heap list) -> heap Node` | removes the first element, returns the rest |
| `append(&mut head, v)` | adds `v` at the end |
| `reverse(heap list) -> heap Node` | reverses the list |
| `length(&head)`, `sum(&head)` | |
| `get(&head, i)`, `set(&mut head, i, v)` | element access; `i` outside `0..length` stops the program (exit code 101) |
| `contains(&head, v)`, `index_of(&head, v)` | 1/0, position or -1 |

The functions that borrow the list (`&Node`) cannot take an empty list, since
a borrow needs a value: test `l != null` first. The list owns its nodes and
releases them when it goes out of scope.
