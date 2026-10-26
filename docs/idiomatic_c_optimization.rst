Idiomatic C Optimization
========================

The Alves style is sometimes described as a reaction to the Linux kernel
style. That description is incomplete. The style is a reaction to ceremony
that has been copied from the kernel without the constraints that produced
it. Mesh is not a kernel. It is a small user-space tool with a different
history, a different review process and a different reader. Its C should be
idiomatic for its own problem, not loyal to a layout that was designed for a
different one.

This document states what "hyper-respect for pure C" means in practice. It
separates the parts of the kernel tradition that are genuine C knowledge from
the parts that are institutional ritual, and it shows how the Alves style
keeps the first and removes the second. The layout argument is in
:doc:`alves_style_manifesto`; the control-flow rule is in
:doc:`complexity_guardrails`.

.. contents::
   :depth: 2
   :local:


Respect the Language, Not the Ceremony
--------------------------------------

C is a small language with a large surface of undefined behaviour. Respecting
it means using its types correctly, checking the return value of every
operation that can fail, keeping ownership explicit and avoiding constructs
whose behaviour depends on the compiler. It does not mean reproducing the
visual conventions of the kernel, because those conventions are not part of
the language.

The kernel style exists for good reasons inside the kernel: a huge tree, a
long-lived history, a distributed review process and a culture that has
standardized on tabs, 80 columns and a particular brace layout. Those reasons
do not automatically transfer to a 7,000-line user-space program. When they
are copied without the reasons, they become ritual: a tab stops at eight
columns because the kernel says so, a function brace moves to its own line
because the kernel says so, and a reviewer spends attention on the shape of
the diff instead of the correctness of the code.

The Alves style keeps the engineering content and drops the ritual. It uses
C17 declarations where they shorten a function, compact operators where the
expression is unambiguous, and column-zero layout because the reader's
horizontal space is finite. It does not use a preprocessor macro to save a
line, a comma operator to merge two statements, or a clever expression that
requires a comment to decode. Density is not the same as cleverness.


What Is Kept
------------

The style is conservative about everything that affects correctness.

**Explicit types.**
Byte counts and offsets use ``uint64_t``. Timestamps use ``int64_t``. Sizes
and indices use ``size_t``. Predicates use ``bool``. ``int`` is not used for
a file size. Format strings use the ``<inttypes.h>`` macros so the same code
is correct on LP64 and other data models.

**Explicit ownership.**
A function that allocates says so in its contract. The caller frees what it
receives. The code does not hide an allocation behind a macro or a getter
that looks like a pure accessor. Error paths free what they acquired, and
the single ``done:`` label is reserved for cleanup, not for control flow.

**Explicit failure.**
Every ``open``, ``read``, ``write``, ``pread``, ``fsync``, ``close``,
``rename``, ``stat``, ``lstat``, ``mkdir`` and ``opendir`` is checked. Short
writes are looped; ``EINTR`` is retried; other errors propagate. A transport
success is not an application success, so HTTP status codes are checked
separately from libcurl return codes.

**Bounded strings.**
``strcpy``, ``strcat``, ``sprintf`` and ``gets`` are forbidden. The code uses
``snprintf`` with an explicit size, ``memcpy`` with a known length, or the
bounded helpers in ``core.h``. A silently truncated path is a bug.

**POSIX and C17.**
The code uses POSIX interfaces and the C17 language. It does not use GNU
statement expressions, unguarded ``__attribute__`` or Linux-only system calls
without a fallback. The one deliberate Linux dependency is
``/proc/self/exe`` in ``mesh_self_dir()``, and it has a ``getcwd()``
fallback.

None of those rules is relaxed by the dense layout. If anything, the flat
style makes them easier to audit: a failed check is a line at column zero,
not a branch hidden under three levels of success.


What Is Removed
---------------

The style removes layout decisions that do not change the program.

.. list-table:: Ceremony and its replacement
   :header-rows: 1
   :widths: 24 34 42

   * - Kernel or Torvalds convention
     - Alves replacement
     - Reason
   * - Tabs at eight columns
     - No indentation
     - The compiler ignores it; the terminal pays for it.
   * - Allman brace for functions
     - Brace on the signature line
     - One less row per function and a direct visual link.
   * - Blank line between phases
     - Comment at the phase boundary
     - A comment carries information; a blank line does not.
   * - ``if (condition)``
     - ``if(condition)``
     - One less token gap in a narrow viewport.
   * - ``char *p``
     - ``char*p``
     - The star belongs to the declarator; the space adds nothing.
   * - 80-column recommendation
     - 100-column hard ceiling
     - Modern terminals and long identifiers need a little more room.
   * - Deep success pyramids
     - Guard clauses and at most three levels
     - The error path stays local and reviewable.

The table is not a claim that the kernel is wrong. It is a claim that the
kernel's layout is optimized for the kernel's constraints. Mesh has a
different set of constraints, and it should not import a convention whose
original justification is absent.

