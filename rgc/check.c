#include "check.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "util.h"

typedef struct {
    const char *name;
    Storage st;
    int idx;      /* register index (reg) or first frame slot (stack/heap) */
    Ty ty;
    StructDef *sd;
    ArrInfo arr;
    int size;     /* bytes of the value */
    int slots;    /* frame slots occupied (stack: whole value, heap: the pointer) */
    int dropped;  /* dropped or moved: no longer usable */
    int moved;
} Local;

typedef struct {
    Program *prog;
    const Abi *abi;
    Func *fn;
    Local *loc;
    int n, cap;
    int nreg, maxreg, nslot, maxslot;
    int heap_ok;  /* next call may yield an owned heap value */
} Ctx;

#define HEAP_MAX_BYTES 4096

enum { B_PRINT_I32, B_PRINT, B_PRINTLN, N_BUILTINS };
static Func builtins[N_BUILTINS];

static int esz(Ty e) { return e == TY_I32 ? 4 : (e == TY_U8 ? 1 : 16); }

static void err(Ctx *c, int line, int col, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    rgx_error(c->prog->file, line, col, "%s", buf);
}

static char *mangle(Program *p, const char *name)
{
    if (!p->package)
        return xsprintf("rgx_%s", name);
    char *pkg = xstrdup(p->package);
    char *out = xmalloc(strlen(pkg) + 1);
    size_t j = 0;
    for (size_t i = 0; pkg[i]; i++) {
        if (pkg[i] == ':') {
            if (pkg[i + 1] == ':') {
                out[j++] = '_';
                i++;
            }
        } else {
            out[j++] = pkg[i];
        }
    }
    out[j] = '\0';
    return xsprintf("rgx_%s__%s", out, name);
}

/* ---- structs ---- */

static StructDef *find_struct(Program *p, const char *name)
{
    for (int i = 0; i < p->nstructs; i++)
        if (strcmp(p->structs[i]->name, name) == 0)
            return p->structs[i];
    return NULL;
}

static Field *find_field(StructDef *sd, const char *name)
{
    for (int i = 0; i < sd->nfields; i++)
        if (strcmp(sd->fields[i].name, name) == 0)
            return &sd->fields[i];
    return NULL;
}

static void layout_struct(Ctx *c, StructDef *sd)
{
    if (sd->state == 2)
        return;
    if (sd->state == 1) {
        err(c, sd->line, sd->col, "struct '%s' contains itself; use a 'heap' field "
            "(not implemented yet) or remove the cycle", sd->name);
        return;
    }
    sd->state = 1;
    int off = 0;
    if (sd->nfields == 0)
        err(c, sd->line, sd->col, "struct '%s' has no fields", sd->name);
    for (int i = 0; i < sd->nfields; i++) {
        Field *f = &sd->fields[i];
        for (int j = 0; j < i; j++)
            if (strcmp(sd->fields[j].name, f->name) == 0)
                err(c, f->line, f->col, "field '%s' is declared twice in struct '%s'",
                    f->name, sd->name);
        f->offset = off;
        if (f->ty == TY_STRUCT) {
            f->sdef = find_struct(c->prog, f->tyname);
            if (!f->sdef) {
                err(c, f->line, f->col, "unknown type '%s'", f->tyname);
                off += 4;
                continue;
            }
            layout_struct(c, f->sdef);
            off += f->sdef->size;
        } else {
            off += 4;
        }
    }
    sd->size = off;
    sd->state = 2;
}

/* ---- functions ---- */

static int imported(Program *p, const char *module)
{
    for (int i = 0; i < p->nimports; i++)
        if (strcmp(p->imports[i], module) == 0)
            return 1;
    return 0;
}

static Func *find_func(Ctx *c, Expr *e)
{
    Program *p = c->prog;
    for (int i = 0; i < p->nfuncs; i++) {
        Func *f = p->funcs[i];
        if (strcmp(f->name, e->name) == 0 || strcmp(f->qname, e->name) == 0)
            return f;
    }
    for (int i = 0; i < N_BUILTINS; i++) {
        if (strcmp(e->name, builtins[i].qname) != 0)
            continue;
        if (!imported(p, "std::io"))
            err(c, e->line, e->col, "'std::io' is not imported (add 'import std::io;')");
        if (i == B_PRINT_I32)
            p->uses_print = 1;
        else if (i == B_PRINT)
            p->uses_write = 1;
        else
            p->uses_writeln = 1;
        return &builtins[i];
    }
    return NULL;
}

