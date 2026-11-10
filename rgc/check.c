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
    int secret;   /* cleared at scope end; never copied or passed on */
    Stmt *decl;   /* declaring statement (NULL for parameters) */
    int root;     /* reference locals: index of the owner variable it borrows from, or -1 */
    int is_local_ref; /* a reference declared in the body (not a parameter) */
    int lock_shared;  /* number of live shared reference locals borrowing from this variable */
    int lock_mut;     /* a live &mut reference local borrows from this variable */
} Local;

typedef struct {
    Program *prog;      /* master program: all functions and structs */
    Program *unit;      /* source file being checked (package, imports) */
    const char *file;
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
    rgx_error(c->file, line, col, "%s", buf);
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

static int same_pkg(const char *a, const char *b)
{
    if (!a || !b)
        return a == b;
    return strcmp(a, b) == 0;
}

/* Structs are visible inside their own package only. */
static StructDef *find_struct(Ctx *c, const char *name)
{
    Program *p = c->prog;
    for (int i = 0; i < p->nstructs; i++)
        if (strcmp(p->structs[i]->name, name) == 0 &&
            same_pkg(p->structs[i]->unit->package, c->unit->package))
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

static void layout_struct_inner(Ctx *c, StructDef *sd);

static void layout_struct(Ctx *c, StructDef *sd)
{
    Program *saved_unit = c->unit;
    const char *saved_file = c->file;
    c->unit = sd->unit;
    c->file = sd->unit->file;
    layout_struct_inner(c, sd);
    c->unit = saved_unit;
    c->file = saved_file;
}

static int round_up(int v, int a) { return (v + a - 1) / a * a; }

static void layout_struct_inner(Ctx *c, StructDef *sd)
{
    if (sd->state == 2)
        return;
    if (sd->state == 1) {
        err(c, sd->line, sd->col, "struct '%s' contains itself by value; make the recursive "
            "field 'heap' (an owned pointer)", sd->name);
        return;
    }
    sd->state = 1;
    int off = 0;
    sd->align = 4;
    if (sd->nfields == 0)
        err(c, sd->line, sd->col, "struct '%s' has no fields", sd->name);
    for (int i = 0; i < sd->nfields; i++) {
        Field *f = &sd->fields[i];
        for (int j = 0; j < i; j++)
            if (strcmp(sd->fields[j].name, f->name) == 0)
                err(c, f->line, f->col, "field '%s' is declared twice in struct '%s'",
                    f->name, sd->name);
        if (f->is_heap) {
            /* an owned, nullable pointer: the target may be this very struct */
            if (f->ty != TY_STRUCT) {
                err(c, f->line, f->col, "a 'heap' field must hold a struct");
            } else {
                f->sdef = find_struct(c, f->tyname);
                if (!f->sdef)
                    err(c, f->line, f->col, "unknown type '%s'", f->tyname);
            }
            f->offset = round_up(off, 8);
            off = f->offset + 8;
            sd->align = 8;
            sd->has_heap = 1;
            continue;
        }
        if (f->ty == TY_STRUCT) {
            f->sdef = find_struct(c, f->tyname);
            if (!f->sdef) {
                err(c, f->line, f->col, "unknown type '%s'", f->tyname);
                f->offset = off;
                off += 4;
                continue;
            }
            layout_struct(c, f->sdef);
            off = round_up(off, f->sdef->align ? f->sdef->align : 4);
            f->offset = off;
            off += f->sdef->size;
            if (f->sdef->align > sd->align)
                sd->align = f->sdef->align;
            if (f->sdef->has_heap)
                sd->has_heap = 1;
        } else {
            f->offset = off;
            off += 4;
        }
    }
    sd->size = round_up(off, sd->align);
    sd->state = 2;
}

/* ---- functions ---- */

static int imported(Program *unit, const char *module)
{
    for (int i = 0; i < unit->nimports; i++)
        if (strcmp(unit->imports[i], module) == 0)
            return 1;
    return 0;
}

/* Unqualified names resolve inside the current package; qualified names
 * (a::b::f) need the package to be imported and the function to be pub. */
static Func *find_func(Ctx *c, Expr *e)
{
    Program *p = c->prog;
    int qualified = strstr(e->name, "::") != NULL;
    for (int i = 0; i < p->nfuncs; i++) {
        Func *f = p->funcs[i];
        int match = qualified ? strcmp(f->qname, e->name) == 0
                              : (strcmp(f->name, e->name) == 0 &&
                                 same_pkg(f->unit->package, c->unit->package));
        if (!match)
            continue;
        if (!same_pkg(f->unit->package, c->unit->package)) {
            if (!imported(c->unit, f->unit->package))
                err(c, e->line, e->col, "package '%s' is not imported (add 'import %s;')",
                    f->unit->package, f->unit->package);
            if (!f->is_pub)
                err(c, e->line, e->col, "function '%s' is not public in package '%s'",
                    f->name, f->unit->package);
        }
        return f;
    }
    for (int i = 0; i < N_BUILTINS; i++) {
        if (strcmp(e->name, builtins[i].qname) != 0)
            continue;
        if (!imported(c->unit, "std::io"))
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
    l.root = -1;
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
    else if (ty == TY_REF)
        l.size = 8;
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

enum { U_READ, U_WRITE, U_MOVE };

/* A variable that a live reference local borrows from cannot be used in a way
 * that conflicts with the borrow (lexical scope, no flow analysis). */
static int touch(Ctx *c, Local *l, int mode, int line, int col)
{
    if (l->lock_mut || (mode != U_READ && l->lock_shared)) {
        err(c, line, col, "'%s' is borrowed by a reference variable and cannot be %s until "
            "that reference goes out of scope", l->name,
            mode == U_READ ? "used" : (mode == U_WRITE ? "modified" : "moved or dropped"));
        return 0;
    }
    return 1;
}

/* Does the variable own something that must be released at scope end? */
static int needs_release(Local *l)
{
    if (l->dropped)
        return 0;
    if (l->st == ST_HEAP)
        return 1;
    return l->st == ST_STACK && l->ty == TY_STRUCT && l->sd && l->sd->has_heap;
}

static Release release_of(Local *l)
{
    Release r;
    r.idx = l->idx;
    r.bslots = l->slots;
    r.sd = l->ty == TY_STRUCT ? l->sd : NULL;
    r.is_heap = l->st == ST_HEAP;
    return r;
}

/* True if `e` names a secret variable (or a field or element of one). */
static int is_secret_expr(Ctx *c, Expr *e)
{
    while (e->kind == E_FIELD)
        e = e->l;
    if (e->kind == E_INDEX)
        e = e->l;
    if (e->kind != E_VAR)
        return 0;
    Local *l = lookup(c, e->name);
    return l && l->secret;
}

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
    if (!touch(c, l, U_READ, v->line, v->col))
        return NULL;
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
        if (l->secret) {
            err(c, a->line, a->col, "a secret cannot be borrowed");
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

typedef struct {
    int dref[8];   /* offsets of owned pointers loaded on the way */
    int n;
    int lastptr;   /* the final field is an owned pointer */
} FPath;

static int resolve_field(Ctx *c, Expr *e, Local **root, int *off, Ty *ty, StructDef **sd,
                         FPath *fp);

/* ---- references (&T, &mut T) ---- */

typedef struct {
    const char *name;
    int mut;
} Use;

static void collect_uses(Expr *e, Use **u, int *n, int *cap)
{
    if (!e)
        return;
    switch (e->kind) {
    case E_VAR: {
        Use x = {e->name, 0};
        VEC_PUSH(*u, *n, *cap, x);
        break;
    }
    case E_ADDR: {
        Expr *r = e->l;
        while (r && r->kind == E_FIELD)
            r = r->l;
        if (r && r->kind == E_VAR) {
            Use x = {r->name, (int)e->val};
            VEC_PUSH(*u, *n, *cap, x);
        }
        break;
    }
    case E_CALL:
        for (int i = 0; i < e->nargs; i++)
            collect_uses(e->args[i], u, n, cap);
        break;
    case E_FIELD:
    case E_LEN:
    case E_NEG:
    case E_DEREF:
        collect_uses(e->l, u, n, cap);
        break;
    case E_BIN:
    case E_INDEX:
        collect_uses(e->l, u, n, cap);
        collect_uses(e->r, u, n, cap);
        break;
    default:
        break;
    }
}

/* A variable borrowed mutably in a call may not be used anywhere else in the
 * same call (no aliasing). */
static void check_aliasing(Ctx *c, Expr *call)
{
    Use *u = NULL;
    int n = 0, cap = 0;
    for (int i = 0; i < call->nargs; i++)
        collect_uses(call->args[i], &u, &n, &cap);
    for (int i = 0; i < n; i++) {
        if (!u[i].mut)
            continue;
        int count = 0;
        for (int j = 0; j < n; j++)
            if (strcmp(u[i].name, u[j].name) == 0)
                count++;
        if (count > 1) {
            err(c, call->line, call->col,
                "'%s' is borrowed mutably and also used elsewhere in this call", u[i].name);
            return;
        }
    }
}

/* Validate the borrow expression `a` (E_ADDR) against the wanted reference
 * type and annotate it. Returns the borrowed variable, or NULL after an error. */
static Local *check_borrow(Ctx *c, Expr *a, Ty want_elem, StructDef *want_sd, int want_mut,
                           const char *pname)
{
    Expr *t = a->l;
    Local *root = NULL;
    int off = 0;
    Ty ty = TY_I32;
    StructDef *sd = NULL;
    FPath fp;
    memset(&fp, 0, sizeof fp);
    if (t->kind == E_VAR) {
        Local *l = lookup(c, t->name);
        if (!l) {
            err(c, t->line, t->col, "undefined variable '%s'", t->name);
            return NULL;
        }
        if (l->dropped) {
            err(c, t->line, t->col, "use of %s variable '%s'", gone(l), t->name);
            return NULL;
        }
        root = l;
        if (l->ty == TY_REF) {
            ty = l->arr.elem;
            sd = l->sd;
        } else if (l->ty == TY_STRUCT) {
            ty = TY_STRUCT;
            sd = l->sd;
        } else if (l->ty == TY_I32) {
            if (l->st == ST_REG) {
                err(c, t->line, t->col,
                    "a 'reg' variable has no address; make '%s' 'stack' or 'heap' to borrow it",
                    t->name);
                return NULL;
            }
        } else {
            err(c, t->line, t->col, "'%s' is an array or slice; borrow it as a slice parameter "
                "(&[T]), not as a reference", t->name);
            return NULL;
        }
    } else if (!resolve_field(c, t, &root, &off, &ty, &sd, &fp)) {
        return NULL;
    }
    if (want_elem != ty || (ty == TY_STRUCT && want_sd != sd)) {
        err(c, a->line, a->col, "type mismatch for reference '%s'", pname);
        return NULL;
    }
    if (want_mut && !a->val) {
        err(c, a->line, a->col, "'%s' needs a mutable borrow: write &mut", pname);
        return NULL;
    }
    if (a->val && root->ty == TY_REF && !root->arr.rmut) {
        err(c, a->line, a->col, "cannot borrow mutably through the shared reference '%s'",
            root->name);
        return NULL;
    }
    if (root->secret) {
        err(c, a->line, a->col, "a secret cannot be borrowed");
        return NULL;
    }
    if (root->ty != TY_REF &&
        !touch(c, root, a->val ? U_WRITE : U_READ, a->line, a->col))
        return NULL;
    a->vty = TY_REF;
    a->reg = root->idx;
    a->foff = off;
    a->ndref = fp.n;
    memcpy(a->dref, fp.dref, sizeof(int) * (size_t)fp.n);
    a->lastptr = fp.lastptr;
    if (root->ty == TY_REF || root->st == ST_HEAP) {
        a->vst = ST_HEAP;
        a->bslots = 1;
    } else {
        a->vst = ST_STACK;
        a->bslots = root->slots;
    }
    if (root->ty == TY_REF || root->st == ST_HEAP || fp.n > 0 || fp.lastptr)
        c->prog->uses_panic = 1;
    return root;
}

/* A reference variable passed on, or &x / &mut x. */
static void as_borrow(Ctx *c, Expr *a, int want_mut)
{
    if (a->kind == E_VAR) {
        Local *l = lookup(c, a->name);
        if (l && l->ty == TY_REF) {
            Expr *v = xcalloc(sizeof *v);
            *v = *a;
            a->kind = E_ADDR;
            a->l = v;
            a->val = want_mut;
        }
    }
}

/* Argument for a reference parameter: &x, &mut x, &x.field, or a reference
 * variable passed on. */
static void check_ref_arg(Ctx *c, Expr *call, Expr *a, Param *p)
{
    as_borrow(c, a, p->arr.rmut);
    if (a->kind != E_ADDR) {
        err(c, a->line, a->col, "the argument for reference parameter '%s' must be &x, &mut x "
            "or a reference variable", p->name);
        return;
    }
    Local *root = check_borrow(c, a, p->arr.elem, p->sdef, p->arr.rmut, p->name);
    if (root && root->ty != TY_REF)
        call->notail = 1; /* the borrowed value lives in this frame: no tail jump */
}

/* ---- expressions ---- */

static int resolve_field(Ctx *c, Expr *e, Local **root, int *off, Ty *ty, StructDef **sd,
                         FPath *fp)
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
        if (l->ty != TY_STRUCT && !(l->ty == TY_REF && l->arr.elem == TY_STRUCT)) {
            err(c, e->line, e->col, "'%s' is not a struct", e->name);
            return 0;
        }
        *root = l;
        *off = 0;
        *ty = TY_STRUCT;
        *sd = l->sd;
        fp->n = 0;
        fp->lastptr = 0;
        return 1;
    }
    if (e->kind == E_FIELD) {
        if (!resolve_field(c, e->l, root, off, ty, sd, fp))
            return 0;
        if (*ty != TY_STRUCT) {
            err(c, e->line, e->col, "cannot access field '%s' of an i32", e->name);
            return 0;
        }
        if (fp->lastptr) { /* go through the owned pointer we just passed */
            if (fp->n >= 8) {
                err(c, e->line, e->col, "field path is too deep");
                return 0;
            }
            fp->dref[fp->n++] = *off;
            *off = 0;
            fp->lastptr = 0;
        }
        Field *f = find_field(*sd, e->name);
        if (!f) {
            err(c, e->line, e->col, "struct '%s' has no field '%s'", (*sd)->name, e->name);
            return 0;
        }
        *off += f->offset;
        fp->lastptr = f->is_heap;
        *ty = f->ty;
        *sd = f->sdef;
        return 1;
    }
    err(c, e->line, e->col, "expected a variable or a field");
    return 0;
}

static void annotate_field(Ctx *c, Expr *e, Local *root, int off, const FPath *fp)
{
    e->reg = root->idx;
    e->foff = off;
    e->ndref = fp->n;
    memcpy(e->dref, fp->dref, sizeof(int) * (size_t)fp->n);
    e->lastptr = fp->lastptr;
    if (root->ty == TY_REF) { /* the slot holds a pointer, like a heap variable */
        e->vst = ST_HEAP;
        e->bslots = 1;
    } else {
        e->vst = root->st;
        e->bslots = root->slots;
    }
    if (root->st == ST_HEAP || root->ty == TY_REF || fp->n > 0 || fp->lastptr)
        c->prog->uses_panic = 1;
}

/* A value for a 'heap Target' slot: null, a heap variable (moved), or a call
 * that returns one. Returns a PK_ constant, or PK_NONE after an error. */
static void check_lit(Ctx *c, Expr *lit, StructDef *sd);

static int check_heap_value(Ctx *c, Expr *x, StructDef *target)
{
    if (x->kind == E_NULL)
        return PK_NULL;
    if (x->kind == E_VAR) {
        Local *l = lookup(c, x->name);
        if (!l) {
            err(c, x->line, x->col, "undefined variable '%s'", x->name);
            return PK_NONE;
        }
        if (l->dropped) {
            err(c, x->line, x->col, "use of %s variable '%s'", gone(l), x->name);
            return PK_NONE;
        }
        if (l->st != ST_HEAP || l->ty != TY_STRUCT || l->sd != target) {
            err(c, x->line, x->col,
                "expected null, a 'heap %s' variable or a call returning one", target->name);
            return PK_NONE;
        }
        if (l->secret) {
            err(c, x->line, x->col, "a secret cannot be moved into a field");
            return PK_NONE;
        }
        if (!touch(c, l, U_MOVE, x->line, x->col))
            return PK_NONE;
        x->reg = l->idx;
        x->vst = ST_HEAP;
        l->dropped = 1;
        l->moved = 1;
        return PK_MOVE;
    }
    if (x->kind == E_STRUCTLIT) {
        check_lit(c, x, target);
        x->reg = c->nslot++;          /* hidden slot for the new block while it is filled */
        if (c->nslot > c->maxslot)
            c->maxslot = c->nslot;
        return PK_LIT;
    }
    if (x->kind == E_CALL) {
        c->heap_ok = 1;
        check_expr(c, x, 0);
        Func *f = x->callee;
        if (f && f->ret_storage == ST_HEAP && f->ret_ty == TY_STRUCT && f->ret_sdef == target)
            return PK_CALL;
        if (f)
            err(c, x->line, x->col, "'%s' does not return a 'heap %s'", f->name, target->name);
        return PK_NONE;
    }
    err(c, x->line, x->col, "expected null, a 'heap %s' variable or a call returning one",
        target->name);
    return PK_NONE;
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
        if (f->is_heap) {
            if (f->sdef)
                check_heap_value(c, lit->args[i], f->sdef);
        } else if (f->ty == TY_STRUCT) {
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
    if (a->kind == E_NULL && p->ty == TY_STRUCT) /* an empty heap struct argument */
        return;
    if (p->ty == TY_STRUCT && a->kind == E_STRUCTLIT) { /* a new heap value built in place */
        check_heap_value(c, a, p->sdef);
        return;
    }
    if (a->kind == E_CALL) { /* ownership of a call's result moves straight in */
        c->heap_ok = 1;
        check_expr(c, a, 0);
        Func *g = a->callee;
        if (g && (g->ret_storage != ST_HEAP || g->ret_ty != p->ty || g->ret_sdef != p->sdef))
            err(c, a->line, a->col, "'%s' does not return the 'heap' type parameter '%s' expects",
                g->name, p->name);
        return;
    }
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
    if (l->secret) {
        err(c, a->line, a->col, "a secret cannot be passed to a function");
        return;
    }
    if (l->ty != p->ty || l->sd != p->sdef) {
        err(c, a->line, a->col, "type mismatch for parameter '%s'", p->name);
        return;
    }
    if (!touch(c, l, U_MOVE, a->line, a->col))
        return;
    a->reg = l->idx;
    a->vst = ST_HEAP;
    l->dropped = 1;
    l->moved = 1;
}

static void check_null_operand(Ctx *c, Expr *x)
{
    const char *msg = "only 'heap' struct variables and 'heap' fields can be compared with null";
    if (x->kind == E_VAR) {
        Local *l = lookup(c, x->name);
        if (!l) {
            err(c, x->line, x->col, "undefined variable '%s'", x->name);
            return;
        }
        if (l->dropped) {
            err(c, x->line, x->col, "use of %s variable '%s'", gone(l), x->name);
            return;
        }
        if (l->st != ST_HEAP || l->ty != TY_STRUCT) {
            err(c, x->line, x->col, "%s", msg);
            return;
        }
        touch(c, l, U_READ, x->line, x->col);
        x->reg = l->idx;
        x->vst = ST_HEAP;
        return;
    }
    if (x->kind == E_FIELD) {
        Local *root;
        int off;
        Ty ty;
        StructDef *sd;
        FPath fp;
        if (!resolve_field(c, x, &root, &off, &ty, &sd, &fp))
            return;
        if (!fp.lastptr) {
            err(c, x->line, x->col, "%s", msg);
            return;
        }
        if (root->ty != TY_REF)
            touch(c, root, U_READ, x->line, x->col);
        annotate_field(c, x, root, off, &fp);
        return;
    }
    err(c, x->line, x->col, "%s", msg);
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
        if (l->ty != TY_REF)
            touch(c, l, U_READ, e->line, e->col);
        if (l->ty == TY_STRUCT)
            err(c, e->line, e->col, "struct variable '%s' cannot be used as a value; "
                "use one of its fields", e->name);
        if (l->ty == TY_ARRAY || l->ty == TY_SLICE)
            err(c, e->line, e->col, "'%s' cannot be used as a value; index it, use '%s.len', "
                "or borrow it with &%s", e->name, e->name, e->name);
        if (l->ty == TY_REF)
            err(c, e->line, e->col, "reference '%s' cannot be used as a value; dereference it "
                "with *%s (or use %s.field)", e->name, e->name, e->name);
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
        FPath fp;
        if (resolve_field(c, e, &root, &off, &ty, &sd, &fp)) {
            if (root->ty != TY_REF)
                touch(c, root, U_READ, e->line, e->col);
            if (ty == TY_STRUCT)
                err(c, e->line, e->col,
                    fp.lastptr ? "field '%s' is an owned pointer; use its fields, compare it with "
                                 "null, or move it"
                               : "field '%s' is a struct; use one of its fields",
                    e->name);
            annotate_field(c, e, root, off, &fp);
        }
        return TY_I32;
    }
    case E_DEREF: {
        Expr *v = e->l;
        Local *l = v->kind == E_VAR ? lookup(c, v->name) : NULL;
        if (v->kind != E_VAR) {
            err(c, e->line, e->col, "only a reference variable can be dereferenced");
        } else if (!l) {
            err(c, v->line, v->col, "undefined variable '%s'", v->name);
        } else if (l->dropped) {
            err(c, v->line, v->col, "use of %s variable '%s'", gone(l), v->name);
        } else if (l->ty != TY_REF) {
            err(c, v->line, v->col, "'%s' is not a reference", v->name);
        } else if (l->arr.elem != TY_I32) {
            err(c, v->line, v->col, "'%s' refers to a struct; use its fields (%s.field)",
                v->name, v->name);
        } else {
            e->reg = l->idx;
            e->vst = ST_HEAP;
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
    case E_BIN: {
        if ((e->op == OP_EQ || e->op == OP_NE) && (e->l->kind == E_NULL || e->r->kind == E_NULL)) {
            if (e->l->kind == E_NULL && e->r->kind == E_NULL) {
                err(c, e->line, e->col, "null cannot be compared with null");
                return TY_I32;
            }
            Expr *other = e->l->kind == E_NULL ? e->r : e->l;
            check_null_operand(c, other);
            e->kind = E_ISNULL;
            e->l = other;
            e->r = NULL;
            e->val = e->op == OP_EQ;
            return TY_I32;
        }
        check_expr(c, e->l, 0);
        check_expr(c, e->r, 0);
        return TY_I32;
    }
    case E_NULL:
        err(c, e->line, e->col,
            "null can only be assigned to, passed as, or compared with a 'heap' struct value");
        return TY_I32;
    case E_ISNULL:
        return TY_I32;
    case E_CALL: {
        int heap_ok = c->heap_ok;
        c->heap_ok = 0;
        Func *f = find_func(c, e);
        for (int i = 0; i < e->nargs; i++) {
            Param *p = (f && i < f->nparams) ? &f->params[i] : NULL;
            if (p && p->ty == TY_SLICE)
                check_slice_arg(c, e, e->args[i], p);
            else if (p && p->ty == TY_REF)
                check_ref_arg(c, e, e->args[i], p);
            else if (p && p->storage == ST_HEAP)
                check_move_arg(c, e->args[i], p);
            else {
                check_expr(c, e->args[i], 0);
                if (is_secret_expr(c, e->args[i]))
                    err(c, e->args[i]->line, e->args[i]->col,
                        "a secret cannot be passed to a function");
            }
        }
        if (!f && strcmp(e->name, "copy") == 0) {
            err(c, e->line, e->col,
                "copy(x) can only initialize a struct or array variable: 'stack q: T = copy(p)'");
            return TY_I32;
        }
        if (!f) {
            err(c, e->line, e->col, "undefined function '%s'", e->name);
            return TY_I32;
        }
        e->callee = f;
        check_aliasing(c, e);
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

static int user_func_named(Ctx *c, const char *name)
{
    for (int i = 0; i < c->prog->nfuncs; i++)
        if (strcmp(c->prog->funcs[i]->name, name) == 0)
            return 1;
    return 0;
}

/* `stack q: T = copy(p)` duplicates the bytes; `stack q: T = p` moves (the
 * source can no longer be used). Structs and arrays only. */
static void check_copy_move(Ctx *c, Stmt *s, Expr *v, int is_copy)
{
    Local *l = lookup(c, v->name);
    if (!l) {
        err(c, v->line, v->col, "undefined variable '%s'", v->name);
        return;
    }
    if (l->dropped) {
        err(c, v->line, v->col, "use of %s variable '%s'", gone(l), v->name);
        return;
    }
    int same;
    if (l->ty == TY_REF) {
        same = s->ty == TY_STRUCT && l->arr.elem == TY_STRUCT && l->sd == s->sdef;
        if (same && !is_copy) {
            err(c, v->line, v->col, "cannot move out of a reference; use copy(%s)", v->name);
            return;
        }
    } else {
        same = l->ty == s->ty &&
               (s->ty == TY_STRUCT ? l->sd == s->sdef
                                   : (l->arr.elem == s->arr.elem && l->arr.alen == s->arr.alen));
    }
    if (!same) {
        err(c, v->line, v->col, "type mismatch: '%s' has a different type", v->name);
        return;
    }
    if (is_copy && s->ty == TY_STRUCT && l->sd && l->sd->has_heap) {
        err(c, v->line, v->col, "a struct that owns 'heap' fields cannot be copied (no deep copy yet)");
        return;
    }
    if (is_copy && l->secret) {
        err(c, v->line, v->col, "a secret cannot be copied; move it instead");
        return;
    }
    if (l->secret && !s->secret) {
        err(c, v->line, v->col, "a secret can only be moved into another 'secret' variable");
        return;
    }
    if (l->ty != TY_REF && !touch(c, l, is_copy ? U_READ : U_MOVE, v->line, v->col))
        return;
    s->cpy = is_copy ? CP_COPY : CP_MOVE;
    s->cst = l->ty == TY_REF ? ST_HEAP : l->st; /* a reference holds a pointer, like heap */
    if (s->cst == ST_HEAP)
        c->prog->uses_panic = 1;
    s->cidx = l->idx;
    s->cbs = l->slots;
    if (!is_copy) {
        l->dropped = 1;
        l->moved = 1;
    }
}

static void check_decl(Ctx *c, Stmt *s)
{
    StructDef *sd = NULL;
    if (s->ty == TY_STRUCT || (s->ty == TY_REF && s->arr.elem == TY_STRUCT)) {
        sd = find_struct(c, s->tyname);
        if (!sd)
            err(c, s->line, s->col, "unknown type '%s'", s->tyname);
    }
    s->sdef = sd;
    if (s->ty == TY_SLICE)
        err(c, s->line, s->col, "slice variables are not supported; slices exist as parameters");
    if (s->ty == TY_REF && s->storage != ST_STACK)
        err(c, s->line, s->col, "a reference variable must be 'stack'");
    if (s->ty == TY_REF && s->secret)
        err(c, s->line, s->col, "a reference cannot be 'secret'");
    if (s->secret && sd && sd->has_heap)
        err(c, s->line, s->col, "a struct that owns 'heap' fields cannot be 'secret'");
    int new_root = -1;
    if ((s->ty == TY_STRUCT || s->ty == TY_ARRAY) && s->storage == ST_REG)
        err(c, s->line, s->col,
            "%s variable '%s' cannot be 'reg'; it lives in 'stack' or 'heap'",
            s->ty == TY_ARRAY ? "array" : "struct", s->name);
    int bytes = s->ty == TY_ARRAY ? s->arr.alen * esz(s->arr.elem) : (sd ? sd->size : 4);
    if (s->storage == ST_HEAP && bytes > HEAP_MAX_BYTES)
        err(c, s->line, s->col, "heap values larger than %d bytes are not supported",
            HEAP_MAX_BYTES);

    Expr *copy_src = NULL;
    int is_copy = 0;
    if (s->expr) {
        Expr *x = s->expr;
        if (x->kind == E_CALL && strcmp(x->name, "copy") == 0 && x->nargs == 1 &&
            x->args[0]->kind == E_VAR && !user_func_named(c, "copy")) {
            copy_src = x->args[0];
            is_copy = 1;
        } else if (x->kind == E_VAR && (s->ty == TY_STRUCT || s->ty == TY_ARRAY)) {
            copy_src = x;
        }
    }

    if (!s->expr) {
        err(c, s->line, s->col, "variable '%s' needs an initializer", s->name);
    } else if (copy_src && (s->ty == TY_STRUCT || s->ty == TY_ARRAY)) {
        check_copy_move(c, s, copy_src, is_copy);
    } else if (s->ty == TY_ARRAY) {
        check_array_init(c, s);
    } else if (s->ty == TY_REF) {
        as_borrow(c, s->expr, s->arr.rmut);
        if (s->expr->kind != E_ADDR) {
            err(c, s->expr->line, s->expr->col,
                "a reference variable must be initialized with &x, &mut x or another reference");
        } else {
            Local *root = check_borrow(c, s->expr, s->arr.elem, sd, s->arr.rmut, s->name);
            if (root)
                new_root = root->ty == TY_REF ? root->root : (int)(root - c->loc);
        }
    } else if (s->ty == TY_SLICE) {
        /* already reported */
    } else if (s->ty == TY_STRUCT) {
        if (!sd) {
            /* already reported */
        } else if (s->expr->kind == E_STRUCTLIT) {
            check_lit(c, s->expr, sd);
        } else if (s->expr->kind == E_NULL && s->storage == ST_HEAP) {
            s->pk = PK_NULL;
        } else if (s->expr->kind == E_CALL && strcmp(s->expr->name, "take") == 0 &&
                   s->expr->nargs == 1 && s->expr->args[0]->kind == E_FIELD &&
                   s->storage == ST_HEAP && !user_func_named(c, "take")) {
            Expr *a = s->expr->args[0];
            Local *root;
            int off;
            Ty ty;
            StructDef *tsd;
            FPath fp;
            if (resolve_field(c, a, &root, &off, &ty, &tsd, &fp)) {
                if (!fp.lastptr)
                    err(c, a->line, a->col, "take(x) needs a 'heap' field");
                else if (tsd != sd)
                    err(c, a->line, a->col, "type mismatch: the field holds a 'heap %s'", tsd->name);
                else if (root->ty == TY_REF && !root->arr.rmut)
                    err(c, a->line, a->col, "cannot take through the shared reference '%s'",
                        root->name);
                else {
                    if (root->ty != TY_REF)
                        touch(c, root, U_WRITE, a->line, a->col);
                    annotate_field(c, a, root, off, &fp);
                    s->pk = PK_TAKE;
                }
            }
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
        if (!s->secret && is_secret_expr(c, s->expr))
            err(c, s->expr->line, s->expr->col,
                "a secret cannot be copied into a non-secret variable");
    }

    s->reg = declare(c, s->name, s->storage, s->ty, sd, &s->arr);
    Local *l = &c->loc[c->n - 1];
    l->secret = s->secret;
    l->decl = s;
    if (s->ty == TY_REF) {
        l->is_local_ref = 1;
        l->root = new_root;
        if (new_root >= 0) {
            Local *o = &c->loc[new_root];
            if (s->arr.rmut)
                o->lock_mut = 1;
            else
                o->lock_shared++;
        }
    }
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
            if (x->kind == E_NULL) {
                if (f->ret_ty != TY_STRUCT)
                    err(c, x->line, x->col, "null can only be returned as a 'heap' struct value");
            } else if (x->kind == E_VAR) {
                Local *l = lookup(c, x->name);
                if (!l) {
                    err(c, x->line, x->col, "undefined variable '%s'", x->name);
                } else if (l->dropped) {
                    err(c, x->line, x->col, "use of %s variable '%s'", gone(l), x->name);
                } else if (l->st != ST_HEAP || l->ty == TY_ARRAY) {
                    err(c, x->line, x->col, "'%s' is not a movable 'heap' value", x->name);
                } else if (l->secret) {
                    err(c, x->line, x->col, "a secret cannot be returned");
                } else if (!same_type(l->ty, l->sd, f->ret_ty, f->ret_sdef)) {
                    err(c, x->line, x->col, "type mismatch in return value");
                } else {
                    touch(c, l, U_MOVE, x->line, x->col);
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
            if (is_secret_expr(c, x))
                err(c, x->line, x->col, "a secret cannot be returned");
        }
    }
    for (int i = 0; i < c->n; i++)
        if (c->loc[i].secret && !c->loc[i].dropped)
            VEC_PUSH(s->wipe, s->nwipe, s->wipecap, c->loc[i].decl);
    for (int i = 0; i < c->n; i++)
        if (needs_release(&c->loc[i])) {
            Release r = release_of(&c->loc[i]);
            VEC_PUSH(s->rel, s->nrel, s->relcap, r);
        }
}

static void check_stmt(Ctx *c, Stmt *s)
{
    switch (s->kind) {
    case S_DECL:
        check_decl(c, s);
        break;
    case S_ASSIGN: {
        Local *l = lookup(c, s->name);
        if (l && l->ty == TY_REF && !l->dropped) { /* re-point a reference variable */
            as_borrow(c, s->expr, l->arr.rmut);
            if (s->expr->kind != E_ADDR) {
                err(c, s->line, s->col,
                    "a reference variable can only be assigned &x, &mut x or another reference");
                break;
            }
            Local *root = check_borrow(c, s->expr, l->arr.elem, l->sd, l->arr.rmut, l->name);
            if (root) {
                int newroot = root->ty == TY_REF ? root->root : (int)(root - c->loc);
                if (newroot != l->root)
                    err(c, s->line, s->col,
                        "reference '%s' cannot be re-pointed at a different variable "
                        "(borrow through '%s' itself, e.g. &%s.field)", l->name, l->name, l->name);
            }
            s->reg = l->idx;
            s->storage = l->st;
            break;
        }
        if (l && l->ty == TY_STRUCT && l->st == ST_HEAP && !(l->dropped && !l->moved)) {
            /* a heap struct variable can be given a new owner (null, a moved heap
             * variable or a call result); a value it still owns is released first */
            s->pk = check_heap_value(c, s->expr, l->sd);
            touch(c, l, U_WRITE, s->line, s->col);
            s->sdef = l->sd;
            s->adopt = !l->dropped;
            l->dropped = 0;
            l->moved = 0;
            s->reg = l->idx;
            s->storage = l->st;
            break;
        }
        if (!l) {
            err(c, s->line, s->col, "undefined variable '%s'", s->name);
        } else {
            if (l->dropped)
                err(c, s->line, s->col, "assignment to %s variable '%s'", gone(l), s->name);
            else
                touch(c, l, U_WRITE, s->line, s->col);
            if (l->ty == TY_STRUCT || l->ty == TY_ARRAY || l->ty == TY_SLICE)
                err(c, s->line, s->col, "cannot assign a whole %s; assign its %s",
                    l->ty == TY_STRUCT ? "struct" : "array or slice",
                    l->ty == TY_STRUCT ? "fields" : "elements");
            s->reg = l->idx;
            s->storage = l->st;
        }
        check_expr(c, s->expr, 0);
        if (l && !l->secret && is_secret_expr(c, s->expr))
            err(c, s->line, s->col, "a secret cannot be copied into a non-secret variable");
        break;
    }
    case S_FASSIGN: {
        int tsec = 1;
        Local *root;
        int off;
        Ty ty;
        StructDef *sd;
        FPath fp;
        if (resolve_field(c, s->lhs, &root, &off, &ty, &sd, &fp)) {
            annotate_field(c, s->lhs, root, off, &fp);
            tsec = root->secret;
            if (root->ty == TY_REF && !root->arr.rmut)
                err(c, s->line, s->col, "cannot assign through the shared reference '%s' (use &mut)",
                    root->name);
            if (root->ty != TY_REF)
                touch(c, root, U_WRITE, s->line, s->col);
            if (ty == TY_STRUCT && fp.lastptr) {
                s->lhs->sdef = sd;
                if (sd)
                    s->pk = check_heap_value(c, s->expr, sd);
                break;
            }
            if (ty == TY_STRUCT)
                err(c, s->line, s->col, "cannot assign a whole struct; assign its fields");
        }
        check_expr(c, s->expr, 0);
        if (!tsec && is_secret_expr(c, s->expr))
            err(c, s->line, s->col, "a secret cannot be copied into a non-secret variable");
        break;
    }
    case S_DASSIGN: {
        Expr *d = s->lhs;
        Expr *v = d->l;
        Local *l = v->kind == E_VAR ? lookup(c, v->name) : NULL;
        if (v->kind != E_VAR) {
            err(c, s->line, s->col, "only a reference variable can be dereferenced");
        } else if (!l) {
            err(c, v->line, v->col, "undefined variable '%s'", v->name);
        } else if (l->dropped) {
            err(c, v->line, v->col, "use of %s variable '%s'", gone(l), v->name);
        } else if (l->ty != TY_REF) {
            err(c, v->line, v->col, "'%s' is not a reference", v->name);
        } else if (l->arr.elem != TY_I32) {
            err(c, v->line, v->col, "'%s' refers to a struct; assign its fields", v->name);
        } else if (!l->arr.rmut) {
            err(c, s->line, s->col, "cannot assign through the shared reference '%s' (use &mut)",
                v->name);
        } else {
            d->reg = l->idx;
            d->vst = ST_HEAP;
        }
        check_expr(c, s->expr, 0);
        break;
    }
    case S_IASSIGN:
        if (check_index(c, s->lhs, 0) && s->lhs->vty == TY_SLICE)
            err(c, s->line, s->col, "slices are read-only; assign through the array itself");
        check_expr(c, s->expr, 0);
        {
            Local *bl = s->lhs->l->kind == E_VAR ? lookup(c, s->lhs->l->name) : NULL;
            if (bl && !bl->secret && is_secret_expr(c, s->expr))
                err(c, s->line, s->col, "a secret cannot be copied into a non-secret variable");
        }
        break;
    case S_DROP: {
        Local *l = lookup(c, s->name);
        if (!l) {
            err(c, s->line, s->col, "undefined variable '%s'", s->name);
        } else if (l->dropped) {
            err(c, s->line, s->col, "variable '%s' is already %s", s->name, gone(l));
        } else if (l->ty == TY_SLICE) {
            err(c, s->line, s->col, "'%s' is a borrowed slice and cannot be dropped", s->name);
        } else if (!touch(c, l, U_MOVE, s->line, s->col)) {
            /* reported */
        } else {
            s->reg = l->idx;
            s->storage = l->st;
            s->ty = l->ty;
            s->sdef = l->sd;
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
        if (c->loc[i].secret && !c->loc[i].dropped)
            VEC_PUSH(b->wipe, b->nwipe, b->wipecap, c->loc[i].decl);
    for (int i = saved_n; i < c->n; i++)
        if (needs_release(&c->loc[i])) {
            Release r = release_of(&c->loc[i]);
            VEC_PUSH(b->rel, b->nrel, b->relcap, r);
        }
    for (int i = saved_n; i < c->n; i++) { /* borrows end with their reference variable */
        Local *r = &c->loc[i];
        if (r->is_local_ref && r->root >= 0) {
            Local *o = &c->loc[r->root];
            if (r->arr.rmut)
                o->lock_mut = 0;
            else if (o->lock_shared > 0)
                o->lock_shared--;
        }
    }
    c->n = saved_n;
    c->nreg = saved_reg;
    c->nslot = saved_slot;
}

static void check_func(Ctx *c, Func *f)
{
    c->fn = f;
    c->unit = f->unit;
    c->file = f->unit->file;
    c->n = 0;
    c->nreg = c->maxreg = c->nslot = c->maxslot = 0;
    c->heap_ok = 0;

    if (f->ret_ty == TY_STRUCT) {
        f->ret_sdef = find_struct(c, f->ret_tyname);
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
            p->sdef = find_struct(c, p->tyname);
            if (!p->sdef)
                err(c, p->line, p->col, "unknown type '%s'", p->tyname);
            if (p->storage != ST_HEAP)
                err(c, p->line, p->col,
                    "struct parameter '%s' must be 'heap' (the value moves into the callee)",
                    p->name);
        }
        if (p->ty == TY_REF) {
            if (p->arr.elem == TY_STRUCT) {
                p->sdef = find_struct(c, p->tyname);
                if (!p->sdef)
                    err(c, p->line, p->col, "unknown type '%s'", p->tyname);
            }
            if (p->storage != ST_STACK)
                err(c, p->line, p->col, "reference parameter '%s' must be 'stack'", p->name);
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
        if (needs_release(&c->loc[i])) {
            Release r = release_of(&c->loc[i]);
            VEC_PUSH(f->body->rel, f->body->nrel, f->body->relcap, r);
        }

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

Program *link_units(Program **files, int n)
{
    Program *m = xcalloc(sizeof *m);
    m->file = files[0]->file;
    m->units = files;
    m->nunits = n;
    for (int u = 0; u < n; u++) {
        for (int i = 0; i < files[u]->nfuncs; i++) {
            files[u]->funcs[i]->unit = files[u];
            VEC_PUSH(m->funcs, m->nfuncs, m->fcap, files[u]->funcs[i]);
        }
        for (int i = 0; i < files[u]->nstructs; i++) {
            files[u]->structs[i]->unit = files[u];
            VEC_PUSH(m->structs, m->nstructs, m->scap, files[u]->structs[i]);
        }
    }
    return m;
}

int check_program(Program *prog, const Abi *abi, int require_main)
{
    Ctx c;
    memset(&c, 0, sizeof c);
    c.prog = prog;
    c.abi = abi;
    c.unit = prog->units[0];
    c.file = c.unit->file;
    int before = rgx_nerrors;

    init_builtin(B_PRINT_I32, "print_i32", TY_I32);
    init_builtin(B_PRINT, "print", TY_SLICE);
    init_builtin(B_PRINTLN, "println", TY_SLICE);

    /* imports must name a package that exists (or the standard module) */
    for (int u = 0; u < prog->nunits; u++) {
        Program *un = prog->units[u];
        c.unit = un;
        c.file = un->file;
        for (int i = 0; i < un->nimports; i++) {
            const char *im = un->imports[i];
            int found = strcmp(im, "std::io") == 0;
            for (int k = 0; k < prog->nunits && !found; k++)
                found = prog->units[k]->package && strcmp(prog->units[k]->package, im) == 0;
            if (!found)
                err(&c, 1, 1, "package '%s' not found", im);
        }
    }

    for (int i = 0; i < prog->nstructs; i++) {
        StructDef *si = prog->structs[i];
        c.unit = si->unit;
        c.file = si->unit->file;
        for (int j = 0; j < i; j++)
            if (strcmp(prog->structs[j]->name, si->name) == 0 &&
                same_pkg(prog->structs[j]->unit->package, si->unit->package))
                err(&c, si->line, si->col, "struct '%s' is defined twice", si->name);
    }
    for (int i = 0; i < prog->nstructs; i++)
        layout_struct(&c, prog->structs[i]);
    for (int i = 0; i < prog->nstructs; i++) {
        StructDef *sd = prog->structs[i];
        if (sd->has_heap) {
            prog->uses_heap = 1;
            const char *pkg = sd->unit->package;
            char *p2 = pkg ? xstrdup(pkg) : xstrdup("");
            for (char *q = p2; *q; q++)
                if (*q == ':')
                    *q = '_';
            sd->fsym = pkg ? xsprintf("rgx_free_%s__%s", p2, sd->name)
                           : xsprintf("rgx_free_%s", sd->name);
        }
    }

    for (int i = 0; i < prog->nfuncs; i++) {
        Func *f = prog->funcs[i];
        const char *pkg = f->unit->package;
        c.unit = f->unit;
        c.file = f->unit->file;
        f->qname = pkg ? xsprintf("%s::%s", pkg, f->name) : xstrdup(f->name);
        f->sym = mangle(f->unit, f->name);
        for (int j = 0; j < i; j++)
            if (strcmp(prog->funcs[j]->name, f->name) == 0 &&
                same_pkg(prog->funcs[j]->unit->package, pkg))
                err(&c, f->line, f->col, "function '%s' is defined twice", f->name);
    }
    /* resolve signatures first so calls may refer to later functions */
    for (int i = 0; i < prog->nfuncs; i++) {
        Func *f = prog->funcs[i];
        c.unit = f->unit;
        c.file = f->unit->file;
        if (f->ret_ty == TY_STRUCT)
            f->ret_sdef = find_struct(&c, f->ret_tyname);
        int areg = 0;
        for (int j = 0; j < f->nparams; j++) {
            Param *p = &f->params[j];
            if (p->ty == TY_STRUCT || (p->ty == TY_REF && p->arr.elem == TY_STRUCT))
                p->sdef = find_struct(&c, p->tyname);
            p->areg = areg;
            areg += p->ty == TY_SLICE ? 2 : 1;
        }
    }
    for (int i = 0; i < prog->nfuncs; i++)
        check_func(&c, prog->funcs[i]);

    if (require_main) {
        Func *m = NULL;
        int count = 0;
        for (int i = 0; i < prog->nfuncs; i++)
            if (strcmp(prog->funcs[i]->name, "main") == 0) {
                m = prog->funcs[i];
                count++;
            }
        c.unit = prog->units[0];
        c.file = c.unit->file;
        if (!m) {
            err(&c, 1, 1, "no 'main' function");
        } else if (count > 1) {
            err(&c, m->line, m->col, "more than one 'main' function");
        } else {
            c.unit = m->unit;
            c.file = m->unit->file;
            if (m->nparams > 1 || (m->nparams == 1 && !is_args_param(&m->params[0])))
                err(&c, m->line, m->col,
                    "'main' takes no parameters or one: 'stack args: &[&[u8]]'");
            if (m->ret_ty != TY_VOID && (m->ret_storage != ST_REG || m->ret_ty != TY_I32))
                err(&c, m->line, m->col, "'main' must return 'reg i32' or 'void'");
        }
    }
    return rgx_nerrors - before;
}
