#ifndef RGC_CHECK_H
#define RGC_CHECK_H
#include "abi.h"
#include "ast.h"

/* Resolve names, check types and storage rules, assign register slots.
 * Returns the number of errors reported. */
int check_program(Program *prog, const Abi *abi, int require_main);

#endif
