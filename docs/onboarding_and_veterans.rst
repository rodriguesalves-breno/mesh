Onboarding and Veterans
=======================

The Alves style is the house layout of Mesh, but its adoption is deliberately
optional for individuals. That sentence sounds contradictory until the two
audiences are separated. The trunk needs one layout at the merge boundary. A
developer needs room to change habits without being treated as a
correctness problem. The governance decision is to enforce the first and
protect the second.

This document explains why the project chose optional adoption, how a new
contributor can learn the style without a rewrite, and how experienced
developers who have internalized another C tradition can migrate on their own
schedule. It is a document about people and review process, not about
braces. The technical argument is in :doc:`alves_style_manifesto` and
:doc:`complexity_guardrails`.

.. contents::
   :depth: 2
   :local:


The Governance Decision
-----------------------

The project distinguishes three things that are often conflated.

**The trunk style.**
Code merged into the main tree follows the Alves layout. This is not
negotiable, because a mixed layout at the merge boundary makes every future
diff harder to read. The rule applies to new files and to the lines a patch
actually changes.

**The personal style.**
A contributor may keep another layout in a private branch, a downstream
fork, a scratch experiment or a different project. The project does not audit
private buffers. The style is an interface for collaboration, not a
membership test.

**The migration path.**
A contributor who has written C in the kernel style for twenty years is not
required to reformat their entire mental model before their first patch. They
are required to follow the trunk style for the code they submit. The
conversion of that code can be incremental, reviewed and limited to the
lines that changed.

This is the compromise: one style at the boundary, no forced conversion
inside the person. It is stricter than "everyone does their own thing" and
gentler than "reformat your habits or leave." It treats style as
infrastructure, not identity.

The style is not a beginner-only dialect. It applies to the nine-line entry
point and to the largest command in the tree. A larger function is simply
more likely to need extraction; the rules do not change with the author's
seniority.


Why Optional Adoption Works
---------------------------

A style can be imposed by a formatter, a linter or a maintainer with commit
access. It cannot be imposed as understanding. If a veteran writes Alves-style
code because a rule says so, but believes the layout is unreadable, the code
will drift at the first difficult review. The next patch will reintroduce a
blank line "for clarity," then an indent "for this one nested case," then a
function brace on its own line "because it is easier to see." The rule loses
by erosion.

Optional adoption attacks the belief rather than the formatting. The veteran
is invited to observe three things:

* the same build, with no warnings and the same command-level behaviour;
* a smaller diff, because moving a block does not reindent every line;
* a shorter review, because the error path is visible at column zero.

Those observations are more persuasive than a document. The style becomes
durable when a senior developer notices that a patch took less time to review,
not when a policy says that it should.

Optional adoption also protects the project from a predictable failure mode:
the style war. A mandatory conversion of every contributor's habits produces
long arguments about tabs, braces and line length, and those arguments
consume the attention that should go to correctness. The project has a
technical case; it does not need a culture war to win it. The trunk rule is
enough to keep the code consistent, and the optional path gives veterans time
to see the benefit.


What Veterans Are Asked to Unlearn
----------------------------------

The phrase "unlearn" is uncomfortable because experienced developers are
proud of their habits, and often rightly so. The habits were learned in a
different context and rewarded there. The kernel style is not a mistake; it
is a solution to a different set of constraints. A veteran is not being asked
to admit that twenty years of work was wrong. The veteran is being asked to
separate the engineering content from the institutional packaging.

The specific habits that conflict with the Alves style are:

* **Indentation as structure.** In the kernel style, the leading tabs encode
  the block. In the Alves style, braces do. The veteran has to stop reading
  the staircase and start reading the braces.
* **Blank lines as punctuation.** A blank line between phases feels natural
  after years of reading code that uses it. The Alves style replaces it with
  a comment, which carries information the blank line does not.
* **The function brace on its own line.** The Allman brace is visually
  comfortable on a wide screen. On a 24-row terminal it costs a row per
  function and separates the signature from its body.
* **The 80-column reflex.** A hard 80-column rule is a reasonable default
  for a large, old tree. A small tree with long identifiers can use 100
  columns and still fit a modern terminal.
