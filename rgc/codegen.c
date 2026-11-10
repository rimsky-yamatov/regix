#include "codegen.h"

#include <stdarg.h>
#include <string.h>

#include "util.h"

/*
 * Code generation strategy (v0.2)
 *
 *  - 'reg' variables live in callee-saved registers for their whole scope.
 *    Nothing is ever spilled.
 *  - 'stack' variables live in 8-byte frame slots below rbp. A stack struct
 *    or array occupies consecutive slots; its bytes start at the lowest
 *    address. A slice parameter takes two slots (pointer, length).
 *  - 'heap' variables keep a pointer in an 8-byte slot; the value is
 *    allocated by the runtime (rgx_rt_alloc, size in r10d) and released at
 *    scope end, before any return, or by drop(). A heap value can be moved
 *    into a 'heap' parameter or out through a '-> heap' return.
 *  - A slice argument uses two argument registers (pointer, length).
 *  - Indexing is bounds checked; failure jumps to rgx_rt_panic (exit 101).
 *  - eax / r10 / r11 are compiler scratch registers. Temporaries that must
 *    survive a nested call are pushed on the machine stack; that is a
 *    compiler-internal detail, not a Regix variable.
 *  - `return(f(...))` becomes a jump (guaranteed tail call), except when an
 *    argument borrows an array that lives in the current frame.
 */

static FILE *out;
static const Abi *abi;
static Program *prog_g;
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

static int esz(Ty e) { return e == TY_I32 ? 4 : (e == TY_U8 ? 1 : 16); }

/* ---- operands ---- */

/* Memory operand `off` bytes into a stack value occupying `bslots` slots that
 * start at slot `idx`. */
static void mem_at(char *buf, size_t sz, const char *kw, int idx, int bslots, int off)
{
    snprintf(buf, sz, "%s PTR [rbp-%d]", kw, 8 * (idx + bslots) - off);
}

static void dword_at(char *buf, size_t sz, int idx, int bslots, int off)
{
    mem_at(buf, sz, "DWORD", idx, bslots, off);
}

static void qword_slot(char *buf, size_t sz, int idx)
{
    snprintf(buf, sz, "QWORD PTR [rbp-%d]", 8 * (idx + 1));
}

/* Second slot of a slice: its length. */
static void len_slot(char *buf, size_t sz, const char *kw, int idx)
{
    snprintf(buf, sz, "%s PTR [rbp-%d]", kw, 8 * (idx + 2));
}

static void mem_via(char *buf, size_t sz, const char *kw, const char *base, int off)
{
    if (off)
        snprintf(buf, sz, "%s PTR [%s+%d]", kw, base, off);
    else
        snprintf(buf, sz, "%s PTR [%s]", kw, base);
}

static void dword_via(char *buf, size_t sz, const char *base, int off)
{
    mem_via(buf, sz, "DWORD", base, off);
}

static int is_var_like(Expr *e) { return e->kind == E_VAR || e->kind == E_FIELD; }

/* Operands usable directly as a source and untouched by calls. */
static int is_simple(Expr *e)
{
    if (e->kind == E_FIELD && e->ndref > 0)
        return 0;
    return e->kind == E_INT || (is_var_like(e) && e->vst != ST_HEAP);
}

static void operand(Expr *e, char *buf, size_t sz)
{
    if (e->kind == E_INT)
        snprintf(buf, sz, "%ld", e->val);
    else if (e->kind == E_VAR && e->vst == ST_REG)
        snprintf(buf, sz, "%s", abi->pool32[e->reg]);
    else if (e->kind == E_VAR)
        dword_at(buf, sz, e->reg, 1, 0);
    else
        dword_at(buf, sz, e->reg, e->bslots, e->foff);
}

static int is_reg_or_imm(Expr *e)
{
    return e->kind == E_INT || (e->kind == E_VAR && e->vst == ST_REG);
}

/* r = address of the struct holding the final field of `e`: the variable's
 * storage, then every owned pointer on the way (each checked for null). */
static void load_path_base(const char *r, Expr *e)
{
    char slot[64];
    if (e->vst == ST_STACK) {
        emit("lea %s, [rbp-%d]", r, 8 * (e->reg + e->bslots));
    } else {
        qword_slot(slot, sizeof slot, e->reg);
        emit("mov %s, %s", r, slot);
        emit("test %s, %s", r, r);
        emit("jz rgx_rt_panic");
    }
    for (int i = 0; i < e->ndref; i++) {
        emit("mov %s, QWORD PTR [%s+%d]", r, r, e->dref[i]);
        emit("test %s, %s", r, r);
        emit("jz rgx_rt_panic");
    }
}

static void gen_expr(Expr *e);
static void gen_alloc(int size, int slot_idx);
static void gen_struct_init(Storage st, int idx, int bslots, StructDef *sd, Expr *lit, int base);

/* ---- arrays and slices ---- */

/* Load the address of element 0 of the array or slice `e` refers to. */
static void load_base(const char *reg, Expr *e)
{
    char slot[48];
    if (e->vty == TY_ARRAY && e->vst == ST_STACK) {
        emit("lea %s, [rbp-%d]", reg, 8 * (e->reg + e->bslots));
    } else {
        qword_slot(slot, sizeof slot, e->reg);   /* heap pointer, or slice pointer */
        emit("mov %s, %s", reg, slot);
    }
}

/* eax holds an index: panic unless it is below the length. */
static void bounds_check(Expr *e)
{
    char len[48];
    if (e->vty == TY_SLICE) {
        len_slot(len, sizeof len, "DWORD", e->reg);
        emit("cmp eax, %s", len);
    } else {
        emit("cmp eax, %d", e->ai.alen);
    }
    emit("jae rgx_rt_panic");
}

static void gen_index_prep(Expr *e)
{
    gen_expr(e->r);
    bounds_check(e);
    load_base("r11", e);
}

/* ---- calls ---- */

enum { A_COMPLEX, A_SIMPLE, A_HEAP, A_SLICE, A_REF };

static int arg_kind(Expr *call, int i)
{
    Param *p = &call->callee->params[i];
    if (p->storage == ST_HEAP && p->ty != TY_SLICE &&
        (call->args[i]->kind == E_CALL || call->args[i]->kind == E_STRUCTLIT))
        return A_COMPLEX;   /* a new owner is built first and its pointer pushed */
    if (p->ty == TY_SLICE)
        return A_SLICE;
    if (p->ty == TY_REF)
        return A_REF;
    if (p->storage == ST_HEAP)
        return A_HEAP;
    return is_simple(call->args[i]) ? A_SIMPLE : A_COMPLEX;
}

