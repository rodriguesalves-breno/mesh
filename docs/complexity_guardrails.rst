Complexity Guardrails
=====================

The Alves style is not only a layout. It is a control-flow contract. A
function may nest control constructs at most three levels below its body.
When a fourth level appears, the function is not "a little deep"; it is a
review failure. The logic must be flattened with a guard clause or extracted
into a helper with a name that states its purpose.

This document defines the ceiling, explains why it exists, and shows how two
representative Mesh functions, ``chunk_work_tree()`` and ``graph_commit()``,
are kept inside it. The layout argument is in
:doc:`alves_style_manifesto`; the measured effect of the rewrite is in
:doc:`metrics_and_line_optimization`.

.. contents::
   :depth: 2
   :local:


What Counts as a Level
----------------------

The function body is level zero. A top-level ``if``, ``for``, ``while``,
``switch`` or compound block inside the body is level one. A construct inside
that block is level two. A construct inside level two is level three. Level
four is forbidden.

The definition is deliberately mechanical. It does not depend on how the code
is indented, because the style does not indent. It does not depend on the
number of braces on a line, because a brace that opens a block still opens a
level. It does not exempt ``else``: an ``else`` branch is another block and
participates in the count.

A ``switch`` counts as one level for the switch itself. Each ``case`` body is
one additional level. A ``case`` that contains an ``if`` and a ``for`` has
already reached the ceiling; a second nested condition must be extracted or
turned into a guard.

Three levels are enough for the legitimate cases in a small tool: a loop
over entries, a condition on each entry, and a small branch inside the
condition. They are not enough for a parser, a protocol state machine or a
recursive tree walk that wants to live in one function. Those are exactly the
places where a helper makes the code easier to test and easier to name.


Why Three
---------

The ceiling is a working-memory limit, not a compiler limit. A reviewer who
is reading a patch on a narrow terminal can hold a function body, one loop
and one condition in view. At the fourth level, the reader must remember the
enclosing conditions while interpreting the innermost branch, and the
successful path becomes a staircase that is hard to scan.

Deep nesting also hides error handling. When every failure is handled in an
``else`` branch at the bottom of a pyramid, the reader must reconstruct the
conditions that led there. When the same failure is a guard clause at the
top, the condition and the response are adjacent. The second form is shorter,
but the important difference is that it is local.

The ceiling is not a mandate to write tiny functions. A function can be long
if its logic is a flat sequence of steps. ``graph_commit()`` is a good
example: it has many statements, but the control flow is linear. Length is
not the enemy; hidden state and nested alternatives are.


Guard Clauses
-------------

A guard clause is an early exit that rejects a precondition the rest of the
function cannot handle. The common forms are:

* ``if(!ptr)`` followed by an error return;
* ``if(fd<0)`` followed by a return or a jump to cleanup;
* ``if(size<THRESHOLD) continue;`` inside a loop;
* ``if(total>UINT64_MAX-chunk)`` followed by an overflow error.

The guard clause is not a ``goto`` in disguise. It is a statement that the
remaining code is valid only when the condition is false. The reader can
then ignore the rejected case and follow the success path without carrying a
nested alternative in mind.

A guard clause should be close to the operation it protects. Do not collect
five unrelated validations at the top of a 300-line function and call that
flattening; the reader still has to remember them. Validate an argument
before it is used, validate a file descriptor before reading it, and validate
a length before allocating for it.


Case Study: chunk_work_tree
---------------------------

``chunk_work_tree()`` walks a file list and builds a chunk table for every
file large enough to need one. It is a loop with several failure modes:
path construction can fail, the chunk scan can fail, the chunk lengths can
overflow, and the sum can disagree with the file size. The Alves rewrite
keeps the function within the three-level ceiling by making every failure a
local exit. The deepest construct is the overflow guard inside the chunk
loop, at level three.

The structure is:

* loop over entries (level one);
* skip entries below ``MESH_LARGE_FILE`` with ``continue``;
* join the repository path and return on failure;
* scan the file and return on failure;
* accumulate chunk lengths and return on overflow after freeing the list;
* compare the sum with the entry size and return on mismatch;
* transfer ownership of the chunk array to the entry.

Every rejection is adjacent to the operation that can fail. There is no
``else`` branch that depends on the success of three earlier branches. The
chunk list is freed before each error return that owns it, so the flat shape
also makes the ownership rule visible: the function either transfers the
array to the entry or releases it before returning.

This is the kind of function that produces subtle bugs when written as a
success pyramid. An overflow path that returns without freeing the chunk
list leaks memory. A mismatch path that falls through stores a chunk table
whose lengths do not sum to the file size. A path-join failure that is
handled at the bottom of the function can leave the entry in a partially
initialized state. The guard-clause form makes each of those exits a single,
reviewable line.

