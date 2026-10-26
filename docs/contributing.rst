Contributing
============

Mesh is maintained in the spirit of small, auditable C: small patches,
explicit reasoning, no hidden machinery and no feature without a measured
problem. The source follows the Alves style (:doc:`alves_style_manifesto`),
not the Linux kernel layout. This document is the contract for contributors.
It is intentionally stricter than a typical application project because the
code touches user files and runs on the critical path of a developer's day.

.. contents::
   :depth: 2
   :local:


Before You Write Code
---------------------

Open a discussion or a bug report first when the change is larger than a
bug fix. State:

* the problem in one paragraph, with a reproduction;
* the observed behaviour and the expected behaviour;
* the smallest change you believe fixes it;
* any alternative you rejected and why.

A patch that solves a problem nobody has is harder to review than a patch that
solves a reported one. "It would be cleaner" is not a problem statement. "A
10 MiB edit in a 100 MiB file re-uploads all chunks" is.

Do not add a dependency without a separate, explicit discussion. Mesh exists
partly to prove that a useful tool can fit in a small dependency graph. libcurl
and pthreads are the baseline. A JSON library, a logging framework or a custom
allocator needs a stronger argument than convenience.


Coding Style
------------

Mesh uses the Alves style. It is a style for C written to be read on a
terminal, reviewed in a patch and executed by a machine that does not care
about whitespace. The rules below are mandatory for new and modified code.
They are explained at length in :doc:`alves_style_manifesto` and enforced with
the nesting rule in :doc:`complexity_guardrails`.

**Zero left margin.**
Do not indent control flow or declarations. Every statement begins at column
zero. Braces carry the structure; the reader's eye does not have to count
leading spaces. This is the single most important rule, and it is the one
that saves horizontal space on an 80-column SSH terminal.

**No decorative blank lines.**
Do not insert blank lines inside a function or between functions. A comment
marks a phase. A blank line is not a paragraph break in code; it is a row
stolen from the next screen.

**Braces.**
The opening brace stays on the line that introduces the construct. The
closing brace stays on the line of the continuation:

.. code-block:: c

   int fd=open(path, O_RDONLY);
   if(fd<0)
   return -1;
   if(S_ISREG(st.st_mode)) {
   /* ... */
   } else {
   errno=EINVAL;
   return -1;
   }

Functions use the same rule: the opening brace is on the signature line.
There is no Allman brace and no orphan brace line.

**Compact syntax.**
Write ``if(condition)``, ``for(size_t i=0;i<n;i++)`` and
``while((n=read(fd,buf,sizeof(buf)))>0)``. Binary operators are written
without surrounding spaces when the expression remains unambiguous:
``off+MESH_CHUNK_SIZE``, ``list->n==list->cap``, ``p&&*p``. The comma keeps
one following space. The goal is not code golf; the goal is that a short
expression fits in one glance.

**Pointers and types.**
Bind the asterisk to the name or the type without a space: ``char*path``,
``struct mesh_chunk*v``, ``const char*message``. Use fixed-width types for
sizes, offsets and timestamps.

**Three-level ceiling.**
A function may nest control constructs at most three levels below its body.
If a fourth level appears, invert the condition, use a guard clause or
extract a helper. A successful path should read as a sequence of early exits
and one linear block. The detailed rule and the ``chunk_work_tree`` case are
in :doc:`complexity_guardrails`.

**Line length.**
The hard ceiling is 100 columns. The current source has no line above 100.
Break a signature or a condition at a semantic boundary; do not wrap a line
so that an error check disappears into a continuation.

**Comments.**
Use ``/* ... */`` with the asterisks at the left margin in file headers.
Explain why, not what. A comment that restates the next line is noise. A
comment that records a trade-off, a protocol constraint or a kernel behaviour
is valuable. Do not use comment banners to create visual separation; the code
should not need a banner to be scannable.

**Names.**
Lowercase with underscores. Public functions are prefixed by their module:
``graph_load``, ``manifest_save``, ``net_get``, ``conflict_choose``. Local
helpers are ``static`` and may use short names when their scope is obvious.
Structure names use ``struct mesh_*``.

**Declarations.**
Declare variables at the point where they are first needed when that shortens
the function. C17 permits it. Do not hide an uninitialized variable behind a
later branch.

**No dangerous string functions.**
``strcpy``, ``strcat``, ``sprintf`` and ``gets`` are forbidden. Use
``snprintf`` with an explicit size, ``memcpy`` with a known length, or the
bounded helpers in ``core.h``. Check every truncation. A silently truncated
path is a bug, not a convenience.

