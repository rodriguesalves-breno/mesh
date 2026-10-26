The Ideal Programming Style: Reference
======================================

This document is the technical index for the 64-page book *O Estilo de
Programação Ideal* ("The Ideal Programming Style"). It does not reproduce the
book. It maps the book's themes to the Mesh documentation, states the reading
order for different audiences, and provides one synthetic C example that
combines the book's three central concerns: a function, a conditional and a
repetition.

The book is short by design. Sixty-four pages is enough to state a thesis and
demonstrate it; it is not enough to hide a weak argument behind length. The
same constraint applies to this directory. The documents are meant to be read
in a terminal, not displayed as a coffee-table object.

.. contents::
   :depth: 2
   :local:


What the Book Argues
--------------------

The book's argument can be reduced to five propositions. They are stated here
in the project's own words, not as quotations.

**Code is read in a viewport.**
The reader has a finite number of rows and columns. Every layout decision
spends or saves that budget. A style that ignores the budget is optimizing
for the author's editor rather than the reviewer's terminal.

**Whitespace is not free.**
The compiler discards it; the reader does not. Blank lines, indentation and
brace ceremony consume space that could carry a condition, a name or an
error check. Removing them is not compression; it is reallocating attention.

**Structure should be explicit.**
Braces, guard clauses and early returns make control flow visible. Deep
indentation makes it inferred. The reader should not have to reconstruct a
pyramid to find the failure path.

**Density requires discipline.**
A dense function is not a minified function. Names stay meaningful, checks
stay present, ownership stays documented and helpers have one job. Density
without discipline is obfuscation.

**Adoption is cultural, not mechanical.**
A formatter can change spaces; it cannot change how a team reviews code. The
style becomes durable when contributors experience a shorter review and a
smaller diff, not when a policy is announced.

Those propositions are developed in the documents listed below. The book is
the compact statement; the documentation is the engineering evidence.


Cross-Reference
---------------

.. list-table:: Book themes and Mesh documents
   :header-rows: 1
   :widths: 24 34 42

   * - Book theme
     - Mesh document
     - What it adds
   * - Density and vertical economy
     - :doc:`alves_style_manifesto`
     - The full manifesto, the main sample and the SSH argument.
   * - Control flow and guard clauses
     - :doc:`complexity_guardrails`
     - The three-level ceiling and the ``chunk_work_tree`` case.
   * - Idiomatic C
     - :doc:`idiomatic_c_optimization`
     - What is kept from C and what is dropped from kernel ceremony.
   * - Measurement
     - :doc:`metrics_and_line_optimization`
     - The 1,041-line reduction, per-file tables and caveats.
   * - Terminal ergonomics
     - :doc:`ssh_ergonomics`
     - Vim, Neovim, Emacs, tmux and the 100-column ceiling.
   * - Team adoption
     - :doc:`onboarding_and_veterans`
     - Optional adoption, veteran psychology and governance rules.
   * - Architecture
     - :doc:`architecture`
     - The structures and ownership rules that the layout makes visible.
   * - Review discipline
     - :doc:`contributing`
     - The patch, build and review contract for the trunk.

The table is also a reading order for a new maintainer. Start with the
manifesto, then the guardrails, then the metrics. Read the architecture and
contributing documents with the source open. Read the SSH document when the
first review happens on a narrow terminal.


Reading Paths
-------------

Different readers need different entries. The book is short enough to read
twice; the documentation is organized so that the second reading can be
targeted.

**The new contributor.**
Read :doc:`alves_style_manifesto` for the thesis, then
:doc:`complexity_guardrails` for the rule. Read ``src/main.c`` and
``chunk_work_tree()`` with the text open. Do not start with the metrics; the
number is meaningless before the shape is understood. Finish with
:doc:`onboarding_and_veterans` to understand why the project does not demand
a conversion.

**The reviewer.**
Read :doc:`contributing` first, then :doc:`complexity_guardrails`. The review
checklist is the operational form of the book. Keep :doc:`ssh_ergonomics`
nearby if the review happens over a remote connection. The metrics document
is useful when a patch claims a style improvement that is really a reformat.

**The maintainer.**
Read :doc:`metrics_and_line_optimization` and :doc:`benchmarks` together. One
measures source surface; the other measures runtime. Do not let either
number substitute for the other. Read :doc:`architecture` for the ownership
rules that the layout is supposed to make visible.

**The veteran.**
Read :doc:`onboarding_and_veterans` first. It states the compromise before
the technical argument, which is the right order for a reader who has seen
style campaigns before. Then read :doc:`idiomatic_c_optimization` for the
C-specific case. The manifesto can wait until the governance question is
settled.