static void set_slice_regs(Expr *a, int ar)
{
    char buf[64], len[64];
    const char *ptr = abi->arg64[ar];
    switch (a->kind) {
    case E_STR:
        if (abi->windows)
            emit("lea %s, rgx_str%ld", ptr, a->val);
        else
            emit("lea %s, [rip+.Lstr%ld]", ptr, a->val);
        emit("mov %s, %d", abi->arg32[ar + 1], prog_g->strs[a->val].len);
        break;
    case E_ADDR:
        if (a->vty == TY_SLICE) {
            qword_slot(buf, sizeof buf, a->reg);
            len_slot(len, sizeof len, "QWORD", a->reg);
            emit("mov %s, %s", ptr, buf);
            emit("mov %s, %s", abi->arg64[ar + 1], len);
        } else {
            load_base(ptr, a);
            emit("mov %s, %d", abi->arg32[ar + 1], a->ai.alen);
        }
        break;
    case E_INDEX: /* an element of an &[&[u8]] */
        operand(a->r, buf, sizeof buf);
        emit("mov eax, %s", buf);
        bounds_check(a);
        qword_slot(buf, sizeof buf, a->reg);
        emit("mov r11, %s", buf);
        emit("shl rax, 4");
        emit("mov %s, QWORD PTR [r11+rax]", ptr);
        emit("mov %s, QWORD PTR [r11+rax+8]", abi->arg64[ar + 1]);
        break;
    default:
        break;
    }
}

/* Pass the address of a borrowed variable or field in argument register `ar`. */
static void gen_borrow_addr(Expr *a, const char *r)
{
    char slot[64];
    if (a->ndref == 0 && !a->lastptr) {
        if (a->vst == ST_HEAP) {      /* heap value or reference: the slot holds a pointer */
            qword_slot(slot, sizeof slot, a->reg);
            emit("mov %s, %s", r, slot);
            emit("test %s, %s", r, r);
            emit("jz rgx_rt_panic");
            if (a->foff)
                emit("add %s, %d", r, a->foff);
        } else {                      /* stack value: address inside the frame */
            emit("lea %s, [rbp-%d]", r, 8 * (a->reg + a->bslots) - a->foff);
        }
        return;
    }
    load_path_base("r11", a);
    if (a->lastptr) {                 /* borrow what the owned pointer points to */
        emit("mov r11, QWORD PTR [r11+%d]", a->foff);
        emit("test r11, r11");
        emit("jz rgx_rt_panic");
        emit("mov %s, r11", r);
    } else if (a->foff) {
        emit("lea %s, [r11+%d]", r, a->foff);
    } else {
        emit("mov %s, r11", r);
    }
}

static void set_ref_reg(Expr *a, int ar)
{
    gen_borrow_addr(a, abi->arg64[ar]);
}

