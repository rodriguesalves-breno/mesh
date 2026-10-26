GUI and IDE Integration
=======================

Mesh has no daemon, no IPC socket and no plugin API. Its integration surface is
a single file:

.. code-block:: text

   <repository>/.mesh/monitor/status.json

Every operation that changes repository state rewrites that file atomically.
A graphical interface reads it, parses it as ordinary JSON and renders
whatever it finds. No handshake is required, no version negotiation is
required, and an interface can disappear and reconnect without telling Mesh.

This document defines the contract for editors, status bars, file managers and
custom GUIs.

.. contents::
   :depth: 2
   :local:


Why a File
----------

A file is the lowest common denominator that is still fast enough.

* It works across process boundaries and language runtimes.
* It survives editor restarts and crashes.
* It can be inspected with ``cat`` when something goes wrong.
* It requires no port allocation, no authentication and no protocol version.
* It is written by rename, so a reader never observes a partially written
  document.

The cost is polling. That cost is negligible: a status document is typically
under 2 KiB and is rewritten only after a command. An editor that watches the
file's modification time can read it only when it changes.

Do not open the file on every animation frame. Open it when:

* the editor starts;
* the file's ``mtime`` or size changes;
* a Mesh command started by the editor exits;
* the user explicitly refreshes the view.

A 500 ms to 2 s timer is more than adequate for a background status bar. The
file is not a high-frequency telemetry stream.


Document Schema
---------------

The document is a flat JSON object with one nested object. All keys are
lowercase with underscores. Unknown keys must be ignored by readers; new keys
may be added in a minor release.

.. list-table::
   :header-rows: 1
   :widths: 20 10 70

   * - Key
     - Type
     - Meaning
   * - ``mesh_version``
     - string
     - Always ``"1.0"`` for this release.
   * - ``repo_root``
     - string
     - Absolute path of the working tree. JSON-escaped.
   * - ``repo_name``
     - string
     - Last path component of ``repo_root``.
   * - ``branch``
     - string
     - Active branch, normally ``"main"``.
   * - ``head``
     - string
     - Full 128-character snapshot hash, or empty before the first save.
   * - ``head_short``
     - string
     - First 12 characters of ``head``; convenient for labels.
   * - ``parent``
     - string
     - First parent hash, or empty.
   * - ``tree``
     - string
     - Full tree hash, or empty before the first save.
   * - ``snapshots``
     - integer
     - Number of nodes in local history.
   * - ``files``
     - integer
     - Number of files in the HEAD tree.
   * - ``bytes``
     - integer
     - Sum of file sizes in the HEAD tree.
   * - ``chunks``
     - integer
     - Number of chunk records across all files in HEAD.
   * - ``conflicts``
     - integer
     - Number of entries in the conflict index.
   * - ``state``
     - string
     - ``"ready"`` when ``conflicts`` is zero, ``"conflicted"`` otherwise.
   * - ``changed``
     - object
     - ``{"added": n, "modified": n, "deleted": n, "bytes": n}`` relative to
       the parent snapshot at save time. Other operations may leave zeros.
   * - ``last_operation``
     - string
     - Command that wrote the file: ``start``, ``save``, ``obtain``, ``send``,
       ``sync`` or ``choose``. Readers must tolerate other values.
   * - ``message``
     - string
     - Human-readable summary of the operation. JSON-escaped.
   * - ``author_name``
     - string
     - Author name recorded in the HEAD snapshot; empty when the snapshot has
       no identity. JSON-escaped.
   * - ``author_email``
     - string
     - Author e-mail recorded in the HEAD snapshot; empty when absent.
       JSON-escaped.
   * - ``last_path``
     - string
     - Path touched by the last download, when applicable; otherwise empty.
   * - ``download_bytes``
     - integer
     - Final size of the last download, when applicable.
   * - ``resumed_from``
     - integer
     - Bytes already present before the last download resumed; zero otherwise.
   * - ``download_hash``
     - string
     - BLAKE2b-512 digest of the last download, when applicable.
   * - ``timestamp``
     - integer
     - Unix time in seconds when the document was written.
   * - ``repo_icon_source``
     - string
     - Always ``"mesh_icon.svg"``. The icon lives at
       ``<repo_root>/mesh_icon.svg``.

