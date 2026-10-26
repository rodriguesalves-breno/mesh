The Alves Style Manifesto
=========================

The Alves style is the house layout of the Mesh C source. It is not a theme,
a color scheme or a personal preference imposed on reviewers. It is a set of
constraints on how source text occupies a terminal, chosen because source
text is read far more often than it is written and because a large part of
that reading happens over SSH, inside Vim, Neovim or Emacs, on a screen that
is narrower and shorter than a desktop editor.

The central idea is **efficiency through density**. A program should spend
its available rows and columns on information: names, conditions, calls,
error checks and comments that explain a decision. It should not spend them
on indentation that the compiler discards, on blank lines that separate
nothing, or on brace arrangements that make a function look taller without
making it clearer.

.. contents::
   :depth: 2
   :local:


The Machine Does Not Care
-------------------------

C is a free-form language. Whitespace separates tokens and terminates
preprocessor directives; beyond that, the compiler does not preserve it.
Indentation, blank lines and the position of a brace have no meaning in the
generated object code. A function written in the kernel style and the same
function written in the Alves style compile to the same instructions if the
tokens are the same.

That fact cuts in two directions. It means the style cannot make the program
faster by itself. It also means the style is free to optimize for the reader
without sacrificing machine performance. The only performance question that
matters for a layout is human: how quickly can a reviewer find the error
path, the ownership transfer and the next branch?

Mesh does not use whitespace as a proxy for structure. Structure is carried
by braces, keywords and indentation-free adjacency. A reader who loses the
leading spaces has lost nothing, because there were none to lose.


The Rules
---------

The style is small enough to state completely. The detailed enforcement
rules, including the nesting ceiling, are in :doc:`complexity_guardrails`;
the C-specific argument is in :doc:`idiomatic_c_optimization`.

**Zero left margin.**
Every statement begins at column zero. There is no tab, no four-space block
and no alignment column for declarations. A nested statement is recognized
by the brace that encloses it, not by how far it has been pushed to the
right. This preserves horizontal space for the expression itself.

**No decorative blank lines.**
There are no blank lines inside functions and no blank lines between
functions. A comment marks a phase. A blank line is not a paragraph break in
code; on a 24-row terminal it is a row stolen from the next screen.

**Braces bind to their construct.**
The opening brace stays on the line that introduces the block. The closing
brace stays on the line of the continuation: ``} else {``, ``} else if(...)``
and ``} while(...)``. There is no orphan brace line and no Allman-style
function brace.

**Compact control syntax.**
Write ``if(condition)``, ``for(size_t i=0;i<n;i++)`` and
``while((n=read(fd,buf,sizeof(buf)))>0)``. Binary operators are written
without decorative spaces when the expression remains unambiguous. The goal
is not code golf. The goal is that a short condition fits in one glance and
that a long condition is broken at a semantic boundary, not at an arbitrary
column.

**Guard clauses over success pyramids.**
Invalid input, missing resources and failed system calls return early. The
success path is a linear sequence, not a staircase of nested ``if`` blocks.
A function that would need a fourth level of control nesting is divided into
helpers.

**One screen per function.**
A function should fit on a normal terminal screen at 100 columns or less. The
current source has no line longer than 100 characters. This is a soft target
for very small helpers and a hard ceiling for new code.

**Names remain explicit.**
Density never justifies cryptic identifiers. A short function may use ``i``,
``fd`` or ``n`` when the scope is obvious. A public function keeps its module
prefix: ``graph_load``, ``manifest_save``, ``net_get``, ``conflict_choose``.
The style removes layout noise, not meaning.


A Minimal Entry Point
---------------------

The smallest complete example is the process entry point. The listing below
is the actual shape of ``src/main.c``: no indentation, no blank lines, one
guard clause and a linear success path. The three-space offset in the RST
block is documentation framing, not source indentation; in the file, every
line of C begins at column zero.

.. code-block:: c

   int main(int argc, char**argv) {
   int rc;
   signal(SIGPIPE, SIG_IGN);
   if(net_init()<0)
   return 1;
   rc=cli_main(argc, argv);
   net_shutdown();
   return rc;
   }

The function is nine lines. It contains a declaration, a signal
configuration, a guarded initialization, a call, a cleanup call and a return.
There is no blank line between the declaration and the signal call, because
the two are part of the same setup phase. There is no brace on its own line,
because the signature already announces the block. There is no indentation,
because the reader can see the guard clause from the shape of the text.

