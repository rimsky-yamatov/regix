#ifndef RGX_LEXER_H
#define RGX_LEXER_H
#include <stddef.h>

typedef enum {
    T_EOF,
    T_IDENT,
    T_INT,
    /* keywords */
    T_KW_FN, T_KW_REG, T_KW_STACK, T_KW_HEAP,
    T_KW_IF, T_KW_ELSE, T_KW_WHILE, T_KW_RETURN,
    T_KW_PACKAGE, T_KW_IMPORT, T_KW_STRUCT, T_KW_PUB,
    /* punctuation */
    T_LPAREN, T_RPAREN, T_LBRACE, T_RBRACE,
    T_COMMA, T_COLON, T_COLONCOLON, T_SEMI, T_ARROW, T_ASSIGN,
    T_PLUS, T_MINUS, T_STAR,
    T_LT, T_GT, T_LE, T_GE, T_EQ, T_NE
} TokKind;

typedef struct {
    TokKind kind;
    int line, col;
    const char *text; /* points into the source buffer */
    int len;
    long ival;        /* T_INT */
} Token;

/* Tokenize `src`. The result ends with T_EOF. Exits on lexical errors. */
Token *lex(const char *file, const char *src, size_t *count);

const char *tok_name(TokKind k);

#endif