Example document:

.. code-block:: json

   {
     "mesh_version": "1.0",
     "repo_root": "/home/user/demo",
     "repo_name": "demo",
     "branch": "main",
     "head": "5d08affd9cb2e6d7498c3179941d0599147f0e363a4e835d374bd8d7ccb12f9c95ee25d61cf8b574a4511cc8aabdb8d95ae8bcde9f32f7b08f148ebf296021f8",
     "head_short": "5d08affd9cb2",
     "parent": "",
     "tree": "76fe60df789791d49399d15737090e149a2080cc0706a669f1be8f531900cd76d800f33cd0f9dc759310df84928920be8bb2e44e0664981c72cdff52de08e934",
     "snapshots": 1,
     "files": 4,
     "bytes": 13822,
     "chunks": 0,
     "conflicts": 0,
     "state": "ready",
     "changed": {"added": 0, "modified": 0, "deleted": 0, "bytes": 0},
     "last_operation": "save",
     "message": "local state",
     "author_name": "Ada Lovelace",
     "author_email": "ada@example.com",
     "last_path": "",
     "download_bytes": 0,
     "resumed_from": 0,
     "download_hash": "",
     "timestamp": 1792515685,
     "repo_icon_source": "mesh_icon.svg"
   }

The file is UTF-8. Strings are escaped for quotes, backslashes and control
characters. A reader should use a real JSON parser rather than line-based
matching; the field order is stable in this release but is not part of the
contract.


State Machine
-------------

Only two persisted states exist. Transient states are not written, because
Mesh commands are synchronous: while a save is running, the old status remains
valid and the new one appears only when the work is complete.

.. code-block:: text

   (no status file)
          |
          | mesh start
          v
   +--------------+   sync stages; skips POST  +--------------+
   |    ready     | -------------------------> |  conflicted  |
   +--------------+                            +--------------+
          ^                                            |
          |              mesh choose alpha|beta        |
          +--------------------------------------------+
          |
          | save / obtain / send / sync (no conflicts)
          v
        ready

Practical rules for a UI:

* Missing file: show "not initialized" or "no status yet"; do not treat it as
  an error. ``mesh start`` creates it.
* ``state == "ready"``: show the branch, short head and file count normally.
* ``state == "conflicted"``: show a prominent conflict badge with the count and
  offer resolution actions. The working tree may still build; do not block the
  editor.
* ``head == ""``: the repository exists but has no snapshots. Show "no
  snapshots" and suggest ``mesh save``.
* ``last_operation == "obtain"``: the download fields are meaningful. Show the
  hash and, when nonzero, the resumed byte count.
* Unknown ``last_operation`` or unknown keys: ignore them and keep rendering
  the known fields.

Do not infer state by scanning the file system. The status document is the
contract precisely so that a GUI does not need to reimplement the scanner.


Reading the File Safely
-----------------------

The writer uses write-then-rename, so an open descriptor always refers to a
complete document. Still, a reader can race a replacement between ``stat`` and
``open``. The safe pattern is:

1. ``stat`` the file and remember size and ``mtime``.
2. Open and read the whole file.
3. Parse JSON.
4. If parsing fails, wait briefly and retry once; a reader on a network file
   system may have observed a stale directory cache.
5. If the retry fails, display a non-blocking warning and keep the last good
   document.

Never lock the file. The writer does not take a reader lock, and advisory locks
would only create contention. Atomic replacement is the synchronization
mechanism.

Example in JavaScript (Node or Electron renderer with ``fs``):

