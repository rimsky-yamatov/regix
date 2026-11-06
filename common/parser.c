#include "parser.h"

#include <string.h>

#include "lexer.h"
#include "util.h"

typedef struct {
    const char *file;
    Token *t;
    size_t pos;
} Parser;

static Token *cur(Parser *p) { return &p->t[p->pos]; }
static Token *peek1(Parser *p)
{
    return cur(p)->kind == T_EOF ? cur(p) : &p->t[p->pos + 1];
}
static int at(Parser *p, TokKind k) { return cur(p)->kind == k; }

static Token *advance(Parser *p)
{
    Token *t = cur(p);
    if (t->kind != T_EOF)
        p->pos++;
    return t;
}

static int accept(Parser *p, TokKind k)
{
    if (!at(p, k))
        return 0;
    advance(p);
    return 1;
}

static Token *expect(Parser *p, TokKind k, const char *what)
{
    Token *t = cur(p);
    if (t->kind != k)
        rgx_die(p->file, t->line, t->col, "expected %s, found %s", what, tok_name(t->kind));
    return advance(p);
}

static char *ident_text(Token *t) { return xstrndup(t->text, (size_t)t->len); }

static char *parse_qname(Parser *p)
{
    char *s = ident_text(expect(p, T_IDENT, "identifier"));
    while (at(p, T_COLONCOLON) && peek1(p)->kind == T_IDENT) {
        advance(p);
        char *part = ident_text(advance(p));
        char *n = xsprintf("%s::%s", s, part);
        s = n;
    }
    return s;
}

static Storage accept_storage(Parser *p)
{
    switch (cur(p)->kind) {
    case T_KW_REG: advance(p); return ST_REG;
    case T_KW_STACK: advance(p); return ST_STACK;
    case T_KW_HEAP: advance(p); return ST_HEAP;
    default: return ST_NONE;
    }
}

static Ty parse_type(Parser *p, int allow_void)
{
    Token *t = expect(p, T_IDENT, "type");
    if (t->len == 3 && memcmp(t->text, "i32", 3) == 0)
        return TY_I32;
    if (allow_void && t->len == 4 && memcmp(t->text, "void", 4) == 0)
        return TY_VOID;
    rgx_die(p->file, t->line, t->col, "unsupported type '%.*s' (v0.1 supports only i32)",
            t->len, t->text);
}

/* ---- expressions ---- */

static Expr *mk_expr(ExprKind k, Token *t)
{
    Expr *e = xcalloc(sizeof *e);
    e->kind = k;
    e->line = t->line;
    e->col = t->col;
    return e;
}

static Expr *parse_expr(Parser *p);

static Expr *parse_primary(Parser *p)
{
    Token *t = cur(p);
    if (t->kind == T_INT) {
        advance(p);
        Expr *e = mk_expr(E_INT, t);
        e->val = t->ival;
        return e;
    }
    if (t->kind == T_IDENT) {
        char *name = parse_qname(p);
        if (accept(p, T_LPAREN)) {
            Expr *e = mk_expr(E_CALL, t);
            e->name = name;
            if (!at(p, T_RPAREN)) {
                do {
                    Expr *a = parse_expr(p);
                    VEC_PUSH(e->args, e->nargs, e->cap, a);
                } while (accept(p, T_COMMA));
            }
            expect(p, T_RPAREN, "')'");
            return e;
        }
        Expr *e = mk_expr(E_VAR, t);
        e->name = name;
        return e;
    }
    if (t->kind == T_LPAREN) {
        advance(p);
        Expr *e = parse_expr(p);
        expect(p, T_RPAREN, "')'");
        return e;
    }
    rgx_die(p->file, t->line, t->col, "expected expression, found %s", tok_name(t->kind));
}

static Expr *parse_unary(Parser *p)
{
    Token *t = cur(p);
    if (accept(p, T_MINUS)) {
        Expr *e = mk_expr(E_NEG, t);
        e->l = parse_unary(p);
        return e;
    }
    return parse_primary(p);
}

static Expr *mk_bin(BinOp op, Token *t, Expr *l, Expr *r)
{
    Expr *e = mk_expr(E_BIN, t);
    e->op = op;
    e->l = l;
    e->r = r;
    return e;
}