static void init_builtin(int i, const char *name, Ty pty)
{
    Func *f = &builtins[i];
    memset(f, 0, sizeof *f);
    f->name = (char *)name;
    f->qname = xsprintf("std::io::%s", name);
    f->sym = xsprintf("rgx_std_io_%s", name);
    f->ret_ty = TY_VOID;
    f->nparams = 1;
    f->params = xcalloc(sizeof(Param));
    f->params[0].name = "s";
    f->params[0].ty = pty;
    if (pty == TY_SLICE) {
        f->params[0].storage = ST_STACK;
        f->params[0].arr.elem = TY_U8;
    } else {
        f->params[0].storage = ST_REG;
    }
}

/* ---- locals ---- */

static Local *lookup(Ctx *c, const char *name)
{
    for (int i = c->n - 1; i >= 0; i--)
        if (strcmp(c->loc[i].name, name) == 0)
            return &c->loc[i];
    return NULL;
}

static int declare(Ctx *c, const char *name, Storage st, Ty ty, StructDef *sd, const ArrInfo *ai)
{
    Local l;
    memset(&l, 0, sizeof l);
    l.name = name;
    l.st = st;
    l.ty = ty;
    l.sd = sd;
    if (ai)
        l.arr = *ai;
    if (ty == TY_ARRAY)
        l.size = l.arr.alen * esz(l.arr.elem);
    else if (ty == TY_SLICE)
        l.size = 16;
    else if (ty == TY_STRUCT && sd)
        l.size = sd->size;
    else
        l.size = 4;
    if (st == ST_STACK)
        l.slots = ty == TY_SLICE ? 2 : (l.size + 7) / 8;
    else if (st == ST_HEAP)
        l.slots = 1;
    if (st == ST_REG) {
        l.idx = c->nreg++;
        if (c->nreg > c->maxreg)
            c->maxreg = c->nreg;
    } else {
        l.idx = c->nslot;
        c->nslot += l.slots;
        if (c->nslot > c->maxslot)
            c->maxslot = c->nslot;
    }
    VEC_PUSH(c->loc, c->n, c->cap, l);
    return l.idx;
}

static const char *gone(Local *l) { return l->moved ? "moved" : "dropped"; }

/* ---- drop state (flow-sensitive) ---- */

static char *snap(Ctx *c, int count)
{
    char *s = xmalloc((size_t)count + 1);
    for (int i = 0; i < count; i++)
        s[i] = (char)c->loc[i].dropped;
    return s;
}

static void restore(Ctx *c, const char *s, int count)
{
    for (int i = 0; i < count; i++)
        c->loc[i].dropped = s[i];
}

static int first_diff(const char *a, const char *b, int count)
{
    for (int i = 0; i < count; i++)
        if (a[i] != b[i])
            return i;
    return -1;
}

/* ---- arrays and slices ---- */

/* Record on `e` which array or slice variable it refers to. */
static void bind_seq(Expr *e, Local *l)
{
    e->reg = l->idx;
    e->vst = l->st;
    e->vty = l->ty;
    e->ai = l->arr;
    e->bslots = l->slots;
}

static Local *seq_local(Ctx *c, Expr *v, const char *what)
{
    if (v->kind != E_VAR) {
        err(c, v->line, v->col, "%s must be a variable", what);
        return NULL;
    }
    Local *l = lookup(c, v->name);
    if (!l) {
        err(c, v->line, v->col, "undefined variable '%s'", v->name);
        return NULL;
    }
    if (l->dropped) {
        err(c, v->line, v->col, "use of %s variable '%s'", gone(l), v->name);
        return NULL;
    }
    if (l->ty != TY_ARRAY && l->ty != TY_SLICE) {
        err(c, v->line, v->col, "'%s' is not an array or slice", v->name);
        return NULL;
    }
    return l;
}

static Ty check_expr(Ctx *c, Expr *e, int allow_void);

/* e is E_INDEX: validate the base and index; allow_seq permits an element that
 * is itself a slice (only for passing as an argument). */
static int check_index(Ctx *c, Expr *e, int allow_seq)
{
    Local *l = seq_local(c, e->l, "the indexed value");
    check_expr(c, e->r, 0);
    c->prog->uses_panic = 1;
    if (!l)
        return 0;
    if (l->arr.elem == TY_SLICE && !allow_seq) {
        err(c, e->line, e->col, "an element of '%s' is itself a slice; pass it as an argument",
            l->name);
        return 0;
    }
    bind_seq(e, l);
    if (l->ty == TY_ARRAY && e->r->kind == E_INT && e->r->val >= l->arr.alen) {
        err(c, e->r->line, e->r->col, "index %ld is out of bounds for an array of length %d",
            e->r->val, l->arr.alen);
        return 0;
    }
    return 1;
}

