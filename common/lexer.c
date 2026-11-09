#include "lexer.h"

#include <ctype.h>
#include <string.h>

#include "util.h"

static const struct {
    const char *text;
    TokKind kind;
} keywords[] = {
    {"fn", T_KW_FN},         {"reg", T_KW_REG},         {"stack", T_KW_STACK},
    {"heap", T_KW_HEAP},     {"if", T_KW_IF},           {"else", T_KW_ELSE},
    {"while", T_KW_WHILE},   {"return", T_KW_RETURN},   {"package", T_KW_PACKAGE},
    {"import", T_KW_IMPORT}, {"struct", T_KW_STRUCT},   {"pub", T_KW_PUB},
    {"secret", T_KW_SECRET}, {"null", T_KW_NULL},
};

const char *tok_name(TokKind k)
{
    switch (k) {
    case T_EOF: return "end of file";
    case T_IDENT: return "identifier";
    case T_INT: return "integer";
    case T_KW_FN: return "'fn'";
    case T_KW_REG: return "'reg'";
    case T_KW_STACK: return "'stack'";
    case T_KW_HEAP: return "'heap'";
    case T_KW_IF: return "'if'";
    case T_KW_ELSE: return "'else'";
    case T_KW_WHILE: return "'while'";
    case T_KW_RETURN: return "'return'";
    case T_KW_PACKAGE: return "'package'";
    case T_KW_IMPORT: return "'import'";
    case T_KW_STRUCT: return "'struct'";
    case T_KW_PUB: return "'pub'";
    case T_KW_SECRET: return "'secret'";
    case T_KW_NULL: return "'null'";
    case T_LPAREN: return "'('";
    case T_RPAREN: return "')'";
    case T_LBRACE: return "'{'";
    case T_RBRACE: return "'}'";
    case T_COMMA: return "','";
    case T_COLON: return "':'";
    case T_COLONCOLON: return "'::'";
    case T_SEMI: return "';'";
    case T_ARROW: return "'->'";
    case T_ASSIGN: return "'='";
    case T_DOT: return "'.'";
    case T_LBRACKET: return "'['";
    case T_RBRACKET: return "']'";
    case T_AMP: return "'&'";
    case T_STRING: return "string";
    case T_PLUS: return "'+'";
    case T_MINUS: return "'-'";
    case T_STAR: return "'*'";
    case T_LT: return "'<'";
    case T_GT: return "'>'";
    case T_LE: return "'<='";
    case T_GE: return "'>='";
    case T_EQ: return "'=='";
    case T_NE: return "'!='";
    }
    return "?";
}

static int is_ident_start(char c) { return isalpha((unsigned char)c) || c == '_'; }
static int is_ident_char(char c) { return isalnum((unsigned char)c) || c == '_'; }

