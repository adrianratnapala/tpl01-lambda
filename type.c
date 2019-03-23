#define _GNU_SOURCE
#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "lambda.h"
#include "untestable.h"

#define MAX_TOKS 26
#define MAX_DEPTH 16

typedef struct Type Type;
struct Type {
        // FIX: switch to index notation.
        // FIX: say first, or prior, not master.
        Type *master_t;
        Type *arg_t, *ret_t;
};

typedef struct {
        const AstNode *postfix;
        uint32_t size;
        uint32_t pad;
        Type *bindings[MAX_TOKS];
        Type types[];
} TypeTree;

// FIX: get rid of this.
static Type *pmasterise(Type *t)
{
        Type *m = t->master_t;
        if (m == t) {
                return t;
        }
        return t->master_t = pmasterise(m);
}

// FIX: be consistent about int vs uint
static uint32_t masterise(Type *types, uint32_t idx)
{
        Type *t = pmasterise(types + idx);
        assert(t >= types);
        return t - types;
}

static void print_typename(FILE *oot, const AstNode *exprs, int32_t idx)
{
        int k = 0;
        uint32_t val = idx;
        while (ANT_CALL == ast_unpack(exprs, val, &val)) {
                k++;
        }

        fputc(val + 'A', oot);
        while (k--) {
                fputc('r', oot);
        }
}

static void unify(TypeTree *ttree, uint32_t ia, uint32_t ib)
{
        Type *types = ttree->types;

        // FIX: masterise first!
        if (ia == ib)
                return;

        // FIX: make sure ia, ib are already masterised?
        ia = masterise(types, ia);
        ib = masterise(types, ib);
        Type a = types[ia], b = types[ib];

        if (!a.arg_t && b.arg_t) {
                types[ia] = b;
                types[ia].master_t = ia + types;

                types[ib] = (Type){.master_t = ia + types};
                masterise(types, ib); // FIX: redundant.
                return;
        }

        types[ib].master_t = types + ia;
        masterise(types, ib); // FIX: redundant, also repeated.

        if (!b.arg_t) {
                return;
        }

        // Both sides turned out to be function types.  Unify the components.
        unify(ttree, a.arg_t - types, b.arg_t - types);
        unify(ttree, a.ret_t - types, b.ret_t - types);
}

static void coerce_to_fun_type(TypeTree *ttree, uint32_t ifun, uint32_t icall)
{
        Type *types = ttree->types;
        assert(ifun < icall);

        uint32_t iarg = ast_arg_idx(ttree->postfix, icall);
        uint32_t iret = icall;
        // fputs("DBG: ", stderr);
        // print_typename(stderr, ttree->postfix, ifun);
        // fprintf(stderr, " <= new fun %d at from %d, %d\n", ifun, iarg, iret);

        ifun = masterise(types, ifun);
        Type fun = types[ifun];
        if (fun.arg_t) {
                // The target already as a fun-type, so leave it be.
                // But unify its children.
                unify(ttree, fun.arg_t - types, iarg);
                unify(ttree, fun.ret_t - types, icall);
                return;
        }

        types[ifun].arg_t = types + masterise(types, iarg);
        types[ifun].ret_t = types + masterise(types, iret);
}

static void solve_types(TypeTree *ttree)
{
        const AstNode *exprs = ttree->postfix;
        Type *types = ttree->types;
        uint32_t size = ttree->size;

        uint32_t val;
        for (int k = 0; k < size; k++)
                switch (ast_unpack(exprs, k, &val)) {
                case ANT_VAR:
                        DIE_IF(val > MAX_TOKS, "Overbig token %u", val);
                        if (ttree->bindings[val]) {
                                types[k] = *ttree->bindings[val];
                        } else {
                                ttree->bindings[val] = types + k;
                        }
                        continue;
                case ANT_CALL:
                        coerce_to_fun_type(ttree, val, k);
                        continue;
                }
}

static TypeTree *build_type_tree(const Ast *ast)
{
        uint32_t size;
        const AstNode *postfix = ast_postfix(ast, &size);
        TypeTree *tree =
            realloc_or_die(HERE, 0, sizeof(TypeTree) + sizeof(Type) * size);
        *tree = (TypeTree){.postfix = postfix, .size = size};
        for (int k = 0; k < size; k++) {
                // FIX: merge this loop with the one in solve_tree
                Type *t = tree->types + k;
                *t = (Type){.master_t = t};
        }

        solve_types(tree);
        return tree;
}

// ------------------------------------------------------------------

typedef struct {
        FILE *oot;
        const AstNode *exprs; // FIX: pick a name, exprs or postfix
        Type *types; // FIX: make this const, let one-hop master be the rule.
        uint32_t depth;
        uint32_t ntypes;
        Type *stack[MAX_DEPTH];
} Unparser;

static bool unparse_push(Unparser *unp, Type *type)
{
        assert(type == type->master_t); // FIX: the names
        uint32_t depth = unp->depth, k = depth;
        while (k--)
                if (unp->stack[k] == type)
                        return false;
        unp->stack[depth] = type;
        unp->depth = depth + 1;
        return true;
}

static void unparse_pop(Unparser *unp)
{
        int depth = (int)unp->depth - 1;
        assert(depth >= 0);
        unp->depth = depth;
}

static void unparse_type_(Unparser *unp, Type *t)
{
        Type *types = unp->types;
        FILE *oot = unp->oot;

        uint32_t idx = masterise(types, t - types);
        t = types + idx;

        print_typename(oot, unp->exprs, idx);

        Type ty = *t->master_t;
        if (!ty.arg_t) {
                // if it's not a function there is no structure to expand.
                return;
        }

        if (!unparse_push(unp, t)) {
                // Push failure means we have found recursion.
                return;
        }

        fputs("=(", oot);
        unparse_type_(unp, ty.arg_t);
        fputc(' ', oot);
        unparse_type_(unp, ty.ret_t);
        fputc(')', oot);
        unparse_pop(unp);
}

// FIX: make all the args const.
static void unparse_type(FILE *oot, TypeTree *tree, Type *t)
{
        Unparser unp = {
            .oot = oot,
            .exprs = tree->postfix, // FIX: pick a name, exprs or postfix.
            .types = tree->types,
        };

        unparse_type_(&unp, t);
}

int act_type(FILE *oot, const Ast *ast)
{
        TypeTree *ttree = build_type_tree(ast);
        for (size_t k = 0; k < ttree->size; k++) {
                unparse_type(oot, ttree, ttree->types + k);
                fputc('\n', oot);
        }

        free(ttree);
        fflush(oot);
        return 0;
}
