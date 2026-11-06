#include "codegen.h"

#include <stdarg.h>
#include <string.h>

#include "util.h"

/*
 * Code generation strategy (v0.1)
 *
 *  - 'reg' variables live in callee-saved registers for their whole scope.
 *    Nothing is ever spilled.
 *  - 'stack' variables live in 8-byte slots below rbp.
 *  - 'heap' variables keep a pointer in an 8-byte slot; the value is
 *    allocated by the runtime (rgx_rt_alloc) and released at scope end,
 *    before any return, or by drop().
 *  - eax / r10 / r11 are compiler scratch registers. Temporaries that must
 *    survive a nested call are pushed on the machine stack; that is a
 *    compiler-internal detail, not a Regix variable.
 *  - `return(f(...))` always becomes a jump (guaranteed tail call).
 */

static FILE *out;
static const Abi *abi;
static Func *fn;
static int label_seq;
static int cur_push;  /* 8-byte temporaries currently pushed */
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

/* ---- operands ---- */

static int slot_off(int idx) { return 8 * (idx + 1); }

static void dword_slot(char *buf, size_t sz, int idx)
{
    snprintf(buf, sz, "DWORD PTR [rbp-%d]", slot_off(idx));
}

static void qword_slot(char *buf, size_t sz, int idx)
{
    snprintf(buf, sz, "QWORD PTR [rbp-%d]", slot_off(idx));
}

/* Operands usable directly as a source and untouched by calls. */
static int is_simple(Expr *e)
{
    return e->kind == E_INT || (e->kind == E_VAR && e->vst != ST_HEAP);
}

static void operand(Expr *e, char *buf, size_t sz)
{
    if (e->kind == E_INT)
        snprintf(buf, sz, "%ld", e->val);
    else if (e->vst == ST_REG)
        snprintf(buf, sz, "%s", abi->pool32[e->reg]);
    else
        dword_slot(buf, sz, e->reg);
}

static int is_reg_or_imm(Expr *e)
{
    return e->kind == E_INT || (e->kind == E_VAR && e->vst == ST_REG);
}

static void gen_expr(Expr *e);

/* ---- calls ---- */

