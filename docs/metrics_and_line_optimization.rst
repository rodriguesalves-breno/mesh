Metrics and Line Optimization
=============================

This document reports the source-level result of the Alves conversion. It is
an engineering record, not a marketing claim. The conversion removed 1,041
physical lines from the Mesh C source, reducing the tree from 8,272 to 7,231
lines, a change of 12.6%. The build remained warning-free and the
command-level smoke tests passed. No runtime benchmark is reported here; the
performance protocol is in :doc:`benchmarks`.

.. note::

   This document is a historical record of the C-to-Alves conversion. The C
   installer measured below has since been replaced by the standalone Python
   program ``install_uninstall.py``. The line counts and byte counts are intentionally
   left unchanged so the original measurement remains reproducible.

The number is interesting because of how it was produced. It was not a
minifier, a code generator or a campaign to delete error checks. It was a
reviewed layout and control-flow change: blank lines were removed, braces
were rebound to their constructs, declarations were merged where they were
only serving the old layout, and deeply nested success paths were rewritten
as guard clauses. The result is a smaller review surface with the same
behavioural contract.

.. contents::
   :depth: 2
   :local:


Scope and Method
----------------

The measured file set was the complete C source tree at the time of the
conversion:

* ``src/*.c``: ten translation units;
* ``src/core.h``: the shared header;
* the standalone C installer, subsequently replaced by ``install_uninstall.py``.

The ``Makefile`` and the ``docs/`` directory are excluded. The comparison is
between the pre-conversion backup and the current tree, using the same file
names and the same compiler flags. Every line is a physical line as reported
by ``wc -l``; no attempt is made to count logical statements, tokens or
generated instructions.

The measurement is reproducible with three operations: count the lines with
``wc -l`` over the file set, count the empty lines with ``grep -c '^$'`` over
the same set, and measure the byte count with ``wc -c``. The build check is a
clean ``make`` with ``-std=c17 -O3 -Wall -Wextra``.

A line-count metric is easy to game. The project therefore reports the raw
before and after values, the blank-line split and the per-file distribution.
A single aggregate number would hide whether the reduction came from one
large file or from the whole tree. The per-file table below shows that every
file became smaller and that no file was inflated to compensate.


Headline Result
---------------

.. list-table:: Source reduction
   :header-rows: 1
   :widths: 34 18 18 30

   * - Metric
     - Before
     - After
     - Change
   * - Physical lines
     - 8,272
     - 7,231
     - -1,041 (-12.6%)
   * - Blank lines
     - 743
     - 0
     - -743
   * - Non-blank lines
     - 7,529
     - 7,231
     - -298 (-4.0%)
   * - Source bytes
     - 197,377
     - 166,349
     - -31,028 (-15.7%)
   * - Average bytes per line
     - 23.86
     - 23.00
     - -0.86 (-3.6%)
   * - Longest line
     - 101
     - 100
     - -1
   * - Lines over 100 columns
     - 1
     - 0
     - -1

The blank-line result is exact: the pre-conversion tree contained 743 empty
lines; the current tree contains none. Those 743 lines account for 71.4% of
the total reduction. The remaining 298 lines are a net figure that includes
brace reflow, declaration merging, guard-clause extraction, helper
introduction and comment consolidation. It is not a count of deleted logic.
No error check, allocation or cleanup path was removed to reach the number.

The byte reduction is larger than the line reduction because the old layout
also carried leading indentation on many lines. Removing that indentation
reduces bytes without changing the number of lines. The two figures answer
different questions: lines measure vertical review surface, bytes measure
the size of the source text. Neither measures the size of the binary.


Per-File Distribution
---------------------

.. list-table:: Lines by file
   :header-rows: 1
   :widths: 28 12 12 12 12 24

   * - File
     - Before
     - After
     - Removed
     - Change
     - Note
   * - ``src/blake2b.c``
     - 393
     - 331
     - 62
     - -15.8%
     - Hash core; dense already.
   * - ``src/bridge.c``
     - 301
     - 268
     - 33
     - -11.0%
     - Patch writer.
   * - ``src/chunking.c``
     - 383
     - 337
     - 46
     - -12.0%
     - Boundary and store.
   * - ``src/cli.c``
     - 1,433
     - 1,271
     - 162
     - -11.3%
     - Largest translation unit.
   * - ``src/conflict.c``
     - 544
     - 470
     - 74
     - -13.6%
     - Conflict index.
   * - ``src/env.c``
     - 488
     - 431
     - 57
     - -11.7%
     - Shell and IDE setup.
   * - ``src/graph.c``
     - 1,255
     - 1,100
     - 155
     - -12.4%
     - History and manifests.
   * - ``src/main.c``
     - 21
     - 16
     - 5
     - -23.8%
     - Entry point; largest relative drop.
   * - ``src/monitor.c``
     - 335
     - 298
     - 37
     - -11.0%
     - Status writer.
   * - ``src/network.c``
     - 729
     - 648
     - 81
     - -11.1%
     - libcurl engine.
   * - ``src/core.h``
     - 876
     - 735
     - 141
     - -16.1%
     - Inline helpers and contracts.
   * - Former C installer
     - 1,514
     - 1,326
     - 188
     - -12.4%
     - Standalone C installer; now ``install_uninstall.py``.
   * - **Total**
     - **8,272**
     - **7,231**
     - **1,041**
     - **-12.6%**
     - All measured files.

The distribution matters. The largest absolute reduction was in the former C
installer at 188 lines, followed by ``cli.c`` at 162 and ``graph.c`` at 155. The largest relative reduction is ``main.c`` at 23.8%, but that file is
only 21 lines before the change; a five-line difference is meaningful for a
demonstration but not for the aggregate. ``core.h`` lost 16.1%, which is
expected: it contains many small inline helpers whose one-line bodies and
brace layout were previously spread across several lines each.

No file grew. That is a useful sanity check against a reformat that simply
moves text from one file to another or expands a header to compensate for a
smaller implementation file. The reduction was distributed across the tree,
including the C installer, which was a separate translation unit and did not
share ``core.h``. That installer has since been replaced by ``install_uninstall.py``.


What Was Actually Removed
-------------------------

The reduction is easier to trust when it is classified. The categories below
are descriptive, not a line-by-line attribution; a single line can move
between categories when a function is reflowed.

.. list-table:: Composition of the reduction
   :header-rows: 1
   :widths: 30 18 52

   * - Category
     - Lines
     - Effect
   * - Blank lines
     - 743
     - Removed entirely; comments now mark phases.
   * - Brace and indentation reflow
     - ~180
     - A function brace, nested block or else branch occupies fewer rows.
   * - Declaration and expression merging
     - ~70
     - A declaration or guard that was split for the old width fits one line.
   * - Guard-clause extraction
     - ~48
     - A nested success path becomes an early return or a small helper.
   * - Net total
     - 1,041
     - The sum of the categories above, not a separate measurement.

The approximate figures for the non-blank categories are estimates from the
review, not machine-classified counts; the blank-line figure and the total
are exact. Comment consolidation and small reflow changes are folded into the
brace and declaration estimates, so the category split is indicative rather
than additive. The project reports the categories separately so that a reader
does not mistake "1,041 lines of blank space" for the truth. Blank lines are
the largest component, but they are not the whole story.

The guard-clause extraction is the part that could not have been produced by
a formatter. A formatter can remove a blank line; it cannot decide that a
function should return early instead of carrying a nested ``else``. That is a
semantic review decision, and it is why the rewrite was done function by
function. The rule behind it is in :doc:`complexity_guardrails`.


Build and Behaviour
-------------------

The conversion was accepted only after the tree compiled cleanly. The build
used the project's normal flags, ``-std=c17 -O3 -Wall -Wextra``, and produced
no warnings. The rewritten binary completed the command-level smoke tests
used by the project. The Makefile was not modified to accommodate the
layout.

Those checks are evidence, not proof. A clean build says that the tokens are
valid C and that the compiler did not detect a type or control-flow mistake;
it does not prove that every runtime path is correct. The guards and the
ownership rules were reviewed separately, and the test scenarios in
:doc:`contributing` remain the acceptance criteria for behavioural changes.
The claim in this document is deliberately narrow: the layout change did not
break the build or the smoke paths, and the structural refactors were
reviewed rather than assumed harmless.

No runtime performance claim is made from the line count. A shorter source
file does not imply a faster binary, and a longer one does not imply a slower
binary. The only defensible runtime statement is the one established by the
workloads in :doc:`benchmarks`: the program's time and memory behaviour must
be measured, not inferred from formatting.


Interpreting the Number
-----------------------

Three cautions apply to any line-count result.

**Physical lines are not logical complexity.**
A 30-line function with one branch can be simpler than a 10-line function
with four nested ternaries. The metric measures the rows a reviewer
traverses, not the number of states the reviewer must hold. The nesting
ceiling in :doc:`complexity_guardrails` is the complementary metric.

**Blank lines are not always waste.**
In prose, a blank line separates ideas. In code, the Alves style uses a
comment for that purpose because a comment carries information. A project
with a different comment discipline might reasonably keep a small number of
blank lines. The 743-to-zero result is specific to this style, not a
universal law.

**The denominator matters.**
A 12.6% reduction on an 8,272-line tree is 1,041 lines. The same percentage
on a 100-line file is 13 lines, which may be noise. The per-file table is
provided so that the aggregate is not used to hide a local anomaly. In this
case every file decreased, so the aggregate is representative.

The number should be read as a review-surface measurement. It says that the
same behavioural contract now occupies 12.6% fewer rows. It does not say that
the program is 12.6% faster, 12.6% safer or 12.6% better. Those claims need
their own evidence.


Historical Record
-----------------

.. list-table:: Conversion record
   :header-rows: 1
   :widths: 30 70

   * - Item
     - Value
   * - Baseline
     - Pre-conversion C tree, 8,272 physical lines.
   * - Result
     - Alves-style C tree, 7,231 physical lines.
   * - Reduction
     - 1,041 lines, 12.6%.
   * - Blank lines
     - 743 before, 0 after.
   * - Build
     - ``-std=c17 -O3 -Wall -Wextra``; zero warnings.
   * - Runtime checks
     - Command-level smoke tests passed.
   * - Scope
     - Historical C tree: ``src/*.c``, ``src/core.h`` and the former installer.
   * - Excluded
     - ``Makefile`` and ``docs/``.
   * - Source of baseline
     - Pre-conversion backup of the same file set.
   * - Reproducibility
     - ``wc -l``, ``wc -c`` and a clean ``make``.

The record is intentionally boring. A style change should not need a
narrative of heroism; it should need a baseline, a result, a build and a
review. The interesting part is not the number but the fact that the number
was obtained without deleting checks, hiding failures or changing the public
contract. The next document, :doc:`ssh_ergonomics`, explains why those 1,041
rows matter to a developer working through a terminal.

.. seealso::
   For the current implementation boundaries and safe operating mitigations, see
   :doc:`limitations`.
