#ifndef RGX_PARSER_H
#define RGX_PARSER_H
#include "ast.h"

/* Parse a source file. Exits with a diagnostic on the first syntax error. */
Program *parse_program(const char *file, const char *src);

#endif
