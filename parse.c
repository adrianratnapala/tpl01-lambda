#define _GNU_SOURCE
#include <assert.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "lambda.h"
#include "untestable.h"

typedef struct SyntaxError SyntaxError;
struct SyntaxError {
        SyntaxError *prev;
        char *zmsg;
};

struct Ast {
        const char *zname;
        const char *zsrc;
        SyntaxError *error;
        uint32_t zsrc_len;
        uint32_t nnodes_alloced;
        uint32_t nnodes;
        uint32_t current_depth;
        uint32_t binding_depths[26];
        AstNode nodes[];
};

// ------------------------------------------------------------------

const AstNode *ast_postfix(const Ast *ast, uint32_t *size_ret)
{
        uint32_t nnodes = ast->nnodes;
        DIE_IF(!nnodes, "An empty AST is postfix.");
        *size_ret = nnodes;
        return ast->nodes;
}

static const AstNode *ast_root(const Ast *ast)
{
        uint32_t nnodes = ast->nnodes;
        DIE_IF(!nnodes, "Empty AST has no root");
        return ast->nodes + nnodes - 1;
}

static AstNode *ast_node_alloc(Ast *ast, size_t n)
{
        size_t u = ast->nnodes;
        size_t nu = u + n;
        DIE_IF(nu > ast->nnodes_alloced,
               "BUG: %s is using %lu Ast nodes, only %d are alloced",
               ast->zname, nu, ast->nnodes_alloced);

        ast->nnodes = nu;
        return ast->nodes + u;
}

static SyntaxError *add_syntax_error(Ast *ast, const char *zloc,
                                     const char *zfmt, ...)
{
        size_t n = zloc - ast->zsrc;
        DIE_IF(n > ast->zsrc_len, "Creating error at invalid source loc %ld",
               n);
        SyntaxError *e = realloc_or_die(HERE, 0, sizeof(SyntaxError));
        *e = (SyntaxError){.prev = ast->error};

        char *prefix = NULL, *suffix = NULL;

        int nprefix =
            asprintf(&prefix, "%s:%lu: Syntax error: ", ast->zname, n);
        DIE_IF(nprefix < 0 || !prefix, "Couldn't format syntax_error location");

        va_list va;
        va_start(va, zfmt);
        int nsuffix = vasprintf(&suffix, zfmt, va);
        va_end(va);
        DIE_IF(nsuffix < 0 || !suffix, "Couldn't format %s%s...", prefix, zfmt);

        size_t len = nprefix + nsuffix + 1;
        prefix = realloc_or_die(HERE, prefix, len + 1);
        memcpy(prefix + nprefix, suffix, nsuffix);
        free(suffix);
        prefix[len - 1] = '.';
        prefix[len] = 0;

        e->zmsg = prefix;
        return ast->error = e;
}

static int print_syntax_errors(FILE *oot, const SyntaxError *e)
{
        if (!e)
                return 0;
        int n = print_syntax_errors(oot, e->prev);
        fputs(e->zmsg, oot);
        fputc('\n', oot);
        return n + 1;
}

int report_syntax_errors(FILE *oot, Ast *ast)
{
        return print_syntax_errors(oot, ast->error);
}

void delete_ast(Ast *ast)
{
        SyntaxError *e, *pe = ast->error;
        while ((e = pe)) {
                pe = e->prev;
                free(e->zmsg);
                free(e);
        }
        free(ast);
}

// ------------------------------------------------------------------

static const char *eat_white(const char *z0)
{
        for (;; z0++) {
                char ch = *z0;
                switch (ch) {
                case ' ':
                case '\t':
                case '\n':
                        continue;
                default:
                        return z0;
                }
        }
}

static uint8_t idx_from_letter(char c) { return (uint8_t)c - (uint8_t)'a'; }

static const char *lex_varname(Ast *ast, int32_t *idxptr, const char *z0)
{
        uint8_t idx = idx_from_letter(*z0);
        if (idx >= 26) {
                *idxptr = -1;
                return z0;
        }
        *idxptr = idx;

        const char *z = z0 + 1;
        if (idx_from_letter(*z) >= 26) {
                return z;
        }

        while (idx_from_letter(*z) < 26)
                z++;
        add_syntax_error(ast, z0, "Multi-byte varnames aren't allowed.  '%.*s'",
                         z - z0, z0);
        return z;
}

static uint8_t idx_from_digit(char c) { return (uint8_t)c - (uint8_t)'0'; }

static const char *lex_int(Ast *ast, int32_t *idxptr, const char *z0)
{
        uint8_t idx = idx_from_digit(*z0);
        if (idx >= 10) {
                *idxptr = -1;
                return z0;
        }
        *idxptr = idx;

        const char *z = z0 + 1;
        if (idx_from_digit(*z) >= 10) {
                return z;
        }

        while (idx_from_digit(*z) < 10)
                z++;
        add_syntax_error(ast, z0, "Multi-digit nums aren't allowed.  '%.*s'",
                         z - z0, z0);
        return z;
}

static void push_varname(Ast *ast, int32_t token)
{
        DIE_IF(token + 'a' > 'z', "Bad token %u.", token);

        AstNode *pn = ast_node_alloc(ast, 1);
        DBG("pushed expr %lu: VAR token=%d", pn - ast->nodes, token);
        *pn = (AstNode){
            .type = ANT_VAR,
            .VAR = {.token = token},
        };
}