**No hidden allocation.**
A function that allocates says so in its name or its contract. Ownership is
documented next to the prototype in ``core.h``. Error paths free what they
allocated; success paths transfer ownership exactly once.

**No global mutable state.**
The hash pool has a mutex-protected work queue, but it lives on the stack of
the calling function. There is no global repository object, no global buffer and
no singleton connection. This keeps commands independent and makes tests
repeatable.

**Errors are values.**
Return -1 and set ``errno`` for system-level failures. Return 0 on success.
Use a ``done:`` label for cleanup when a function acquires more than one
resource. Save ``errno`` before cleanup that may overwrite it. Never print
inside a low-level helper unless the message is part of the user interface;
return the error and let the CLI decide how to phrase it.

**Types.**
Use ``uint64_t`` for byte counts and offsets, ``int64_t`` for timestamps,
``size_t`` for memory sizes and indices, and ``bool`` for predicates. Do not
use ``int`` for a file size. Include ``<inttypes.h>`` and use ``PRIu64`` /
``PRId64`` in format strings.

**Portability.**
Stick to POSIX and C17. Do not use GNU statement expressions, ``__attribute__``
unless guarded, or Linux-only syscalls without a fallback. The one deliberate
Linux dependency is ``/proc/self/exe`` in ``mesh_self_dir()``, and it already
has a ``getcwd()`` fallback.


What a Good Patch Looks Like
----------------------------

A patch should be reviewable in one sitting.

* **One problem per patch.** Do not mix a bug fix, a refactor and a new
  feature. Reviewers cannot reason about three changes at once.
* **Small diff.** If a patch touches more than a few hundred lines, explain
  why it cannot be split.
* **No drive-by reformatting.** Do not reindent a function you are not
  changing. It hides the real diff and makes ``git blame`` useless. The
  one-time Alves conversion is complete; a feature patch must not repeat it.
* **Tests or a reproduction.** A bug fix should include the command that
  failed before and succeeds after. A feature should include a minimal
  scenario that exercises it.
* **No dead code.** Remove the old path. Do not leave ``#if 0`` blocks or
  commented-out calls "for reference".
* **No speculative abstraction.** Add the third copy before you add the
  generic layer. Mesh prefers three short functions to one clever dispatcher.

Commit messages use a subsystem prefix and an imperative summary::

   subsystem: short imperative summary

   Longer explanation of the problem. State what the user observed, why the
   old code did it and why this change is the smallest correct fix. Mention
   any trade-off or limitation.

   Reproduction:
     mesh start
     dd if=/dev/urandom of=big.bin bs=1M count=60
     mesh save "big"
     # edit one byte at offset 20M and save again

   Signed-off-by: Your Name <you@example.com>

Use the imperative mood in the subject: "fix chunk boundary at EOF", not
"fixed" or "fixes". Keep the subject under 72 characters. The body is for
reasoning, not for a diary.


Testing Checklist
-----------------

Run the build with warnings enabled before sending a patch:

.. code-block:: sh

   make clean
   make CFLAGS="-std=c17 -O3 -Wall -Wextra"

The build must be warning-free. A warning that is "obviously harmless" is still
a warning; fix it or explain why the compiler is wrong.

Then exercise the affected path. The following scenarios cover most changes.

.. list-table::
   :header-rows: 1
   :widths: 34 66

   * - Area
     - Scenario
   * - Repository init
     - ``mesh start`` in an empty directory; run it twice and verify one PATH
       block per shell file.
   * - Save
     - Add, modify and delete files; save; verify counts, status and history
       length.
   * - Hashing
     - Compare ``mesh obtain --file`` output with an independent BLAKE2b tool
       for an empty file, a text file and a binary file.
   * - Chunking
     - Create a 60 MiB file, save, modify 1 KiB in the middle, save again and
       verify that only one new object appears.
   * - Resume
     - Interrupt a download, rerun it and verify the resumed offset and final
       hash.
   * - Range fallback
     - Use a server that ignores ``Range`` and verify that the client restarts
       instead of appending.
   * - Conflict
     - Sync two repositories with a divergent file; verify the working file is
       untouched and ``alpha``/``beta`` are staged; resolve both variants.
   * - Patch
     - Save a text change and apply ``.mesh/monitor/last.patch`` with
       ``git apply`` in a clean Git repository.
   * - Auth
     - Store a wildcard token and a host-scoped token, inspect the mode of
       ``.mesh/credentials``, confirm the matching ``Authorization`` header on
       the wire and confirm that ``mesh obtain --file`` sends none.
   * - Failure
     - Run against an unreachable host, a 404 and a 500; verify exit status 1
       and a readable message.