static int add_string(Program *p, const char *data, int len)
{
    StrLit s;
    s.data = xstrndup(data, (size_t)len);
    s.len = len;
    VEC_PUSH(p->strs, p->nstrs, p->strcap, s);
    return p->nstrs - 1;
}

/* Argument for a slice parameter. */
static void check_slice_arg(Ctx *c, Expr *call, Expr *a, Param *p)
{
    if (a->kind == E_STR) {
        if (p->arr.elem != TY_U8)
            err(c, a->line, a->col, "a string literal is a [u8] slice");
        a->val = add_string(c->prog, a->sval, a->slen);
        return;
    }
    if (a->kind == E_VAR) {
        /* a slice variable is passed on as it is */
        Expr *v = xcalloc(sizeof *v);
        *v = *a;
        a->kind = E_ADDR;
        a->l = v;
    }
    if (a->kind == E_ADDR) {
        Local *l = seq_local(c, a->l, "a borrowed value");
        if (!l)
            return;
        if (l->arr.elem != p->arr.elem || l->arr.elem2 != p->arr.elem2) {
            err(c, a->line, a->col, "type mismatch for slice parameter '%s'", p->name);
            return;
        }
        bind_seq(a, l);
        if (l->ty == TY_ARRAY)
            call->notail = 1; /* the array dies with this frame: no tail jump */
        return;
    }
    if (a->kind == E_INDEX) {
        if (!check_index(c, a, 1))
            return;
        if (a->ai.elem != TY_SLICE || a->ai.elem2 != p->arr.elem || p->arr.elem == TY_SLICE) {
            err(c, a->line, a->col, "type mismatch for slice parameter '%s'", p->name);
            return;
        }
        Expr *ix = a->r;
        int simple = ix->kind == E_INT ||
                     (ix->kind == E_VAR && ix->vst != ST_HEAP && ix->vst != ST_NONE);
        if (!simple)
            err(c, ix->line, ix->col,
                "the index of a slice element passed as an argument must be a constant or variable");
        return;
    }
    err(c, a->line, a->col, "the argument for slice parameter '%s' must be &array, a slice "
        "variable or a string literal", p->name);
}

/* ---- expressions ---- */

static int resolve_field(Ctx *c, Expr *e, Local **root, int *off, Ty *ty, StructDef **sd)
{
    if (e->kind == E_VAR) {
        Local *l = lookup(c, e->name);
        if (!l) {
            err(c, e->line, e->col, "undefined variable '%s'", e->name);
            return 0;
        }
        if (l->dropped) {
            err(c, e->line, e->col, "use of %s variable '%s'", gone(l), e->name);
            return 0;
        }
        if (l->ty != TY_STRUCT) {
            err(c, e->line, e->col, "'%s' is not a struct", e->name);
            return 0;
        }
        *root = l;
        *off = 0;
        *ty = TY_STRUCT;
        *sd = l->sd;
        return 1;
    }
    if (e->kind == E_FIELD) {
        if (!resolve_field(c, e->l, root, off, ty, sd))
            return 0;
        if (*ty != TY_STRUCT) {
            err(c, e->line, e->col, "cannot access field '%s' of an i32", e->name);
            return 0;
        }
        Field *f = find_field(*sd, e->name);
        if (!f) {
            err(c, e->line, e->col, "struct '%s' has no field '%s'", (*sd)->name, e->name);
            return 0;
        }
        *off += f->offset;
        *ty = f->ty;
        *sd = f->sdef;
        return 1;
    }
    err(c, e->line, e->col, "expected a variable or a field");
    return 0;
}

static void annotate_field(Expr *e, Local *root, int off)
{
    e->reg = root->idx;
    e->vst = root->st;
    e->foff = off;
    e->bslots = root->slots;
}

static void check_lit(Ctx *c, Expr *lit, StructDef *sd)
{
    if (strcmp(lit->name, sd->name) != 0)
        err(c, lit->line, lit->col, "expected a '%s' value but found a '%s' literal",
            sd->name, lit->name);
    lit->sdef = sd;
    for (int i = 0; i < lit->nargs; i++) {
        Field *f = find_field(sd, lit->fnames[i]);
        for (int j = 0; j < i; j++)
            if (strcmp(lit->fnames[j], lit->fnames[i]) == 0)
                err(c, lit->args[i]->line, lit->args[i]->col,
                    "field '%s' is initialized twice", lit->fnames[i]);
        if (!f) {
            err(c, lit->args[i]->line, lit->args[i]->col,
                "struct '%s' has no field '%s'", sd->name, lit->fnames[i]);
            continue;
        }
        if (f->ty == TY_STRUCT) {
            if (lit->args[i]->kind == E_STRUCTLIT && f->sdef)
                check_lit(c, lit->args[i], f->sdef);
            else
                err(c, lit->args[i]->line, lit->args[i]->col,
                    "field '%s' needs a '%s' struct literal", f->name, f->tyname);
        } else {
            check_expr(c, lit->args[i], 0);
        }
    }
    for (int i = 0; i < sd->nfields; i++) {
        int found = 0;
        for (int j = 0; j < lit->nargs; j++)
            if (strcmp(lit->fnames[j], sd->fields[i].name) == 0)
                found = 1;
        if (!found)
            err(c, lit->line, lit->col, "missing field '%s' in '%s' literal",
                sd->fields[i].name, sd->name);
    }
}

