#ifndef RGC_CHECK_H
#define RGC_CHECK_H
#include "abi.h"
#include "ast.h"

/* Combine parsed source files into one program (functions and structs are
 * tagged with their file). The files array must outlive the result. */
Program *link_units(Program **files, int n);

/* Resolve names, check types and storage rules, assign register slots.
 * `prog` comes from link_units. Returns the number of errors reported.
 * Exactly one `main` must exist across all files. */
int check_program(Program *prog, const Abi *abi, int require_main);

/* True for modules implemented by the runtime (std::io, std::process); other
 * std::* modules are Regix source files loaded from the std directory. */
int is_builtin_module(const char *name);

#endif
