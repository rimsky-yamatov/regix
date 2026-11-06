#include "codegen.h"

#include <stdarg.h>
#include <string.h>

#include "util.h"

/*
 * Code generation strategy (v0.1)
 *
 *  - Every 'reg' variable lives in a callee-saved register for its whole
 *    scope. Nothing is ever spilled.
 *  - eax / r10d are compiler scratch registers. Temporaries that must
 *    survive a nested call are pushed on the machine stack; that is a
 *    compiler-internal detail, not a 'reg' variable.
 *  - `return(f(...))` always becomes a jump (guaranteed tail call).
 */

static FILE *out;
static const Abi *abi;
static Func *fn;
static int label_seq;
static int cur_push;  /* 8-byte temporaries currently pushed */
static int cur_pad;   /* alignment padding in the current frame */
static int lbl_ret, lbl_body;

static void emit(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("    ", out);
    vfprintf(out, fmt, ap);
    fputc('\n', out);
    va_end(ap);
}

static void label_def(int n)
{
    fprintf(out, abi->windows ? "L%d:\n" : ".L%d:\n", n);
}

static void label_ref(char *buf, size_t sz, int n)
{
    snprintf(buf, sz, abi->windows ? "L%d" : ".L%d", n);
}

static void emit_jump(const char *op, int label)
{
    char buf[32];
    label_ref(buf, sizeof buf, label);
    emit("%s %s", op, buf);
}

static int is_simple(Expr *e) { return e->kind == E_INT || e->kind == E_VAR; }

static void operand(Expr *e, char *buf, size_t sz)
{
    if (e->kind == E_INT)
        snprintf(buf, sz, "%ld", e->val);
    else
        snprintf(buf, sz, "%s", abi->pool32[e->reg]);
}

static void gen_expr(Expr *e);

static void setup_args(Expr *call)
{
    int n = call->nargs;
    char buf[32];
    for (int i = 0; i < n; i++) {
        if (!is_simple(call->args[i])) {
            gen_expr(call->args[i]);
            emit("push rax");
            cur_push++;
        }
    }
    for (int i = n - 1; i >= 0; i--) {
        if (!is_simple(call->args[i])) {
            emit("pop %s", abi->arg64[i]);
            cur_push--;
        }
    }
    for (int i = 0; i < n; i++) {
        if (is_simple(call->args[i])) {
            operand(call->args[i], buf, sizeof buf);
            emit("mov %s, %s", abi->arg32[i], buf);
        }
    }
}

static void gen_call(Expr *e)
{
    setup_args(e);
    int adj = abi->shadow + ((cur_push & 1) ? 8 : 0);
    if (adj)
        emit("sub rsp, %d", adj);
    emit("call %s", e->callee->sym);
    if (adj)
        emit("add rsp, %d", adj);
}

static void gen_bin(Expr *e)
{
    char buf[32];
    const char *rhs;
    int imm = 0;

    if (is_simple(e->r)) {
        gen_expr(e->l);
        operand(e->r, buf, sizeof buf);
        rhs = buf;
        imm = e->r->kind == E_INT;
    } else if (is_simple(e->l)) {
        gen_expr(e->r);
        emit("mov r10d, eax");
        operand(e->l, buf, sizeof buf);
        emit("mov eax, %s", buf);
        rhs = "r10d";
    } else {
        gen_expr(e->l);
        emit("push rax");
        cur_push++;
        gen_expr(e->r);
        cur_push--;
        emit("mov r10d, eax");
        emit("pop rax");
        rhs = "r10d";
    }

    switch (e->op) {
    case OP_ADD: emit("add eax, %s", rhs); break;
    case OP_SUB: emit("sub eax, %s", rhs); break;
    case OP_MUL:
        if (imm)
            emit("imul eax, eax, %s", rhs);
        else
            emit("imul eax, %s", rhs);
        break;
    default: {
        const char *set = "sete";
        switch (e->op) {
        case OP_LT: set = "setl"; break;
        case OP_GT: set = "setg"; break;
        case OP_LE: set = "setle"; break;
        case OP_GE: set = "setge"; break;
        case OP_EQ: set = "sete"; break;
        case OP_NE: set = "setne"; break;
        default: break;
        }
        emit("cmp eax, %s", rhs);
        emit("%s al", set);
        emit("movzx eax, al");
        break;
    }
    }
}

static void gen_expr(Expr *e)
{
    switch (e->kind) {
    case E_INT: emit("mov eax, %ld", e->val); break;
    case E_VAR: emit("mov eax, %s", abi->pool32[e->reg]); break;
    case E_NEG:
        gen_expr(e->l);
        emit("neg eax");
        break;
    case E_BIN: gen_bin(e); break;
    case E_CALL: gen_call(e); break;
    }
}

static void gen_epilogue(void)
{
    if (cur_pad)
        emit("add rsp, %d", cur_pad);
    for (int i = fn->nregs - 1; i >= 0; i--)
        emit("pop %s", abi->pool64[i]);
}

static void gen_block(Block *b);

