Git Migration
=============

Mesh is not a Git replacement in the sense of speaking the Git wire protocol or
accepting every Git workflow. It is a smaller tool with a different centre of
gravity: one-step snapshots, content-addressed storage, chunk reuse and a
passive status file. This document maps the concepts, shows safe transition
recipes and states the gaps plainly.

.. contents::
   :depth: 2
   :local:


Conceptual Mapping
------------------

.. list-table::
   :header-rows: 1
   :widths: 24 26 50

   * - Git concept
     - Mesh equivalent
     - Notes
   * - Repository (``.git``)
     - Repository (``.mesh``)
     - Same idea: a hidden directory at the working-tree root.
   * - Working tree
     - Working tree
     - Scanned recursively; ``.mesh`` and ``.git`` are skipped.
   * - Index / staging area
     - None
     - ``mesh save`` records the working tree directly. There is no partial
       commit.
   * - Commit
     - Snapshot node
     - Stored as a line in ``.mesh/history`` plus a tree manifest.
   * - Commit hash
     - Node hash
     - BLAKE2b-512 over a canonical signature, 128 hex characters.
   * - Tree object
     - Manifest
     - A text file at ``.mesh/store/trees/<hash>``, not a binary object.
   * - Blob object
     - Content object
     - BLAKE2b-addressed, sharded by the first two hex characters.
   * - Packfile
     - None
     - Objects are individual files. The file system and page cache do the
       caching.
   * - Branch ref
     - ``.mesh/refs/<branch>``
     - The branch name comes from ``.mesh/config``.
   * - ``HEAD``
     - ``.mesh/HEAD``
     - Contains the current snapshot hash.
   * - Parent commit
     - ``parent_1``, ``parent_2``
     - The format supports two parents; the CLI creates single-parent
       snapshots.
   * - Remote
     - ``mesh sync URL``
     - A simple HTTP contract, not the Git protocol.
   * - ``.gitignore``
     - None
     - Every regular file is tracked except ``.mesh`` and ``.git``.
   * - Hooks
     - None
     - No pre-commit, post-checkout or server hooks.
   * - Submodules
     - None
     - Nested repositories are not modeled.
   * - Git LFS
     - Chunked large files
     - Files of 50 MiB or more are split into 10 MiB chunks automatically.
   * - ``git apply``
     - ``.mesh/monitor/last.patch``
     - Text-only, full replacement hunks, one patch per save.

The most important difference is the missing index. Git separates "what I
changed" from "what I want to record". Mesh records the whole tree. That makes
``mesh save`` simpler and faster to explain, but it removes partial commits,
interactive staging and the ability to commit a subset of a file. If your
workflow depends on those, keep Git for that repository.

Identity uses the Git key names ``user.name`` and ``user.email``. Configure the
global values once with ``mesh config --global`` and override them per
repository with ``mesh config``. The author is optional: an existing repository
without identity continues to save in the legacy format. When both keys are
present, new history lines and JSON metadata carry the name and e-mail, so a
Git-oriented import can map them to commit authorship. The address is validated
as an ordinary mailbox (one ``@`` with non-empty local and domain parts); Mesh
does not send e-mail and does not implement GPG signing.


Command Equivalence
-------------------