static Expr *parse_mul(Parser *p)
{
    Expr *l = parse_unary(p);
    while (at(p, T_STAR)) {
        Token *t = advance(p);
        l = mk_bin(OP_MUL, t, l, parse_unary(p));
    }
    return l;
}

static Expr *parse_add(Parser *p)
{
    Expr *l = parse_mul(p);
    while (at(p, T_PLUS) || at(p, T_MINUS)) {
        Token *t = advance(p);
        l = mk_bin(t->kind == T_PLUS ? OP_ADD : OP_SUB, t, l, parse_mul(p));
    }
    return l;
}

static Expr *parse_expr(Parser *p)
{
    Expr *l = parse_add(p);
    for (;;) {
        BinOp op;
        switch (cur(p)->kind) {
        case T_LT: op = OP_LT; break;
        case T_GT: op = OP_GT; break;
        case T_LE: op = OP_LE; break;
        case T_GE: op = OP_GE; break;
        case T_EQ: op = OP_EQ; break;
        case T_NE: op = OP_NE; break;
        default: return l;
        }
        Token *t = advance(p);
        l = mk_bin(op, t, l, parse_add(p));
    }
}

/* ---- statements ---- */

static Block *parse_block(Parser *p);

static Stmt *mk_stmt(StmtKind k, Token *t)
{
    Stmt *s = xcalloc(sizeof *s);
    s->kind = k;
    s->line = t->line;
    s->col = t->col;
    return s;
}

static Stmt *parse_if(Parser *p)
{
    Token *t = expect(p, T_KW_IF, "'if'");
    Stmt *s = mk_stmt(S_IF, t);
    expect(p, T_LPAREN, "'('");
    s->expr = parse_expr(p);
    expect(p, T_RPAREN, "')'");
    s->then_b = parse_block(p);
    if (accept(p, T_KW_ELSE)) {
        if (at(p, T_KW_IF)) {
            Block *b = xcalloc(sizeof *b);
            Stmt *inner = parse_if(p);
            VEC_PUSH(b->stmts, b->n, b->cap, inner);
            s->else_b = b;
        } else {
            s->else_b = parse_block(p);
        }
    }
    return s;
}

static Stmt *parse_stmt(Parser *p)
{
    Token *t = cur(p);
    Stmt *s;

    Storage st = accept_storage(p);
    if (st != ST_NONE) {
        s = mk_stmt(S_DECL, t);
        s->storage = st;
        s->name = ident_text(expect(p, T_IDENT, "variable name"));
        expect(p, T_COLON, "':'");
        s->ty = parse_type(p, 0);
        if (accept(p, T_ASSIGN))
            s->expr = parse_expr(p);
    } else if (t->kind == T_KW_IF) {
        return parse_if(p);
    } else if (t->kind == T_KW_WHILE) {
        advance(p);
        s = mk_stmt(S_WHILE, t);
        expect(p, T_LPAREN, "'('");
        s->expr = parse_expr(p);
        expect(p, T_RPAREN, "')'");
        s->then_b = parse_block(p);
        return s;
    } else if (t->kind == T_KW_RETURN) {
        advance(p);
        s = mk_stmt(S_RETURN, t);
        if (!at(p, T_SEMI) && !at(p, T_RBRACE))
            s->expr = parse_expr(p);
    } else if (t->kind == T_LBRACE) {
        s = mk_stmt(S_BLOCK, t);
        s->then_b = parse_block(p);
        return s;
    } else if (t->kind == T_IDENT && t->len == 4 && memcmp(t->text, "drop", 4) == 0 &&
               peek1(p)->kind == T_LPAREN) {
        advance(p);
        advance(p);
        s = mk_stmt(S_DROP, t);
        s->name = ident_text(expect(p, T_IDENT, "variable name"));
        expect(p, T_RPAREN, "')'");
    } else if (t->kind == T_IDENT && peek1(p)->kind == T_ASSIGN) {
        s = mk_stmt(S_ASSIGN, t);
        s->name = ident_text(advance(p));
        advance(p);
        s->expr = parse_expr(p);
    } else {
        s = mk_stmt(S_EXPR, t);
        s->expr = parse_expr(p);
    }
    accept(p, T_SEMI);
    return s;
}

