# Regix

Regix is a small systems language where the storage class of every value is
spelled out in the source: `reg`, `stack` or `heap`.

```
fn fact(reg n: i32, reg acc: i32) -> reg i32 {
    if (n < 2) {
        return(acc)
    }
    return(fact(n - 1, acc * n))
}
```

Status: v0.1.0, early but working. See [docs/spec.md](docs/spec.md) for the language and its known limits, and [CHANGELOG.md](CHANGELOG.md).

## Tools

| Tool    | Purpose                                              |
|---------|------------------------------------------------------|
| `rgc`   | compiler: `.rgx` to assembly, then `as`/`ld` or `ml64`/`link` |
| `rgfmt` | formatter (re-indent only for now)                   |
| `rgpm`  | package manager: `init`, `build`, `run`              |

## Build

Linux (needs `cc`, `make`, binutils `as` and `ld`):

    make
    make test

Windows (Visual Studio with the C++ tools, from an x64 developer prompt):

    scripts\build.bat
    scripts\test.bat

## Use

    rgc examples/fact.rgx -o fact
    ./fact                 # prints 120

    rgpm init hello && cd hello && rgpm run

More programs are in `examples/` (`hello`, `args`, `sum`, `struct`, `heap`).

    rgc examples/fact.rgx --target windows-x64 -S -o fact.asm   # MASM text only

## Tests

Tests are plain `.rgx` files in `tests/cases` with `// expect-*` directives,
run by a small C program (`tests/run_tests.c`). No scripting language is needed.

## Releases

Pushing a tag like `v0.1.0` builds Linux and Windows binaries in GitHub Actions
and attaches them to a GitHub release.

    git tag v0.1.0
    git push origin v0.1.0

On Windows, `rgc` uses `ml64` and `link`; if they are not on PATH it looks for
Visual Studio with `vswhere` and sets up the environment itself.