.. list-table::
   :header-rows: 1
   :widths: 30 30 40

   * - Git
     - Mesh
     - Difference
   * - ``git init``
     - ``mesh start``
     - Also wires PATH, the icon and the VS Code manifest.
   * - ``git config --global user.name`` / ``user.email``
     - ``mesh config --global user.name`` / ``user.email``
     - Same key names. Local values override global values and are used as the
       optional snapshot author.
   * - ``git add -A``
     - (none)
     - There is no staging step.
   * - ``git commit -am "msg"``
     - ``mesh save "msg"``
     - Scans, hashes, chunks, stores, writes the manifest and appends history
       in one command.
   * - ``git status``
     - ``mesh check``
     - Reports the last persisted state; it does not compute a live diff.
   * - ``git log``
     - ``mesh log``
     - Newest first. Prints the author line when the snapshot has identity.
   * - ``git show``
     - (read the manifest and objects)
     - No porcelain. The formats are readable text and hash-named files.
   * - ``git diff``
     - ``mesh diff [a [b]]``
     - Compares snapshots or a snapshot against the working tree. The exported
       patch still describes the last save.
   * - ``git checkout``
     - ``mesh restore <snap>``
     - Non-destructive by default; ``--force`` overwrites modified files.
   * - ``git branch``
     - (edit ``.mesh/config``)
     - No branch creation or switching command.
   * - ``git merge``
     - ``mesh sync`` + ``mesh choose``
     - Sync isolates collisions; the user chooses a pure variant. No automatic
       merge commit.
   * - ``git remote add``
     - (URL passed to ``mesh sync``)
     - No named remotes.
   * - ``git fetch``
     - ``mesh sync URL``
     - Sync pulls and pushes in one exchange.
   * - ``git pull``
     - ``mesh sync URL`` + ``mesh choose`` + ``mesh save``
     - Sync does not commit; save records the result.
   * - ``git push``
     - ``mesh send URL`` or ``mesh sync URL``
     - ``send`` posts metadata only; ``sync`` also uploads missing objects.
   * - ``git clone`` (forge URL)
     - ``mesh obtain URL [DIR]``
     - Imports the current tree through the forge archive endpoint. Git
       history and refs are not imported.
   * - ``git clone`` (Mesh peer)
     - ``mesh start`` + ``mesh sync URL``
     - Initialize an empty repository, then sync from the peer.
   * - ``git apply patch``
     - (Git command, not Mesh)
     - Mesh exports a patch; apply it with Git.
   * - ``curl URL -o file``
     - ``mesh obtain URL -o file``
     - Adds BLAKE2b reporting, resume and status integration. Common download
       flags such as ``-L``, ``-O``, ``-C -``, ``-C N``, ``-I``, ``-w``,
       ``-b``/``-c``, ``-x``, ``-H``, ``-A``, ``--compressed`` and
       ``--output-dir`` are accepted. ``-o -`` streams the body to stdout.
   * - ``wget -c URL``
     - ``mesh obtain URL``
     - Resume is automatic from the destination size.

A command that says "(none)" is not a bug report. It is a deliberate boundary.
Adding a half-compatible version of every Git porcelain command would turn Mesh
into a worse Git. The small set of commands is the product.


Code Style Migration
--------------------

A Git migration changes where history lives. It should not change how the
source is formatted. Mesh's C source was converted to the Alves style in a
single reviewed pass: zero left margin, no decorative blank lines, compact
control syntax, a three-level nesting ceiling and a 100-column limit. The
conversion reduced the source from 8,272 to 7,231 lines, removed 1,041 lines
or 12.6%, and kept the build warning-free.

