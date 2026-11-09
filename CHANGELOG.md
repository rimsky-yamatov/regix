# Changelog

## Unreleased

- Multi-file programs: `rgc` takes several files; `package`, `import` and `pub`
  are enforced across packages; `rgpm build` compiles all of `src/` and checks
  that each file's package matches its directory.
- Linux heap allocator with size classes and free lists (replaces one page
  per allocation).
- `copy(x)` and move for structs and arrays between `stack` and `heap`.
- `secret` variables: wiped at scope end, before return and tail jumps; direct
  copies, passing, borrowing and returning are rejected.
- Arrays `[i32; N]` and `[u8; N]` (`stack` or `heap`) with literals, repeat
  `[v; N]`, string-literal initialization, `a[i]`, `a.len`, bounds checks
  (exit code 101) and byte-wise `drop`.
- Slice parameters `&[T]` (borrowed, read-only), string literals as `&[u8]`.
- `std::io::print` and `std::io::println` for byte slices.
- `main` can take `stack args: &[&[u8]]` (Linux and Windows).
- Tests: runner supports `// args:` and multi-file cases (`tests/multi/NAME.d`); 203 cases.

## v0.1.0

First release.

- `rgc`: compiler for a small subset of Regix, emitting GAS (Linux x86-64) or
  MASM (Windows x86-64) and assembling with `as`/`ld` or `ml64`/`link`.
- Storage classes `reg`, `stack`, `heap` for variables, parameters and
  return values; `drop(x)` with zero-fill and compile-time use-after-drop
  checks; heap ownership moves.
- C-style structs (inline `i32` and nested struct fields).
- Guaranteed tail calls, `while`, `if`/`else`, `package`/`import`,
  `std::io::print_i32`.
- `rgfmt` (re-indenting formatter) and `rgpm` (`init`, `build`, `run`).
- Test suite written in C (`make test`, `scripts\test.bat`).
- GitHub Actions: CI on Linux and Windows; tagging `v*` publishes a release.

Known limits are listed in `docs/spec.md`. The Windows backend and the
Windows CI/release jobs had not been run on a Windows machine when this
version was cut.
