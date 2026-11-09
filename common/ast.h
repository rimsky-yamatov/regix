#ifndef RGX_AST_H
#define RGX_AST_H

typedef enum { TY_VOID, TY_I32, TY_STRUCT, TY_U8, TY_ARRAY, TY_SLICE, TY_REF } Ty;

/* Extra type information for [T; N] arrays and &[T] slices. u8 exists only as
 * an element type; a slice of slices (&[&[u8]]) is only used for main's args. */
typedef struct {
    Ty elem;     /* TY_I32, TY_U8 or (slices only) TY_SLICE */
    Ty elem2;    /* element of the inner slice when elem == TY_SLICE */
    int alen;    /* arrays: number of elements */
    int rmut;    /* references (TY_REF): 1 for &mut T; elem is TY_I32 or TY_STRUCT */
} ArrInfo;
typedef enum { ST_NONE, ST_REG, ST_STACK, ST_HEAP } Storage;
typedef enum { E_INT, E_VAR, E_NEG, E_BIN, E_CALL, E_FIELD, E_STRUCTLIT,
    E_STR, E_ARRAYLIT, E_ARRAYREP, E_INDEX, E_ADDR, E_LEN, E_DEREF,
    E_NULL, E_ISNULL
} ExprKind;
enum { CP_NONE, CP_COPY, CP_MOVE };
enum { PK_NONE, PK_NULL, PK_MOVE, PK_CALL, PK_TAKE };

typedef enum { OP_ADD, OP_SUB, OP_MUL, OP_LT, OP_GT, OP_LE, OP_GE, OP_EQ, OP_NE } BinOp;
typedef enum {
    S_DECL, S_ASSIGN, S_FASSIGN, S_IASSIGN, S_DASSIGN, S_IF, S_WHILE, S_RETURN, S_EXPR, S_BLOCK, S_DROP
} StmtKind;

typedef struct Func Func;
typedef struct Expr Expr;
typedef struct Stmt Stmt;
typedef struct StructDef StructDef;

/* A variable whose resources must be released when its scope ends. */
typedef struct {
    int idx;          /* first frame slot */
    int bslots;       /* slots a stack value occupies */
    StructDef *sd;    /* struct type, or NULL */
    int is_heap;      /* 1: the slot holds a heap pointer; 0: a stack struct that owns heap fields */
} Release;

typedef struct Block {
    Stmt **stmts;
    int n, cap;
    Stmt **wipe;     /* secret locals to clear at scope end (declaring statements) */
    int nwipe, wipecap;
    Release *rel;    /* resources to release at scope end */
    int nrel, relcap;
} Block;

struct Expr {
    ExprKind kind;
    int line, col;
    long val;        /* E_INT */
    char *name;      /* E_VAR, E_CALL (possibly qualified), E_FIELD (field), E_STRUCTLIT (struct) */
    char *sval;      /* E_STR: decoded bytes */
    int slen;        /* E_STR: byte count */
    BinOp op;        /* E_BIN */
    Expr *l, *r;     /* E_NEG and E_FIELD use l; E_BIN uses l and r */
    Expr **args;     /* E_CALL arguments; E_STRUCTLIT field values */
    int nargs, cap;
    char **fnames;   /* E_STRUCTLIT field names, parallel to args */
    int nfn, fncap;
    /* set by check */
    int reg;         /* variable: register index (reg) or first frame slot (stack/heap) */
    Storage vst;     /* variable storage class */
    int foff;        /* E_FIELD: byte offset inside the struct */
    int dref[8];     /* E_FIELD, E_ADDR: offsets of owned pointers to load on the way (a.next.value) */
    int ndref;
    int lastptr;     /* E_FIELD, E_ADDR: the final field is an owned pointer */
    int bslots;      /* E_FIELD: number of frame slots the stack struct occupies */
    Func *callee;    /* E_CALL */
    StructDef *sdef; /* E_STRUCTLIT */
    Ty vty;          /* E_INDEX, E_ADDR, E_LEN: TY_ARRAY or TY_SLICE of the variable */
    ArrInfo ai;      /* E_INDEX, E_ADDR, E_LEN: array or slice details */
    int notail;      /* E_CALL: an argument borrows a local, so no tail jump */
};