.. list-table:: Layout conventions
   :header-rows: 1
   :widths: 22 39 39

   * - Concern
     - Kernel or Torvalds style
     - Alves style
   * - Indentation
     - Tabs, eight columns
     - None; statements start at column zero.
   * - Function braces
     - Allman: brace on its own line
     - Brace on the signature line.
   * - Conditional braces
     - Same line, but blank lines separate phases
     - Same line, with ``} else {`` binding.
   * - Control syntax
     - ``if (condition)``
     - ``if(condition)``.
   * - Blank lines
     - Paragraph separators inside functions
     - None; comments mark phases.
   * - Nesting
     - No hard cap; depth judged by review
     - Three levels below the function body.
   * - Line length
     - 80 columns recommended
     - 100 columns hard ceiling.
   * - Pointer syntax
     - ``char *p``
     - ``char*p``.

None of these conventions is part of the Mesh wire format, the object format,
the manifest format or the patch format. A unified diff produced by
``mesh save`` is still a unified diff; the layout of the C source that
generated it is irrelevant to ``git apply``. Do not use a migration as an
excuse to reformat a tree. The conversion is already done, and a drive-by
reformat hides the real change in a wall of whitespace.

If a team maintains a fork with kernel-style code, the compiler does not
care. New code in this repository follows the house style; preserving a local
layout in a downstream fork is a local decision. The one rule that should not
be relaxed is the separation of concerns: a behavioural patch and a layout
patch are reviewed separately, so that either can be reverted without the
other. See :doc:`alves_style_manifesto` and
:doc:`metrics_and_line_optimization`.


Storage Differences
-------------------

Hashing
~~~~~~~

Git uses SHA-1 by default, with SHA-256 repositories available. Mesh uses
BLAKE2b-512. Digests are therefore not interchangeable. A Mesh hash is 128 hex
characters; a Git SHA-1 is 40. You cannot copy an object between stores by
name.

BLAKE2b is fast in software, has no known practical collision attack and fits
the tool's bias toward modern CPUs. The trade-off is ecosystem compatibility:
no Git server understands a Mesh object name without a translation layer.

Objects
~~~~~~~

Git stores blobs, trees and commits as zlib-compressed loose objects or
packfiles. Mesh stores:

* raw content objects named by hash, one file each;
* text manifests named by the hash of their bytes;
* a line-oriented history file.

The Mesh format is intentionally inspectable. ``cat`` a manifest, ``xxd`` an
object, ``tail`` the history. There is no packfile index to reconstruct and no
delta chain to walk. The cost is more files and no built-in packing; the
benefit is predictability and trivial recovery.

Large files
~~~~~~~~~~~

Git stores a changed large file as a new blob unless a filter such as LFS is
configured. Mesh splits files of 50 MiB or more into 10 MiB chunks with a 4 KiB
realignment window. A small edit reuses the unchanged chunks.

This is not a replacement for Git LFS in a shared Git workflow. LFS has a
server protocol, pointer files and fetch/push semantics. Mesh chunks are
internal to the Mesh object store and are transferred by the Mesh sync
protocol.

History
~~~~~~~

Git history is a graph of commit objects. Mesh history is an append-only text
log whose lines are nodes. Parent hashes are fields, and the loader resolves
them to pointers. The graph is a reverse linked list from newest to oldest.
``tools/git_import.py`` can import a selected ref's first-parent tree sequence
and subjects into a new Mesh repository; it is not lossless and omits merge
parents, full refs, Git OIDs, signatures and exact commit timestamps.

The format has two parent slots. The current CLI passes one parent (or none).
A future merge command can fill the second slot without changing the file
format. Until then, ``mesh sync`` does not create merge nodes; it stages
conflicts and asks the user.

Deletion and garbage collection
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Mesh has a conservative object collector, ``mesh admin gc``. It is a dry run
by default; ``mesh admin gc --apply`` removes only objects not referenced by
any snapshot in the local history. It preserves all snapshots, history lines,
tree manifests and reachable objects. Run ``mesh admin verify`` and inspect the
dry-run output before applying it.

History remains append-only during ordinary operation, and ``mesh admin gc``
does not prune snapshots or tree manifests. For an explicit reset,
``tools/history_restart.py`` copies the complete old ``.mesh`` directory to a
new backup path, verifies a staged repository containing the current working
tree, and atomically replaces the active metadata with a new root snapshot.
This is not in-place compaction: old IDs, refs, tags and remote configuration
remain in the backup and are not retained in the active repository. Review
:doc:`limitations` before using it. Never edit ``.mesh/history`` by hand.


Transition Recipes
------------------

Recipe 1 - Evaluate Mesh beside Git
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Keep the existing Git repository and add Mesh for a trial period. Mesh skips
``.git``, and Git can be configured to ignore ``.mesh``.

.. code-block:: sh

   cd existing-project
   printf '.mesh/\n' >> .gitignore
   mesh start
   mesh save "mesh baseline"
   # work normally with Git
   # periodically compare mesh check with git status

This is a low-risk evaluation. It duplicates storage and hashing work, so do
not use it as a permanent production setup for a large repository.

Recipe 2 - One-way export to Git
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Use Mesh as the local recorder and export each save as a Git patch.

.. code-block:: sh

   cd project
   mesh save "change parser"
   cd /path/to/git-mirror
   git apply --index /path/to/project/.mesh/monitor/last.patch
   git commit -m "change parser"

Limitations:

* the patch contains only the difference between the last two Mesh snapshots;
* binary changes and changes larger than 4 MiB are omitted with a warning;
* text changes are full replacement hunks, so the patch is larger than a
  minimal Git diff;
* renames are seen as a delete plus an add;
* the patch does not carry the Mesh message or timestamp automatically; pass
  them to ``git commit`` yourself.

This recipe works well for a small team that wants Mesh's snapshot speed but
must publish to a Git host. It does not preserve Mesh history.

Recipe 3 - Move a repository to Mesh
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: sh

   # Work on a copy until the migration is verified.
   cp -a project project.mesh-test
   cd project.mesh-test
   rm -rf .git            # optional: Mesh ignores it anyway
   printf '.mesh/\n' > .gitignore   # keep if you still use Git elsewhere
   mesh start
   mesh save "import from git"

Verify:

.. code-block:: sh

   mesh check
   find .mesh/store/objects -type f | wc -l
   # Compare the working tree with the original
   diff -r --exclude=.mesh --exclude=.git ../project .

If the tree matches, rename the copy into place. Keep a backup of the original
``.git`` directory until you are confident. Mesh cannot reconstruct deleted
Git history, so the backup is your only path back.

Recipe 4 - Clone a tree
~~~~~~~~~~~~~~~~~~~~~~~

For a repository hosted on GitHub, GitLab, Gitea, Forgejo or a compatible
forge, ``mesh obtain`` downloads the current source archive and creates the
first Mesh snapshot:

.. code-block:: sh

   mesh obtain https://github.com/owner/project
   mesh obtain --ref v1.2.0 https://gitlab.com/group/project my-project
   mesh check

The command extracts only the selected tree. ``--ref`` chooses which archive
tree to import; commit messages, ancestry, branches, tags, reflogs, submodule
state and deleted Git history are not imported by ``obtain``. The separate
local first-parent importer preserves the tree sequence and subjects only.
Keep the original Git clone or a verified complete backup if full history
matters. Self-hosted forges with a non-standard archive route can pass
``--archive https://host/path/source.tar.gz``.

For a native Mesh peer, initialize an empty repository and sync:

.. code-block:: sh

   mkdir project && cd project
   mesh start
   mesh auth "$MESH_TOKEN"
   mesh sync https://mesh.example.com/project
   mesh check
   mesh save "initial pull"

The first sync pulls every object referenced by the remote manifest. On a large
repository this is a full transfer; subsequent syncs use hash comparison and
object deduplication.

Recipe 5 - Replace Wget and cURL in scripts
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: sh

   # before
   wget -c https://example.com/data.bin
   curl -fL -o data.bin https://example.com/data.bin

   # after
   mesh obtain https://example.com/data.bin -o data.bin

``mesh obtain`` resumes automatically when the destination exists, reports the
BLAKE2b hash and updates the repository status when run inside a repository. It
does not implement every cURL option; for custom headers, methods and payloads,
keep cURL or use the Mesh JSON API commands.


What Does Not Translate
-----------------------

Be explicit with a team before switching.

**No partial commits.**
You cannot stage one hunk and commit another. The whole working tree is the
snapshot. A large generated file that changes on every build will be stored on
every save unless you delete it or move it outside the repository.

**Different ignore rules.**
Mesh does not parse ``.gitignore``. It has its own ``.meshignore`` rules, so
Git ignore patterns do not automatically carry over. Review the active rules
with ``mesh ignore`` and configure ``.meshignore`` for generated output,
virtual environments and datasets before saving.

**No in-place history rewrite.**
There is no rebase, amend, filter-branch or interactive staging. Ordinary
history remains append-only. ``tools/history_restart.py`` can explicitly reset
the active repository to the current tree only after making a full backup; this
starts new history and does not preserve old IDs or refs in the active store.

**No merge command.**
Two parents are representable in the history format, but the CLI does not
create merge nodes. ``mesh sync`` stages file and symlink/file-type conflicts
with their kind and mode, and does not publish while any conflict is pending.
``mesh choose alpha`` or ``mesh choose beta`` selects a variant for the whole
staged batch; after ``mesh save``, the resulting snapshot has one parent.

**Restore is not a destructive checkout.**
``mesh restore <snapshot>`` can materialize files from an older snapshot, but
it does not delete extra working-tree files. It is a conservative restore, not
a complete checkout operation that replaces the directory contents. Review the
working tree and use ``--force`` only when the documented overwrite behavior is
intended.

**No hooks or automation.**
No pre-commit checks, no post-save scripts, no server-side policy. Wrap Mesh in
your CI or editor if you need enforcement.

**No signed commits.**
There is no GPG or SSH signature field. Authentication is a bearer token for
network calls; it does not sign local history.

**No submodules or worktrees.**
One working tree per ``.mesh`` directory. Nested repositories are not modeled.

**No packfiles or delta compression.**
Many small files mean many objects and many inodes. This is fine on modern
filesystems but can be noticeable on network filesystems with high per-file
latency.

**No Git protocol.**
A Mesh peer is an HTTP service, not a Git server. You cannot point
``git clone`` at it and expect a repository. ``mesh obtain`` reads forge source
archives over HTTPS; it does not speak the Git upload-pack protocol and does
not fetch Git history.


Interoperability Checklist
--------------------------

Before declaring a migration successful:

.. list-table::
   :header-rows: 1
   :widths: 12 88

   * - Done
     - Check
   * - [ ]
     - A full copy of the original repository exists outside the migration
       path.
   * - [ ]
     - ``mesh start`` created ``.mesh/config``, ``HEAD``, ``history`` and the
       store directories.
   * - [ ]
     - ``mesh save`` completed with the expected file count.
   * - [ ]
     - The working tree matches the original (excluding ``.mesh`` and
       ``.git``).
   * - [ ]
     - ``mesh check`` reports ``state: ready`` and the expected snapshot count.
   * - [ ]
     - A second save with no changes produces a new snapshot and reuses the
       existing objects.
   * - [ ]
     - A small edit produces a new object and a readable manifest.
   * - [ ]
     - ``.mesh/monitor/last.patch`` applies cleanly to a Git mirror for a text
       change.
   * - [ ]
     - A test sync against a staging peer succeeds and a conflict is staged
       without modifying the working file.
   * - [ ]
     - Disk space is sufficient for the full object store; no garbage
       collector exists.
   * - [ ]
     - The team accepts the missing index, ignore rules and history rewrite.

Do not migrate a production repository on a Friday afternoon. Do it on a copy,
measure the storage growth, run the checks and keep the old tool available
until the new workflow has survived a full release cycle.


A Pragmatic Recommendation
--------------------------

Use Mesh when:

* the working tree is moderate and mostly text or already-compressed binaries;
* one-step snapshots fit the workflow better than staging;
* large files change in small regions and storage or bandwidth matters;
* a GUI needs a cheap, passive status surface;
* you want a small codebase you can read and audit.

Keep Git when:

* you need the Git ecosystem, hosting, pull requests or CI integrations that
  speak Git;
* partial commits, rebase, bisect, hooks or submodules are part of your daily
  work;
* you need a mature garbage collector and packfile storage for a very large
  history;
* multiple teams depend on identical commit hashes across tools.

The tools can coexist in one working directory, but their histories and
metadata remain independent. Mesh excludes Git's reserved ``.git`` namespace;
Git itself does not interpret Mesh snapshots and may report ``.mesh`` as
untracked unless the project deliberately ignores it. Do not edit Git's ignore
rules without reviewing repository policy. ``mesh git-info`` and
``tools/git_coexist.py`` provide read-only inventory; neither stages, commits,
rewrites refs, or reconciles histories. Use Git as usual for Git operations and
Mesh only for its separate snapshots. Keep the source clone and export paths
tested during migrations.

.. seealso::
   For the current implementation boundaries and safe operating mitigations, see
   :doc:`limitations`.