For anything touching the object store or history, also test with a path that
contains spaces and a path near 4096 bytes. These are the inputs that expose
bounds and parsing mistakes.

Memory testing is expected for patches that add allocation or change ownership.
Run the affected command under Valgrind or an equivalent tool:

.. code-block:: sh

   valgrind --leak-check=full --error-exitcode=1 ./mesh save "test"

A leak in a short-lived command is still a defect. The process may be
short-lived, but the code is a library of habits.


Review Process
--------------

Review is about the code, not the author.

Reviewers check, in order:

1. **Correctness.** Does it solve the stated problem? Are the error paths
   complete? Can it write outside the repository? Can it overflow a buffer?
2. **Scope.** Is this the smallest change that solves the problem? Does it
   touch unrelated files?
3. **Style.** Zero left margin, no decorative blank lines, compact syntax,
   brace binding, names, comments, the 100-column limit and the three-level
   nesting ceiling. Style comments are not nitpicking; consistency is what
   lets a reader scan unfamiliar code on a narrow terminal.
4. **Tests.** Is there a reproduction? Does the test fail before the patch?
5. **Documentation.** Does a user-visible behaviour change require an update to
   ``docs/commands.rst``, ``docs/network.rst`` or ``docs/architecture.rst``?

Authors should expect questions rather than commands. A reviewer who says
"this looks racy" should be answered with a scenario, not with a defence. If
the reviewer is right, fix it. If the reviewer is wrong, explain the invariant
that makes the code safe, and add a comment so the next reader does not ask the
same question.

Do not force-push a patch series while review is active unless the maintainer
asks for it. Incremental fixes make the discussion traceable. Squash before
merge, not during review.

A patch is ready when:

* the build is warning-free;
* the relevant scenarios pass;
* new behaviour is documented;
* the commit message explains the problem;
* no unrelated file is modified.


Documentation Changes
---------------------

Documentation is part of the interface. Update it in the same patch as the
code when you change:

* a command name, flag or output line;
* the status JSON schema;
* the manifest or history format;
* the sync protocol;
* a compile-time limit or threshold.

Use ReStructuredText with the conventions already present in ``docs/``:
section titles underlined with ``=``, subsections with ``-``, deeper
subsections with ``~``, code blocks declared with ``.. code-block::`` and
tables written as ``list-table`` or plain grid tables. Keep lines readable;
RST does not require a fixed width, but a reviewer does.

The philosophical document ``docs/reason.rst`` is not marketing copy. It states
the design argument behind the implementation. If your change weakens or
strengthens that argument, say so in the commit message. Do not edit it to
match a feature that does not exist.

A change to the layout rules belongs in :doc:`alves_style_manifesto` or
:doc:`complexity_guardrails`, not in an isolated comment. A new function that
demonstrates a guard-clause refactor should be described in the former, and a
new measured line-count result belongs in
:doc:`metrics_and_line_optimization`. Documentation is not decoration; it is
the place where a style rule becomes reviewable.


Reporting Bugs
--------------

Include:

* the exact command line;
* the Mesh version (``mesh --version``);
* the operating system and file system;
* whether the repository is local or remote;
* the smallest input that reproduces the problem;
* relevant lines from ``.mesh/monitor/status.json`` (remove tokens and private
  paths if necessary);
* the exit status.

Do not paste an entire repository. A minimal reproduction is worth more than a
gigabyte of logs. If the bug involves a large file, state its size and the
offset of the edit; the chunk boundary is often the clue.

Security issues should be reported privately to the maintainer rather than in a
public issue. The threat model is documented in :doc:`network`; a report that
shows a remote peer escaping the repository root or exhausting memory beyond
the documented ceiling is a security bug, not a feature request.


Final Word
----------

Mesh is small because its contributors resist the urge to make it larger. The
best patch is often the one that deletes a branch, simplifies an error path or
replaces a clever abstraction with three obvious lines. Measure first, cut
second, document third. That order is not negotiable.

The Alves style is the visual form of the same discipline. It is not a
contest to write the fewest lines; it is a commitment that every line kept
must carry information, and every row removed must not hide a check. A dense
function that is wrong is still wrong. A dense function that is right is
easier to audit, easier to review and easier to keep right.

.. seealso::
   For the current implementation boundaries and safe operating mitigations, see
   :doc:`limitations`.