static void setup_args(Expr *call)
{
    int n = call->nargs;
    char buf[64];
    for (int i = 0; i < n; i++) {
        if (arg_kind(call, i) == A_COMPLEX) {
            gen_expr(call->args[i]);
            emit("push rax");
            cur_push++;
        }
    }
    for (int i = n - 1; i >= 0; i--) {
        if (arg_kind(call, i) == A_COMPLEX) {
            emit("pop %s", abi->arg64[call->callee->params[i].areg]);
            cur_push--;
        }
    }
    for (int i = 0; i < n; i++) {
        int ar = call->callee->params[i].areg;
        switch (arg_kind(call, i)) {
        case A_HEAP:
            if (call->args[i]->kind == E_NULL) {
                emit("mov %s, 0", abi->arg64[ar]);
            } else {
                qword_slot(buf, sizeof buf, call->args[i]->reg);
                emit("mov %s, %s", abi->arg64[ar], buf);
            }
            break;
        case A_SIMPLE:
            operand(call->args[i], buf, sizeof buf);
            emit("mov %s, %s", abi->arg32[ar], buf);
            break;
        case A_SLICE:
            set_slice_regs(call->args[i], ar);
            break;
        case A_REF:
            set_ref_reg(call->args[i], ar);
            break;
        default:
            break;
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
    char buf[64];
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
    char buf[64], slot[64];
    switch (e->kind) {
    case E_INT: emit("mov eax, %ld", e->val); break;
    case E_VAR:
        if (e->vst == ST_HEAP) {
            qword_slot(slot, sizeof slot, e->reg);
            emit("mov r11, %s", slot);
            emit("mov eax, DWORD PTR [r11]");
        } else {
            operand(e, buf, sizeof buf);
            emit("mov eax, %s", buf);
        }
        break;
    case E_FIELD:
        if (e->vst == ST_STACK && e->ndref == 0) {
            operand(e, buf, sizeof buf);
            emit("mov eax, %s", buf);
        } else {
            load_path_base("r11", e);
            dword_via(buf, sizeof buf, "r11", e->foff);
            emit("mov eax, %s", buf);
        }
        break;
    case E_ISNULL: {
        Expr *x = e->l;
        if (x->kind == E_VAR) {
            qword_slot(slot, sizeof slot, x->reg);
            emit("mov rax, %s", slot);
        } else {
            load_path_base("r11", x);
            emit("mov rax, QWORD PTR [r11+%d]", x->foff);
        }
        emit("test rax, rax");
        emit(e->val ? "sete al" : "setne al");
        emit("movzx eax, al");
        break;
    }
    case E_INDEX:
        gen_index_prep(e);
        if (e->ai.elem == TY_U8)
            emit("movzx eax, BYTE PTR [r11+rax]");
        else
            emit("mov eax, DWORD PTR [r11+rax*4]");
        break;
    case E_DEREF:
        qword_slot(slot, sizeof slot, e->reg);
        emit("mov r11, %s", slot);
        emit("mov eax, DWORD PTR [r11]");
        break;
    case E_LEN:
        if (e->vty == TY_SLICE) {
            len_slot(buf, sizeof buf, "DWORD", e->reg);
            emit("mov eax, %s", buf);
        } else {
            emit("mov eax, %d", e->ai.alen);
        }
        break;
    case E_NEG:
        gen_expr(e->l);
        emit("neg eax");
        break;
    case E_BIN: gen_bin(e); break;
    case E_CALL: gen_call(e); break;
    case E_STRUCTLIT:                  /* a new heap block, filled in place; pointer in rax */
        gen_alloc(e->sdef->size, e->reg);
        gen_struct_init(ST_HEAP, e->reg, 1, e->sdef, e, 0);
        qword_slot(slot, sizeof slot, e->reg);
        emit("mov rax, %s", slot);
        break;
    default: break; /* literals and borrows are handled by their context */
    }
}

/* ---- stores ---- */

/* Store expression `e` (esz bytes) at byte `off` of a stack or heap value. */
static void gen_store_elem(Storage st, int idx, int bslots, int off, int size, Expr *e)
{
    char buf[64], mem[64], slot[64];
    const char *kw = size == 1 ? "BYTE" : "DWORD";
    const char *src = size == 1 ? "al" : "eax";
    if (st == ST_STACK) {
        mem_at(mem, sizeof mem, kw, idx, bslots, off);
        if (e->kind == E_INT) {
            emit("mov %s, %ld", mem, size == 1 ? (e->val & 255) : e->val);
        } else if (size == 4 && is_reg_or_imm(e)) {
            operand(e, buf, sizeof buf);
            emit("mov %s, %s", mem, buf);
        } else {
            gen_expr(e);
            emit("mov %s, %s", mem, src);
        }
    } else {
        gen_expr(e);
        qword_slot(slot, sizeof slot, idx);
        emit("mov r11, %s", slot);
        mem_via(mem, sizeof mem, kw, "r11", off);
        emit("mov %s, %s", mem, src);
    }
}

static void gen_store_mem(Storage st, int idx, int bslots, int off, Expr *e)
{
    gen_store_elem(st, idx, bslots, off, 4, e);
}

static void gen_store_var(Storage st, int idx, Expr *e)
{
    char buf[64];
    if (st == ST_REG) {
        if (is_simple(e)) {
            operand(e, buf, sizeof buf);
            emit("mov %s, %s", abi->pool32[idx], buf);
        } else {
            gen_expr(e);
            emit("mov %s, eax", abi->pool32[idx]);
        }
    } else {
        gen_store_mem(st, idx, 1, 0, e);
    }
}

static Expr *lit_value(Expr *lit, const char *name)
{
    for (int i = 0; i < lit->nargs; i++)
        if (strcmp(lit->fnames[i], name) == 0)
            return lit->args[i];
    return NULL;
}

/* Store an owned pointer (null, a moved heap variable, or a call result) at
 * byte `off` of a stack or heap struct. */
static void gen_store_ptr(Storage st, int idx, int bslots, int off, Expr *x)
{
    char slot[64];
    if (x->kind == E_NULL) {
        emit("xor eax, eax");
    } else if (x->kind == E_VAR) {
        qword_slot(slot, sizeof slot, x->reg);
        emit("mov rax, %s", slot);
    } else {
        gen_expr(x);                 /* a call: the owned pointer comes back in rax */
    }
    if (st == ST_STACK) {
        emit("mov QWORD PTR [rbp-%d], rax", 8 * (idx + bslots) - off);
    } else {
        qword_slot(slot, sizeof slot, idx);
        emit("mov r11, %s", slot);
        emit("mov QWORD PTR [r11+%d], rax", off);
    }
}

static void gen_struct_init(Storage st, int idx, int bslots, StructDef *sd, Expr *lit, int base)
{
    for (int i = 0; i < sd->nfields; i++) {
        Field *f = &sd->fields[i];
        Expr *v = lit_value(lit, f->name);
        if (f->is_heap)
            gen_store_ptr(st, idx, bslots, base + f->offset, v);
        else if (f->ty == TY_STRUCT)
            gen_struct_init(st, idx, bslots, f->sdef, v, base + f->offset);
        else
            gen_store_mem(st, idx, bslots, base + f->offset, v);
    }
}

static void gen_alloc(int size, int slot_idx)
{
    char slot[64];
    emit("mov r10d, %d", size);
    emit("call rgx_rt_alloc");       /* r10 = new zeroed block; other registers kept */
    qword_slot(slot, sizeof slot, slot_idx);
    emit("mov %s, r10", slot);
}

/* r10 = base address of the array being initialized. */
static void load_array_base(Stmt *s)
{
    char slot[64];
    if (s->storage == ST_STACK) {
        emit("lea r10, [rbp-%d]", 8 * (s->reg + s->bslots));
    } else {
        qword_slot(slot, sizeof slot, s->reg);
        emit("mov r10, %s", slot);
    }
}

static void gen_array_init(Stmt *s)
{
    Expr *x = s->expr;
    int sz = esz(s->arr.elem);
    if (s->storage == ST_HEAP)
        gen_alloc(s->size, s->reg);
    if (x->kind == E_ARRAYLIT) {
        for (int i = 0; i < x->nargs; i++)
            gen_store_elem(s->storage, s->reg, s->bslots, i * sz, sz, x->args[i]);
    } else if (x->kind == E_STR) {
        if (x->slen > 0)
            load_array_base(s);
        for (int i = 0; i < x->slen; i++)
            emit("mov BYTE PTR [r10+%d], %d", i, (unsigned char)x->sval[i]);
    } else { /* E_ARRAYREP */
        int l_loop = label_seq++;
        gen_expr(x->l);
        load_array_base(s);
        emit("mov r11d, %ld", x->val);
        label_def(l_loop);
        if (sz == 1)
            emit("mov BYTE PTR [r10+r11-1], al");
        else
            emit("mov DWORD PTR [r10+r11*4-4], eax");
        emit("dec r11d");
        emit_jump("jnz", l_loop);
    }
}

/* Initialize a struct or array from another variable: byte copy, or a move.
 * Uses rcx, rdx, r8, r9 (free at statement level). */
static void gen_copy_decl(Stmt *s)
{
    char slot[64];
    int l_loop = label_seq++;
    if (s->cpy == CP_MOVE && s->storage == ST_HEAP && s->cst == ST_HEAP) {
        qword_slot(slot, sizeof slot, s->cidx);   /* ownership of the block moves */
        emit("mov r10, %s", slot);
        qword_slot(slot, sizeof slot, s->reg);
        emit("mov %s, r10", slot);
        return;
    }
    if (s->storage == ST_HEAP)
        gen_alloc(s->size, s->reg);
    if (s->cst == ST_STACK) {
        emit("lea r8, [rbp-%d]", 8 * (s->cidx + s->cbs));
    } else {
        qword_slot(slot, sizeof slot, s->cidx);
        emit("mov r8, %s", slot);
        emit("test r8, r8");               /* the source may be a null heap struct */
        emit("jz rgx_rt_panic");
    }
    if (s->storage == ST_STACK) {
        emit("lea r9, [rbp-%d]", 8 * (s->reg + s->bslots));
    } else {
        qword_slot(slot, sizeof slot, s->reg);
        emit("mov r9, %s", slot);
    }
    emit("mov ecx, %d", s->size);
    label_def(l_loop);
    emit("mov dl, BYTE PTR [r8+rcx-1]");
    emit("mov BYTE PTR [r9+rcx-1], dl");
    emit("dec ecx");
    emit_jump("jnz", l_loop);
    if (s->cpy == CP_MOVE && s->cst == ST_HEAP) {
        qword_slot(slot, sizeof slot, s->cidx);   /* the old block is no longer needed */
        emit("mov r10, %s", slot);
        emit("call rgx_rt_free");
    }
}

static void gen_decl(Stmt *s)
{
    char slot[64];
    if (s->cpy) {
        gen_copy_decl(s);
        return;
    }
    if (s->ty == TY_REF) {                 /* reference variable: store the borrowed address */
        gen_borrow_addr(s->expr, "r10");
        qword_slot(slot, sizeof slot, s->reg);
        emit("mov %s, r10", slot);
        return;
    }
    if (s->pk == PK_NULL) {
        qword_slot(slot, sizeof slot, s->reg);
        emit("mov %s, 0", slot);
        return;
    }
    if (s->pk == PK_TAKE) {                /* move the owned pointer out of a field */
        Expr *a = s->expr->args[0];
        load_path_base("r9", a);
        emit("mov r10, QWORD PTR [r9+%d]", a->foff);
        emit("mov QWORD PTR [r9+%d], 0", a->foff);
        qword_slot(slot, sizeof slot, s->reg);
        emit("mov %s, r10", slot);
        return;
    }
    if (s->ty == TY_ARRAY) {
        gen_array_init(s);
    } else if (s->storage == ST_HEAP) {
        if (s->adopt) {
            gen_expr(s->expr);       /* callee returns an owned pointer in rax */
            qword_slot(slot, sizeof slot, s->reg);
            emit("mov %s, rax", slot);
        } else if (s->ty == TY_STRUCT) {
            gen_alloc(s->size, s->reg);
            gen_struct_init(ST_HEAP, s->reg, 1, s->sdef, s->expr, 0);
        } else {
            gen_expr(s->expr);       /* value in eax survives the allocation */
            gen_alloc(4, s->reg);
            emit("mov DWORD PTR [r10], eax");
        }
    } else if (s->ty == TY_STRUCT) {
        gen_struct_init(ST_STACK, s->reg, s->bslots, s->sdef, s->expr, 0);
    } else {
        gen_store_var(s->storage, s->reg, s->expr);
    }
}

/* r10 = address of a struct; release every owned chain inside it (not the
 * struct's own storage). `zero` also clears the released blocks. */
static void emit_free_chain(StructDef *sd, int zero)
{
    if (sd && sd->has_heap) {
        emit("mov r11d, %d", zero);
        emit("call %s", sd->fsym);
    }
}

static void gen_release(const Release *rel, int n)
{
    char slot[64];
    for (int i = n - 1; i >= 0; i--) {
        const Release *r = &rel[i];
        if (r->is_heap) {
            int l_skip = label_seq++;
            qword_slot(slot, sizeof slot, r->idx);
            emit("mov r10, %s", slot);
            emit("test r10, r10");     /* a heap struct may be null */
            emit_jump("jz", l_skip);
            emit_free_chain(r->sd, 0);
            emit("call rgx_rt_free");
            label_def(l_skip);
        } else {                        /* a stack struct that owns heap fields */
            emit("lea r10, [rbp-%d]", 8 * (r->idx + r->bslots));
            emit_free_chain(r->sd, 0);
        }
    }
}

/* Zero the whole value of a declared variable; release the heap block too
 * when `release` is set (drop). Leaves r10 holding the heap pointer. */
static void gen_zero(Stmt *s, int release)
{
    char slot[64], mem[64];
    if (s->ty == TY_ARRAY) {
        int l_loop = label_seq++;
        load_array_base(s);
        emit("mov r11d, %d", s->size);
        label_def(l_loop);
        emit("mov BYTE PTR [r10+r11-1], 0");
        emit("dec r11d");
        emit_jump("jnz", l_loop);
        if (release && s->storage == ST_HEAP)
            emit("call rgx_rt_free");
        return;
    }
    switch (s->storage) {
    case ST_REG:
        emit("xor %s, %s", abi->pool32[s->reg], abi->pool32[s->reg]);
        break;
    case ST_STACK:
        if (release && s->sdef && s->sdef->has_heap) {
            emit("lea r10, [rbp-%d]", 8 * (s->reg + s->bslots));
            emit_free_chain(s->sdef, 1);   /* owned chains first, then the bytes */
        }
        for (int off = 0; off < s->size; off += 4) {
            dword_at(mem, sizeof mem, s->reg, s->bslots, off);
            emit("mov %s, 0", mem);
        }
        break;
    case ST_HEAP: {
        int l_skip = label_seq++;
        qword_slot(slot, sizeof slot, s->reg);
        emit("mov r10, %s", slot);
        emit("test r10, r10");             /* a heap struct may be null */
        emit_jump("jz", l_skip);
        if (release)
            emit_free_chain(s->sdef, 1);
        for (int off = 0; off < s->size; off += 4) {
            dword_via(mem, sizeof mem, "r10", off);
            emit("mov %s, 0", mem);
        }
        if (release)
            emit("call rgx_rt_free");
        label_def(l_skip);
        break;
    }
    default:
        break;
    }
}

static void gen_drop(Stmt *s)
{
    gen_zero(s, 1);
}

/* Clear secret locals that are still alive at the end of a scope. */
static void gen_wipe(Stmt **list, int n)
{
    for (int i = n - 1; i >= 0; i--)
        gen_zero(list[i], 0);
}

/* `x.f = value` for an i32 field, following owned pointers and checking null. */
static void gen_store_field(Expr *lhs, Expr *rhs)
{
    char mem[64];
    if (lhs->vst == ST_STACK && lhs->ndref == 0) {
        gen_store_mem(ST_STACK, lhs->reg, lhs->bslots, lhs->foff, rhs);
        return;
    }
    gen_expr(rhs);
    load_path_base("r11", lhs);
    dword_via(mem, sizeof mem, "r11", lhs->foff);
    emit("mov %s, eax", mem);
}

/* `x.next = null | heap_var | call()`: release what the field owned, then store
 * the new owner. The release routines keep every register. */
static void gen_ptr_assign(Stmt *s)
{
    Expr *lhs = s->lhs, *x = s->expr;
    char slot[64];
    int l_skip = label_seq++;
    if (s->pk == PK_NULL) {
        emit("xor eax, eax");
    } else if (s->pk == PK_MOVE) {
        qword_slot(slot, sizeof slot, x->reg);
        emit("mov rax, %s", slot);
    } else {
        gen_expr(x);                       /* call: new owner in rax */
    }
    load_path_base("r9", lhs);
    emit("mov r10, QWORD PTR [r9+%d]", lhs->foff);
    emit("test r10, r10");
    emit_jump("jz", l_skip);
    emit_free_chain(lhs->sdef, 0);
    emit("call rgx_rt_free");
    label_def(l_skip);
    emit("mov QWORD PTR [r9+%d], rax", lhs->foff);
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

static void gen_index_assign(Stmt *s)
{
    Expr *lhs = s->lhs;
    int byte = lhs->ai.elem == TY_U8;
    if (s->expr->kind == E_INT) {
        gen_index_prep(lhs);
        if (byte)
            emit("mov BYTE PTR [r11+rax], %ld", s->expr->val & 255);
        else
            emit("mov DWORD PTR [r11+rax*4], %ld", s->expr->val);
        return;
    }
    gen_expr(s->expr);
    emit("push rax");
    cur_push++;
    gen_index_prep(lhs);
    cur_push--;
    emit("pop r10");
    if (byte)
        emit("mov BYTE PTR [r11+rax], r10b");
    else
        emit("mov DWORD PTR [r11+rax*4], r10d");
}

static void gen_stmt(Stmt *s)
{
    switch (s->kind) {
    case S_DECL:
        gen_decl(s);
        break;
    case S_ASSIGN:
        if (s->pk && s->storage == ST_HEAP) {     /* heap struct variable: new owner */
            char slot[64];
            if (s->pk == PK_NULL) {
                emit("xor eax, eax");
            } else if (s->pk == PK_MOVE) {
                qword_slot(slot, sizeof slot, s->expr->reg);
                emit("mov rax, %s", slot);
            } else {
                gen_expr(s->expr);
            }
            qword_slot(slot, sizeof slot, s->reg);
            if (s->adopt) {                     /* release what it owned */
                int l_skip = label_seq++;
                emit("mov r10, %s", slot);
                emit("test r10, r10");
                emit_jump("jz", l_skip);
                emit_free_chain(s->sdef, 0);
                emit("call rgx_rt_free");
                label_def(l_skip);
            }
            emit("mov %s, rax", slot);
            break;
        }
        if (s->expr->kind == E_ADDR) {       /* re-point a reference variable */
            char slot[64];
            gen_borrow_addr(s->expr, "r10");
            qword_slot(slot, sizeof slot, s->reg);
            emit("mov %s, r10", slot);
        } else {
            gen_store_var(s->storage, s->reg, s->expr);
        }
        break;
    case S_FASSIGN:
        if (s->pk)
            gen_ptr_assign(s);
        else
            gen_store_field(s->lhs, s->expr);
        break;
    case S_IASSIGN:
        gen_index_assign(s);
        break;
    case S_DASSIGN:
        gen_store_elem(ST_HEAP, s->lhs->reg, 1, 0, 4, s->expr);
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
        if (s->expr && s->expr->kind == E_CALL && !s->expr->notail) {
            /* guaranteed tail call: arguments first, then release, then jump */
            setup_args(s->expr);
            gen_wipe(s->wipe, s->nwipe);
            gen_release(s->rel, s->nrel);
            if (s->expr->callee == fn) {
                emit_jump("jmp", lbl_body);
            } else {
                gen_epilogue();
                emit("jmp %s", s->expr->callee->sym);
            }
        } else {
            if (s->expr && fn->ret_storage == ST_HEAP && s->expr->kind == E_VAR) {
                char slot[64];       /* move the owned pointer out */
                qword_slot(slot, sizeof slot, s->expr->reg);
                emit("mov rax, %s", slot);
            } else if (s->expr && s->expr->kind == E_NULL) {
                emit("xor eax, eax");
            } else if (s->expr) {
                gen_expr(s->expr);
            }
            gen_wipe(s->wipe, s->nwipe);
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
    gen_wipe(b->wipe, b->nwipe);
    gen_release(b->rel, b->nrel);
}

/* ---- functions ---- */

static void gen_func(Func *f)
{
    char slot[64];
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
    for (int i = 0; i < f->nparams; i++) {
        Param *p = &f->params[i];
        if (p->ty == TY_SLICE) {
            qword_slot(slot, sizeof slot, p->idx);
            emit("mov %s, %s", slot, abi->arg64[p->areg]);
            len_slot(slot, sizeof slot, "QWORD", p->idx);
            emit("mov %s, %s", slot, abi->arg64[p->areg + 1]);
        } else if (p->ty == TY_REF) {
            qword_slot(slot, sizeof slot, p->idx);
            emit("mov %s, %s", slot, abi->arg64[p->areg]);
        } else if (p->storage == ST_REG) {
            emit("mov %s, %s", abi->pool32[p->idx], abi->arg32[p->areg]);
        } else if (p->storage == ST_STACK) {
            dword_at(slot, sizeof slot, p->idx, 1, 0);
            emit("mov %s, %s", slot, abi->arg32[p->areg]);
        } else {
            qword_slot(slot, sizeof slot, p->idx);
            emit("mov %s, %s", slot, abi->arg64[p->areg]);
        }
    }

    gen_block(f->body);

    label_def(lbl_ret);
    gen_epilogue();
    emit("ret");
    if (abi->windows)
        fprintf(out, "%s ENDP\n", f->sym);
    fputc('\n', out);
}

/* ---- runtime ---- */

/* rgx_rt_alloc: r10d = size; returns a zeroed block in r10, preserves all
 *               other registers. (Linux: size-class allocator.)
 * rgx_rt_free:  releases the block in r10, preserves all registers.
 * Call sites are 16-byte aligned. */
static void gen_heap_runtime(void)
{
    if (!abi->windows) {
        /* Size-class allocator: each block has a 16-byte header holding
         * log2(block size). Blocks are powers of two from 32 to 8192 bytes
         * (9 classes), carved from 1 MiB mmap chunks; free blocks go on a
         * per-class list and are reused. Fresh memory is zero; reused blocks
         * are not cleared (Regix initializes every byte it hands out). */
        int l_big = label_seq++, l_carve = label_seq++, l_retry = label_seq++;
        int l_grow = label_seq++, l_done = label_seq++;
        fputs("rgx_rt_alloc:\n", out);
        emit("push rax");
        emit("push rcx");
        emit("push rdx");
        emit("push rsi");
        emit("push rdi");
        emit("push r8");
        emit("push r9");
        emit("push r11");
        emit("mov eax, r10d");
        emit("add eax, 16");
        emit("cmp eax, 32");
        emit_jump("jae", l_big);
        emit("mov eax, 32");
        label_def(l_big);
        emit("dec eax");
        emit("bsr ecx, eax");
        emit("inc ecx");               /* ecx = log2(block size) */
        emit("mov edx, ecx");
        emit("sub edx, 5");            /* class index 0..8 */
        emit("lea r8, [rip+rgx_heap_free]");
        emit("mov r9, QWORD PTR [r8+rdx*8]");
        emit("test r9, r9");
        emit_jump("jz", l_carve);
        emit("mov rax, QWORD PTR [r9]");   /* pop a free block */
        emit("mov QWORD PTR [r8+rdx*8], rax");
        emit("mov QWORD PTR [r9], rcx");
        emit("lea r10, [r9+16]");
        emit_jump("jmp", l_done);
        label_def(l_carve);
        emit("mov r11d, 1");
        emit("shl r11, cl");               /* r11 = block size */
        label_def(l_retry);
        emit("lea r8, [rip+rgx_heap_cur]");
        emit("mov r9, QWORD PTR [r8]");
        emit("lea rax, [r9+r11]");
        emit("cmp rax, QWORD PTR [r8+8]");
        emit_jump("ja", l_grow);
        emit("mov QWORD PTR [r8], rax");
        emit("mov QWORD PTR [r9], rcx");
        emit("lea r10, [r9+16]");
        emit_jump("jmp", l_done);
        label_def(l_grow);
        emit("push rcx");
        emit("push r11");
        emit("mov eax, 9");            /* mmap(0, 1 MiB, RW, PRIVATE|ANON, -1, 0) */
        emit("xor edi, edi");
        emit("mov esi, 1048576");
        emit("mov edx, 3");
        emit("mov r10d, 34");
        emit("mov r8, -1");
        emit("xor r9d, r9d");
        emit("syscall");
        emit("pop r11");
        emit("pop rcx");
        emit("lea r8, [rip+rgx_heap_cur]");
        emit("mov QWORD PTR [r8], rax");
        emit("add rax, 1048576");
        emit("mov QWORD PTR [r8+8], rax");
        emit_jump("jmp", l_retry);
        label_def(l_done);
        emit("pop r11");
        emit("pop r9");
        emit("pop r8");
        emit("pop rdi");
        emit("pop rsi");
        emit("pop rdx");
        emit("pop rcx");
        emit("pop rax");
        emit("ret");
        fputs("\nrgx_rt_free:\n", out);
        emit("push rax");
        emit("push rcx");
        emit("push rdx");
        emit("push r8");
        emit("lea r8, [r10-16]");          /* block header */
        emit("mov rcx, QWORD PTR [r8]");
        emit("sub ecx, 5");
        emit("lea rdx, [rip+rgx_heap_free]");
        emit("mov rax, QWORD PTR [rdx+rcx*8]");
        emit("mov QWORD PTR [r8], rax");   /* link into the class list */
        emit("mov QWORD PTR [rdx+rcx*8], r8");
        emit("pop r8");
        emit("pop rdx");
        emit("pop rcx");
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
        emit("push r10");
        emit("sub rsp, 32");
        emit("call GetProcessHeap");
        emit("mov rcx, rax");
        emit("mov edx, 8");        /* HEAP_ZERO_MEMORY */
        emit("mov r8d, DWORD PTR [rsp+32]");
        emit("call HeapAlloc");
        emit("mov r10, rax");
        emit("add rsp, 32");
        emit("add rsp, 8");        /* discard the saved size */
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

/* ---- release routines for structs that own heap fields ---- */

typedef struct {
    int off;
    StructDef *target;
} PtrField;

static void collect_ptrs(StructDef *sd, int base, PtrField **v, int *n, int *cap)
{
    for (int i = 0; i < sd->nfields; i++) {
        Field *f = &sd->fields[i];
        if (f->is_heap) {
            PtrField p = {base + f->offset, f->sdef};
            VEC_PUSH(*v, *n, *cap, p);
        } else if (f->ty == TY_STRUCT && f->sdef && f->sdef->has_heap) {
            collect_ptrs(f->sdef, base + f->offset, v, n, cap);
        }
    }
}

/* If r9d (the clear flag) is set, zero `size` bytes at [base]. Uses rcx. */
static void gen_zero_if_flag(const char *base, int size)
{
    int l_skip = label_seq++, l_loop = label_seq++;
    emit("test r9d, r9d");
    emit_jump("jz", l_skip);
    emit("mov ecx, %d", size);
    label_def(l_loop);
    emit("mov BYTE PTR [%s+rcx-1], 0", base);
    emit("dec ecx");
    emit_jump("jnz", l_loop);
    label_def(l_skip);
}

/*
 * rgx_free_<Struct>: r10 = address of a struct, r11d = clear flag. Releases
 * every block the struct owns, recursively (the struct's own storage is the
 * caller's business). Keeps all registers. A chain through the last field that
 * points to the same struct type (a list's `next`) is walked in a loop, so a
 * long list does not recurse.
 */
static void gen_free_routine(StructDef *sd)
{
    PtrField *pf = NULL;
    int n = 0, cap = 0;
    collect_ptrs(sd, 0, &pf, &n, &cap);
    if (abi->windows)
        fprintf(out, "%s PROC\n", sd->fsym);
    else
        fprintf(out, "%s:\n", sd->fsym);
    emit("push rax");
    emit("push rcx");
    emit("push rdx");
    emit("push r8");
    emit("push r9");
    emit("push r10");
    emit("push r11");
    emit("mov r8, r10");
    emit("mov r9d, r11d");
    int last_self = n > 0 && pf[n - 1].target == sd;
    int plain = last_self ? n - 1 : n;
    int l_node = 0, l_done = 0, l_nofree = 0;
    if (last_self) {
        l_node = label_seq++;
        l_done = label_seq++;
        l_nofree = label_seq++;
        emit("xor edx, edx");              /* edx: 0 for the first node, 1 for children */
        label_def(l_node);
    }
    for (int i = 0; i < plain; i++) {
        int l_skip = label_seq++;
        emit("mov r10, QWORD PTR [r8+%d]", pf[i].off);
        emit("test r10, r10");
        emit_jump("jz", l_skip);
        if (pf[i].target && pf[i].target->has_heap) {
            emit("mov r11d, r9d");
            emit("call %s", pf[i].target->fsym);
        }
        gen_zero_if_flag("r10", pf[i].target ? pf[i].target->size : 0);
        emit("call rgx_rt_free");
        emit("mov QWORD PTR [r8+%d], 0", pf[i].off);
        label_def(l_skip);
    }
    if (last_self) {
        int off = pf[n - 1].off;
        emit("mov rax, QWORD PTR [r8+%d]", off);
        emit("mov QWORD PTR [r8+%d], 0", off);
        emit("test rdx, rdx");
        emit_jump("jz", l_nofree);
        gen_zero_if_flag("r8", sd->size);
        emit("mov r10, r8");
        emit("call rgx_rt_free");
        label_def(l_nofree);
        emit("test rax, rax");
        emit_jump("jz", l_done);
        emit("mov r8, rax");
        emit("mov edx, 1");
        emit_jump("jmp", l_node);
        label_def(l_done);
    }
    emit("pop r11");
    emit("pop r10");
    emit("pop r9");
    emit("pop r8");
    emit("pop rdx");
    emit("pop rcx");
    emit("pop rax");
    emit("ret");
    if (abi->windows)
        fprintf(out, "%s ENDP\n", sd->fsym);
    fputc('\n', out);
}

/* Jumped to when an index is out of range: exit code 101. */
static void gen_panic_runtime(void)
{
    if (!abi->windows) {
        fputs("rgx_rt_panic:\n", out);
        emit("mov edi, 101");
        emit("mov eax, 60");
        emit("syscall");
        fputc('\n', out);
    } else {
        fputs("rgx_rt_panic PROC\n", out);
        emit("and rsp, -16");
        emit("sub rsp, 32");
        emit("mov ecx, 101");
        emit("call ExitProcess");
        fputs("rgx_rt_panic ENDP\n\n", out);
    }
}

/* std::io::print_i32: prints a signed decimal and a newline. */
static void gen_print_runtime(void)
{
    int l_pos = label_seq++, l_loop = label_seq++, l_nosign = label_seq++;
    const char *arg = abi->arg32[0];
    if (abi->windows)
        fputs("rgx_std_io_print_i32 PROC\n", out);
    else
        fputs("rgx_std_io_print_i32:\n", out);
    emit("push rbp");
    emit("mov rbp, rsp");
    emit("sub rsp, %d", abi->windows ? 96 : 32);
    emit("movsxd rax, %s", arg);
    emit("mov r9, rax");
    emit("test rax, rax");
    emit_jump("jns", l_pos);
    emit("neg rax");
    label_def(l_pos);
    emit("lea r8, [rbp-1]");
    emit("mov BYTE PTR [r8], 10");
    emit("mov r10, 10");
    label_def(l_loop);
    emit("xor edx, edx");
    emit("div r10");
    emit("add dl, 48");
    emit("dec r8");
    emit("mov BYTE PTR [r8], dl");
    emit("test rax, rax");
    emit_jump("jnz", l_loop);
    emit("test r9, r9");
    emit_jump("jns", l_nosign);
    emit("dec r8");
    emit("mov BYTE PTR [r8], 45");
    label_def(l_nosign);
    if (!abi->windows) {
        emit("mov rdx, rbp");
        emit("sub rdx, r8");
        emit("mov rsi, r8");
        emit("mov edi, 1");
        emit("mov eax, 1");
        emit("syscall");
    } else {
        emit("mov QWORD PTR [rbp-40], r8");
        emit("mov rax, rbp");
        emit("sub rax, r8");
        emit("mov QWORD PTR [rbp-48], rax");
        emit("mov ecx, -11");      /* STD_OUTPUT_HANDLE */
        emit("call GetStdHandle");
        emit("mov rcx, rax");
        emit("mov rdx, QWORD PTR [rbp-40]");
        emit("mov r8, QWORD PTR [rbp-48]");
        emit("lea r9, [rbp-52]");
        emit("mov QWORD PTR [rsp+32], 0");
        emit("call WriteFile");
    }
    emit("mov rsp, rbp");
    emit("pop rbp");
    emit("ret");
    if (abi->windows)
        fputs("rgx_std_io_print_i32 ENDP\n", out);
    fputc('\n', out);
}

/* std::io::print / println: write a [u8] slice (pointer, length) to stdout. */
static void gen_write_runtime(int newline)
{
    const char *name = newline ? "rgx_std_io_println" : "rgx_std_io_print";
    if (abi->windows)
        fprintf(out, "%s PROC\n", name);
    else
        fprintf(out, "%s:\n", name);
    emit("push rbp");
    emit("mov rbp, rsp");
    if (!abi->windows) {
        emit("sub rsp, 16");
        emit("mov rdx, rsi");
        emit("mov rsi, rdi");
        emit("mov edi, 1");
        emit("mov eax, 1");
        emit("syscall");
        if (newline) {
            emit("mov BYTE PTR [rbp-1], 10");
            emit("lea rsi, [rbp-1]");
            emit("mov edx, 1");
            emit("mov edi, 1");
            emit("mov eax, 1");
            emit("syscall");
        }
    } else {
        emit("sub rsp, 80");
        emit("mov QWORD PTR [rbp-8], rcx");
        emit("mov QWORD PTR [rbp-16], rdx");
        emit("mov ecx, -11");      /* STD_OUTPUT_HANDLE */
        emit("call GetStdHandle");
        emit("mov QWORD PTR [rbp-40], rax");
        emit("mov rcx, rax");
        emit("mov rdx, QWORD PTR [rbp-8]");
        emit("mov r8, QWORD PTR [rbp-16]");
        emit("lea r9, [rbp-24]");
        emit("mov QWORD PTR [rsp+32], 0");
        emit("call WriteFile");
        if (newline) {
            emit("mov BYTE PTR [rbp-32], 10");
            emit("mov rcx, QWORD PTR [rbp-40]");
            emit("lea rdx, [rbp-32]");
            emit("mov r8d, 1");
            emit("lea r9, [rbp-24]");
            emit("mov QWORD PTR [rsp+32], 0");
            emit("call WriteFile");
        }
    }
    emit("mov rsp, rbp");
    emit("pop rbp");
    emit("ret");
    if (abi->windows)
        fprintf(out, "%s ENDP\n", name);
    fputc('\n', out);
}

/* ---- process entry ---- */

/* Linux: build the &[&[u8]] of (pointer, length) pairs from argc/argv. */
static void gen_args_linux(void)
{
    int l_loop = label_seq++, l_done = label_seq++, l_len = label_seq++, l_lend = label_seq++;
    emit("mov rdi, QWORD PTR [rsp]");
    emit("lea rsi, [rsp+8]");
    emit("mov rax, rdi");
    emit("shl rax, 4");
    emit("sub rsp, rax");
    emit("and rsp, -16");
    emit("mov rcx, rsp");
    emit("xor edx, edx");
    label_def(l_loop);
    emit("cmp rdx, rdi");
    emit_jump("jae", l_done);
    emit("mov r8, QWORD PTR [rsi+rdx*8]");
    emit("xor r9d, r9d");
    label_def(l_len);
    emit("cmp BYTE PTR [r8+r9], 0");
    emit_jump("je", l_lend);
    emit("inc r9");
    emit_jump("jmp", l_len);
    label_def(l_lend);
    emit("mov rax, rdx");
    emit("shl rax, 4");
    emit("mov QWORD PTR [rcx+rax], r8");
    emit("mov QWORD PTR [rcx+rax+8], r9");
    emit("inc rdx");
    emit_jump("jmp", l_loop);
    label_def(l_done);
    emit("mov rsi, rdi");
    emit("mov rdi, rcx");
}

/* Windows: split GetCommandLineA() in place (spaces and tabs separate
 * arguments, double quotes group; no backslash rules). At most 128 arguments. */
static void gen_args_windows(void)
{
    int l_next = label_seq++, l_skip = label_seq++, l_adv = label_seq++, l_sdone = label_seq++;
    int l_tok = label_seq++, l_inq = label_seq++, l_copy = label_seq++, l_sp = label_seq++;
    int l_end = label_seq++, l_done = label_seq++;
    emit("call GetCommandLineA");
    emit("mov rsi, rax");
    emit("lea rdi, [rbp-2048]");
    emit("xor r12d, r12d");
    label_def(l_next);
    label_def(l_skip);
    emit("mov al, BYTE PTR [rsi]");
    emit("cmp al, 32");
    emit_jump("je", l_adv);
    emit("cmp al, 9");
    emit_jump("jne", l_sdone);
    label_def(l_adv);
    emit("inc rsi");
    emit_jump("jmp", l_skip);
    label_def(l_sdone);
    emit("cmp BYTE PTR [rsi], 0");
    emit_jump("je", l_done);
    emit("cmp r12, 128");
    emit_jump("jae", l_done);
    emit("mov r8, rsi");
    emit("mov r14, rsi");
    emit("xor r9d, r9d");
    label_def(l_tok);
    emit("mov al, BYTE PTR [rsi]");
    emit("test al, al");
    emit_jump("je", l_end);
    emit("test r9d, r9d");
    emit_jump("jne", l_inq);
    emit("cmp al, 32");
    emit_jump("je", l_sp);
    emit("cmp al, 9");
    emit_jump("je", l_sp);
    emit("cmp al, 34");
    emit_jump("jne", l_copy);
    emit("mov r9d, 1");
    emit("inc rsi");
    emit_jump("jmp", l_tok);
    label_def(l_inq);
    emit("cmp al, 34");
    emit_jump("jne", l_copy);
    emit("xor r9d, r9d");
    emit("inc rsi");
    emit_jump("jmp", l_tok);
    label_def(l_copy);
    emit("mov BYTE PTR [r8], al");
    emit("inc r8");
    emit("inc rsi");
    emit_jump("jmp", l_tok);
    label_def(l_sp);
    emit("inc rsi");
    label_def(l_end);
    emit("mov BYTE PTR [r8], 0");
    emit("mov rax, r8");
    emit("sub rax, r14");
    emit("mov rdx, r12");
    emit("shl rdx, 4");
    emit("mov QWORD PTR [rdi+rdx], r14");
    emit("mov QWORD PTR [rdi+rdx+8], rax");
    emit("inc r12");
    emit_jump("jmp", l_next);
    label_def(l_done);
    emit("mov rcx, rdi");
    emit("mov rdx, r12");
}

static void gen_entry(Func *m)
{
    int is_void = m->ret_ty == TY_VOID;
    int has_args = m->nparams == 1;
    if (!abi->windows) {
        fputs("_start:\n", out);
        if (has_args)
            gen_args_linux();
        emit("call %s", m->sym);
        if (is_void)
            emit("xor edi, edi");
        else
            emit("mov edi, eax");
        emit("mov eax, 60");
        emit("syscall");
    } else {
        fputs("mainCRTStartup PROC\n", out);
        emit("push rbp");
        emit("mov rbp, rsp");
        emit("sub rsp, 2080");     /* 128 argument slots + shadow space */
        if (has_args)
            gen_args_windows();
        emit("call %s", m->sym);
        if (is_void)
            emit("xor ecx, ecx");
        else
            emit("mov ecx, eax");
        emit("call ExitProcess");
        fputs("mainCRTStartup ENDP\n", out);
    }
}

/* ---- data ---- */

/* Linux allocator state: nine free lists, then the bump pointer and its end. */
static void gen_heap_data(void)
{
    if (abi->windows || !prog_g->uses_heap)
        return;
    fputs(".section .bss\n.balign 8\nrgx_heap_free:\n    .zero 72\nrgx_heap_cur:\n    .zero 8\nrgx_heap_end:\n    .zero 8\n", out);
}

static void gen_strings(void)
{
    if (prog_g->nstrs == 0)
        return;
    fputs(abi->windows ? ".const\n" : ".section .rodata\n", out);
    for (int i = 0; i < prog_g->nstrs; i++) {
        StrLit *s = &prog_g->strs[i];
        if (abi->windows)
            fprintf(out, "rgx_str%d BYTE ", i);
        else
            fprintf(out, ".Lstr%d:\n    .byte ", i);
        if (s->len == 0)
            fputs("0", out);
        for (int j = 0; j < s->len; j++) {
            if (j && j % 16 == 0)
                fputs(abi->windows ? "\n    BYTE " : "\n    .byte ", out);
            else if (j)
                fputc(',', out);
            fprintf(out, "%d", (unsigned char)s->data[j]);
        }
        fputc('\n', out);
    }
}

void codegen_program(Program *prog, const Abi *a, FILE *o)
{
    out = o;
    abi = a;
    prog_g = prog;
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
        if (prog->uses_print || prog->uses_write || prog->uses_writeln) {
            fputs("EXTRN GetStdHandle:PROC\n", out);
            fputs("EXTRN WriteFile:PROC\n", out);
        }
        if (main_fn && main_fn->nparams == 1)
            fputs("EXTRN GetCommandLineA:PROC\n", out);
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
        gen_heap_runtime();
    for (int i = 0; i < prog->nstructs; i++)
        if (prog->structs[i]->has_heap)
            gen_free_routine(prog->structs[i]);
    if (prog->uses_panic)
        gen_panic_runtime();
    if (prog->uses_print)
        gen_print_runtime();
    if (prog->uses_write)
        gen_write_runtime(0);
    if (prog->uses_writeln)
        gen_write_runtime(1);
    if (main_fn)
        gen_entry(main_fn);

    gen_strings();
    gen_heap_data();

    if (abi->windows)
        fputs("END\n", out);
    else
        fputs(".section .note.GNU-stack,\"\",@progbits\n", out);
}