/* Argument for a 'heap' parameter: a heap variable of the same type, moved. */
static void check_move_arg(Ctx *c, Expr *a, Param *p)
{
    if (a->kind != E_VAR) {
        err(c, a->line, a->col, "the argument for 'heap' parameter '%s' must be a heap variable",
            p->name);
        return;
    }
    Local *l = lookup(c, a->name);
    if (!l) {
        err(c, a->line, a->col, "undefined variable '%s'", a->name);
        return;
    }
    if (l->dropped) {
        err(c, a->line, a->col, "use of %s variable '%s'", gone(l), a->name);
        return;
    }
    if (l->st != ST_HEAP) {
        err(c, a->line, a->col, "'%s' is not a 'heap' variable; only heap values can be moved",
            a->name);
        return;
    }
    if (l->ty != p->ty || l->sd != p->sdef) {
        err(c, a->line, a->col, "type mismatch for parameter '%s'", p->name);
        return;
    }
    a->reg = l->idx;
    a->vst = ST_HEAP;
    l->dropped = 1;
    l->moved = 1;
}

static Ty check_expr(Ctx *c, Expr *e, int allow_void)
{
    switch (e->kind) {
    case E_INT:
        return TY_I32;
    case E_VAR: {
        Local *l = lookup(c, e->name);
        if (!l) {
            err(c, e->line, e->col, "undefined variable '%s'", e->name);
            return TY_I32;
        }
        if (l->dropped)
            err(c, e->line, e->col, "use of %s variable '%s'", gone(l), e->name);
        if (l->ty == TY_STRUCT)
            err(c, e->line, e->col, "struct variable '%s' cannot be used as a value; "
                "use one of its fields", e->name);
        if (l->ty == TY_ARRAY || l->ty == TY_SLICE)
            err(c, e->line, e->col, "'%s' cannot be used as a value; index it, use '%s.len', "
                "or borrow it with &%s", e->name, e->name, e->name);
        e->reg = l->idx;
        e->vst = l->st;
        return TY_I32;
    }
    case E_FIELD: {
        if (e->l->kind == E_VAR) {
            Local *l = lookup(c, e->l->name);
            if (l && (l->ty == TY_ARRAY || l->ty == TY_SLICE)) {
                if (strcmp(e->name, "len") != 0) {
                    err(c, e->line, e->col, "arrays and slices have only the field 'len'");
                    return TY_I32;
                }
                if (l->dropped)
                    err(c, e->line, e->col, "use of %s variable '%s'", gone(l), l->name);
                e->kind = E_LEN;
                bind_seq(e, l);
                return TY_I32;
            }
        }
        Local *root;
        int off;
        Ty ty;
        StructDef *sd;
        if (resolve_field(c, e, &root, &off, &ty, &sd)) {
            if (ty == TY_STRUCT)
                err(c, e->line, e->col, "field '%s' is a struct; use one of its fields", e->name);
            annotate_field(e, root, off);
        }
        return TY_I32;
    }
    case E_INDEX:
        check_index(c, e, 0);
        return TY_I32;
    case E_ADDR:
        err(c, e->line, e->col, "a borrow (&) can only be passed as a call argument");
        return TY_I32;
    case E_STR:
        err(c, e->line, e->col, "a string literal can only initialize a [u8; N] array or be "
            "passed as a slice");
        return TY_I32;
    case E_ARRAYLIT:
    case E_ARRAYREP:
        err(c, e->line, e->col, "an array literal is only allowed as a variable initializer");
        return TY_I32;
    case E_STRUCTLIT:
        err(c, e->line, e->col, "a struct literal is only allowed as a variable initializer");
        return TY_I32;
    case E_LEN:
        return TY_I32;
    case E_NEG:
        check_expr(c, e->l, 0);
        return TY_I32;
    case E_BIN:
        check_expr(c, e->l, 0);
        check_expr(c, e->r, 0);
        return TY_I32;
    case E_CALL: {
        int heap_ok = c->heap_ok;
        c->heap_ok = 0;
        Func *f = find_func(c, e);
        for (int i = 0; i < e->nargs; i++) {
            Param *p = (f && i < f->nparams) ? &f->params[i] : NULL;
            if (p && p->ty == TY_SLICE)
                check_slice_arg(c, e, e->args[i], p);
            else if (p && p->storage == ST_HEAP)
                check_move_arg(c, e->args[i], p);
            else
                check_expr(c, e->args[i], 0);
        }
        if (!f) {
            err(c, e->line, e->col, "undefined function '%s'", e->name);
            return TY_I32;
        }
        e->callee = f;
        if (e->nargs != f->nparams)
            err(c, e->line, e->col, "function '%s' takes %d argument(s) but %d given",
                f->name, f->nparams, e->nargs);
        if (f->ret_ty == TY_VOID && !allow_void) {
            err(c, e->line, e->col, "function '%s' returns void; its value cannot be used",
                f->name);
            return TY_I32;
        }
        if (f->ret_storage == ST_HEAP && !heap_ok)
            err(c, e->line, e->col, "the result of '%s' is a 'heap' value owned by the caller: "
                "bind it with a 'heap' declaration or return it", f->name);
        return f->ret_ty;
    }
    }
    return TY_I32;
}