.. code-block:: javascript

   const fs = require('fs');
   const path = require('path');

   function readStatus(repoRoot) {
     const file = path.join(repoRoot, '.mesh', 'monitor', 'status.json');
     try {
       const text = fs.readFileSync(file, 'utf8');
       return JSON.parse(text);
     } catch (err) {
       if (err.code === 'ENOENT') return null;
       throw err;
     }
   }

   function label(status) {
     if (!status) return 'Mesh: not initialized';
     if (status.state === 'conflicted')
       return `Mesh: ${status.conflicts} conflict(s)`;
     if (!status.head) return 'Mesh: no snapshots';
     return `Mesh: ${status.branch} @ ${status.head_short}`;
   }

In a browser context, the same document can be served by a local development
server. Do not expose ``.mesh/credentials``; it is not part of the status
document and should never be copied into a web root.


Icon Resolution
---------------

The official icon is ``mesh_icon.svg``: a black triangle with graph nodes on a
Persian blue background (``#003153``). The status key ``repo_icon_source`` is
always the file name, not an absolute path. Resolve it as:

.. code-block:: text

   <repo_root>/<repo_icon_source>

``mesh start`` copies the icon next to the working tree when it is absent. It
does not overwrite an existing file with the same name. The installer places a
copy next to the binary and in ``PREFIX/share/mesh``.

``mesh start`` also writes a VS Code manifest when the user has no settings
file:

.. code-block:: json

   {
     "files.associations": {
       "*.meshconfig": "json",
       "status.json": "json"
     },
     "workbench.iconTheme": "mesh-icons",
     "mesh.iconPath": "${workspaceFolder}/mesh_icon.svg"
   }

The rules are conservative:

* If ``.vscode/settings.json`` does not exist, Mesh writes the document above.
* If it exists and already contains ``"mesh.iconPath"``, Mesh assumes it owns
  the file and leaves it alone.
* If it exists without that key, Mesh never overwrites it. The user may have
  hand-written settings, and an installer has no right to destroy them.
* ``mesh.iconPath`` is a custom key. An extension that contributes the
  ``mesh-icons`` theme reads it; VS Code itself ignores unknown keys.

An extension should therefore check both the custom key and the conventional
``<repo_root>/mesh_icon.svg`` path. The status file's ``repo_icon_source`` is
the tie-breaker if a future release renames the asset.


Conflict Workflow for Interfaces
--------------------------------

Conflicts are the only place where a GUI needs more than a status badge.

The facts:

* The working file is untouched. It contains neither local nor remote markers;
  it contains exactly what the user had before the sync.
* For supported file and file/symlink conflicts, the two pure variant payloads
  are in ``.mesh/store/conflicts/<16-hex-path-hash>-<basename>.alpha`` and the
  matching ``.beta`` file. Payloads are ordinary private files; the typed index
  says whether choosing each payload installs a regular file or symlink, and
  records its mode. The stem comes from a BLAKE2b digest of the full path.
* The index at ``.mesh/store/conflicts/index`` stores an escaped
  repository-relative path plus variant kind/mode metadata. Legacy path-only
  and ``<key>\t<escaped-path>`` indexes remain readable as regular-file pairs;
  a legacy key is ignored.
* ``alpha`` is the local variant; ``beta`` is the remote variant. The resolver
  safely materializes a chosen symlink via a temporary sibling and rename.
  Directories and special filesystem objects are not supported variants.
* ``mesh choose alpha`` or ``mesh choose beta`` applies one variant to every
  listed path, removes both staged files and rewrites the index.
* ``mesh choose`` with no argument lists the conflicts.
* After choosing, run ``mesh save "resolve"`` to record the result.

A recommended UI:

.. code-block:: text

   +--------------------------------------------------+
   | 2 conflicts need a decision                      |
   |                                                  |
   | src/parser.c     [view alpha] [view beta]        |
   | docs/index.rst   [view alpha] [view beta]        |
   |                                                  |
   | [Use local for all]  [Use remote for all]        |
   +--------------------------------------------------+

When the user selects a variant for a single file, a per-file command does not
exist in version 1.0. The command applies the chosen variant to every staged
conflict. A UI that wants per-file control should:

1. read the index;
2. present the diff of the two staged files;
3. call ``mesh choose alpha`` or ``mesh choose beta`` only after the user
   accepts the batch scope, or resolve files in separate sync/choose rounds.

Do not edit the working file directly and then delete the staged variants.
That bypasses the index and can leave ``conflicts`` nonzero forever. Use the
command, then refresh the status document.

Displaying a diff is safe because staged payloads are ordinary private files;
for a symlink endpoint, present the payload as a link target rather than
following it. Do not display a synthetic three-way merge unless the extension
implements one itself. Mesh does not store a base version at conflict time; it
stores the two endpoints.

When a sync discovers conflicts, it leaves the conflicting working files
untouched and does not POST the local snapshot. It exits non-zero with an
actionable diagnostic. A subsequent sync also stops before contacting the
peer while the conflict index is non-empty. The UI should show the staged
variants, explain that choosing applies to the full batch, and prompt for
``mesh save`` before retrying. A pull may already have materialized other,
non-conflicting paths, so refresh status and inspect the tree even when the
sync process exits with an error.


Invoking Commands
-----------------

A GUI should spawn the binary with an argument vector, not through a shell.

.. code-block:: javascript

   const { spawn } = require('child_process');

   function mesh(repoRoot, args) {
     return new Promise((resolve, reject) => {
       const child = spawn('mesh', args, { cwd: repoRoot });
       let stderr = '';
       child.stderr.on('data', d => { stderr += d.toString(); });
       child.on('error', reject);
       child.on('close', code => {
         if (code === 0) resolve();
         else reject(new Error(stderr.trim() || `mesh exited ${code}`));
       });
     });
   }

   // Example: save after the user edits a file.
   await mesh(repoRoot, ['save', 'editor autosave']);

Guidelines:

* Pass the repository root as ``cwd``. Do not build a command string by
  concatenating a message; messages may contain spaces and quotes.
* Treat exit status 1 as a runtime failure and status 2 as a UI bug (invalid
  arguments). Show stderr, not a generic "operation failed".
* Debounce autosave. ``mesh save`` hashes the whole tree; calling it on every
  keystroke is wasteful. Save on explicit user action, on window blur, or after
  a quiet period measured in seconds.
* Do not run two mutating commands concurrently in the same repository.
  History appends and conflict index updates are locked, but a save and a sync
  racing each other produce confusing state. Serialize commands in the
  extension.
* Refresh the status file after the child exits, not before.

``mesh check`` is a cheap way to read and format the status for a terminal
panel. When the cached ``head`` matches ``.mesh/HEAD`` it does not rescan the
tree, so it is safe to call from a command palette action. A stale cache is
rebuilt once.


Progress and Long Operations
-----------------------------

Mesh does not write incremental progress to ``status.json``. A save or sync
writes the document once, at the end. For long operations, a UI should show an
indeterminate spinner and stream the child's stdout/stderr into an output
channel.

If a future release adds progress fields, they will be additive keys. Readers
that ignore unknown keys will remain compatible. Do not assume that
``timestamp`` changes during an operation; it changes when the document is
written.

For downloads, ``download_bytes`` is the final size, not a live counter.
``resumed_from`` tells the user how much was already on disk. A progress bar
driven by these fields would be wrong; use the process output instead.


Multi-Repository Workspaces
---------------------------

Each repository has its own status file. A workspace with three Mesh
repositories has three independent documents. There is no global registry and
no workspace-level status.

A UI should:

* discover repositories by looking for ``.mesh/config`` in the workspace
  folders, not by scanning the entire disk;
* key its state by the canonical ``repo_root`` path;
* handle a repository that is moved or renamed by treating the old status as
  stale and re-reading after the move;
* avoid assuming that the workspace root is the repository root. Mesh searches
  upward, so a project can live in a subdirectory.

Nested repositories are not supported by the scanner: a ``.mesh`` directory
inside another repository's working tree is skipped only if it is named
``.mesh`` at any depth, so a nested repository's metadata is excluded but its
files are still part of the outer snapshot. Keep repositories separate unless
you intend that behaviour.


Accessibility and Labels
------------------------

Use text, not color alone, to communicate state.

.. list-table::
   :header-rows: 1
   :widths: 18 82

   * - State
     - Suggested label
   * - No status
     - "Mesh: not initialized"
   * - No snapshots
     - "Mesh: no snapshots yet"
   * - Ready
     - "Mesh: main at 5d08affd9cb2"
   * - Conflicted
     - "Mesh: 2 conflicts need a decision"
   * - Fetch
     - "Mesh: downloaded image.iso (812.4 MiB, resumed 100 MiB)"
   * - Error
     - "Mesh: last operation failed; see output"

Avoid showing the full 128-character hash in a status bar. Use
``head_short``. Show the full hash in a tooltip or details panel. The full value
is useful for comparing two machines; the short value is for humans.


Compatibility Rules
-------------------

The status document is a public interface. Changes follow these rules.

* Existing keys keep their meaning and type for the lifetime of the major
  version.
* New keys may be added at any time. Readers must ignore unknown keys.
* A key may be deprecated but not removed in a minor release.
* ``state`` remains one of ``"ready"`` or ``"conflicted"`` in version 1.x.
* ``repo_icon_source`` remains the file name of the official icon.
* The file path remains ``.mesh/monitor/status.json``.

If a breaking change is unavoidable, ``mesh_version`` changes and the old path
continues to be written for one transition release. A GUI should compare the
major version and degrade gracefully rather than refusing to render.


Implementation Notes
--------------------

The status writer in ``src/monitor.c`` is the clearest example of the Alves
style in the integration surface. It is a flat sequence of ``APPEND``
invocations into one growable buffer, one field per line. On the reader side,
``json_unescape_char()`` is a small helper with one job and ``json_field()``
is its guarded counterpart. The writer has no JSON library, no schema file
and no nested success pyramid.

The rewrite did not change a single key, type or path in the contract above.
The document is still UTF-8 JSON, still replaced atomically and still
rewritten only at command boundaries. An integration should continue to
ignore unknown keys and to rely on the documented fields, not on the order in
which the writer emits them.

Read :doc:`alves_style_manifesto` for the layout rationale and
:doc:`complexity_guardrails` for the nesting limit. Neither is part of the
status-file contract; they explain why the producer is small enough to audit
when a field is missing.


Minimal Integration Checklist
-----------------------------

.. list-table::
   :header-rows: 1
   :widths: 12 88

   * - Done
     - Requirement
   * - [ ]
     - Locate repositories by ``.mesh/config``.
   * - [ ]
     - Read ``.mesh/monitor/status.json`` as UTF-8 JSON.
   * - [ ]
     - Ignore unknown keys and tolerate missing optional fields.
   * - [ ]
     - Render ``state``, ``branch``, ``head_short`` and ``conflicts``.
   * - [ ]
     - Resolve the icon from ``repo_root`` and ``repo_icon_source``.
   * - [ ]
     - Offer ``choose alpha`` / ``choose beta`` when conflicts exist.
   * - [ ]
     - Spawn commands with an argument vector and the repository as ``cwd``.
   * - [ ]
     - Serialize mutating commands.
   * - [ ]
     - Refresh after the child exits and after the file ``mtime`` changes.
   * - [ ]
     - Never read or display ``.mesh/credentials``.

That is the entire contract. A working integration can be a few hundred lines
of code, which is the point: the version control system should not require a
second version control system for its own user interface.

.. seealso::
   For the current implementation boundaries and safe operating mitigations, see
   :doc:`limitations`.