static void push_bound(Ast *ast, int32_t depth)
{
        DIE_IF(depth < 0, "Bad depth %u.", depth);

        AstNode *pn = ast_node_alloc(ast, 1);
        DBG("pushed expr %lu: BOUND depth=%d", pn - ast->nodes, depth);
        *pn = (AstNode){
            .type = ANT_BOUND,
            .BOUND = {.depth = depth},
        };
}

static void push_var(Ast *ast, int32_t token)
{
        DIE_IF(token + 'a' > 'z', "Bad token %u.", token);
        uint32_t bdepth = ast->binding_depths[token];
        return bdepth ? push_bound(ast, ast->current_depth - bdepth)
                      : push_varname(ast, token);
}

static const char *parse_expr(Ast *ast, const char *z0);
static const char *parse_non_call_expr(Ast *ast, const char *z0);

static const char *parse_lambda(Ast *ast, const char *z0)
{
        DIE_IF(*z0 != '[', "bad call to %s.", z0);
        int32_t token;
        const char *zE = eat_white(z0 + 1);
        zE = lex_varname(ast, &token, zE);
        zE = eat_white(zE);
        if (*zE == ']') {
                zE++;
        } else {
                size_t n = zE - z0;
                if (*zE)
                        n++;
                // FIX: test this error
                add_syntax_error(ast, z0, "Lambda '%.*s' doesn't end in ']'", n,
                                 z0);
        }

        uint32_t inner_depth = ast->current_depth + 1;
        uint32_t sink = 0, *binding = &sink;
        if (token >= 0)
                binding = ast->binding_depths + token;
        uint32_t prev_bound = *binding;

        ast->current_depth = inner_depth;
        *binding = inner_depth;

        DBG("Bound token %d to depth=%u", token, inner_depth);
        const char *zbody = zE;
        zE = parse_non_call_expr(ast, zE);
        if (!zE) {
                add_syntax_error(ast, zbody, "Expected lambda body");
                return NULL;
        }

        // FIX: ast_root is a bad name
        const AstNode *body = ast_root(ast);
        *binding = prev_bound;
        ast->current_depth = inner_depth - 1;

        push_varname(ast, token);
        AstNode *pn = ast_node_alloc(ast, 1);
        *pn = (AstNode){
            .type = ANT_LAMBDA,
        };
        DBG("pushed expr %lu: LAMBDA inner depth=%u", pn - ast->nodes,
            inner_depth);
        assert(pn - body == 2);
        return zE;
}

static const char *parse_non_call_expr(Ast *ast, const char *z0)
{
        int32_t token;
        const char *zE = lex_varname(ast, &token, z0);
        if (token >= 0) {
                push_var(ast, token);
                return zE;
        }
        zE = lex_int(ast, &token, z0);
        if (token >= 0) {
                if (token == 0) {
                        add_syntax_error(ast, z0,
                                         "0 is an invalid debrujin index");
                        token++;
                }
                push_bound(ast, token - 1);
                return zE;
        }

        switch (*z0) {
        case '(':
                zE = parse_expr(ast, z0 + 1);
                if (!zE || *zE != ')') {
                        add_syntax_error(ast, z0, "Unmatched '('");
                        return zE;
                }
                return zE + 1;
        case '[':
                return parse_lambda(ast, z0);
        }

        return NULL;
}

static const char *parse_expr(Ast *ast, const char *z0)
{
        const char *z, *z1 = eat_white(z0);
        while (!(z = parse_non_call_expr(ast, z1))) {
                if (!ast->error)
                        add_syntax_error(ast, z0, "Expected expr");
                if (!*z1)
                        return NULL;
                z1 = eat_white(z1 + 1);
        }

        for (;;) {
                const AstNode *func = ast_root(ast);
                z = eat_white(z);
                const char *z1 = parse_non_call_expr(ast, z);
                size_t arg_size = ast_root(ast) - func;
                if (!z1) {
                        return z;
                }
                DIE_IF(arg_size > INT32_MAX,
                       "Huge arg parsed %lu nodes, why no ENOMEM?", arg_size);
                z = z1;
                AstNode *call = ast_node_alloc(ast, 1);
                *call =
                    (AstNode){.type = ANT_CALL, .CALL = {.arg_size = arg_size}};
                DBG("pushed expr %lu: CALL arg_size=%lu", call - ast->nodes,
                    arg_size);
        }
}

Ast *parse(const char *zname, const char *zsrc)
{
        size_t n = strlen(zsrc) + 8;

        Ast *ast = realloc_or_die(HERE, 0, sizeof(Ast) + sizeof(AstNode) * n);
        *ast = (Ast){
            .zname = zname,
            .zsrc = zsrc,
            .zsrc_len = (int32_t)n,
            .nnodes_alloced = n,
        };
        for (int k = 0; k < n; k++) {
                ast->nodes[k] = (AstNode){0};
        }

        const char *zE = parse_expr(ast, zsrc);
        DIE_IF(zE && *zE, "Unused bytes after program source: '%.*s...'", 10,
               zE);

        return ast;
}
