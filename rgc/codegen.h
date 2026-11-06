#ifndef RGC_CODEGEN_H
#define RGC_CODEGEN_H
#include <stdio.h>

#include "abi.h"
#include "ast.h"

/* Write assembly for a checked program: GAS (Intel syntax) for linux-x64,
 * MASM for windows-x64. Includes the process entry stub calling `main`. */
void codegen_program(Program *prog, const Abi *abi, FILE *out);

#endif
