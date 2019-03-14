Terrible Programming Language 1: The lambda-calculus
====================================================

Author     : Adrian Ratnapala

Copyright (C) 2019, Adrian Ratnapala, published under the GNU Public License
version 2.  See file [LICENSE](LICENSE).

To build the interpreter, just run make:

        make

To run interpreter, do:

        b/lambda < YOUR_SOURCE_CODE

To run the tests, you can do:

        TEST_MODE=full make clean all test

But this requires lots of dependencies, such as `clang-format`, `valgrind`,
`gcovr`, `py.test` and maybe other things I have forgotten.


Part 0: Brack-cat
-----------------

Let's make a language that does almost nothing.  It just puts parentheses
around its input.  That means the source:

        x

Is the program that prints:

        (x)

To standard output.


The point of writing this is to set down some infrastructure.  Things like the
build system (`make`), the test framework (`py.test`) and coverage analysis
(`gcc` and `gcovr`).  None of that is boring; but none of it is the topic of
this README.

The code that is on topic is in `lambda.c`, which is very exciting:


        void interpret(FILE *oot, size_t src_len, const char *zsrc)
        {
                assert(!zsrc[src_len]);
                assert(strlen(zsrc) == src_len);

                fputc('(', oot);
                fputs(zsrc, oot);
                fputc(')', oot);
                fputc('\n', oot);
                fflush(oot);
        }


Part 1: Left-associativity
--------------------------

To step towards a real language, lets make it  implement left-associativity of
function application.

What?

We are trying to implement the [Lambda Calculus][wiki-lc], which is what [Alonzo
Church][wiki-ac] came up with when he showed how arbitrary computation could be
represented with nothing but function application.  The language contains
nothing but function-literals and variables (which are placeholders for
functions and their args).

[wiki-lc]: https://en.wikipedia.org/wiki/Lambda_calculus
[wiki-lc]: https://en.wikipedia.org/wiki/Alonzo_Church

Our first step is to drop even the functions, and have only variables which
behave like functions syntactically.  In particular stringing variables
together means applying them to each other as functions, left-associatively.  I
that mean if we have the program text:

        x y z

We get the output:

        ((x y) z)

This means: _"Call `x` with `y` as an argument.  The result is a function, call
it it with `z` as an argument."_ `y` got paired with `x` (to the left) rather
than `z` to the right.  We can override this default with explicit parentheses.
Thus input:

        x (y z)

yields the output:

        ((x y) z)


## Parsing

There might be a clever way to implement the above without parsing into an AST
and then printing out the AST; but we want an AST anyway so we might as well
write a parser.

Our parser implements the following grammar:

        varname         ::= [a-z]
        non-call-expr   ::= varname | '(' expr ')'
        expr            ::= non-call-expr | expr non-call-expr

The parser is a top-down parser in which each of the elements above corresponds
to a function with a name beginning with `parse_` or `lex_`.  These parse
whatever thing they are named after and return the result (into the AST for
`parse_*`, and into a caller-supplied location for `lex_*`).

So grammar rules and parser-internal functions map onto each other closely.  But
what about node types in the AST?  Each node is of the form:

        struct AstNode {
                uint32_t type;
                union {
                        AstCall CALL;
                        AstFree FREE;
                };
        };

So there are three data-types in all, `AstCall` and `AstFree` for the two types
of node, and `AstNode` itself which can be either.  These don't map very
obviously onto the syntax.

But lets consider the stricter grammar:

        varname ::= [a-z]
        call    ::= '(' expr expr ')'
        expr    ::= call | varname

Now we have a very clear mapping between data types and rules:

        varname -> AstFree
        call    -> AstCall
        expr    -> AstNode

Better still the stricter grammar, and the mapping are reflected in our code.
Not in the parser, but in the printer function `unparse`, which is:

        void unparse(FILE *oot, const Ast *ast, const AstNodeId root)
        {
                AstNode node = ast_node(ast, root);
                switch ((AstNodeType)node.type) {
                case ANT_FREE:
                        fputc(node.FREE.token + 'a', oot);
                        return;
                case ANT_CALL:
                        fputc('(', oot);
                        unparse(oot, ast, node.CALL.func);
                        fputc(' ', oot);
                        unparse(oot, ast, node.CALL.arg);
                        fputc(')', oot);
                        return;
                }
                DIE_LCOV_EXCL_LINE("unparsing found ast node with invalid type id %u",
                                   node.type);
        }

The two `case` clauses correspond to the `call` and `varname` rules, while the
function as a whole corresponds to the `expr` clause.

So set down the following tentative, informal, hypothesis:

### Hypothesis: Data-structures map closely to strict grammars.

For any given data structure we might like to serialise, there are strict
grammars whose rules map closely to type in the data structure.  Looser
grammars can be written for the same data structure, but they tend to be more
complicated because they need various catch-alls that don't represent
particular data types.

### The AST and its post-fixity.

How is the AST actually put together?  The `AstNode`s are stored contiguously
in memory along with some metadata.

        typedef struct {
                const char *zname;
                const char *zsrc;
                SyntaxError *error;
                uint32_t zsrc_len;
                uint32_t nnodes_alloced;
                uint32_t nnodes;
                AstNode nodes[];
        } Ast;

Also we have:

        typedef struct {
                uint32_t token;
        } AstFree;

Which means that free variables are just represented by a number representing
the varname.  More interestingly we have:

        typedef struct {
                uint32_t arg_size;
        } AstCall;

What???

We are relying on a guarantee that nodes are stored in post-fix order.  That
means each sub-tree in the AST is stored contiguously and the root of every
tree is the last node in it.

Given a pointer to a CALL, we can calculate the pointer to the root of its
argument g by just subtracting 1.  But the full argument sub-tree has a
variable size, which we must store.  The root of the called function
is at

        pointer-to-called-function = pointer-to-CALL - arg_size - 1