The one convention that is not merely visual is the brace. Keeping ``} else
{`` on one line is a readability decision: the reader sees that the two
branches are alternatives without scanning a separate line. Keeping the
function brace on the signature line is the same decision at a larger scale.


A Loop Without Decoration
-------------------------

The following synthetic loop shows the shape of iteration in the style. It
walks a file list, skips entries that do not need chunk processing and
accumulates a total. There is no blank line between the loop header and the
first statement, no indentation and no decorative brace line.

.. code-block:: c

   for(size_t i=0;i<list->n;i++) {
   struct mesh_file_entry*entry=&list->v[i];
   if(entry->size<MESH_LARGE_FILE)
   continue;
   if(entry->nchunks==0)
   continue;
   total+=entry->size;
   }

The loop is six statements in eight lines. The declaration of ``entry`` is
inside the loop because it is not needed outside. The two ``continue`` guards
reject the common cases before the accumulation. The closing brace is at
column zero, aligned with the ``for`` that opened it.

A kernel-style version of the same loop would use a tab before every
statement, a brace on its own line, blank lines around the body and spaces
around every operator. It would be perhaps fourteen lines and would say
exactly the same thing to the compiler. The difference is not functional. It
is the number of rows the reviewer must traverse to confirm that the loop
does not touch a small file.

The loop also illustrates a boundary: the style does not forbid multiple
statements per line, but it does not encourage them. The declaration, the
guards and the accumulation each occupy their own line because each is a
separate idea. Density removes empty rows, not semantic boundaries.


Idioms That Survive
-------------------

The style keeps the C idioms that make the language usable.

**``for`` with a local index.**
``for(size_t i=0;i<n;i++)`` is the default for arrays. The index type matches
the array size, and the scope ends with the loop.

**``while`` with an assignment.**
``while((n=read(fd,buf,sizeof(buf)))>0)`` is idiomatic when the read result is
the loop condition. The parentheses around the assignment are mandatory and
are not removed by the style.

**``if(!ptr)``.**
A null check is written without a comparison to ``NULL`` when the pointer is
the whole condition. The shorter form is unambiguous in C.

**``sizeof(*p)``.**
Allocation and copy sizes are expressed in terms of the destination object,
not a repeated type name. This survives refactoring.

**``goto done``.**
A single cleanup label is acceptable when a function acquires more than one
resource. The label is not a general-purpose jump; it is the tail of the
error path.

**``static`` helpers.**
A function that is not part of the module contract is ``static``. The header
is the public surface; everything else is an implementation detail.

**``const`` on inputs.**
A pointer that is not modified is ``const``. This is documentation the
compiler can enforce.

The style does not add new idioms. It removes the visual distance between the
idiom and the reader.


What Density Does Not Mean
--------------------------

Density is not an excuse for any of the following:

* **Code golf.** A one-line expression that requires a comment to decode is
  not dense; it is compressed.
* **Hidden control flow.** A comma operator that performs two assignments, a
  macro that expands to a loop, or an assignment inside a condition that is
  not the loop's main test all hide structure.
* **Removed checks.** A shorter error path that omits a check is a bug, not a
  style improvement.
* **Meaningless names.** ``a``, ``b`` and ``c`` are acceptable for a
  three-line loop and unacceptable for a public function.
* **Blank-line removal in prose.** Comments and documentation still use
  paragraphs. The rule applies to code layout, not to explanatory text.
* **A line-count target.** The 1,041-line reduction is a result, not a quota.
  A correct patch that adds a necessary line is welcome.

The style is successful when a reviewer can read a function once and state
what it does, what it owns and how it fails. The line count is a symptom of
that success, not its definition.


Performance and the Compiler
----------------------------

Formatting does not change the instruction stream if the tokens are the same.
The Alves conversion did include structural changes: guard clauses, extracted
helpers and merged declarations. Those changes can affect inlining and code
layout, so the project did not assume they were free. The tree was rebuilt
with ``-std=c17 -O3 -Wall -Wextra`` and produced no warnings. The runtime
checks exercised the command paths that the rewrite touched.

The performance claims in :doc:`benchmarks` are about wall time, memory,
bytes and requests, not about source lines. The source-density measurement in
:doc:`metrics_and_line_optimization` is a review metric. Keeping the two
separate prevents a familiar fallacy: assuming that a shorter source file
must produce a faster binary. It need not, and the project does not claim
that it does.

What the style does claim is narrower and defensible: for the same behaviour
and the same warning-free build, the dense layout gives the reviewer more
space for the logic and less space for the scaffolding. That is an
engineering benefit even when the binary is byte-for-byte identical.

The next document, :doc:`metrics_and_line_optimization`, quantifies the
change. The practical terminal consequences are in :doc:`ssh_ergonomics`.

.. seealso::
   For the current implementation boundaries and safe operating mitigations, see
   :doc:`limitations`.
