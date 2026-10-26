SSH Ergonomics
==============

Mesh is often read and patched on a remote machine. The developer may be
connected through SSH to a build server, a container, a virtual machine or a
machine in another room. The terminal may be a tmux pane, a Vim split, an
Emacs frame or a plain 80-by-24 xterm. There may be no graphical file
manager, no minimap and no second monitor. The Alves style is designed for
that environment as much as for a desktop editor.

This document is a practical guide to reading and editing the Mesh source
over a text-only connection. It explains the horizontal and vertical budgets
that the style protects, gives editor-specific advice for Vim, Neovim and
Emacs, and lists the settings that silently destroy the layout. The rationale
for the style is in :doc:`alves_style_manifesto`; the control-flow rule is in
:doc:`complexity_guardrails`.

.. contents::
   :depth: 2
   :local:


The Terminal Budget
-------------------

A terminal is a grid. The common defaults are 80 columns by 24 rows, and
tmux users often split that into two panes of 40 columns each. A laptop
terminal may be 120 columns; a phone SSH client may be narrower. The source
cannot assume the wide case.

The left margin is the first budget. In a kernel-style file, a statement at
the fourth nesting level may begin 32 columns from the left edge. On an
80-column terminal, that leaves 48 columns for the expression, the comment
and the line number gutter. If the editor also shows a file tree or a Git
blame column, the usable width is smaller still. The Alves style spends zero
columns on indentation, so the expression gets the full width.

The vertical budget is the second. A 24-row terminal with a two-row status
line, a one-row command line and a three-row tmux bar has about 18 rows for
source. A function with a blank line every three statements spends a
noticeable fraction of those rows on nothing. The style removes decorative
blank lines, so a 40-line function stays 40 lines instead of becoming 50.

The third budget is attention. A reader who is scanning for an error path
should not have to reconstruct a pyramid of nested conditions. Guard clauses
put the rejection next to the operation, and the three-level ceiling keeps
the success path short. This is an ergonomic property even on a large screen;
on a small one it is decisive.


Reading at Column Zero
----------------------

Column-zero code changes how the eye moves. In an indented file, the eye
tracks a staircase: each level shifts right, and the reader uses the shift to
infer the block structure. In an Alves file, the eye tracks braces and
keywords. The structure is explicit rather than inferred from whitespace.

This has three practical consequences.

