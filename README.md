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

Status: early work in progress (v0.1).
