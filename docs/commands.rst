Commands and Global Flags
=========================

This document is the complete command reference for Mesh 1.0. The command
router lives in ``src/cli.c``. Every command validates its arguments before it
touches the file system or the network.

.. contents::
   :depth: 2
   :local:


Implementation Style
--------------------

The command router is written in the Alves style. Each command is a sequence
of guard clauses: argument count, repository discovery, lock acquisition and
input validation fail early, before any file or network operation. The
success path then reads from top to bottom with at most three levels of
control nesting. ``cmd_save()`` is the largest command, and it still uses one
``done:`` label and one cleanup path.

The dense layout is not a different command language. Flags, output, exit
codes and file formats are unchanged by the rewrite. What changed is the
amount of vertical ceremony a reader must cross to verify the same behaviour.
The practical benefit is auditability: a reviewer can see that validation
precedes every file or network operation without tracing a nested success
path. The rationale is in :doc:`alves_style_manifesto`; the nesting rule and
the ``chunk_work_tree`` case are in :doc:`complexity_guardrails`.


Global Flags
------------

.. list-table::
   :header-rows: 1
   :widths: 22 78

   * - Invocation
     - Behaviour
   * - ``mesh -v``, ``mesh --version``
     - Print ``Mesh 1.0`` followed by a newline and exit with status 0.
   * - ``mesh -h``, ``mesh --help``, ``mesh``
     - Print the help menu and exit with status 0.
   * - ``mesh --unknown``
     - Print an unknown-flag error and exit with status 2.
   * - ``mesh unknown``
     - Print an unknown-command error and exit with status 2.

The help menu is reproduced verbatim below. Scripts may rely on the version
string, but should not parse the help text.

.. code-block:: text

   Mesh - The Simple, Ultra-Fast Version Control System (1.0)

   Usage:
     mesh [command] [options]

   Core Commands:
     start          Initialize or repair a repository, shell PATH and VS Code
                    integration.
     config [--global|--local] [--list] [key [value]]
                    Read or write user.name and user.email identity settings.
     branch [list|create|switch|delete] ...
                    Manage named branches and the current branch pointer.
     tag [name [snapshot]] | tag -d name
                    List, create or delete lightweight snapshot tags.
     reflog         Show recent HEAD and branch movements.
     ignore         List the active .meshignore rules.
     clean (-n|-f) [-x]
                    List or remove working-tree files not tracked by HEAD.
     save "msg"     Record the working tree as a new snapshot.
     log            Show snapshot history, newest first.
     diff [a [b]]   Compare snapshots, or a snapshot/HEAD against the working
                    tree.
     restore <snap> [--force] [--] [path...]
                    Restore files from a snapshot without deleting extra files.
     check          Show repository state, refreshing status.json when needed.
     choose [alpha|beta]
                    List conflicts or resolve them by choosing a variant.
     admin <cmd>    Inspect, verify and maintain repository administration state.
                    Run 'mesh admin help' for the available subcommands.

   Network Commands:
     obtain [options] URL [DEST]
                    Clone a forge repository or download a file
                    (wget/curl-style). See 'mesh obtain --help' for options.
     fetch [options] URL [DEST]
                    Deprecated alias for 'mesh obtain --file'.
     auth [--host HOST] [token]
                    Store or report repository-scoped credentials (mode 0600).
     send <url>     Send the current snapshot and bounded history as JSON.
     sync <url>     Synchronize manifests and objects with a remote API.

   Global Flags:
     -v, --version  Show current Mesh software version.
     -h, --help     Display this help menu with the list of available commands.


Common Conventions
------------------

**Repository discovery.**
Commands that need a repository search the current directory and then each
parent until they find a directory containing ``.mesh/config``. The search
stops at the file-system root. If no repository is found, the command prints
``mesh: not a Mesh repository (run 'mesh start' first)`` and exits with
status 1.

**Paths.**
All repository-relative paths use forward slashes. The scanner records regular
files and symbolic links with their mode and nanosecond modification time; it
never follows a symbolic link. It skips ``.mesh``, ``.git`` and other
non-regular objects, refuses directory nesting deeper than 64 levels and
rejects absolute, empty, ``.``, ``..``, reserved components, ASCII control
bytes (``0x00``-``0x1f`` and ``0x7f``), invalid UTF-8 and paths of
``MESH_PATH_MAX`` (4096) bytes or more before writing. Manifests also reject
paths that collide under Unicode NFC normalization and full case-folding;
original path spelling is preserved.

**Atomic files.**
Status, manifests, configuration rewrites, history appends and exported patches
are written through a temporary file and an atomic rename, or through a locked
append. A crash does not produce a partially written JSON document or a
truncated tree.

**Exit status.**

.. list-table::
   :header-rows: 1
   :widths: 12 88

   * - Code
     - Meaning
   * - 0
     - The requested operation completed.
   * - 1
     - Runtime failure: I/O error, network error, invalid repository state,
       missing credentials file with a hard error, or a command-specific
       failure.
   * - 2
     - Usage error: missing argument, unknown option, invalid variant name or
       mutually exclusive flags.


mesh start
----------

Initializes a repository in the current directory and automates the local
environment.

.. code-block:: sh

   mesh start

What it does:

1. Searches upward for an existing repository. If one exists, it reports
   ``Repository already initialized`` and continues with environment repair.
2. Creates ``.mesh`` and the subdirectories ``store/objects``,
   ``store/trees``, ``store/conflicts``, ``store/remote``, ``store/tmp``,
   ``monitor``, ``refs``, ``refs/tags`` and ``logs``.
3. Writes ``.mesh/config`` with ``version=1.0``, ``branch=main`` and
   ``icon=mesh_icon.svg`` if the file does not exist. An existing config is
   never overwritten, so local identity survives a repeated ``mesh start``.
4. Creates empty ``HEAD``, ``history`` and ``refs/main`` files if absent.
5. Copies ``mesh_icon.svg`` from the directory containing the ``mesh`` binary
   into the working tree when the icon is missing.