static void setup_args(Expr *call)
{
    int n = call->nargs;
    char buf[40];
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

/* ---- expressions ---- */

static void gen_bin(Expr *e)
{
    char buf[40];
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
    char buf[40];
    switch (e->kind) {
    case E_INT: emit("mov eax, %ld", e->val); break;
    case E_VAR:
        if (e->vst == ST_HEAP) {
            qword_slot(buf, sizeof buf, e->reg);
            emit("mov r11, %s", buf);
            emit("mov eax, DWORD PTR [r11]");
        } else {
            operand(e, buf, sizeof buf);
            emit("mov eax, %s", buf);
        }
        break;
    case E_NEG:
        gen_expr(e->l);
        emit("neg eax");
        break;
    case E_BIN: gen_bin(e); break;
    case E_CALL: gen_call(e); break;
    }
}

/* ---- stores, drop, release ---- */

static void gen_store(Storage st, int idx, Expr *e)
{
    char buf[40], slot[40];
    switch (st) {
    case ST_REG:
        if (is_simple(e)) {
            operand(e, buf, sizeof buf);
            emit("mov %s, %s", abi->pool32[idx], buf);
        } else {
            gen_expr(e);
            emit("mov %s, eax", abi->pool32[idx]);
        }
        break;
    case ST_STACK:
        dword_slot(slot, sizeof slot, idx);
        if (is_reg_or_imm(e)) {
            operand(e, buf, sizeof buf);
            emit("mov %s, %s", slot, buf);
        } else {
            gen_expr(e);
            emit("mov %s, eax", slot);
        }
        break;
    case ST_HEAP:
        gen_expr(e);
        qword_slot(slot, sizeof slot, idx);
        emit("mov r11, %s", slot);
        emit("mov DWORD PTR [r11], eax");
        break;
    default:
        break;
    }
}

static void gen_heap_decl(int idx, Expr *init)
{
    char slot[40];
    gen_expr(init);
    emit("call rgx_rt_alloc");       /* r10 = new block; other registers kept */
    emit("mov DWORD PTR [r10], eax");
    qword_slot(slot, sizeof slot, idx);
    emit("mov %s, r10", slot);
}

static void gen_release(Stmt **rel, int n)
{
    char slot[40];
    for (int i = n - 1; i >= 0; i--) {
        qword_slot(slot, sizeof slot, rel[i]->reg);
        emit("mov r10, %s", slot);
        emit("call rgx_rt_free");
    }
}

static void gen_drop(Stmt *s)
{
    char slot[40];
    switch (s->storage) {
    case ST_REG:
        emit("xor %s, %s", abi->pool32[s->reg], abi->pool32[s->reg]);
        break;
    case ST_STACK:
        dword_slot(slot, sizeof slot, s->reg);
        emit("mov %s, 0", slot);
        break;
    case ST_HEAP:
        qword_slot(slot, sizeof slot, s->reg);
        emit("mov r10, %s", slot);
        emit("mov DWORD PTR [r10], 0");
        emit("call rgx_rt_free");
        break;
    default:
        break;
    }
}

static void gen_epilogue(void)
{
    for (int i = fn->nregs - 1; i >= 0; i--)
        emit("pop %s", abi->pool64[i]);
    emit("mov rsp, rbp");
    emit("pop rbp");
}

/* ---- statements ---- */

static void gen_block(Block *b);

static void gen_stmt(Stmt *s)
{
    switch (s->kind) {
    case S_DECL:
        if (s->storage == ST_HEAP)
            gen_heap_decl(s->reg, s->expr);
        else
            gen_store(s->storage, s->reg, s->expr);
        break;
    case S_ASSIGN:
        gen_store(s->storage, s->reg, s->expr);
        break;
    case S_DROP:
        gen_drop(s);
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
            /* guaranteed tail call: arguments first, then release, then jump */
            setup_args(s->expr);
            gen_release(s->rel, s->nrel);
            if (s->expr->callee == fn) {
                emit_jump("jmp", lbl_body);
            } else {
                gen_epilogue();
                emit("jmp %s", s->expr->callee->sym);
            }
        } else {
            if (s->expr)
                gen_expr(s->expr);
            gen_release(s->rel, s->nrel);
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
    gen_release(b->rel, b->nrel);
}

/* ---- functions ---- */

static void gen_func(Func *f)
{
    fn = f;
    cur_push = 0;
    lbl_ret = label_seq++;
    lbl_body = label_seq++;

    /* After push rbp, rsp is 16-aligned. Keep it aligned after the slots and
     * the callee-saved pushes so every call site is aligned. */
    int frame = 8 * f->nslots;
    if ((f->nslots + f->nregs) % 2)
        frame += 8;

    if (abi->windows)
        fprintf(out, "%s PROC\n", f->sym);
    else
        fprintf(out, "%s:\n", f->sym);
    emit("push rbp");
    emit("mov rbp, rsp");
    if (frame)
        emit("sub rsp, %d", frame);
    for (int i = 0; i < f->nregs; i++)
        emit("push %s", abi->pool64[i]);
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

/* ---- heap runtime ---- */

/* rgx_rt_alloc: returns a zeroed block in r10, preserves all other registers.
 * rgx_rt_free:  releases the block in r10, preserves all registers.
 * Call sites are 16-byte aligned. */
static void gen_runtime(void)
{
    if (!abi->windows) {
        fputs("rgx_rt_alloc:\n", out);
        emit("push rax");
        emit("push rdi");
        emit("push rsi");
        emit("push rdx");
        emit("push r8");
        emit("push r9");
        emit("push rcx");
        emit("push r11");
        emit("mov eax, 9");        /* mmap(0, 4096, RW, PRIVATE|ANON, -1, 0) */
        emit("xor edi, edi");
        emit("mov esi, 4096");
        emit("mov edx, 3");
        emit("mov r10d, 34");
        emit("mov r8, -1");
        emit("xor r9d, r9d");
        emit("syscall");
        emit("mov r10, rax");
        emit("pop r11");
        emit("pop rcx");
        emit("pop r9");
        emit("pop r8");
        emit("pop rdx");
        emit("pop rsi");
        emit("pop rdi");
        emit("pop rax");
        emit("ret");
        fputs("\nrgx_rt_free:\n", out);
        emit("push rax");
        emit("push rdi");
        emit("push rsi");
        emit("push rcx");
        emit("push r11");
        emit("mov rdi, r10");      /* munmap(ptr, 4096) */
        emit("mov esi, 4096");
        emit("mov eax, 11");
        emit("syscall");
        emit("pop r11");
        emit("pop rcx");
        emit("pop rsi");
        emit("pop rdi");
        emit("pop rax");
        emit("ret");
        fputc('\n', out);
    } else {
        fputs("rgx_rt_alloc PROC\n", out);
        emit("push rax");
        emit("push rcx");
        emit("push rdx");
        emit("push r8");
        emit("push r9");
        emit("push r11");
        emit("sub rsp, 40");
        emit("call GetProcessHeap");
        emit("mov rcx, rax");
        emit("mov edx, 8");        /* HEAP_ZERO_MEMORY */
        emit("mov r8d, 16");
        emit("call HeapAlloc");
        emit("mov r10, rax");
        emit("add rsp, 40");
        emit("pop r11");
        emit("pop r9");
        emit("pop r8");
        emit("pop rdx");
        emit("pop rcx");
        emit("pop rax");
        emit("ret");
        fputs("rgx_rt_alloc ENDP\n\n", out);
        fputs("rgx_rt_free PROC\n", out);
        emit("push rax");
        emit("push rcx");
        emit("push rdx");
        emit("push r8");
        emit("push r9");
        emit("push r11");
        emit("push r10");
        emit("sub rsp, 32");
        emit("call GetProcessHeap");
        emit("mov rcx, rax");
        emit("xor edx, edx");
        emit("mov r8, QWORD PTR [rsp+32]");
        emit("call HeapFree");
        emit("add rsp, 32");
        emit("pop r10");
        emit("pop r11");
        emit("pop r9");
        emit("pop r8");
        emit("pop rdx");
        emit("pop rcx");
        emit("pop rax");
        emit("ret");
        fputs("rgx_rt_free ENDP\n\n", out);
    }
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
        if (prog->uses_heap) {
            fputs("EXTRN GetProcessHeap:PROC\n", out);
            fputs("EXTRN HeapAlloc:PROC\n", out);
            fputs("EXTRN HeapFree:PROC\n", out);
        }
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

    if (prog->uses_heap)
        gen_runtime();
    if (main_fn)
        gen_entry(main_fn);

    if (abi->windows)
        fputs("END\n", out);
    else
        fputs(".section .note.GNU-stack,\"\",@progbits\n", out);
}