**The tooling author.**
Read :doc:`ssh_ergonomics` and :doc:`gui_integration`. The first explains the
terminal budget; the second explains the passive status contract. Neither
requires a plugin, but both benefit from an editor that does not reindent the
buffer.


A Synthetic Example
-------------------

The following example is synthetic. It is not a quotation from the Mesh
source and it is not intended to be copied as production code. It exists to
show the three concerns of the book in one small function: a guard clause, a
loop and a conditional, all at column zero with no decorative rows.

.. code-block:: c

   static int count_large(const struct mesh_file_list*list) {
   size_t n=0;
   if(!list)
   return -1;
   for(size_t i=0;i<list->n;i++) {
   if(list->v[i].size>=MESH_LARGE_FILE)
   n++;
   }
   return n>(size_t)INT_MAX?-1:(int)n;
   }

The function is ten lines. It validates the input before dereferencing it,
iterates over the entries, counts the ones above the large-file threshold
and refuses to return a value that does not fit in an ``int``. The sample
assumes ``<limits.h>`` for ``INT_MAX``; it is a teaching fragment, not a
translation unit. The loop body
is one conditional and one increment. The function has a single return type,
a single ownership rule (it borrows the list) and no hidden allocation.

The example also shows what the style does not do. It does not merge the
guard and the loop, because they are different decisions. It does not use a
ternary for the input check, because the early return is clearer. It does not
remove the cast on ``INT_MAX``, because the comparison must be correct. The
density is in the absence of blank rows and indentation, not in the absence
of semantics.

A kernel-style layout of the same function would add a blank line after the
declaration, a brace on its own line for the function, a tab before every
statement, spaces around every operator and perhaps a comment banner. The
compiler would see the same program. The reviewer would see a taller file.
The book's claim is that the taller file is not more readable merely because
it is taller.


How to Read the Example
-----------------------

Read the example in four passes.

1. **Signature.** What does the function return, what does it take and what
   does it own? The pointer is borrowed; the function allocates nothing.
2. **Guard.** What input is rejected before the loop? A null list returns
   ``-1``. The rest of the function can assume a valid list.
3. **Loop.** What is the iteration domain? Every entry in the list. What is
   the condition? The entry is at least ``MESH_LARGE_FILE`` bytes. What is
   the effect? Increment the count.
4. **Return.** What invariant does the final expression enforce? The count
   must fit in an ``int``, or the function reports an error.

Those four passes are the same ones a reviewer should use on a real
function. The style is successful when the passes are short and the
structure is visible without scrolling. The example is ten lines because the
problem is small; a real function may be forty lines, but the four passes
should still apply.

The example is deliberately not clever. It does not use a pointer walk, a
comma operator or a single-letter accumulator. It is idiomatic C with a
compact layout. That combination is the book's subject.


What the Book Is Not
--------------------

The book is not a language specification. It does not define C17, POSIX or
the Mesh file formats. Those are in the source, the headers and
:doc:`architecture`.

The book is not a formatter specification. It describes a layout and a
control-flow discipline, but it does not provide a parser, a linter or an
editor plugin. A formatter can remove blank lines; it cannot decide that a
function should be flattened. The review rules in
:doc:`complexity_guardrails` are the operational part.

The book is not a claim that one style fits every project. The kernel style
is appropriate for the kernel. A large application framework may need a
different layout, a different line length or a different comment policy. The
book argues for a style that fits a small, auditable C tool read over a
terminal. Mesh is the demonstration, not the universal law.

The book is not a substitute for measurement. The 1,041-line reduction is
evidence for this tree, not a promise for another. A team that adopts the
style should measure its own tree, run its own build and review its own
diffs. The method in :doc:`metrics_and_line_optimization` is reusable; the
number is not.


The 64-Page Constraint
----------------------

The page count is part of the argument. A style guide that expands to 300
pages becomes a bureaucracy; a guide that fits in 64 pages can be read in an
afternoon and consulted in a review. The constraint forces the author to
choose the rules that matter and to state them without hedging.

The Mesh documentation follows the same principle at a different scale. Each
new document has one job:

* the manifesto states the thesis;
* the guardrails state the control-flow rule;
* the C document separates idiom from ceremony;
* the metrics document reports the measurement;
* the SSH document translates the thesis into terminal practice;
* the onboarding document states the governance compromise;
* this document provides the index.

Seven new documents, one thesis, no filler. The directory is the long form of
the book; the book is the short form of the directory. A reader can start at
either end and reach the same argument.

The next step is practical. Open :doc:`alves_style_manifesto`, read the main
sample, then open ``src/main.c`` in a terminal and compare the two. The
argument is easier to test than to describe.

.. seealso::
   For the current implementation boundaries and safe operating mitigations, see
   :doc:`limitations`.