6. Appends a marked PATH block to ``~/.bashrc`` and ``~/.zshrc`` when those
   files exist. If neither exists, it creates ``~/.bashrc``. It appends a fish
   block to ``~/.config/fish/config.fish`` when that file exists.
7. Writes ``.vscode/settings.json`` when the file is absent. If a settings file
   exists and already contains ``"mesh.iconPath"``, it is considered
   Mesh-managed and left as-is. A hand-written settings file without that key
   is never overwritten; the command prints a notice instead.
8. Writes ``.mesh/monitor/status.json`` with operation ``start``.

The PATH block is guarded by ``# >>> mesh path >>>`` and
``# <<< mesh path <<<`` markers. Re-running ``mesh start`` does not duplicate
the block. If an existing block points at a different binary directory, for
example after moving the installation or passing a new ``--bin-dir`` to the
installer, the stale block is replaced atomically instead of being left in
front of the new one.

Example output::

   Initialized Mesh repository: /home/user/demo
   Shell integration: 2 startup file(s) updated (PATH: /home/user/.local/bin)
   IDE manifest: /home/user/demo/.vscode/settings.json
   Next: mesh save "initial snapshot"


mesh config
-----------

Reads or writes the two Git-style identity keys used by Mesh snapshots.

.. code-block:: sh

   mesh config --global user.name "Ada Lovelace"
   mesh config --global user.email "ada@example.com"
   mesh config user.name "Local Builder"
   mesh config user.name
   mesh config --global --list

The local file is ``.mesh/config``, alongside ``version``, ``branch`` and
``icon``. The global file is ``$XDG_CONFIG_HOME/mesh/config`` when
``XDG_CONFIG_HOME`` is an absolute path, otherwise
``$HOME/.config/mesh/config``. Local values override global values key by key.
When resolving the snapshot author, if neither file supplies ``user.email``,
the ``EMAIL`` environment variable is accepted as a fallback. The fallback is
not written to either file and is not returned by a ``mesh config`` read.

Inside a repository, ``--local`` is the default. Outside a repository, use
``--global`` explicitly; ``--local`` and an implicit local lookup both fail
with the usual "not a Mesh repository" diagnostic. ``--list`` prints the
selected scope and requires no key. Without ``--list`` or a value, the command
prints the selected value and exits with status 1 when the key is unset.

Values are validated before they are written. ``user.name`` must be non-empty,
at most 128 bytes, and free of control bytes and ``|``. ``user.email`` must be
non-empty, at most 254 bytes, contain exactly one ``@`` with non-empty local and
domain parts, and contain no whitespace, control bytes, ``|``, ``<``, ``>``,
``/`` or ``\``. Invalid keys and values exit with status 2. The configuration
file is rewritten atomically and preserves unrelated keys, comments and local
repository settings.

Identity is optional for compatibility with existing repositories. A save
without a complete identity still succeeds, prints a warning and writes the
legacy seven-field history line without author metadata. When both values are
present, new snapshots record the author. See :doc:`architecture` for the exact
history encoding.


mesh branch
-----------

Lists and edits the branch refs below ``.mesh/refs``.

.. code-block:: sh

   mesh branch
   mesh branch create feature [snapshot]
   mesh branch switch feature
   mesh branch delete feature