Token *lex(const char *file, const char *src, size_t *count)
{
    Token *toks = NULL;
    size_t n = 0, cap = 0;
    int line = 1, col = 1;
    const char *p = src;

    for (;;) {
        char c = *p;
        if (c == '\0')
            break;
        if (c == '\n') {
            line++;
            col = 1;
            p++;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r') {
            p++;
            col++;
            continue;
        }
        if (c == '/' && p[1] == '/') {
            while (*p && *p != '\n')
                p++;
            continue;
        }
        if (c == '/' && p[1] == '*') {
            int sl = line, sc = col;
            p += 2;
            col += 2;
            while (*p && !(*p == '*' && p[1] == '/')) {
                if (*p == '\n') {
                    line++;
                    col = 1;
                } else {
                    col++;
                }
                p++;
            }
            if (!*p)
                rgx_die(file, sl, sc, "unterminated block comment");
            p += 2;
            col += 2;
            continue;
        }

        Token tk;
        memset(&tk, 0, sizeof tk);
        tk.line = line;
        tk.col = col;
        tk.text = p;

        if (is_ident_start(c)) {
            const char *s = p;
            while (is_ident_char(*p))
                p++;
            tk.len = (int)(p - s);
            tk.kind = T_IDENT;
            for (size_t i = 0; i < sizeof keywords / sizeof keywords[0]; i++) {
                if ((int)strlen(keywords[i].text) == tk.len &&
                    memcmp(keywords[i].text, s, (size_t)tk.len) == 0) {
                    tk.kind = keywords[i].kind;
                    break;
                }
            }
            col += tk.len;
        } else if (isdigit((unsigned char)c)) {
            const char *s = p;
            long long v = 0;
            while (isdigit((unsigned char)*p)) {
                v = v * 10 + (*p - '0');
                if (v > 2147483647LL)
                    rgx_die(file, line, col, "integer literal out of range for i32");
                p++;
            }
            if (is_ident_char(*p))
                rgx_die(file, line, col, "invalid character in number");
            tk.kind = T_INT;
            tk.len = (int)(p - s);
            tk.ival = (long)v;
            col += tk.len;
        } else if (c == '"') {
            const char *s = p + 1;
            char *buf = NULL;
            size_t n2 = 0, cap2 = 0;
            int cl = col + 1;
            for (;;) {
                if (*s == '\0' || *s == '\n')
                    rgx_die(file, line, col, "unterminated string literal");
                if (*s == '"')
                    break;
                char ch = *s;
                if (ch == '\\') {
                    s++;
                    cl++;
                    switch (*s) {
                    case 'n': ch = '\n'; break;
                    case 't': ch = '\t'; break;
                    case 'r': ch = '\r'; break;
                    case '0': ch = '\0'; break;
                    case '\\': ch = '\\'; break;
                    case '"': ch = '"'; break;
                    default:
                        rgx_die(file, line, cl, "unknown escape sequence in string");
                    }
                }
                VEC_PUSH(buf, n2, cap2, ch);
                s++;
                cl++;
            }
            tk.kind = T_STRING;
            tk.sval = buf ? buf : xstrdup("");
            tk.slen = (int)n2;
            tk.len = (int)(s + 1 - p);
            p = s + 1;
            col = cl + 1;
        } else {
            char d = p[1];
            tk.len = 1;
            switch (c) {
            case '(': tk.kind = T_LPAREN; break;
            case ')': tk.kind = T_RPAREN; break;
            case '{': tk.kind = T_LBRACE; break;
            case '}': tk.kind = T_RBRACE; break;
            case ',': tk.kind = T_COMMA; break;
            case ';': tk.kind = T_SEMI; break;
            case '.': tk.kind = T_DOT; break;
            case '[': tk.kind = T_LBRACKET; break;
            case ']': tk.kind = T_RBRACKET; break;
            case '&': tk.kind = T_AMP; break;
            case '+': tk.kind = T_PLUS; break;
            case '*': tk.kind = T_STAR; break;
            case ':':
                if (d == ':') { tk.kind = T_COLONCOLON; tk.len = 2; }
                else tk.kind = T_COLON;
                break;
            case '-':
                if (d == '>') { tk.kind = T_ARROW; tk.len = 2; }
                else tk.kind = T_MINUS;
                break;
            case '=':
                if (d == '=') { tk.kind = T_EQ; tk.len = 2; }
                else tk.kind = T_ASSIGN;
                break;
            case '<':
                if (d == '=') { tk.kind = T_LE; tk.len = 2; }
                else tk.kind = T_LT;
                break;
            case '>':
                if (d == '=') { tk.kind = T_GE; tk.len = 2; }
                else tk.kind = T_GT;
                break;
            case '!':
                if (d == '=') { tk.kind = T_NE; tk.len = 2; break; }
                /* fall through */
            default:
                rgx_die(file, line, col, "unexpected character '%c'", c);
            }
            p += tk.len;
            col += tk.len;
        }
        VEC_PUSH(toks, n, cap, tk);
    }

    Token eof;
    memset(&eof, 0, sizeof eof);
    eof.kind = T_EOF;
    eof.line = line;
    eof.col = col;
    eof.text = p;
    VEC_PUSH(toks, n, cap, eof);
    if (count)
        *count = n;
    return toks;
}