* **The idea that density is showing off.** A dense function can look
  aggressive to a reader who expects whitespace. The style's answer is not
  "get used to it"; it is "read the guard clause and tell me whether the
  error path is clearer."

None of those habits is a character flaw. They are defaults that become
visible when the environment changes. The onboarding process should name them
without mocking them. A veteran who feels mocked will defend the habit; a
veteran who feels respected will test the alternative.


The Maturity Curve
------------------

The project has observed a predictable sequence in contributors who adopt
the style voluntarily. It is not a formal assessment, and no one is graded on
it. It is a description of the questions people ask, in order.

.. list-table:: Stages of adoption
   :header-rows: 1
   :widths: 16 44 40

   * - Stage
     - Typical question
     - What usually changes
   * - Stage 1: Resistance
     - "Why would I remove indentation? It is how I read code."
     - The contributor keeps the old layout and watches a review.
   * - Stage 2: Comparison
     - "The diff is smaller, but is the function actually clearer?"
     - The contributor reads a guard-clause function beside a nested one.
   * - Stage 3: Experiment
     - "I will try this one helper in the new style."
     - The contributor writes a small patch and gets review feedback.
   * - Stage 4: Fluency
     - "I can see the error path without scrolling."
     - The contributor stops translating and starts reading.
   * - Stage 5: Advocacy
     - "This would be easier if the old function were flattened."
     - The contributor proposes a refactor, not a reformat.

The curve is not linear. A contributor may accept the layout for new code and
still prefer the old layout for a legacy file. That is acceptable. The trunk
rule applies to the lines being changed, not to the entire history of the
file. A patch that touches one function should not reformat the other
forty.

The most important stage is the second. A style argument conducted in the
abstract is a matter of taste. A side-by-side comparison of a nested function
and its guard-clause equivalent is a matter of evidence. The project should
make that comparison easy: point to ``chunk_work_tree()`` and
``graph_commit()``, show the diff, and let the reader count the rows.


Onboarding a New Contributor
----------------------------

A new contributor does not need a lecture on aesthetics. The onboarding path
is a sequence of small, concrete tasks.

**Read the smallest file first.**
``src/main.c`` is the complete process lifetime in a few lines. It shows the
style without the noise of a larger module. Ask the contributor to explain
what the process does and where it can fail.

**Read one guard-clause function.**
``chunk_work_tree()`` is a loop with several failure modes. Ask the
contributor to list every early return and what it owns. This teaches both
the style and the ownership rules.

**Read one long linear function.**
``graph_commit()`` shows that density is not the same as shortness. The
function is long, but its control flow is flat. Ask the contributor to find
the point at which the history line becomes durable.

**Write a small helper.**
A new contributor should write a static helper with one job, one return path
and no more than three levels. The helper should have a name that a reviewer
can understand without reading the body. Review the name before reviewing the
formatting.

**Review a patch.**
Ask the contributor to review a small diff and state whether the error path
is local. The review checklist in :doc:`complexity_guardrails` is the
reference. The point is not to memorize the rules; it is to practice reading
for structure.

**Measure one file.**
Ask the contributor to count the lines before and after a change, not as a
target but as an observation. A patch that adds a necessary line is fine. A
patch that removes a blank line and calls it optimization is not. The
measurement habit is more valuable than the number.

The onboarding path should not begin with a mass reformat. A new contributor
who reformats a file creates a diff that no one can review and learns the
wrong lesson: that style is a global operation rather than a local discipline.


Handling Resistance
-------------------

Resistance is normal. It usually appears in one of four forms, and each has a
different response.

**"This is unreadable."**
Ask which function is unreadable and read it together. The answer may be a
specific construct, not the whole style. A long condition, a poor name or a
missing comment can make a dense function hard to read; the fix may be a
helper, not a return to indentation.

**"This is just code golf."**
Agree that code golf is bad, then show the rule that prevents it: no merged
statements, no cryptic names, no removed checks, no line-count target. The
style removes whitespace, not meaning. A dense function that is correct is
not golf; a short function that hides a bug is.