struct Stmt {
    StmtKind kind;
    int line, col;
    char *name;      /* S_DECL, S_ASSIGN, S_DROP */
    Storage storage; /* S_DECL; resolved variable storage for S_ASSIGN, S_DROP */
    Ty ty;           /* S_DECL */
    char *tyname;    /* S_DECL: struct name when ty == TY_STRUCT */
    StructDef *sdef; /* S_DECL: resolved struct */
    Expr *expr;      /* init / rhs / condition / return value / expression */
    Expr *lhs;       /* S_FASSIGN: field expression */
    Block *then_b;   /* S_IF, S_WHILE, S_BLOCK */
    Block *else_b;   /* S_IF, may be NULL */
    int reg;         /* S_DECL, S_ASSIGN, S_DROP: register index or first frame slot */
    int size;        /* S_DECL, S_DROP: size of the value in bytes */
    int bslots;      /* S_DECL, S_DROP: frame slots of a stack value */
    int adopt;       /* S_DECL (heap): initializer is a call returning an owned heap value */
    ArrInfo arr;     /* S_DECL: array details */
    int secret;      /* S_DECL: cleared at scope end; never copied */
    int cpy;         /* S_DECL: CP_COPY or CP_MOVE when initialized from another variable */
    Storage cst;     /* source variable storage for cpy */
    int cidx, cbs;   /* source variable first slot and slot count for cpy */
    Stmt **wipe;     /* S_RETURN: secret locals to clear before leaving */
    int nwipe, wipecap;
    Release *rel;    /* S_RETURN: resources to release before leaving */
    int nrel, relcap;
    int pk;          /* pointer-valued initializer/rhs: PK_NULL, PK_MOVE, PK_CALL, PK_TAKE */
};

typedef struct {
    char *name;
    Ty ty;
    char *tyname;
    StructDef *sdef;
    int offset;      /* set by check */
    int is_heap;     /* owned, nullable pointer to a struct (declared with heap) */
    int line, col;
} Field;

typedef struct Program Program;
struct StructDef {
    Program *unit;   /* file the struct was declared in */
    char *name;
    Field *fields;
    int nfields, fcap;
    int size;        /* bytes, set by check */
    int align;       /* 4, or 8 when the struct holds a pointer; set by check */
    int has_heap;    /* owns heap fields directly or through inline structs; set by check */
    char *fsym;      /* assembler symbol of its release routine; set by check */
    int state;       /* layout progress: 0 new, 1 in progress, 2 done */
    int line, col;
    int is_pub;
};

typedef struct {
    char *name;
    Storage storage;
    Ty ty;
    char *tyname;
    StructDef *sdef;
    int line, col;
    int idx;         /* set by check: register index or frame slot */
    ArrInfo arr;     /* slice parameters */
    int areg;        /* set by check: first argument register used (slices use two) */
} Param;

struct Func {
    Program *unit;   /* file (package, imports) the function belongs to */
    char *name;
    char *qname;     /* package::name, set by check */
    char *sym;       /* assembler symbol, set by check */
    Param *params;
    int nparams, pcap;
    Storage ret_storage;
    Ty ret_ty;
    char *ret_tyname;
    StructDef *ret_sdef;
    Block *body;
    int nregs;       /* callee-saved registers needed, set by check */
    int nslots;      /* 8-byte frame slots needed, set by check */
    int line, col;
    int is_pub;
};

typedef struct {
    char *data;
    int len;
} StrLit;

struct Program {
    const char *file;
    char *package;   /* "a::b" or NULL */
    char **imports;
    int nimports, icap;
    Func **funcs;
    int nfuncs, fcap;
    StructDef **structs;
    int nstructs, scap;
    Program **units;  /* master program only: the linked source files */
    int nunits;
    int uses_heap;   /* set by check: emit the heap runtime */
    int uses_print;  /* set by check: emit std::io::print_i32 */
    int uses_write;  /* set by check: emit std::io::print */
    int uses_writeln; /* set by check: emit std::io::println */
    int uses_panic;  /* set by check: emit the bounds-check failure handler */
    StrLit *strs;    /* string literals passed as slices */
    int nstrs, strcap;
};

#endif
