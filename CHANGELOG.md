# Changelog

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