**"I have always done it this way."**
Respect the experience and separate it from the decision. The veteran's
habits were learned in a context where they worked. The question is whether
they work for this tree, with this review surface, on this terminal. The
answer can be tested without insulting the past.

**"The build does not care."**
Correct. The build does not care about layout, and the style does not claim
that it does. The build cares about tokens and types. The style cares about
the reader who has to verify those tokens. A clean build is necessary and
insufficient; the review is the other half.

A maintainer should not win these arguments by authority. The style has a
measurable result, a review rule and a body of code that demonstrates it. Use
those. If a contributor still disagrees, the trunk rule applies to the code
they merge, and the disagreement can remain a disagreement. The project does
not need unanimity; it needs a consistent boundary and a respectful process.


The Cost of a Mixed Tree
------------------------

Optional adoption does not mean a permanently mixed tree. The trunk converges
because every patch brings the lines it touches into the house style. The
convergence is slow, local and reviewable. The alternative, a global reformat
by a new contributor, is faster and much worse: it destroys blame, hides
behavioural changes and forces reviewers to read whitespace instead of logic.

The project therefore accepts a temporary mixed state inside a file, but not
inside a patch. A patch should not mix a behavioural change with a reformat
of untouched lines. If a file needs a larger conversion, it is a separate
change with its own review, its own tests and its own commit message. The
rule is the same one stated in :doc:`contributing`: one problem per patch.

This is also why the original conversion was done as a single reviewed pass
rather than as a background formatter. The conversion touched every file, so
it needed a baseline, a build, a smoke test and a per-file record. The result
is in :doc:`metrics_and_line_optimization`. A future conversion of a
downstream fork can follow the same pattern, but it should not be smuggled
into a bug fix.


Metrics Without Weapons
-----------------------

The 1,041-line reduction is useful evidence, and it is also easy to abuse. A
maintainer who says "this patch adds twelve lines, therefore it is bad" has
misunderstood the metric. A necessary error check, a clearer name or a
comment that records a protocol constraint can add lines and improve the
code.

The metric should be used in three ways only:

* as a baseline for the conversion, to show that the change was real;
* as a review signal, when a patch adds a large amount of decorative
  whitespace;
* as a teaching example, when a contributor asks why the style is worth the
  effort.

It should not be used as a quota, a performance claim or a way to embarrass a
contributor. The style is successful when the code is easier to review, not
when the line count is lower. A patch that removes a blank line but leaves a
nested success pyramid has missed the point.

The same caution applies to the build. "Zero warnings" is a quality gate, not
a proof of correctness. The style document should say what the build proves
and what it does not. The project's credibility depends on making claims that
survive scrutiny.


A Practical Policy
------------------

The governance rules can be stated on one page.

1. **New files follow the Alves style.** No exceptions for "small" files;
   small files are where the style is easiest to learn.
2. **Modified lines follow the Alves style.** A patch brings the lines it
   touches into the house layout, even if the rest of the file is older.
3. **Untouched lines are not reformatted.** A drive-by reformat is a separate
   change, reviewed on its own.
4. **No global reformat without a baseline.** A conversion needs a before and
   after count, a clean build and a smoke test.
5. **Private branches are private.** The project does not audit a
   contributor's editor buffer or downstream fork.
6. **Review correctness before style.** A style comment should not arrive
   before a question about the error path or the ownership rule.
7. **Style disagreements are resolved with evidence.** Point to a function,
   a diff, a terminal width or a measurement. Do not appeal to seniority.
8. **The trunk is the boundary.** A contributor may disagree with the style
   and still contribute; the merged code follows the house layout.

Those rules are deliberately modest. They do not require a style committee,
a linter or a signed pledge. They require a maintainer who applies them
consistently and a contributor who is willing to test the alternative on one
function.

The deepest argument for optional adoption is not kindness. It is
sustainability. A style that depends on enforcement will be abandoned when
the enforcer changes roles. A style that a team has experienced as useful
will survive because the team keeps choosing it. The Alves style is offered
as a better interface for review, not as a test of loyalty. The code is the
argument; the team decides when to accept it.

.. seealso::
   For the current implementation boundaries and safe operating mitigations, see
   :doc:`limitations`.