static void gen_store(int reg, Expr *e)
{
    char buf[32];
    if (is_simple(e)) {
        operand(e, buf, sizeof buf);
        emit("mov %s, %s", abi->pool32[reg], buf);
    } else {
        gen_expr(e);
        emit("mov %s, eax", abi->pool32[reg]);
    }
}

static void gen_stmt(Stmt *s)
{
    switch (s->kind) {
    case S_DECL:
    case S_ASSIGN:
        gen_store(s->reg, s->expr);
        break;
    case S_IF: {
        int l_else = label_seq++;
        gen_expr(s->expr);
        emit("test eax, eax");
        emit_jump("jz", l_else);
        gen_block(s->then_b);
        if (s->else_b) {
            int l_end = label_seq++;
            emit_jump("jmp", l_end);
            label_def(l_else);
            gen_block(s->else_b);
            label_def(l_end);
        } else {
            label_def(l_else);
        }
        break;
    }
    case S_WHILE: {
        int l_top = label_seq++, l_end = label_seq++;
        label_def(l_top);
        gen_expr(s->expr);
        emit("test eax, eax");
        emit_jump("jz", l_end);
        gen_block(s->then_b);
        emit_jump("jmp", l_top);
        label_def(l_end);
        break;
    }
    case S_RETURN:
        if (s->expr && s->expr->kind == E_CALL) {
            /* guaranteed tail call */
            setup_args(s->expr);
            if (s->expr->callee == fn) {
                emit_jump("jmp", lbl_body);
            } else {
                gen_epilogue();
                emit("jmp %s", s->expr->callee->sym);
            }
        } else {
            if (s->expr)
                gen_expr(s->expr);
            emit_jump("jmp", lbl_ret);
        }
        break;
    case S_EXPR:
        gen_expr(s->expr);
        break;
    case S_BLOCK:
        gen_block(s->then_b);
        break;
    }
}

static void gen_block(Block *b)
{
    for (int i = 0; i < b->n; i++)
        gen_stmt(b->stmts[i]);
}

static void gen_func(Func *f)
{
    fn = f;
    cur_push = 0;
    cur_pad = (f->nregs % 2 == 0) ? 8 : 0; /* keep rsp 16-byte aligned at calls */
    lbl_ret = label_seq++;
    lbl_body = label_seq++;

    if (abi->windows)
        fprintf(out, "%s PROC\n", f->sym);
    else
        fprintf(out, "%s:\n", f->sym);
    for (int i = 0; i < f->nregs; i++)
        emit("push %s", abi->pool64[i]);
    if (cur_pad)
        emit("sub rsp, %d", cur_pad);
    label_def(lbl_body);
    for (int i = 0; i < f->nparams; i++)
        emit("mov %s, %s", abi->pool32[i], abi->arg32[i]);

    gen_block(f->body);

    label_def(lbl_ret);
    gen_epilogue();
    emit("ret");
    if (abi->windows)
        fprintf(out, "%s ENDP\n", f->sym);
    fputc('\n', out);
}

static void gen_entry(Func *m)
{
    int is_void = m->ret_ty == TY_VOID;
    if (!abi->windows) {
        fputs("_start:\n", out);
        emit("call %s", m->sym);
        if (is_void)
            emit("xor edi, edi");
        else
            emit("mov edi, eax");
        emit("mov eax, 60");
        emit("syscall");
    } else {
        fputs("mainCRTStartup PROC\n", out);
        emit("sub rsp, 40");
        emit("call %s", m->sym);
        if (is_void)
            emit("xor ecx, ecx");
        else
            emit("mov ecx, eax");
        emit("call ExitProcess");
        fputs("mainCRTStartup ENDP\n", out);
    }
}

void codegen_program(Program *prog, const Abi *a, FILE *o)
{
    out = o;
    abi = a;
    label_seq = 0;

    Func *main_fn = NULL;
    for (int i = 0; i < prog->nfuncs; i++)
        if (strcmp(prog->funcs[i]->name, "main") == 0)
            main_fn = prog->funcs[i];

    if (abi->windows) {
        fputs("OPTION CASEMAP:NONE\n", out);
        fputs("EXTRN ExitProcess:PROC\n", out);
        for (int i = 0; i < prog->nfuncs; i++)
            fprintf(out, "PUBLIC %s\n", prog->funcs[i]->sym);
        if (main_fn)
            fputs("PUBLIC mainCRTStartup\n", out);
        fputs(".code\n\n", out);
    } else {
        fputs(".intel_syntax noprefix\n.text\n", out);
        for (int i = 0; i < prog->nfuncs; i++)
            fprintf(out, ".globl %s\n", prog->funcs[i]->sym);
        if (main_fn)
            fputs(".globl _start\n", out);
        fputc('\n', out);
    }

    for (int i = 0; i < prog->nfuncs; i++)
        gen_func(prog->funcs[i]);

    if (main_fn)
        gen_entry(main_fn);

    if (abi->windows)
        fputs("END\n", out);
    else
        fputs(".section .note.GNU-stack,\"\",@progbits\n", out);
}