/* ---- statements ---- */

static void check_block(Ctx *c, Block *b);

static int always_returns(Block *b)
{
    if (b->n == 0)
        return 0;
    Stmt *s = b->stmts[b->n - 1];
    switch (s->kind) {
    case S_RETURN: return 1;
    case S_BLOCK: return always_returns(s->then_b);
    case S_IF: return s->else_b && always_returns(s->then_b) && always_returns(s->else_b);
    default: return 0;
    }
}

static int same_type(Ty a, StructDef *sa, Ty b, StructDef *sb)
{
    return a == b && sa == sb;
}

static void check_array_init(Ctx *c, Stmt *s)
{
    Expr *x = s->expr;
    int n = s->arr.alen;
    if (x->kind == E_ARRAYLIT) {
        if (x->nargs != n)
            err(c, x->line, x->col, "the array holds %d elements but the literal has %d", n,
                x->nargs);
        for (int i = 0; i < x->nargs; i++)
            check_expr(c, x->args[i], 0);
    } else if (x->kind == E_ARRAYREP) {
        if (x->val != n)
            err(c, x->line, x->col, "the array holds %d elements but the repeat count is %ld",
                n, x->val);
        check_expr(c, x->l, 0);
    } else if (x->kind == E_STR) {
        if (s->arr.elem != TY_U8)
            err(c, x->line, x->col, "a string literal initializes a [u8; N] array");
        else if (x->slen != n)
            err(c, x->line, x->col, "the string has %d bytes but the array holds %d", x->slen, n);
    } else {
        err(c, x->line, x->col,
            "an array needs an array literal [a, b], a repeat [v; N] or a string literal");
    }
}

static void check_decl(Ctx *c, Stmt *s)
{
    StructDef *sd = NULL;
    if (s->ty == TY_STRUCT) {
        sd = find_struct(c->prog, s->tyname);
        if (!sd)
            err(c, s->line, s->col, "unknown type '%s'", s->tyname);
    }
    s->sdef = sd;
    if (s->ty == TY_SLICE)
        err(c, s->line, s->col, "slice variables are not supported; slices exist as parameters");
    if ((s->ty == TY_STRUCT || s->ty == TY_ARRAY) && s->storage == ST_REG)
        err(c, s->line, s->col,
            "%s variable '%s' cannot be 'reg'; it lives in 'stack' or 'heap'",
            s->ty == TY_ARRAY ? "array" : "struct", s->name);
    int bytes = s->ty == TY_ARRAY ? s->arr.alen * esz(s->arr.elem) : (sd ? sd->size : 4);
    if (s->storage == ST_HEAP && bytes > HEAP_MAX_BYTES)
        err(c, s->line, s->col, "heap values larger than %d bytes are not supported",
            HEAP_MAX_BYTES);

    if (!s->expr) {
        err(c, s->line, s->col, "variable '%s' needs an initializer", s->name);
    } else if (s->ty == TY_ARRAY) {
        check_array_init(c, s);
    } else if (s->ty == TY_SLICE) {
        /* already reported */
    } else if (s->ty == TY_STRUCT) {
        if (!sd) {
            /* already reported */
        } else if (s->expr->kind == E_STRUCTLIT) {
            check_lit(c, s->expr, sd);
        } else if (s->expr->kind == E_CALL && s->storage == ST_HEAP) {
            c->heap_ok = 1;
            check_expr(c, s->expr, 0);
            Func *f = s->expr->callee;
            if (f && f->ret_storage == ST_HEAP && same_type(f->ret_ty, f->ret_sdef, s->ty, sd))
                s->adopt = 1;
            else if (f)
                err(c, s->expr->line, s->expr->col,
                    "'%s' does not return a 'heap %s'", f->name, sd->name);
        } else {
            err(c, s->expr->line, s->expr->col,
                "a struct variable needs a struct literal (or, for 'heap', a call returning one)");
        }
    } else if (s->expr->kind == E_CALL && s->storage == ST_HEAP) {
        c->heap_ok = 1;
        check_expr(c, s->expr, 0);
        Func *f = s->expr->callee;
        if (f && f->ret_storage == ST_HEAP) {
            if (f->ret_ty != TY_I32)
                err(c, s->expr->line, s->expr->col, "type mismatch: '%s' returns a struct",
                    f->name);
            s->adopt = 1;
        }
    } else {
        check_expr(c, s->expr, 0);
    }

    s->reg = declare(c, s->name, s->storage, s->ty, sd, &s->arr);
    Local *l = &c->loc[c->n - 1];
    s->size = l->size;
    s->bslots = l->slots;
    if (s->storage == ST_HEAP)
        c->prog->uses_heap = 1;
}