**Search anchors are stable.**
A search for ``^int`` or ``^static int`` finds function definitions without
matching indented declarations. A search for ``^if(`` finds top-level guards.
A search for ``^} else {`` finds the continuation pattern. The anchors do not
depend on how deeply a function is nested.

**Diffs are smaller.**
Moving a block does not reindent every line. A patch that extracts a helper
changes the lines that actually move, not a wall of leading tabs. A reviewer
on a narrow terminal sees the logic change instead of the whitespace change.

**Folds are explicit.**
Editors that fold by indentation have nothing to fold in an Alves file. That
is not a defect; it is a signal to use brace-aware or syntax-aware folding.
The next sections give the settings.

The style does not make code invisible. It makes the structure depend on
tokens that survive copy-paste, email, terminal wrapping and a `TERM=dumb`
session. Color and indentation are useful, but they are not the only carriers
of meaning.


A Compact Signature
-------------------

The function signature is the first thing a reader sees. The style keeps it
horizontal when possible and breaks it at a semantic boundary when not. The
following is the actual shape of ``chunk_work_tree()``: the return type and
name stay on the first line, the second parameter continues at column zero,
and the opening brace closes the signature on the second line.

.. code-block:: c

   static int chunk_work_tree(const struct mesh_repo*repo,
   struct mesh_file_list*list);

The signature is kept on two lines at a semantic boundary: the repository
argument on the first, the file list on the second. The continuation is not
indented under the opening parenthesis; that would spend columns to align
with a punctuation mark. It starts at column zero, like every other line. A
reader scanning the file sees the function name at the left edge and the
second parameter immediately below it. In the definition, the opening brace
follows the second line; the sample shows the signature without the body.

The same pattern applies to calls with many arguments: break after a comma,
keep the continuation at column zero, and do not align under the previous
argument. Alignment is visually pleasing on a wide screen and expensive on a
narrow one. The style chooses the narrow case.


Vim and Neovim
--------------

Vim is the default terminal editor for many Mesh contributors. The following
settings make the dense layout easier to read without changing the file.

.. list-table:: Vim and Neovim settings
   :header-rows: 1
   :widths: 30 70

   * - Setting
     - Effect
   * - ``set number``
     - Absolute line numbers for patch review and log references.
   * - ``set relativenumber``
     - Relative jumps without leaving the absolute number behind.
   * - ``set scrolloff=5``
     - Keeps context above and below the cursor on a short terminal.
   * - ``set cursorline``
     - Marks the current row in a file with no indentation cues.
   * - ``set colorcolumn=100``
     - Shows the style's hard ceiling without wrapping the line.
   * - ``set foldmethod=syntax``
     - Folds by braces and syntax, not by indentation.
   * - ``set foldlevelstart=99``
     - Starts unfolded; fold only when a function is too tall.

The critical setting is ``foldmethod``. The default ``indent`` method assumes
that nesting is expressed by leading whitespace. In an Alves file it is not,
so an indent fold may collapse an entire file into one line or refuse to fold
at all. Use ``syntax``, ``expr`` or manual folds. If the syntax file is
unavailable, ``set foldmethod=marker`` with explicit markers is preferable to
indentation.

Do not run ``gg=G`` or any auto-indent command on a Mesh source file. Vim's
``=`` operator will reindent the entire buffer according to its own rules and
destroy the layout. If a file has been accidentally reindented, the fix is a
reviewed patch, not another reindent. The same warning applies to
``:autocmd FileType c setlocal indentexpr=...`` rules that run on paste.

For navigation, ``%`` jumps between matching braces, ``[[`` and ``]]`` move
between functions in many C files, and ``gd`` follows a local declaration.
The style keeps braces at column zero, so ``%`` and brace-based motions are
reliable even when the cursor is at the start of a line. Search with
``/\<chunk_work_tree\>`` rather than a partial name; the dense file has many
short identifiers.

If the terminal is only 80 columns, consider disabling the line-number gutter
or using a narrow relative-number column. The gutter is useful, but it is
also 4 to 6 columns that could show the right operand of a condition. A
``:set numberwidth=3`` is often enough for a file under 1,000 lines.


Emacs
-----

Emacs users have equivalent tools. The goal is the same: preserve the
column-zero layout, use brace-aware structure and avoid modes that silently
reindent.

.. list-table:: Emacs settings
   :header-rows: 1
   :widths: 30 70

   * - Setting or mode
     - Effect
   * - ``global-display-line-numbers-mode``
     - Line numbers for patches and logs.
   * - ``column-number-mode``
     - Shows the current column; useful for the 100-column ceiling.
   * - ``hl-line-mode``
     - Highlights the current row.
   * - ``which-function-mode``
     - Shows the enclosing function in the mode line.
   * - ``hs-minor-mode``
     - Hides and shows brace-delimited blocks.
   * - ``electric-pair-mode``
     - Balances braces without changing layout.
   * - ``prog-mode`` hooks
     - Keep the C indentation engine from rewriting the buffer.

The default ``c-mode`` indentation engine expects to indent lines after
braces. It is useful for a kernel-style file and counterproductive for an
Alves file. Do not enable ``aggressive-indent-mode`` or a save hook that runs
``indent-region``. If the indentation engine is active, set
``indent-tabs-mode`` to ``nil`` and do not call ``indent-region``; better
still, use a mode that leaves the layout alone.

``hs-minor-mode`` hides a block between braces, which matches the style's
structure. ``forward-sexp`` moves over a balanced brace expression, and
``beginning-of-defun`` usually finds a function by its opening brace. Those
motions are more reliable than indentation-based navigation.

For a remote session, ``M-x tramp`` can edit a file over SSH, but a local
terminal editor or a tmux session on the remote host is usually faster. If
Tramp is used, disable modes that scan the whole tree on save; a remote
filesystem makes every extra pass visible.


tmux and the SSH Session
------------------------

A persistent tmux session is the simplest way to survive a dropped SSH
connection. It also makes the viewport budget explicit.

.. list-table:: Session practices
   :header-rows: 1
   :widths: 32 68

   * - Practice
     - Reason
   * - ``tmux new -A -s mesh``
     - Attach to an existing session or create one; work survives disconnects.
   * - Two panes, not four
     - A 40-column pane is below the comfortable width for C.
   * - ``set -g status-interval 5``
     - Keeps the status bar cheap on a slow link.
   * - ``set -g history-limit 50000``
     - Enough scrollback for a build log without unbounded memory.
   * - ``TERM=xterm-256color``
     - Syntax highlighting works; the layout still survives without it.
   * - ``LANG=C.UTF-8``
     - Consistent width for the ASCII source and UTF-8 documentation.

The source is ASCII C, so a UTF-8 locale is not required for correctness.
It is useful for the documentation, commit messages and log output. If the
terminal is set to a non-UTF-8 locale, the RST files may render replacement
characters; the C files are unaffected.

Do not use a proportional font for code review. A monospaced font is not an
aesthetic preference; it is what makes column alignment and the 100-column
ceiling meaningful. In a browser-based terminal, disable ligatures that merge
``!=``, ``==`` or ``->`` into a single glyph if they make a condition harder
to read.


The 100-Column Ceiling
----------------------

The style allows up to 100 columns per line. The current source has no line
longer than 100, and only one line in the pre-conversion tree exceeded that
limit. The ceiling is a compromise: 80 columns is the traditional terminal
width, but modern identifiers, format strings and function signatures often
need a little more room. A hard 100-column limit keeps a line visible on a
120-column terminal with a gutter and a split pane.

The ceiling is not an invitation to fill every line to 100 columns. A short
line is easier to scan than a long one. Break at a comma, a boolean operator
or a scope boundary; do not break in the middle of an identifier or between a
function name and its opening parenthesis. Never break a line in a way that
hides an error check or separates a condition from its body.

Measure a file with ``awk '{ if (length > 100) print FNR }' file.c`` or
``wc -L``. The first gives line numbers; the second gives the longest line.
A patch that introduces a line over 100 columns should explain why the break
would be worse than the length.


Color, Contrast and Monochrome
------------------------------

Syntax highlighting is useful, but it is not part of the contract. A
terminal may be monochrome, a log may strip ANSI codes, a diff may be pasted
into an email and a screen reader may ignore color entirely. The layout must
carry the structure without color.

The Alves style helps here because braces and keywords are visible in plain
text. A reader can distinguish a guard clause from a nested block without
relying on a theme. The style does not require a particular palette, and the
project does not ship one. Use a theme with sufficient contrast, avoid
low-contrast comments and do not encode meaning only in color.

For accessibility, density is a trade-off. A reader with low vision may need
larger text and fewer columns; a screen reader may prefer one statement per
line. The style keeps one statement per line and avoids horizontal
alignment, which helps both cases. It does not remove the need for zoom,
wrapping or a personal editor configuration. The project's layout is a
default, not a medical prescription.


Practical Checklist
-------------------

Before a remote review session:

1. Use a monospaced font and a terminal of at least 80 columns.
2. Disable indentation-based folding and auto-indent on C files.
3. Set a visual column at 100 if the terminal is wide enough.
4. Keep one tmux pane for the editor and one for the build or test command.
5. Search from column-zero anchors: ``^static``, ``^int``, ``^if(``.
6. Use brace motions, not indentation motions, to move between blocks.
7. Check a diff with ``diff -u`` or the project's patch output, not a
   reindent tool.
8. If a file looks wrong, do not "fix" it with ``gg=G`` or
   ``indent-region``; inspect the patch.

The style is designed to make those practices sufficient. It does not
require a plugin, a language server or a graphical IDE. A text grid, a
monospaced font and a brace-aware editor are enough.

The next document, :doc:`onboarding_and_veterans`, explains how a team adopts
the style without turning editor settings into a loyalty test.

.. seealso::
   For the current implementation boundaries and safe operating mitigations, see
   :doc:`limitations`.