static Block *parse_block(Parser *p)
{
    expect(p, T_LBRACE, "'{'");
    Block *b = xcalloc(sizeof *b);
    while (!at(p, T_RBRACE)) {
        if (at(p, T_EOF))
            rgx_die(p->file, cur(p)->line, cur(p)->col, "expected '}', found end of file");
        Stmt *s = parse_stmt(p);
        VEC_PUSH(b->stmts, b->n, b->cap, s);
    }
    expect(p, T_RBRACE, "'}'");
    return b;
}

/* ---- items ---- */

static Func *parse_func(Parser *p, int is_pub)
{
    Token *ft = expect(p, T_KW_FN, "'fn'");
    Func *f = xcalloc(sizeof *f);
    f->line = ft->line;
    f->col = ft->col;
    f->is_pub = is_pub;
    f->name = ident_text(expect(p, T_IDENT, "function name"));
    expect(p, T_LPAREN, "'('");
    if (!at(p, T_RPAREN)) {
        do {
            Param pr;
            memset(&pr, 0, sizeof pr);
            Token *pt = cur(p);
            pr.line = pt->line;
            pr.col = pt->col;
            pr.storage = accept_storage(p);
            if (pr.storage == ST_NONE) {
                if (pt->kind == T_IDENT && peek1(p)->kind == T_COLON)
                    rgx_die(p->file, pt->line, pt->col,
                            "parameter '%.*s' needs a storage class (reg, stack or heap)",
                            pt->len, pt->text);
                rgx_die(p->file, pt->line, pt->col,
                        "expected parameter storage class (reg, stack or heap), found %s",
                        tok_name(pt->kind));
            }
            pr.name = ident_text(expect(p, T_IDENT, "parameter name"));
            expect(p, T_COLON, "':'");
            pr.ty = parse_type(p, 0);
            VEC_PUSH(f->params, f->nparams, f->pcap, pr);
        } while (accept(p, T_COMMA));
    }
    expect(p, T_RPAREN, "')'");

    Token *at_tok = cur(p);
    if (!accept(p, T_ARROW))
        rgx_die(p->file, at_tok->line, at_tok->col,
                "return type is required for function '%s': write '-> reg i32' or '-> void'",
                f->name);
    Token *rt = cur(p);
    if (rt->kind == T_IDENT && rt->len == 4 && memcmp(rt->text, "void", 4) == 0) {
        advance(p);
        f->ret_ty = TY_VOID;
        f->ret_storage = ST_NONE;
    } else {
        f->ret_storage = accept_storage(p);
        if (f->ret_storage == ST_NONE)
            rgx_die(p->file, rt->line, rt->col,
                    "return type needs a storage class (reg, stack or heap), e.g. '-> reg i32'");
        f->ret_ty = parse_type(p, 0);
    }
    f->body = parse_block(p);
    return f;
}

Program *parse_program(const char *file, const char *src)
{
    Parser p;
    p.file = file;
    p.t = lex(file, src, NULL);
    p.pos = 0;

    Program *prog = xcalloc(sizeof *prog);
    prog->file = file;

    if (accept(&p, T_KW_PACKAGE)) {
        prog->package = parse_qname(&p);
        accept(&p, T_SEMI);
    }
    while (at(&p, T_KW_IMPORT)) {
        advance(&p);
        char *name = parse_qname(&p);
        VEC_PUSH(prog->imports, prog->nimports, prog->icap, name);
        accept(&p, T_SEMI);
    }
    while (!at(&p, T_EOF)) {
        int is_pub = accept(&p, T_KW_PUB);
        if (!at(&p, T_KW_FN)) {
            Token *t = cur(&p);
            rgx_die(file, t->line, t->col, "expected 'fn', found %s", tok_name(t->kind));
        }
        Func *f = parse_func(&p, is_pub);
        VEC_PUSH(prog->funcs, prog->nfuncs, prog->fcap, f);
    }
    return prog;
}