static void check_return(Ctx *c, Stmt *s)
{
    Func *f = c->fn;
    if (f->ret_ty == TY_VOID) {
        if (s->expr)
            err(c, s->line, s->col, "void function '%s' cannot return a value", f->name);
    } else if (!s->expr) {
        err(c, s->line, s->col, "function '%s' must return a value", f->name);
    }
    if (s->expr && f->ret_ty != TY_VOID) {
        Expr *x = s->expr;
        if (f->ret_storage == ST_HEAP) {
            if (x->kind == E_VAR) {
                Local *l = lookup(c, x->name);
                if (!l) {
                    err(c, x->line, x->col, "undefined variable '%s'", x->name);
                } else if (l->dropped) {
                    err(c, x->line, x->col, "use of %s variable '%s'", gone(l), x->name);
                } else if (l->st != ST_HEAP || l->ty == TY_ARRAY) {
                    err(c, x->line, x->col, "'%s' is not a movable 'heap' value", x->name);
                } else if (!same_type(l->ty, l->sd, f->ret_ty, f->ret_sdef)) {
                    err(c, x->line, x->col, "type mismatch in return value");
                } else {
                    x->reg = l->idx;
                    x->vst = ST_HEAP;
                    l->dropped = l->moved = 1; /* ownership moves to the caller */
                }
            } else if (x->kind == E_CALL) {
                c->heap_ok = 1;
                check_expr(c, x, 0);
                Func *g = x->callee;
                if (g && (g->ret_storage != ST_HEAP ||
                          !same_type(g->ret_ty, g->ret_sdef, f->ret_ty, f->ret_sdef)))
                    err(c, x->line, x->col, "'%s' does not return the same 'heap' type",
                        g->name);
            } else {
                err(c, x->line, x->col, "a function returning a 'heap' value must return a "
                    "heap variable or a call that returns one");
            }
        } else {
            check_expr(c, x, 0);
        }
    }
    for (int i = 0; i < c->n; i++)
        if (c->loc[i].st == ST_HEAP && !c->loc[i].dropped)
            VEC_PUSH(s->rel, s->nrel, s->relcap, c->loc[i].idx);
}

