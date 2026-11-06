#ifndef RGC_ABI_H
#define RGC_ABI_H

/* Per-target calling convention and register tables. */
typedef struct {
    const char *name;     /* "linux-x64" or "windows-x64" */
    int windows;          /* emit MASM instead of GAS */
    int nargs;            /* integer argument registers */
    const char *arg64[6];
    const char *arg32[6];
    int npool;            /* callee-saved registers usable for 'reg' variables */
    const char *pool64[8];
    const char *pool32[8];
    int shadow;           /* shadow space bytes the caller reserves around calls */
} Abi;

const Abi *abi_by_name(const char *name);
const Abi *abi_host(void);

#endif