The same discipline appears in the helper that computes a boundary. The
nominal edge is checked first; the bounded look-ahead is a single loop; the
newline or NUL test is a guard inside that loop. No part of the boundary
logic needs a fourth level.


Case Study: graph_commit
------------------------

``graph_commit()`` creates a snapshot node and appends its canonical line to
``.mesh/history``. It is longer than ``chunk_work_tree()`` because it must
validate hashes, sanitize the message and branch, build a signature, append
under a lock, flush, and then update the in-memory graph. The rewrite did not
make it shorter by hiding checks; it made it linear by validating before it
touches the history file.

The function begins with one compound guard: the repository, graph, tree
hash, output buffer and both optional parent hashes must be valid. A bad
input returns ``EINVAL`` before a descriptor is opened. The message and
branch are sanitized next, and an invalid branch is another guard. The
signature and history line are formatted with explicit bounds. Only then does
the function open the history file, take the exclusive lock, write, flush and
release the lock. Each system-call failure closes the descriptor and returns.

The success path after the append is a sequence of allocations and copies:
the node is allocated, the hash and tree fields are copied, the message is
duplicated, the parent pointers are resolved and the node is linked at the
head. There is no nested success pyramid around the append. The graph update
is not placed inside the ``if(write succeeded)`` branch of a larger
condition; it follows the append because the function has already returned on
every append failure.

The important invariant is that the history line is not published until the
signature and all bounds checks have succeeded. A malformed hash or an
oversized message cannot produce a partial line. The flat shape makes that
invariant auditable: every check that can reject the input appears before the
``open()``, and every check that can reject the write appears before the
graph mutation.


The Continuous ``} else {`` Form
--------------------------------

The style keeps the closing brace and the continuation on one line. The
following synthetic fragment shows the pattern with no orphan brace lines and
no blank lines. It is a shape, not a quotation from a specific function.

.. code-block:: c

   if(fd<0)
   return -1;
   if(S_ISREG(st.st_mode)) {
   if(st.st_size<0) {
   errno=EINVAL;
   return -1;
   }
   } else {
   errno=EINVAL;
   return -1;
   }

The fragment reaches level two at the inner condition: function body
(level zero), the outer ``if`` (level one), the inner ``if`` (level two).
The ``else`` belongs to the outer condition and stays on the closing brace
line. There is no blank line between the guard and the block it guards,
because the two are a single decision.

A fourth level would require a change of shape, not a change of spacing. The
inner condition would be extracted into a helper, or the outer condition
would be inverted into a guard. The style does not allow the reader to
discover the fourth level by scrolling; it requires the author to remove it.


Extraction Patterns
-------------------

When a fourth level appears, there are four acceptable responses.

**Invert the condition.**
If the nested block is the error case, return it early. The success path then
loses one level. This is the most common fix and the one used throughout
``chunk_work_tree()``.

**Continue the loop.**
If the nested block applies only to a subset of entries, skip the rest with
``continue``. The loop body stays at one level for the common case.

**Extract a predicate.**
If the condition is a long boolean expression, give it a name. A function
such as ``work_tree_stable()`` turns a compound test into a single call and
keeps the caller flat.

**Extract a helper.**
If the nested block has its own failure modes and its own cleanup, it is a
function. ``fetch_object()``, ``json_unescape_char()`` and ``append_block()``
exist for this reason: each has one job and one level of responsibility.

Do not extract a helper solely to satisfy the line count. A helper that
takes eight parameters and returns a status code through a pointer has moved
the complexity, not removed it. The test is whether the helper has a name
that a reviewer can understand without reading its body.


Review Checklist
----------------

A reviewer checking the ceiling asks:

1. How many control constructs enclose the deepest statement? Count the
   function body as zero.
2. Is each error return adjacent to the operation that can fail?
3. Does every path that owns a resource release it before returning?
4. Is there an ``else`` branch that exists only because the success path was
   not allowed to return early?
5. Could the fourth level be removed by inverting a condition, continuing a
   loop, naming a predicate or extracting a helper?
6. Does the extracted helper have a narrow contract, or did it merely move
   the same tangle behind a new name?

The ceiling is a review rule, not a compiler check. The compiler will accept
a ten-level function. The project will not. The reason is not aesthetic
purity; it is that the reviewer's working memory is the bottleneck in a
small codebase, and the bottleneck is easier to protect than to repair after
a defect.

The measured result of the rewrite is reported in
:doc:`metrics_and_line_optimization`. The short version is that 1,041 lines
were removed, 743 of them blank, and the build stayed warning-free. The
control-flow part of that change is the part that cannot be produced by a
formatter: it required deciding where a function should end and what its
helper should be called.

.. seealso::
   For the current implementation boundaries and safe operating mitigations, see
   :doc:`limitations`.