``mesh branch`` and ``mesh branch list`` print the current branch with a
leading ``*``, followed by its name and the first twelve hash characters.
``create`` accepts an optional snapshot argument, resolved like ``diff`` and
``restore`` (``HEAD``, a full hash, or an unambiguous prefix of at least four
characters); without it the new branch points at HEAD. ``switch`` validates
that the named ref points to a snapshot present in history, writes the branch
key in ``.mesh/config``, updates ``.mesh/HEAD`` and refreshes
``.mesh/monitor/status.json``. It moves the branch pointer only; it does not
materialise or delete working-tree files. ``delete`` refuses to remove the
current branch. Branch names are limited to 63 bytes and cannot contain
whitespace, control bytes, ``/``, ``\``, ``|``, ``~``, ``^``, ``:``, ``?``,
``*``, ``[``, ``]``, ``..`` or the ``@{`` and ``.lock`` forms.

Creating and switching branches takes the repository lock. A save while a
branch is current appends its snapshot to that branch's ref; other branch refs
do not move.


mesh tag
--------

Manages lightweight tags that point at immutable snapshots.

.. code-block:: sh

   mesh tag
   mesh tag v1
   mesh tag release HEAD
   mesh tag -d v1

With no arguments, ``mesh tag`` lists each tag and the first twelve hash
characters of its target. A tag name creates a tag at HEAD; an optional
snapshot argument selects another target using the same resolution rules as
``mesh branch create``. Tags live at ``.mesh/refs/tags/<name>`` and share the
branch-name validation rules. Tag creation and deletion take the repository
lock. Duplicate names are rejected instead of silently retargeted.


mesh reflog
-----------

Prints the local HEAD movement log, newest first.

.. code-block:: sh

   mesh reflog

Saves append an entry after HEAD moves. Branch switches append an entry even
when both branches point at the same snapshot. The log is
``.mesh/logs/HEAD``; each line is
``timestamp|old-hash|new-hash|actor|message``. Empty old and new hashes are
allowed for the first snapshot and for ref-only operations. The author is
taken from the effective identity and falls back to ``mesh``. The reflog is a
local navigation aid: it is not part of the snapshot graph and is not sent by
``mesh send`` or ``mesh sync``.


mesh ignore
-----------

Lists the active root ``.meshignore`` rules.

.. code-block:: sh

   mesh ignore

The file uses one pattern per line. Blank lines and lines beginning with ``#``
are ignored. A leading ``!`` negates a rule, and the last matching rule wins.
A trailing ``/`` restricts a rule to directories and their descendants. A
leading ``/`` anchors the pattern at the repository root; otherwise a pattern
without ``/`` matches a basename at any depth and a pattern containing ``/`` is
relative to the root. ``*`` matches any run of bytes except ``/``, ``?``
matches one byte except ``/``, and ``**`` matches across ``/``. A backslash
escapes the next pattern byte, including a leading ``#`` or ``!``.

The rules apply to ``mesh save`` and ``mesh diff``. Ignored directories are
pruned unless a negated rule could re-include a descendant. The root
``.meshignore`` file itself is always included in a snapshot, so the rules that
shaped it remain available with the files they governed. Nested ignore files
are not read. Ignored paths are omitted from new snapshots; a path that was
already in an older snapshot simply appears as deleted until it is restored or
tracked again.


mesh clean
----------

Lists or removes working-tree files that are not present in HEAD's manifest.

.. code-block:: sh

   mesh clean --dry-run
   mesh clean --force
   mesh clean --dry-run --ignored
   mesh clean --force --ignored

Exactly one of ``-n``/``--dry-run`` and ``-f``/``--force`` is required.
Without ``-x``/``--ignored``, ignored files and directories are skipped.
``--ignored`` includes them, which is useful for removing build products left
behind by a closed rule. The command compares against the manifest of the
current HEAD, holds the repository lock, and never deletes a path that HEAD
tracks. Symbolic links are unlinked as links and are never followed. Only
regular files and symbolic links are removed; empty directories are left in
place. A failed unlink is reported but does not stop the remaining candidates,
and the command exits with status 1 if any removal failed.


mesh save
---------

Creates a snapshot of the current working tree in one step. There is no index
and no separate add/commit phase.

.. code-block:: sh

   mesh save "message"

The message is required. It is sanitized so that ``|`` and newlines cannot
corrupt the history format; the stored message is truncated to 511 bytes.

If ``user.name`` and ``user.email`` are configured, the snapshot records that
author. Without a complete identity, ``mesh save`` prints a warning and writes
the legacy seven-field history line; the save still succeeds. Updating the
identity later does not rewrite existing snapshots.

Processing order:

1. Scan the working tree, skipping ``.mesh``, ``.git`` and paths matched by
   the root ``.meshignore`` while recording regular files and symbolic links,
   modes and nanosecond timestamps.
2. Hash every regular file with BLAKE2b-512, using one worker per online CPU
   (maximum 64). A symbolic link is hashed by its target bytes.
3. Split files of 50 MiB or more into 10 MiB chunks with a 4 KiB realignment
   window.
4. Store only objects that do not already exist, verifying the digest of each
   object before publishing it.
5. Check that the working tree did not change during hashing and storage.
6. Write a manifest to ``.mesh/store/trees/<tree-hash>``. The tree hash is the
   BLAKE2b digest of the manifest bytes. Each entry carries
   ``hash size mtime_sec mtime_nsec mode kind path``.
7. Append a node to ``.mesh/history`` and update ``HEAD`` and ``refs/<branch>``.
   The history timestamp is nanoseconds so two saves in the same second remain
   distinct. A configured author adds ``author-name`` and ``author-email`` to
   the line and to the node signature. A local reflog entry is appended after
   HEAD moves.
8. Export a Git-compatible patch to ``.mesh/monitor/last.patch``, comparing
   the previous tree with the new one.
9. Rewrite ``.mesh/monitor/status.json``.

``mesh save`` takes an exclusive ``.mesh/lock`` for the whole snapshot, so a
concurrent save or sync cannot interleave history or object publication.

Saving an unchanged tree still creates a new snapshot. This is deliberate:
``mesh save`` records an intention, not a diff against the previous message.

Example::

   $ mesh save "add parser"
   saved 5d08affd9cb2  add parser
     files: 42 (1.2 MiB)   +3 ~1 -0   snapshots: 7

The short hash is the first 12 hexadecimal characters of the snapshot hash.
The counters are added, modified and deleted files relative to the previous
snapshot. ``snapshots`` is the total number of nodes in the history.


mesh log
--------

Prints the snapshot history, newest first.

.. code-block:: sh

   mesh log

Typical output::

   History (2 snapshots):
     5d08affd9cb2  2026-10-22 11:42:03  [snapshot]  second snapshot
                 by Ada Lovelace <ada@example.com>
     2b8e1f0a6c44  2026-10-22 11:41:58  [snapshot]  initial snapshot
                 by Ada Lovelace <ada@example.com>

The left column is the 12-hexadecimal short hash accepted by ``mesh diff`` and
``mesh restore``. The message is the one stored in the snapshot node. A
snapshot with author metadata prints a ``by Name <email>`` line beneath it; old
snapshots and repositories without identity omit that line. A merge node
prints an extra line naming both parents. ``mesh log`` reads only
``.mesh/history`` and does not scan the working tree. An empty repository prints
``No snapshots yet.`` and exits with status 0.


mesh diff
---------

Compares snapshots, or a snapshot against the working tree.

.. code-block:: sh

   mesh diff                  # HEAD vs working tree
   mesh diff <snapshot>       # <snapshot> vs working tree
   mesh diff <a> <b>          # two snapshots

Snapshot arguments may be full 128-hexadecimal hashes, unique prefixes of at
least four hexadecimal characters, or ``HEAD``. The output is one line per
path::

   M src/parser.c
   A docs/new.rst
   D obsolete.txt
   1 added, 1 modified, 1 deleted

``A`` means present only in the right-hand tree, ``D`` only in the left-hand
tree and ``M`` means the hash, kind or mode differs. With one snapshot the
right-hand side is the current working tree: untracked files appear as ``A``
and files missing from disk as ``D``. The working tree is hashed, including
chunk layout for large files, so the comparison uses exactly the same tree
semantics as ``mesh save``, including the root ``.meshignore`` rules. No
changes prints ``no changes``. A malformed or ambiguous snapshot argument
exits with status 1; too many arguments exit with status 2.


mesh restore
------------

Restores files or symbolic links from a snapshot.

.. code-block:: sh

   mesh restore <snapshot>                  # all paths in the snapshot
   mesh restore <snapshot> src/parser.c     # selected paths
   mesh restore <snapshot> --force          # overwrite modified files

``mesh restore`` is deliberately conservative. A destination that already
exists is skipped when its content, kind and mode already match the snapshot.
If it differs, the command refuses to overwrite it unless ``--force`` is given.
Files and directories that are not in the snapshot are never deleted; restoring
an old snapshot into a newer tree therefore leaves extra files in place. Use
``--`` before a path that begins with ``-``.

The snapshot argument follows the same rules as ``mesh diff``. A path that is
not present in the snapshot is an error. Regular files are written through a
temporary file with the recorded mode and timestamp; symlinks are recreated
with their recorded target. Files whose content, kind and mode already match
are skipped, so their timestamps are not rewritten. The repository lock is
held for the whole restore and ``.mesh/monitor/status.json`` is refreshed with
operation ``restore``.
The command prints how many paths were written and how many were already
current. It exits with status 1 on the first I/O or conflict error and status 2
for a usage error.


mesh check
----------

Prints the repository state stored in ``.mesh/monitor/status.json``. When the
file exists and its recorded ``head`` matches ``.mesh/HEAD`` it does not rescan
the working tree; if the file is missing, unreadable or stale, it collects the
state once, writes the document and prints it.

.. code-block:: sh

   mesh check

Typical output::

   Mesh repository : /home/user/demo
   Branch          : main
   Head            : 5d08affd9cb2e6d7498c3179941d0599147f0e363a4e835d374bd8d7ccb12f9c95ee25d61cf8b574a4511cc8aabdb8d95ae8bcde9f32f7b08f148ebf296021f8
   Tree            : 76fe60df789791d49399d15737090e149a2080cc0706a669f1be8f531900cd76d800f33cd0f9dc759310df84928920be8bb2e44e0664981c72cdff52de08e934
   Parent          : -
   Snapshots       : 7
   Tracked files   : 42 (1.2 MiB)
   Chunks          : 120
   Conflicts       : 0
   State           : ready
   Last operation  : save
   Message         : add parser
   Icon source     : mesh_icon.svg
   Status file     : /home/user/demo/.mesh/monitor/status.json

The exact layout is stable within version 1.x. Scripts should prefer the JSON
document over parsing this text.

``mesh check`` is intentionally cheap when the cache is current: it opens one
small file and formats fields. A GUI can call it repeatedly without causing
disk scans. If the status file is missing, unreadable or records a head that
does not match ``.mesh/HEAD``, the command rebuilds it; a failure during that
rebuild exits with status 1.


mesh git-info
-------------

Reports whether a Git metadata entry exists at the root of the current Mesh
repository. It recognizes a ``.git`` directory and reports a regular
``.git`` worktree pointer without following its target. Symlinks and unusual
filesystem objects are reported but never followed or modified. The command
opens Mesh configuration only to locate the repository; it does not run Git,
change ``.git``, update status files, or reconcile the two histories.

Use ``python3 tools/git_coexist.py --json PATH`` for a read-only inventory of
both tools. That helper asks the installed Git CLI for branch and working-tree
status with optional Git locks disabled, and calls ``mesh git-info``. Neither
command stages or commits files. Continue using Git for Git branches, commits,
index, hooks, remotes and transport; Mesh snapshots are independent.

.. code-block:: sh

   mesh git-info
   python3 tools/git_coexist.py --json .


mesh choose
-----------

Lists or resolves conflicts staged by ``mesh sync``. Mesh never writes
``<<<<<<<`` markers into user files.

.. code-block:: sh

   mesh choose
   mesh choose alpha
   mesh choose beta

With no variant argument, the command lists every staged conflict and the two
available variants. With ``alpha`` or ``beta``, it materializes the selected
file or symlink with its recorded mode, removes both staged variants for that
path and rewrites the conflict index. The status file is updated with
operation ``choose`` and the new conflict count.

The working file is untouched until a choice is made. If a conflict is staged
for ``src/parser.c``, the store contains:

.. code-block:: text

   .mesh/store/conflicts/0123456789abcdef-parser.c.alpha
   .mesh/store/conflicts/0123456789abcdef-parser.c.beta
   .mesh/store/conflicts/index

``alpha`` is the local variant captured at staging time. ``beta`` is the
remote variant. The 16-hexadecimal stem is derived from the full repository
path with BLAKE2b; the sanitized basename is only a readable suffix. The index
stores an escaped repository-relative path and typed kind/mode fields, never an
untrusted variant filename, so a forged index cannot redirect the resolver to
another file. Legacy path-only and ``key<TAB>path`` indexes are still read as
regular-file pairs; the legacy key is ignored.

If some entries cannot be resolved, the command reports the failure and leaves
the remaining entries in the index. It exits with status 1. An invalid variant
name exits with status 2. Choosing when no conflicts exist prints
``No conflicts to resolve.`` and exits with status 0.


mesh admin
----------

Inspects, verifies and maintains repository administration state. The command
is a router for 13 focused C modules; it does not change the snapshot format.

.. code-block:: sh

   mesh admin modules
   mesh admin doctor
   mesh admin verify
   mesh admin inventory [--json]
   mesh admin gc [--apply]
   mesh admin refs
   mesh admin status
   mesh admin audit [message]
   mesh admin quota [bytes]
   mesh admin config
   mesh admin history
   mesh admin export [file|-]

``modules`` prints the module registry. ``doctor`` checks the repository
layout, branch ref, HEAD target and credentials mode. ``verify`` hashes every
object reachable from a manifest and reports missing or modified objects.
``inventory`` counts snapshots, objects, trees, branches, tags, conflicts and
bytes; ``--json`` emits one machine-readable document.

``gc`` lists unreachable object-store files and removes them only with
``--apply``. It never prunes snapshots or ``.mesh/history``. Applying requires
a non-empty history and the repository lock; verify the repository and inspect
the dry run before applying.
``refs`` validates branch, tag and HEAD targets against the graph. ``status``
rewrites ``.mesh/monitor/status.json``. ``audit`` appends a line to
``.mesh/logs/admin.log`` when given a message, otherwise prints the log.
``quota`` reports or sets a byte ceiling in ``.mesh/admin/quota`` and exits
with status 1 when usage exceeds it. ``config`` validates local keys, branch,
icon, author and e-mail. ``history`` validates history and reflog records.
``export`` writes graph metadata as JSON to stdout or an atomically replaced
file.

The admin commands are read-only except ``gc --apply``, ``status``,
``audit`` and ``quota``. A malformed repository, missing object or invalid ref
returns status 1. Usage errors return status 2. The module metadata and the
standard-library Python tools are documented in
:doc:`admin_modules`.


mesh obtain
-----------

Clones a repository from a Git forge or downloads a URL to a file. It is the
single transfer command: repository mode imports a forge archive, and file
mode preserves the ``wget`` and common ``curl`` download workflows, including
resumable transfers, redirects and custom request metadata.

.. code-block:: sh

   mesh obtain https://github.com/owner/repo
   mesh obtain --ref v1.2.0 https://gitlab.com/group/project my-project
   mesh obtain --archive https://example.com/source.tar.gz https://forge.example/owner/repo
   mesh obtain https://example.com/data.bin
   mesh obtain https://example.com/data.bin -o copy.bin
   mesh obtain --file https://example.com/download
   mesh obtain -L -O https://example.com/releases/latest.tar.gz
   mesh obtain -C - -o partial.iso https://example.com/image.iso
   mesh obtain -C 104857600 -o partial.iso https://example.com/image.iso
   mesh obtain -o - https://example.com/data.bin > data.bin
   mesh obtain -I https://example.com/data.bin
   mesh obtain -w '%{http_code} %{size_download}\n' -o data.bin https://example.com/data.bin
   mesh obtain -b cookies.txt -c cookies.txt -o page.html https://example.com/page
   mesh obtain -x http://proxy.example:8080 -o data.bin https://example.com/data.bin
   mesh obtain -H "X-Request-ID: 42" -A mesh/1.0 https://example.com/data.bin
   mesh obtain --compressed -o page.html https://example.com/page

Mode selection:

.. list-table::
   :header-rows: 1
   :widths: 24 76

   * - Input
     - Mode
   * - ``--ref``, ``--archive`` or ``--repo``
     - Repository import.
   * - ``-o``, ``--output``, ``--output-dir``, ``-O``, ``--remote-name``,
       ``-I``, ``--head``, ``-C``, ``--continue-at``, ``--create-dirs``,
       ``-w``, ``--write-out`` or ``--file``
     - File download.
   * - No forcing option
     - Automatic detection. Recognized forge repository URLs import; other
       URLs download.
   * - Contradictory options
     - Rejected with status 2.

Automatic detection recognizes:

* GitHub ``/<owner>/<repo>`` paths with exactly two components.
* GitLab paths that do not contain ``/-/``; nested groups are included.
* Gitea, Forgejo, Codeberg and generic ``ORIGIN/<owner>/<repo>`` paths with
  exactly two components and no file extension in the final component. A
  ``.git`` suffix is accepted.

Everything else is a download. For example, ``https://example.com/files/a.bin``
downloads, while ``https://example.com/owner/repo`` imports. Use ``--repo``
for a self-hosted forge with a nested path or a dotted repository name, and
``--file`` for an extensionless file that happens to use a two-component path.

Options:

.. list-table::
   :header-rows: 1
   :widths: 22 78

   * - Option
     - Effect
   * - ``--ref REF``
     - Repository mode: branch, tag or revision passed to the forge archive
       endpoint. The default is ``HEAD``, which resolves the default branch on
       GitHub, GitLab and Gitea.
   * - ``--archive URL``
     - Repository mode: use this tarball instead of deriving a forge endpoint.
       Useful for GitHub Enterprise, self-hosted GitLab and non-standard
       archive routes.
   * - ``--repo``
     - Force repository mode for an ambiguous URL.
   * - ``--file``
     - Force file mode for an ambiguous URL.
   * - ``-o FILE``, ``--output FILE``
     - File mode: destination path. ``-o -`` streams the response body to
       stdout and writes progress metadata to stderr. Without it, the last URL
       path component is used. Mutually exclusive with a positional ``DEST``.
   * - ``-O``, ``--remote-name``
     - File mode: derive the destination from the URL. This is the default
       when no destination is given, but the explicit flag makes the curl
       intent clear. Mutually exclusive with ``-o`` and positional ``DEST``.
   * - ``-I``, ``--head``
     - File mode: send a HEAD request and write the response headers to the
       selected output (stdout by default). No body is downloaded and resume
       options are rejected. With ``-f`` an HTTP error status makes the command
       fail after the headers are printed; without ``-f`` the headers are still
       useful and the command succeeds.
   * - ``-w FORMAT``, ``--write-out FORMAT``
     - Print transfer metrics after the transfer. ``\n``, ``\r``, ``\t`` and
       ``%%`` are recognised. Variables are ``%{http_code}``,
       ``%{size_download}``, ``%{time_total}``, ``%{time_connect}``,
       ``%{time_starttransfer}``, ``%{url_effective}``,
       ``%{filename_effective}``, ``%{remote_ip}``, ``%{num_redirects}``,
       ``%{content_type}``, ``%{exitcode}`` and ``%{errormsg}``. The output is
       printed even with ``-s`` and on failed transfers; successful metadata
       moves to stderr so stdout stays machine-readable.
   * - ``--output-dir DIR``
     - File mode: store the output under ``DIR``. The output name is still
       derived from ``-o``, ``-O`` or the URL basename.
   * - ``-L``, ``--location``
     - Follow HTTP redirects, up to five hops. Only ``http`` and ``https`` are
       accepted; an ``https`` URL may not redirect down to ``http``. Redirects
       are off by default.
   * - ``-C -``, ``--continue-at -``
     - Resume from the current destination size. Resume is automatic in file
       mode; the explicit flag documents curl-compatible intent.
   * - ``-C N``, ``--continue-at N``
     - Resume at byte ``N``. The destination must already exist and be at
       least ``N`` bytes; a longer file is truncated to exactly ``N`` before
       the transfer. ``-C 0`` is a clean restart. Numeric resume is rejected
       for stdout output.
   * - ``-H LINE``, ``--header LINE``
     - Add an HTTP request header. May be repeated up to 32 times. Header
       names and values containing control bytes are rejected.
   * - ``-A STRING``, ``--user-agent STRING``
     - Override the ``User-Agent`` header.
   * - ``-e URL``, ``--referer URL``
     - Set the ``Referer`` header.
   * - ``-b DATA|FILE``, ``--cookie DATA|FILE``
     - Send cookies. A value containing ``=`` is treated as cookie data;
       otherwise it is a cookie file. Cookie data with control bytes is
       rejected.
   * - ``-c FILE``, ``--cookie-jar FILE``
     - Write cookies received during the transfer to ``FILE`` when the handle
       is released.
   * - ``-x URL``, ``--proxy URL``
     - Use an HTTP proxy for this transfer. Proxy environment variables are
       still honoured when ``-x`` is absent.
   * - ``-U USER:PASSWORD``, ``--proxy-user USER:PASSWORD``
     - Supply proxy credentials.
   * - ``--noproxy LIST``
     - Comma-separated hosts that bypass the configured proxy.
   * - ``-k``, ``--insecure``
     - Disable TLS certificate and hostname verification. A warning is always
       printed. Repository mode refuses this option when ``MESH_TOKEN`` or
       matching repository credentials would be sent.
   * - ``--compressed``
     - Ask libcurl to request and transparently decode compressed responses.
   * - ``--create-dirs``
     - Create missing parent directories for the destination path.
   * - ``--max-time SEC``
     - Limit the whole transfer to ``SEC`` seconds.
   * - ``--connect-timeout SEC``
     - Limit connection setup to ``SEC`` seconds.
   * - ``--limit-rate RATE``
     - Cap download speed in bytes per second. ``K``, ``M`` and ``G`` suffixes
       are accepted.
   * - ``--url URL``
     - Supply the URL as an option instead of positionally.
   * - ``-f``, ``--fail``
     - Make HEAD fail on an HTTP error status after printing its headers. File
       downloads always fail on HTTP error responses and never keep the error
       body, so the flag is otherwise accepted for curl compatibility.
   * - ``-s``, ``--silent``
     - Suppress successful file download output.
   * - ``-S``, ``--show-error``
     - Accepted for curl compatibility. Errors are always shown.
   * - ``DEST``
     - Repository mode: destination directory. File mode: destination file.
       Repository mode accepts an absent path or an existing empty directory;
       a non-empty directory is refused.

Repository mode
~~~~~~~~~~~~~~~

The archive endpoint mapping is:

.. list-table::
   :header-rows: 1
   :widths: 28 72

   * - Source
     - Archive endpoint
   * - ``https://github.com/O/R``
     - ``https://codeload.github.com/O/R/tar.gz/REF``
   * - ``https://gitlab.com/G/P``
     - ``https://gitlab.com/G/P/-/archive/REF/P-archive.tar.gz``
   * - Other hosts
     - ``ORIGIN/PATH/archive/REF.tar.gz``, the Gitea/Forgejo layout used by
       Gitea, Forgejo, Codeberg, Gogs and most compatible servers.

The command downloads the archive into a private sibling staging directory,
extracts it without following links, initializes ``.mesh`` there and records
the first snapshot. The destination is published with one ``rename`` after the
snapshot succeeds, so a failed transfer or a malicious archive never leaves a
partial repository in place.

Repository mode imports the working tree only. It does not import Git commits,
branches, tags, reflogs or submodule contents. The source URL is stored in the
snapshot message as ``obtain <URL>``.

For private archives, set ``MESH_TOKEN`` to a bearer token. The token is
written to the new repository's ``.mesh/credentials`` scoped to the archive
authority and sent only over HTTPS. When ``mesh obtain`` runs inside an
existing repository and ``MESH_TOKEN`` is unset, that repository's matching
credentials are used for the archive request.

``--location`` also applies to repository mode, but Mesh refuses to follow a
redirect when ``MESH_TOKEN`` or matching repository credentials would be
attached. A cross-origin redirect must never receive a bearer token; use a
direct ``--archive`` URL instead.

Example::

   $ mesh obtain https://github.com/octocat/Hello-World
   saved bcf5d9012fbb  obtain https://github.com/octocat/Hello-World
     files: 1 (13 B)   +1 ~0 -0   snapshots: 1
   obtained /home/user/Hello-World
     archive: https://codeload.github.com/octocat/Hello-World/tar.gz/HEAD (255 B)

File mode
~~~~~~~~~

File mode uses the same resumable transfer engine as the rest of the network
layer:

* If the destination exists and is a regular file larger than zero bytes, its
  size is read with ``stat`` and sent as a ``Range`` request.
* ``-C N`` requires an existing regular file of at least ``N`` bytes. A longer
  file is truncated to exactly ``N`` and the transfer resumes there; a shorter
  file or a missing file is rejected. ``-C 0`` truncates to zero and restarts.
* If the server answers ``200`` instead of ``206``, Mesh discards the partial
  file and restarts cleanly rather than appending a full body to a partial
  one.
* A ``416`` response with an existing partial file is discarded and retried
  once from byte zero; a status code alone is not proof that the local file is
  complete.
* ``-o -`` streams the response body to stdout. Progress metadata goes to
  stderr, so stdout contains only the body (and any ``-w`` output). The
  BLAKE2b digest is computed while the bytes are streamed. Resume options are
  rejected because there is no local file.
* ``-I`` writes the response headers instead of the body. Headers go to stdout
  by default, or to the file selected by ``-o``, ``DEST``, ``-O`` or
  ``--output-dir``. ``-f`` turns an HTTP error status into a failure after the
  headers have been printed.
* On success, Mesh prints the final size, the number of bytes that were
  already present and the BLAKE2b-512 digest of the completed file.
* Inside a repository, the status file records operation ``obtain``,
  destination, byte count, resumed offset and digest.
* File downloads never read repository credentials. This keeps a generic URL
  from receiving a token stored for a Mesh peer or forge.

The curl-compatible options cover the common download workflow without trying
to emulate every curl feature. ``-o``, ``-O``, ``--output-dir`` and
``--create-dirs`` control the local name. ``-L`` opts into redirects;
without it, a ``3xx`` response fails the transfer. ``-H``, ``-A`` and ``-e``
add request metadata, while ``--compressed`` enables transparent decoding.
``--max-time``, ``--connect-timeout`` and ``--limit-rate`` bound the transfer.
``-b``/``-c`` manage cookies, ``-x``/``-U``/``--noproxy`` configure a proxy,
and ``-k`` disables TLS verification with an explicit warning. Cookie and proxy
options are passed to libcurl for both file and repository transfers.
Short flags may be clustered, so ``-fsSL`` behaves like ``-f -s -S -L``.
A ``-H`` value can carry an explicit ``Authorization`` header, but that is a
user decision; Mesh never derives one for a generic download.

``-w`` prints the selected metrics after the transfer, including when ``-s``
is used or the transfer fails. When ``-w`` is present, successful metadata
moves to stderr so stdout remains suitable for scripts. Because repository
imports do not expose a single HTTP transfer, ``-w`` selects file mode for an
automatically detected URL and is rejected with explicit repository options.

This is a focused downloader, not a complete curl emulator. Uploads, FTP,
plugin protocols, remote header names and the full curl write-out variable set
remain outside the command. Mesh always verifies TLS unless ``-k`` is given,
and always fails file downloads on HTTP error responses.

Example::

   $ mesh obtain -L -C - -o image.iso https://example.com/image.iso
   obtained image.iso
     size: 812.4 MiB   resumed from: 104857600 bytes
     blake2b: 3916b7c767fde0cc...

Only ``http://`` and ``https://`` URLs are accepted. Redirects are disabled
unless ``-L``/``--location`` is explicitly given, so credentials and upload
bodies are never silently sent to another origin. An ``https`` source may not
redirect down to ``http``, and repository mode refuses ``-L`` when a bearer
credential would be attached. When a matching bearer credential would be sent,
HTTP is rejected; use HTTPS. File mode does not attach credentials and
therefore does not trigger that rule. ``-k`` is likewise refused in repository
mode when ``MESH_TOKEN`` or matching repository credentials would be sent; a
proxy or a TLS override must never turn a bearer token into clear text.

Security and status
~~~~~~~~~~~~~~~~~~~

The command exits with status 0 on success, 1 on a network, transfer,
extraction, snapshot or destination error, and 2 for an invalid URL,
reference, option or mode conflict. Archive names are validated with the same
rules as remote manifests: absolute paths, empty/``.``/``..`` components, ASCII
control bytes, the reserved ``.mesh``/``.git`` namespaces, hard links and
special files are rejected. Gzip CRC and size trailers are verified before the
staging tree is published.

``mesh fetch`` remains as a deprecated compatibility alias for file mode. New
scripts should use ``mesh obtain --file`` or rely on automatic detection.


mesh auth
---------

Stores or reports the bearer token used for remote calls.

.. code-block:: sh

   mesh auth ghp_exampleTokenValue
   mesh auth --host mesh.example.com ghp_exampleTokenValue
   mesh auth

With a token argument, Mesh validates that the token is non-empty, at most
4096 bytes and contains only printable ASCII characters without spaces. It
writes an entry to ``.mesh/credentials`` with mode ``0600`` and a trailing
newline. Existing entries for other hosts are preserved.

The modern credential line is ``<host> <token>``. ``--host HOST`` stores a
token scoped to that exact authority, including an explicit port when one is
present. A plain ``mesh auth TOKEN`` stores the legacy wildcard ``* token``
entry, which applies to every origin. When several entries exist, an exact
host match wins over the wildcard.

With no token, it prints ``Remote credentials stored.`` or
``No credentials stored.``. It never prints the token itself.

When a matching token is present, ``network.c`` adds:

.. code-block:: text

   Authorization: Bearer <token>

Credentials are local to the repository. They are not copied by ``mesh save``
because ``.mesh`` is excluded from the working-tree scan. File downloads through
``mesh obtain`` never read repository credentials.


mesh send
---------

POSTs the current snapshot and a bounded history window as JSON to a remote
endpoint.

.. code-block:: sh

   mesh send https://api.example.com/mesh/snapshots

The command requires at least one snapshot. It builds a JSON object in memory
with these fields:

.. code-block:: json

   {
     "mesh_version": "1.0",
     "action": "snapshot",
     "head": "<128 hex chars>",
     "tree": "<128 hex chars>",
     "parent": "<128 hex chars or empty>",
     "branch": "main",
     "message": "latest message",
     "author_name": "Ada Lovelace",
     "author_email": "ada@example.com",
     "snapshots": 7,
     "files": 42,
     "bytes": 1258291,
     "manifest": "F <hash> <size> <mtime-sec> <mtime-nsec> <mode> <kind> <path>\n...",
     "history": [
       {
         "hash": "<128 hex chars>",
         "tree": "<128 hex chars>",
         "parent": "<128 hex chars or empty>",
         "timestamp": 1792515685123456789,
         "message": "add parser",
         "author_name": "Ada Lovelace",
         "author_email": "ada@example.com"
       }
     ],
     "repo_icon_source": "mesh_icon.svg"
   }

The ``manifest`` field is included only when the manifest text is at most
4 MiB after escaping. The ``history`` array contains at most 64 nodes, newest
first. The history ``timestamp`` value is nanoseconds since the Unix epoch,
despite retaining the short key name. ``author_name`` and ``author_email`` are
empty strings for legacy snapshots without identity. The response body is read into memory
(ceiling 64 MiB) and its first 400 bytes are printed.

``mesh send`` does not upload objects. Use ``mesh sync`` for full object
transfer. The command exits with status 1 if the HTTP status is outside
``2xx`` or if the repository has no snapshots.


mesh sync
---------

Performs a bidirectional synchronization with a Mesh peer.

.. code-block:: sh

   mesh sync https://mesh.example.com/repo.git

The exchange has four phases.

**1. Announce local identity.**
Mesh sends a GET request to the base URL with:

.. code-block:: text

   X-Mesh-Hash: <local HEAD or empty>
   X-Mesh-Tree: <local tree or empty>
   Accept: text/plain

The response body is the remote manifest. The peer announces its own identity
in ``X-Mesh-Hash`` and ``X-Mesh-Tree`` response headers. A ``304`` response, or
a ``2xx`` response whose ``X-Mesh-Hash`` equals the local HEAD, ends the command
with ``already up to date``. Any other status outside ``2xx`` is rejected
before the header comparison, so an error page that echoes the request hash
cannot be mistaken for a successful no-op.

**2. Pull missing objects.**
For every entry in the remote manifest, Mesh requests
``GET <base>/objects/<hash>`` when the object is absent locally. Large files
are fetched chunk by chunk. Objects are downloaded to a temporary name,
verified by their hash, renamed into the store and made read-only (``0444``).

**3. Merge or isolate.**
For each remote path:

* If the local tree already has the same hash and kind, nothing is replaced.
  A missing working file is restored from the manifest, and a mode-only
  difference is applied when the working content still matches the remote
  hash.
* If the path is new and absent from the working tree, it is written with its
  recorded mode, timestamp and symlink target.
* If the working file already carries the remote bytes, it is accepted without
  a conflict, even if the file is not yet in the local snapshot.
* Otherwise, file and symlink variants are staged in
  ``.mesh/store/conflicts`` with their kinds and modes. The conflicting working
  path is not modified. Directories and special files are refused.

If one or more conflicts were staged, the command exits non-zero and does not
POST the local snapshot. It also stops before GET when conflicts are already
pending. Choose a variant and save the resolved tree before retrying. Other
non-conflicting paths may already have been materialized during the pull.

**4. Push snapshot metadata and missing objects (only with no pending conflicts).**
Mesh POSTs the local snapshot JSON to the base URL. The response may contain:

.. code-block:: json

   {"ok": true, "missing_objects": ["<hash>", "<hash>"]}

Each listed object is uploaded with ``PUT <base>/objects/<hash>``. A successful
``2xx`` response completes the phase. Mesh only uploads hashes reachable from
the local snapshot tree; a peer that asks for a foreign hash triggers a warning
and the hash is skipped. This prevents the peer from turning the client into an
arbitrary object-store reader. A local hash that is listed but missing from the
object store is also a warning, not a fatal error.

Plain ``http://`` to a non-loopback host is accepted for compatibility but
prints ``mesh: sync: warning: unencrypted HTTP to a non-loopback host``. Use
HTTPS or a trusted tunnel when the network is not trusted.

Example::

   $ mesh sync http://127.0.0.1:8765
   mesh: sync: snapshot not published; resolve conflicts and save before retrying
   $ mesh choose beta
   resolved 1 conflict(s) with beta
   $ mesh save "resolve"

Sync does not create a snapshot automatically. After a pull or a resolution,
run ``mesh save`` to record the new working state in local history. See
:doc:`network` for the complete wire protocol, header rules and server
expectations. Version 1.0 is not a multi-writer merge protocol: it has no
common-ancestor merge, general three-way deletion propagation or server-side
compare-and-swap by default. A peer advertising ``X-Mesh-CAS: if-match`` and an
``ETag`` receives a conditional ``If-Match`` publish; HTTP 409/412 means the
remote changed after GET and the snapshot was not published. Legacy peers do
not protect concurrent writers, so serialize updates to those peers.


Status File
-----------

Every command that runs inside a repository rewrites
``.mesh/monitor/status.json``. The document is a single flat JSON object. It
includes both the repository state and a ``"repo_icon_source"`` key whose value
is always ``"mesh_icon.svg"``.

.. code-block:: json

   {
     "mesh_version": "1.0",
     "repo_root": "/home/user/demo",
     "repo_name": "demo",
     "branch": "main",
     "head": "<128 hex chars>",
     "head_short": "<12 hex chars>",
     "parent": "<128 hex chars or empty>",
     "tree": "<128 hex chars>",
     "snapshots": 7,
     "files": 42,
     "bytes": 1258291,
     "chunks": 120,
     "conflicts": 0,
     "state": "ready",
     "changed": {"added": 0, "modified": 0, "deleted": 0, "bytes": 0},
     "last_operation": "save",
     "message": "add parser",
     "author_name": "Ada Lovelace",
     "author_email": "ada@example.com",
     "last_path": "",
     "download_bytes": 0,
     "resumed_from": 0,
     "download_hash": "",
     "timestamp": 1792515685,
     "repo_icon_source": "mesh_icon.svg"
   }

The field contract is documented in :doc:`gui_integration`, together with the
recommended polling strategy and conflict workflow.


Examples
--------

A complete local session:

.. code-block:: sh

   mkdir notes && cd notes
   mesh start
   printf 'alpha\n' > a.txt
   mesh save "initial"
   printf 'beta\n' >> a.txt
   mesh save "extend a"
   mesh check

A download that survives a dropped connection:

.. code-block:: sh

   mesh obtain https://example.com/big.iso
   # connection drops at 40%
   mesh obtain https://example.com/big.iso
   # resumes from the bytes already on disk

A remote workflow:

.. code-block:: sh

   mesh auth "$MESH_TOKEN"
   mesh sync https://mesh.example.com/project
   mesh choose beta
   mesh save "merge remote"
   mesh send https://api.example.com/mesh/snapshots

.. seealso::
   For the current implementation boundaries and safe operating mitigations, see
   :doc:`limitations`.