A reviewer can verify the entire lifetime of the process in one screen. That
is the point of the style.


Efficiency Through Density
--------------------------

Density is often misunderstood as compression. The Alves style is not a
minifier. It does not rename variables to one letter, merge unrelated
statements onto one line, remove error checks or replace comments with
cryptic symbols. Those techniques reduce the character count and increase
the defect rate. The Alves style reduces only the whitespace that carries no
information.

The accounting is straightforward. A four-level nested block in the kernel
style can consume 32 columns of an 80-column terminal before the first token
of the expression. The same block in the Alves style consumes zero. On a
narrow SSH session, those 32 columns are the difference between seeing the
condition and scrolling horizontally to find its right operand. The same
logic applies vertically: a function with a blank line every three
statements spends roughly a quarter of its rows on nothing.

The source rewrite measured the effect. The C tree fell from 8,272 to 7,231
physical lines, a reduction of 1,041 lines or 12.6%. Of those removed lines,
743 were blank lines; the remaining 298 were non-blank lines removed by brace
reflow, declaration merging, guard-clause extraction and comment
consolidation. The build remained warning-free. The full accounting,
including per-file numbers and the caveats of using line count as a metric,
is in :doc:`metrics_and_line_optimization`.

Density is not the goal by itself. A file with fewer lines can still be
wrong. The claim is conditional: given the same tokens and the same
behaviour, the dense layout removes rows that carried no information and
leaves the reader more room for the rows that do. The compiler sees the same
program. The reviewer sees less noise.


Why SSH Changes the Argument
----------------------------

A desktop editor can hide indentation behind folding, render whitespace
faintly, show a minimap and allocate a wide monitor to a single file. A
remote session often cannot. The terminal may be 80 columns wide because the
user is splitting a tmux pane. The SSH client may be on a laptop with a
13-inch screen. The editor may be Vim over a slow link, where redraw cost is
visible. The session may be inside a container with no graphical clipboard
and no second monitor.

Under those constraints, the left margin is the most valuable real estate in
the file. Code that starts at column zero uses all of it. Code that starts at
column 24 uses a third of an 80-column terminal before the first meaningful
token. The Alves style is an ergonomic response to the actual environment in
which maintainers review patches, not a rejection of desktop tooling.

The terminal argument is developed in :doc:`ssh_ergonomics`, including
practical settings for Vim, Neovim and Emacs. The style itself does not
require a particular editor. It requires only that the source remain
readable when the editor is reduced to a text grid.


What the Style Is Not
---------------------

The style is not a claim that the Linux kernel is badly written. The kernel
has different constraints: a very large codebase, a long history, a
distributed review process and a culture that has standardized on tabs and a
particular brace layout. Those constraints produced a coherent style. Mesh
is a small, single-binary tool with a different review surface. It can afford
a layout that the kernel cannot impose retroactively.

The style is not a language extension. It uses C17 and POSIX exactly as the
rest of the project does. It does not rely on compiler-specific whitespace
handling, preprocessing tricks or non-standard pragmas. A file formatted in
the style compiles with the same flags as any other C17 file.

The style is not a substitute for architecture. A flat function can still
have a race, a flat module can still have the wrong boundary and a dense
header can still expose the wrong ownership contract. Density makes the
structure easier to inspect; it does not create the structure. The
architectural rules remain in :doc:`architecture` and the correctness rules
remain in :doc:`contributing`.


Adoption
--------

The style is mandatory for new and modified code in the trunk. It is optional
for individual developers in the sense that no one is required to convert a
private branch, a downstream fork or a personal project. The distinction is
deliberate: a team needs one layout at the merge boundary, but it does not
need to win a cultural war over every editor buffer.

The governance argument is in :doc:`onboarding_and_veterans`. The short
version is that veterans are not asked to forget another style; they are
asked to accept the trunk's interface for review. The measured reduction,
the warning-free build and the shorter review surface are the evidence. The
style becomes durable when the team experiences the gain, not when a rule is
announced.

For a reader who wants the full argument in one place,
:doc:`ideal_programming_book_ref` maps the 64-page reference book to the
documents in this directory. For the control-flow rule that gives the style
its discipline, continue with :doc:`complexity_guardrails`.

.. seealso::
   For the current implementation boundaries and safe operating mitigations, see
   :doc:`limitations`.
