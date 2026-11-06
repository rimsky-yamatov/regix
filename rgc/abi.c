#include "abi.h"

#include <string.h>

static const Abi linux_x64 = {
    "linux-x64", 0,
    6,
    {"rdi", "rsi", "rdx", "rcx", "r8", "r9"},
    {"edi", "esi", "edx", "ecx", "r8d", "r9d"},
    5,
    {"rbx", "r12", "r13", "r14", "r15"},
    {"ebx", "r12d", "r13d", "r14d", "r15d"},
    0,
};

static const Abi windows_x64 = {
    "windows-x64", 1,
    4,
    {"rcx", "rdx", "r8", "r9"},
    {"ecx", "edx", "r8d", "r9d"},
    7,
    {"rbx", "rsi", "rdi", "r12", "r13", "r14", "r15"},
    {"ebx", "esi", "edi", "r12d", "r13d", "r14d", "r15d"},
    32,
};

const Abi *abi_by_name(const char *name)
{
    if (strcmp(name, "linux-x64") == 0)
        return &linux_x64;
    if (strcmp(name, "windows-x64") == 0)
        return &windows_x64;
    return NULL;
}

const Abi *abi_host(void)
{
#ifdef _WIN32
    return &windows_x64;
#else
    return &linux_x64;
#endif
}
