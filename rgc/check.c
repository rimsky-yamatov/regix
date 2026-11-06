#include "check.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "util.h"

typedef struct {
    const char *name;
    int reg;
} Local;

typedef struct {
    Program *prog;
    const Abi *abi;
    Func *fn;
    Local *loc;
    int n, cap, max;
} Ctx;

static const char *storage_name(Storage s)
{
    switch (s) {
    case ST_REG: return "reg";
    case ST_STACK: return "stack";
    case ST_HEAP: return "heap";
    default: return "";
    }
}


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

static Func *find_func(Program *p, const char *name)
{
    for (int i = 0; i < p->nfuncs; i++) {
        Func *f = p->funcs[i];
        if (strcmp(f->name, name) == 0 || strcmp(f->qname, name) == 0)
            return f;
    }
    return NULL;
}

static int lookup(Ctx *c, const char *name)
{
    for (int i = c->n - 1; i >= 0; i--)
        if (strcmp(c->loc[i].name, name) == 0)
            return c->loc[i].reg;
    return -1;
}

static void declare(Ctx *c, const char *name)
{
    Local l;
    l.name = name;
    l.reg = c->n;
    VEC_PUSH(c->loc, c->n, c->cap, l);
    if (c->n > c->max)
        c->max = c->n;
}

static Ty check_expr(Ctx *c, Expr *e, int allow_void)
{
    switch (e->kind) {
    case E_INT:
        return TY_I32;
    case E_VAR: {
        int r = lookup(c, e->name);
        if (r < 0) {
            err(c, e->line, e->col, "undefined variable '%s'", e->name);
            return TY_I32;
        }
        e->reg = r;
        return TY_I32;
    }
    case E_NEG:
        check_expr(c, e->l, 0);
        return TY_I32;
    case E_BIN:
        check_expr(c, e->l, 0);
        check_expr(c, e->r, 0);
        return TY_I32;
    case E_CALL: {
        Func *f = find_func(c->prog, e->name);
        for (int i = 0; i < e->nargs; i++)
            check_expr(c, e->args[i], 0);
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
        return f->ret_ty;
    }
    }
    return TY_I32;
}

static void check_block(Ctx *c, Block *b);

static void check_stmt(Ctx *c, Stmt *s)
{
    switch (s->kind) {
    case S_DECL:
        if (s->storage != ST_REG)
            err(c, s->line, s->col,
                "storage class '%s' is not implemented yet (rgc %s supports only 'reg')",
                storage_name(s->storage), "0.1");
        if (!s->expr)
            err(c, s->line, s->col, "variable '%s' needs an initializer", s->name);
        else
            check_expr(c, s->expr, 0);
        s->reg = c->n;
        declare(c, s->name);
        break;
    case S_ASSIGN: {
        int r = lookup(c, s->name);
        if (r < 0)
            err(c, s->line, s->col, "undefined variable '%s'", s->name);
        s->reg = r;
        check_expr(c, s->expr, 0);
        break;
    }
    case S_IF:
        check_expr(c, s->expr, 0);
        check_block(c, s->then_b);
        if (s->else_b)
            check_block(c, s->else_b);
        break;
    case S_WHILE:
        check_expr(c, s->expr, 0);
        check_block(c, s->then_b);
        break;
    case S_RETURN:
        if (c->fn->ret_ty == TY_VOID) {
            if (s->expr)
                err(c, s->line, s->col, "void function '%s' cannot return a value", c->fn->name);
        } else if (!s->expr) {
            err(c, s->line, s->col, "function '%s' must return a value", c->fn->name);
        }
        if (s->expr)
            check_expr(c, s->expr, 0);
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
    int saved = c->n;
    for (int i = 0; i < b->n; i++)
        check_stmt(c, b->stmts[i]);
    c->n = saved;
}

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

static void check_func(Ctx *c, Func *f)
{
    c->fn = f;
    c->n = 0;
    c->max = 0;

    if (f->ret_ty != TY_VOID && f->ret_storage != ST_REG)
        err(c, f->line, f->col,
            "return storage class '%s' is not implemented yet (rgc 0.1 supports only 'reg')",
            storage_name(f->ret_storage));
    if (f->nparams > c->abi->nargs)
        err(c, f->line, f->col, "function '%s' has %d parameters; %s supports at most %d",
            f->name, f->nparams, c->abi->name, c->abi->nargs);
    for (int i = 0; i < f->nparams; i++) {
        Param *p = &f->params[i];
        if (p->storage != ST_REG)
            err(c, p->line, p->col,
                "storage class '%s' is not implemented yet (rgc 0.1 supports only 'reg')",
                storage_name(p->storage));
        declare(c, p->name);
    }
    check_block(c, f->body);
    if (f->ret_ty != TY_VOID && !always_returns(f->body))
        err(c, f->line, f->col, "function '%s' may reach its end without returning a value",
            f->name);

    f->nregs = c->max;
    if (c->max > c->abi->npool)
        err(c, f->line, f->col,
            "function '%s' needs %d 'reg' variables at once but %s provides only %d "
            "callee-saved registers",
            f->name, c->max, c->abi->name, c->abi->npool);
}

int check_program(Program *prog, const Abi *abi, int require_main)
{
    Ctx c;
    memset(&c, 0, sizeof c);
    c.prog = prog;
    c.abi = abi;
    int before = rgx_nerrors;

    for (int i = 0; i < prog->nfuncs; i++) {
        Func *f = prog->funcs[i];
        f->qname = prog->package ? xsprintf("%s::%s", prog->package, f->name) : xstrdup(f->name);
        f->sym = mangle(prog, f->name);
        for (int j = 0; j < i; j++)
            if (strcmp(prog->funcs[j]->name, f->name) == 0)
                err(&c, f->line, f->col, "function '%s' is defined twice", f->name);
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
            if (m->nparams != 0)
                err(&c, m->line, m->col, "'main' with arguments is not implemented yet");
            if (m->ret_ty != TY_VOID && m->ret_storage != ST_REG)
                err(&c, m->line, m->col, "'main' must return 'reg i32' or 'void'");
        }
    }
    return rgx_nerrors - before;
}