static void check_stmt(Ctx *c, Stmt *s)
{
    switch (s->kind) {
    case S_DECL:
        check_decl(c, s);
        break;
    case S_ASSIGN: {
        Local *l = lookup(c, s->name);
        if (!l) {
            err(c, s->line, s->col, "undefined variable '%s'", s->name);
        } else {
            if (l->dropped)
                err(c, s->line, s->col, "assignment to %s variable '%s'", gone(l), s->name);
            if (l->ty == TY_STRUCT || l->ty == TY_ARRAY || l->ty == TY_SLICE)
                err(c, s->line, s->col, "cannot assign a whole %s; assign its %s",
                    l->ty == TY_STRUCT ? "struct" : "array or slice",
                    l->ty == TY_STRUCT ? "fields" : "elements");
            s->reg = l->idx;
            s->storage = l->st;
        }
        check_expr(c, s->expr, 0);
        break;
    }
    case S_FASSIGN: {
        Local *root;
        int off;
        Ty ty;
        StructDef *sd;
        if (resolve_field(c, s->lhs, &root, &off, &ty, &sd)) {
            if (ty == TY_STRUCT)
                err(c, s->line, s->col, "cannot assign a whole struct; assign its fields");
            annotate_field(s->lhs, root, off);
        }
        check_expr(c, s->expr, 0);
        break;
    }
    case S_IASSIGN:
        if (check_index(c, s->lhs, 0) && s->lhs->vty == TY_SLICE)
            err(c, s->line, s->col, "slices are read-only; assign through the array itself");
        check_expr(c, s->expr, 0);
        break;
    case S_DROP: {
        Local *l = lookup(c, s->name);
        if (!l) {
            err(c, s->line, s->col, "undefined variable '%s'", s->name);
        } else if (l->dropped) {
            err(c, s->line, s->col, "variable '%s' is already %s", s->name, gone(l));
        } else if (l->ty == TY_SLICE) {
            err(c, s->line, s->col, "'%s' is a borrowed slice and cannot be dropped", s->name);
        } else {
            s->reg = l->idx;
            s->storage = l->st;
            s->ty = l->ty;
            s->size = l->size;
            s->bslots = l->slots;
            l->dropped = 1;
        }
        break;
    }
    case S_IF: {
        check_expr(c, s->expr, 0);
        int n0 = c->n;
        char *s0 = snap(c, n0);
        check_block(c, s->then_b);
        char *s1 = snap(c, n0);
        restore(c, s0, n0);
        if (s->else_b)
            check_block(c, s->else_b);
        char *s2 = snap(c, n0);
        int r1 = always_returns(s->then_b);
        int r2 = s->else_b ? always_returns(s->else_b) : 0;
        if (r1 && !r2) {
            /* keep the else-side state */
        } else if (r2 && !r1) {
            restore(c, s1, n0);
        } else if (!r1 && !r2) {
            int d = first_diff(s1, s2, n0);
            if (d >= 0) {
                err(c, s->line, s->col,
                    "variable '%s' is %s in only one branch of this 'if'", c->loc[d].name,
                    gone(&c->loc[d]));
                restore(c, s1, n0);
            }
        }
        break;
    }
    case S_WHILE: {
        check_expr(c, s->expr, 0);
        int n0 = c->n;
        char *s0 = snap(c, n0);
        check_block(c, s->then_b);
        char *s1 = snap(c, n0);
        int d = first_diff(s0, s1, n0);
        if (d >= 0)
            err(c, s->line, s->col, "variable '%s' is %s inside a loop", c->loc[d].name,
                gone(&c->loc[d]));
        restore(c, s0, n0);
        break;
    }
    case S_RETURN:
        check_return(c, s);
        break;
    case S_EXPR:
        check_expr(c, s->expr, 1);
        break;
    case S_BLOCK:
        check_block(c, s->then_b);
        break;
    }
}

static void check_block(Ctx *c, Block *b)
{
    int saved_n = c->n, saved_reg = c->nreg, saved_slot = c->nslot;
    for (int i = 0; i < b->n; i++)
        check_stmt(c, b->stmts[i]);
    for (int i = saved_n; i < c->n; i++)
        if (c->loc[i].st == ST_HEAP && !c->loc[i].dropped)
            VEC_PUSH(b->rel, b->nrel, b->relcap, c->loc[i].idx);
    c->n = saved_n;
    c->nreg = saved_reg;
    c->nslot = saved_slot;
}

