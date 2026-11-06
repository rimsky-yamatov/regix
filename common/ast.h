#ifndef RGX_AST_H
#define RGX_AST_H

typedef enum { TY_VOID, TY_I32 } Ty;
typedef enum { ST_NONE, ST_REG, ST_STACK, ST_HEAP } Storage;
typedef enum { E_INT, E_VAR, E_NEG, E_BIN, E_CALL } ExprKind;
typedef enum { OP_ADD, OP_SUB, OP_MUL, OP_LT, OP_GT, OP_LE, OP_GE, OP_EQ, OP_NE } BinOp;
typedef enum { S_DECL, S_ASSIGN, S_IF, S_WHILE, S_RETURN, S_EXPR, S_BLOCK } StmtKind;

typedef struct Func Func;
typedef struct Expr Expr;
typedef struct Stmt Stmt;

typedef struct Block {
    Stmt **stmts;
    int n, cap;
} Block;

struct Expr {
    ExprKind kind;
    int line, col;
    long val;        /* E_INT */
    char *name;      /* E_VAR, E_CALL (possibly qualified) */
    BinOp op;        /* E_BIN */
    Expr *l, *r;     /* E_NEG uses l; E_BIN uses l and r */
    Expr **args;     /* E_CALL */
    int nargs, cap;
    int reg;         /* E_VAR: register slot, set by check */
    Func *callee;    /* E_CALL: set by check */
};

struct Stmt {
    StmtKind kind;
    int line, col;
    char *name;      /* S_DECL, S_ASSIGN */
    Storage storage; /* S_DECL */
    Ty ty;           /* S_DECL */
    Expr *expr;      /* init / rhs / condition / return value / expression */
    Block *then_b;   /* S_IF, S_WHILE, S_BLOCK */
    Block *else_b;   /* S_IF, may be NULL */
    int reg;         /* S_DECL, S_ASSIGN: register slot, set by check */
};

typedef struct {
    char *name;
    Storage storage;
    Ty ty;
    int line, col;
} Param;

struct Func {
    char *name;
    char *qname;     /* package::name, set by check */
    char *sym;       /* assembler symbol, set by check */
    Param *params;
    int nparams, pcap;
    Storage ret_storage;
    Ty ret_ty;
    Block *body;
    int nregs;       /* registers needed, set by check */
    int line, col;
    int is_pub;
};

typedef struct {
    const char *file;
    char *package;   /* "a::b" or NULL */
    char **imports;
    int nimports, icap;
    Func **funcs;
    int nfuncs, fcap;
} Program;

#endif