static void check_func(Ctx *c, Func *f)
{
    c->fn = f;
    c->n = 0;
    c->nreg = c->maxreg = c->nslot = c->maxslot = 0;
    c->heap_ok = 0;

    if (f->ret_ty == TY_STRUCT) {
        f->ret_sdef = find_struct(c->prog, f->ret_tyname);
        if (!f->ret_sdef)
            err(c, f->line, f->col, "unknown type '%s'", f->ret_tyname);
        if (f->ret_storage != ST_HEAP)
            err(c, f->line, f->col, "a function returning a struct must return 'heap %s'",
                f->ret_tyname);
    }
    int areg = 0;
    for (int i = 0; i < f->nparams; i++) {
        Param *p = &f->params[i];
        if (p->ty == TY_STRUCT) {
            p->sdef = find_struct(c->prog, p->tyname);
            if (!p->sdef)
                err(c, p->line, p->col, "unknown type '%s'", p->tyname);
            if (p->storage != ST_HEAP)
                err(c, p->line, p->col,
                    "struct parameter '%s' must be 'heap' (the value moves into the callee)",
                    p->name);
        }
        if (p->ty == TY_ARRAY)
            err(c, p->line, p->col,
                "array parameter '%s' is not supported; pass a slice (&[T]) instead", p->name);
        if (p->ty == TY_SLICE && p->storage != ST_STACK)
            err(c, p->line, p->col, "slice parameter '%s' must be 'stack'", p->name);
        p->areg = areg;
        areg += p->ty == TY_SLICE ? 2 : 1;
        p->idx = declare(c, p->name, p->storage, p->ty, p->sdef, &p->arr);
        if (p->storage == ST_HEAP)
            c->prog->uses_heap = 1;
    }
    if (areg > c->abi->nargs)
        err(c, f->line, f->col, "function '%s' needs %d argument registers; %s has only %d",
            f->name, areg, c->abi->name, c->abi->nargs);
    check_block(c, f->body);

    /* heap parameters still owned at the end of the body are released there */
    for (int i = 0; i < f->nparams; i++)
        if (f->params[i].storage == ST_HEAP && !c->loc[i].dropped)
            VEC_PUSH(f->body->rel, f->body->nrel, f->body->relcap, f->params[i].idx);

    if (f->ret_ty != TY_VOID && !always_returns(f->body))
        err(c, f->line, f->col, "function '%s' may reach its end without returning a value",
            f->name);

    f->nregs = c->maxreg;
    f->nslots = c->maxslot;
    if (c->maxreg > c->abi->npool)
        err(c, f->line, f->col,
            "function '%s' needs %d 'reg' variables at once but %s provides only %d "
            "callee-saved registers",
            f->name, c->maxreg, c->abi->name, c->abi->npool);
}

static int is_args_param(const Param *p)
{
    return p->ty == TY_SLICE && p->storage == ST_STACK && p->arr.elem == TY_SLICE &&
           p->arr.elem2 == TY_U8;
}

int check_program(Program *prog, const Abi *abi, int require_main)
{
    Ctx c;
    memset(&c, 0, sizeof c);
    c.prog = prog;
    c.abi = abi;
    int before = rgx_nerrors;

    init_builtin(B_PRINT_I32, "print_i32", TY_I32);
    init_builtin(B_PRINT, "print", TY_SLICE);
    init_builtin(B_PRINTLN, "println", TY_SLICE);

    for (int i = 0; i < prog->nstructs; i++)
        for (int j = 0; j < i; j++)
            if (strcmp(prog->structs[j]->name, prog->structs[i]->name) == 0)
                err(&c, prog->structs[i]->line, prog->structs[i]->col,
                    "struct '%s' is defined twice", prog->structs[i]->name);
    for (int i = 0; i < prog->nstructs; i++)
        layout_struct(&c, prog->structs[i]);

    for (int i = 0; i < prog->nfuncs; i++) {
        Func *f = prog->funcs[i];
        f->qname = prog->package ? xsprintf("%s::%s", prog->package, f->name) : xstrdup(f->name);
        f->sym = mangle(prog, f->name);
        for (int j = 0; j < i; j++)
            if (strcmp(prog->funcs[j]->name, f->name) == 0)
                err(&c, f->line, f->col, "function '%s' is defined twice", f->name);
    }
    /* resolve signatures first so calls may refer to later functions */
    for (int i = 0; i < prog->nfuncs; i++) {
        Func *f = prog->funcs[i];
        if (f->ret_ty == TY_STRUCT)
            f->ret_sdef = find_struct(prog, f->ret_tyname);
        int areg = 0;
        for (int j = 0; j < f->nparams; j++) {
            Param *p = &f->params[j];
            if (p->ty == TY_STRUCT)
                p->sdef = find_struct(prog, p->tyname);
            p->areg = areg;
            areg += p->ty == TY_SLICE ? 2 : 1;
        }
    }
    for (int i = 0; i < prog->nfuncs; i++)
        check_func(&c, prog->funcs[i]);

    if (require_main) {
        Func *m = NULL;
        for (int i = 0; i < prog->nfuncs; i++)
            if (strcmp(prog->funcs[i]->name, "main") == 0)
                m = prog->funcs[i];
        if (!m) {
            err(&c, 1, 1, "no 'main' function");
        } else {
            if (m->nparams > 1 || (m->nparams == 1 && !is_args_param(&m->params[0])))
                err(&c, m->line, m->col,
                    "'main' takes no parameters or one: 'stack args: &[&[u8]]'");
            if (m->ret_ty != TY_VOID && (m->ret_storage != ST_REG || m->ret_ty != TY_I32))
                err(&c, m->line, m->col, "'main' must return 'reg i32' or 'void'");
        }
    }
    return rgx_nerrors - before;
}
