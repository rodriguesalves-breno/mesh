Architecture
============

Mesh is a single-process, multi-threaded C17 program. Its runtime dependencies
are libcurl, ICU4C (``icu-uc``), pthreads and the C library; it has no
background daemon and no configuration language. The source follows the Alves style described in
:doc:`alves_style_manifesto`. This document describes the internal
structures, the on-disk formats and the ownership rules that keep the
implementation predictable.

.. contents::
   :depth: 2
   :local:


Process Model
-------------

``main.c`` calls ``net_init()``, invokes ``cli_main()``, then calls
``net_shutdown()``. All command work happens synchronously inside the calling
process. Threads are created only for hashing, and only for the duration of a
hash job. There is no global mutable repository state: each command owns a
``struct mesh_repo`` on its stack and passes it by pointer.

The files shared by independent Mesh processes are protected explicitly:

* ``mesh save``, ``mesh sync`` and ``mesh choose`` take an exclusive lock on
  ``.mesh/lock`` for the whole read-modify-write operation.
* ``.mesh/history`` is appended under an exclusive ``flock`` as well, which
  protects writers that do not hold the repository lock.
* ``.mesh/store/conflicts/index`` is appended under an exclusive ``flock``.
* Every other document is replaced atomically with write-then-rename and the
  containing directory is ``fsync``-ed, so readers see either the old file or
  the new file even after a crash.

Repository discovery walks upward from ``getcwd()`` looking for
``.mesh/config``. It stops at ``/`` and returns ``ENOENT`` when no repository
exists. A directory that contains ``.mesh`` but no ``config`` is not a
repository.


Module Map
----------

.. list-table::
   :header-rows: 1
   :widths: 18 82

   * - File
     - Responsibility
   * - ``src/main.c``
     - Process entry point and libcurl lifetime.
   * - ``src/core.h``
     - Constants, structs, prototypes and small side-effect-free helpers. The
       helpers are ``static inline`` to avoid an extra utility object file.
   * - ``src/cli.c``
     - Argument parsing, command routing, working-tree scan, snapshot
       creation, obtain (clone/download), send/sync orchestration and status
       updates.
   * - ``src/env.c``
     - Repository discovery, initialization at an explicit root, local/global
       configuration, identity resolution, shell PATH injection, icon
       provisioning and VS Code manifest generation.
   * - ``src/inflate.c``
     - Streaming gzip and DEFLATE decoder with CRC32 and ISIZE verification.
   * - ``src/archive.c``
     - Incremental tar reader that materializes a forge archive below one
       directory descriptor with ``O_NOFOLLOW``.
   * - ``src/obtain.c``
     - Forge URL mapping, staging-directory lifecycle, archive download and
       single-rename publication.
   * - ``src/network.c``
     - libcurl engine: downloads, GET, POST, PUT, bearer auth, headers and the
       in-memory response buffer.
   * - ``src/blake2b.c``
     - RFC 7693 BLAKE2b-512 implementation, file and buffer hashing, and the
       parallel hash pool.
   * - ``src/chunking.c``
     - Content-defined chunk boundaries and the immutable object store.
   * - ``src/graph.c``
     - Manifest parse/serialize, tree identity, snapshot nodes and history
       traversal.
   * - ``src/conflict.c``
     - Conflict staging, listing and resolution.
   * - ``src/monitor.c``
     - Manual JSON serialization of the passive status document.
   * - ``src/bridge.c``
     - Git-compatible unified patch export.
   * - ``src/git_coexist.c``
     - Read-only detection of ``.git`` beside ``.mesh``; never follows or modifies Git metadata.
   * - ``src/unicode.c``
     - UTF-8 validation and locale-independent NFC/full-casefold collision keys using ICU.
   * - ``src/ignore.c``
     - ``.meshignore`` parsing, glob matching and directory pruning for the
       working-tree scanner.
   * - ``src/branch.c``
     - Named branch refs, branch listing and the current-branch pointer.
   * - ``src/tag.c``
     - Lightweight tags stored as refs below ``.mesh/refs/tags``.
   * - ``src/reflog.c``
     - Append-only log of local HEAD movements and branch switches.
   * - ``src/clean.c``
     - Dry-run and forced removal of working-tree files that HEAD does not
       track, with symlink-safe deletion.
   * - ``src/admin.c`` and ``src/admin_*.c``
     - Read-mostly administration plane: router, registry, doctor, verification,
       inventory, collection, ref, status, audit, quota, config, history and
       JSON export modules.

The header is the contract. Every public function has one owner file and a
single, narrow purpose.


Refs, Ignore Rules and Reflog
-----------------------------

The extension modules keep their state in plain text and reuse the atomic
document discipline:

* ``.mesh/refs/<branch>`` and ``.mesh/refs/tags/<tag>`` contain one full
  128-character snapshot hash followed by a newline. A ref name is one path
  component, at most 63 bytes, and cannot contain whitespace, control bytes,
  ``/``, ``\``, ``|``, ``~``, ``^``, ``:``, ``?``, ``*``, ``[``, ``]``,
  ``..``, ``@{`` or ``.lock``.
* The current branch is the ``branch=`` key in ``.mesh/config``. Switching
  writes that key, ``.mesh/HEAD`` and ``.mesh/refs/<branch>`` under the
  repository lock.
* ``.mesh/logs/HEAD`` is an append-only reflog with one line per HEAD
  movement: ``timestamp|old-hash|new-hash|actor|message``. Writes take an
  exclusive ``flock`` and are ``fsync``-ed before the descriptor is closed.
* ``.mesh/logs/admin.log`` is the local audit trail written by
  ``mesh admin audit``. ``.mesh/admin/quota`` is a single decimal byte ceiling;
  neither file is hashed into a snapshot.
* ``.meshignore`` is a root-level rule file. Ignored directories are pruned
  from the scan unless a negated rule can re-include a descendant. Matching is
  byte-oriented and supports ``*``, ``?`` and ``**``; it does not implement
  character classes or nested ignore files.
* ``mesh clean`` derives the tracked set from HEAD's manifest. There is no
  index to consult, so a path absent from that manifest is untracked. Removal
  uses ``openat()``/``unlinkat()`` and never follows a symbolic link.

These files are local metadata. They are not hashed into snapshots and are not
sent by ``mesh send`` or ``mesh sync``.


Forge Archive Import
--------------------

``mesh obtain`` has three ownership boundaries. ``src/obtain.c`` validates the
source URL, derives the forge archive route and the default destination, and
owns the staging directory. ``src/inflate.c`` decodes gzip/DEFLATE through a
callback without allocating the archive in memory. ``src/archive.c`` consumes
that callback and materializes tar entries below one open directory descriptor.

The staging directory is created with ``mkdtemp()`` as a sibling of the final
destination, so publication is a single ``rename()`` on the same filesystem. A
new repository is initialized there with ``mesh_repo_init_at()`` before the
download starts. The archive is stored at
``.mesh/store/tmp/obtain.tar.gz`` inside the staging repository, removed after
extraction, and therefore never enters the first snapshot. The caller runs the
normal snapshot path in the staging directory; only a successful save reaches
``mesh_obtain_commit()``. Failure calls ``mesh_obtain_abort()``, which removes
the staging tree with the same descriptor-relative, no-follow recursion used by
the extractor.

The tar reader strips exactly one leading path component, the archive's root
directory. Every remaining name is passed through ``mesh_relpath_safe()``
before any file is opened. Directories are opened with ``O_NOFOLLOW``, regular
files are written through a temporary name and renamed, and symlinks are
created with ``symlinkat()``. A later entry below a symlink therefore fails
instead of escaping. Hard links and device/fifo entries are rejected. PAX
extended headers and GNU long names are parsed into bounded local buffers;
unsupported metadata is ignored, but malformed records reject the archive.

The destination may be absent or an existing empty directory. A non-empty
destination is rejected before the network is touched. The final tree is
published with mode ``0755``; the staging directory is ``0700`` while it holds
untrusted content. ``mesh obtain`` imports tree content only. It does not create
Git commits, refs, reflogs or submodule entries in the Mesh store.


Code Style and Vertical Layout
------------------------------

The C source is written in the Alves style. Every function begins at column
zero, control statements use the compact form ``if(condition)``, braces bind
to the construct they close (``} else {``, ``} while(...)``), and blank lines
are not used as decoration. The compiler ignores the layout; the reader does
not. On a narrow SSH terminal, the saved columns and rows are the difference
between a function that fits on one screen and one that requires scrolling.

Two rules are more than cosmetic:

* A function may nest control constructs at most three levels below its body.
  Deeper logic is extracted into a helper or rewritten as a guard clause.
* A function should fit on one terminal screen at 100 columns or less. The
  current source has no line longer than 100 characters.

The conversion removed 1,041 physical lines, or 12.6% of the C source, while
the Makefile build remained warning-free. The measurement and its caveats are
in :doc:`metrics_and_line_optimization`; the control-flow rule is in
:doc:`complexity_guardrails`.

The style is not applied by a formatter. A formatter can remove whitespace,
but it cannot decide that a nested success pyramid should become three guard
clauses. The structural part of the change was reviewed function by function.


Data Structures
---------------

The core types are deliberately small and flat.

**Repository.**

.. code-block:: c

   struct mesh_repo {
   char root[MESH_PATH_MAX];
   char mesh_dir[MESH_PATH_MAX];
   char branch[64];
   char author_name[MESH_AUTHOR_MAX+1];
   char author_email[MESH_EMAIL_MAX+1];
   bool valid;
   };

``root`` is the absolute working-tree path, ``mesh_dir`` is ``root/.mesh`` and
``branch`` is read from ``.mesh/config`` (default ``main``). The effective
``author_name`` and ``author_email`` merge the local ``.mesh/config`` over the
global ``$XDG_CONFIG_HOME/mesh/config`` (or ``$HOME/.config/mesh/config``).
When neither file supplies an e-mail, ``EMAIL`` is the final fallback. The
fields remain empty when identity is not configured.

**Files and chunks.**

.. code-block:: c

   struct mesh_file_entry {
   char*path;
   char hash[MESH_HASH_HEX+1];
   uint64_t size;
   int64_t mtime;
   struct mesh_chunk*chunks;
   size_t nchunks;
   };

``path`` and ``chunks`` are owned by the entry. ``file_list_free()`` releases
both. A small file has ``nchunks == 0`` and is stored as one blob object. A
large file has a chunk array whose offsets are contiguous and whose lengths sum
to ``size``.

**Snapshots.**

.. code-block:: c

   struct mesh_node {
   char hash[MESH_HASH_HEX+1];
   char tree[MESH_HASH_HEX+1];
   char parent_1[MESH_HASH_HEX+1];
   char parent_2[MESH_HASH_HEX+1];
   char author_name[MESH_AUTHOR_MAX+1];
   char author_email[MESH_EMAIL_MAX+1];
   int64_t timestamp;
   char*message;
   struct mesh_node*p1;
   struct mesh_node*p2;
   struct mesh_node*next;
   };

The graph holds a pointer to the newest node and a count. ``next`` points to
the previous node, so the structure is a reverse linked list. Parent hashes are
resolved to pointers after loading, which makes traversal a pointer walk rather
than repeated searches. The format supports two parents; the current CLI
creates single-parent snapshots, leaving merge creation to a future command.

**HTTP response.**

.. code-block:: c

   struct mesh_http {
   char*data;
   size_t len;
   size_t cap;
   long status;
   char remote_hash[MESH_HASH_HEX+1];
   char remote_tree[MESH_HASH_HEX+1];
   bool truncated;
   };

``data`` is a NUL-terminated heap buffer. It is freed by ``mesh_http_free()``.
``remote_hash`` and ``remote_tree`` are filled from ``X-Mesh-Hash`` and
``X-Mesh-Tree`` response headers.


Hashing
-------

Content identity is BLAKE2b-512, producing a 64-byte digest rendered as 128
lowercase hexadecimal characters. ``core.h`` names the byte length
``MESH_HASH_LEN`` and the text length ``MESH_HASH_HEX``.

The implementation in ``blake2b.c`` follows RFC 7693. It supports three input
shapes:

* ``blake2b_hash_buffer()`` for memory buffers such as manifests, history
  signatures and small configuration blobs;
* ``blake2b_hash_fd()`` for a byte range of an open descriptor, used by chunk
  hashing;
* ``blake2b_hash_file()`` and ``blake2b_hash_jobs()`` for paths and parallel
  job arrays.

The parallel pool creates up to ``mesh_threads()`` workers, which is the number
of online processors capped at 64. Each worker takes the next job index under a
mutex, opens the file, seeks to the requested offset and hashes exactly the
requested length. Results are written into the caller's job array, so no
per-thread result allocation is required.

The internal implementation is always compiled into the binary. No external
BLAKE2 library is required, linked or probed.

Hashes are never trusted blindly when received from a peer. An object fetched
by hash is written under that hash only after the server response has been
consumed and the BLAKE2b digest has been verified. The manifest parser
validates that every hash field is exactly 128 lowercase hexadecimal
characters before it is used in a path.


Working-Tree Scan
-----------------

``scan_dir()`` walks the repository recursively.

* ``.`` and ``..`` are skipped.
* ``.mesh`` and ``.git`` are skipped entirely, including nested occurrences.
  The reserved-name check is ASCII case-insensitive for case-insensitive
  filesystems.
* ``lstat()`` is used, so a directory symlink is never followed. A symbolic
  link is recorded as a ``kind=l`` entry whose hash covers the link target.
* Directories are descended; regular files and symbolic links are added;
  everything else (sockets, devices, FIFOs) is ignored.
* The mode and nanosecond modification time of every entry are recorded.
* Nesting deeper than 64 levels is refused with a warning, which bounds stack
  usage and catches accidental loops.
* Paths are joined with a bounds-checked helper and must fit in
  ``MESH_PATH_MAX`` (4096 bytes).

After the scan, entries are sorted with ``strcmp`` on the repository-relative
path. Sorting makes manifests deterministic and allows the patch writer to
merge two tree listings with a linear two-pointer pass. Before a path is
written, ``mesh_relpath_safe()`` rejects absolute paths, empty components,
``.``, ``..`` and the reserved ``.mesh``/``.git`` namespaces.


Manifest Format
---------------

A tree is a UTF-8 text document. Each tracked object starts with an ``F``
line; a large regular file is followed by zero or more ``C`` lines.

.. code-block:: text

   F <hash> <size> <mtime-sec> <mtime-nsec> <mode> <kind> <escaped-path>
   C <chunk-hash> <offset> <length>
   C <chunk-hash> <offset> <length>

``mode`` is octal (for example ``644`` or ``755``). ``kind`` is ``f`` for a
regular file and ``l`` for a symbolic link. For a regular file, ``hash`` is
the BLAKE2b-512 digest of its contents. For a symbolic link, ``hash`` is the
digest of the link target bytes and ``size`` is the target length. The target
itself is stored as an ordinary object, so a symlink never becomes a path
traversal primitive. The nanosecond timestamp is preserved during
materialization.

Fields are separated by single spaces, except the path, which extends to the
end of the line and may contain spaces. The path is escaped as follows:

.. list-table::
   :header-rows: 1
   :widths: 20 80

   * - Byte
     - Escape
   * - backslash
     - ``\\``
   * - newline
     - ``\n``
   * - carriage return
     - ``\r``
   * - tab
     - ``\t``

The parser is fail-closed. It rejects the whole manifest when a hash is not
exactly 128 lowercase hexadecimal characters, a chunk offset is not
contiguous, chunk lengths do not sum to the declared size, a path is absolute,
contains ``.``/``..``/empty components, contains an ASCII control byte, or
enters the reserved ``.mesh`` or ``.git`` namespaces. It also rejects embedded
NUL bytes and invalid UTF-8. Paths colliding under Unicode NFC normalization
and full, locale-independent case-folding are rejected so the tree can be
materialized on common case-insensitive or normalization-aware file systems.
Original spelling is retained; only temporary comparison keys are normalized.
The path is parsed in
the line buffer, so a valid path near the 4095-byte limit is not lost to a
fixed metadata scratch buffer. 4095 bytes is the theoretical limit for the
repository-relative byte string, not a guarantee that a full absolute path of
that length fits: the repository root, separators, escaping and host file
system limits reduce the effective maximum. Path joins reject overflow with
``ENAMETOOLONG`` rather than returning a truncated path.

Before allocation, the parser enforces structural ceilings:
``MESH_MANIFEST_MAX_ENTRIES`` (262144 entries),
``MESH_MANIFEST_MAX_CHUNKS`` (1048576 chunk records in total) and
``MESH_MANIFEST_MAX_CHUNKS_PER_FILE`` (262144 per file). The network response
buffer is also bounded to 64 MiB. These ceilings keep hostile manifests from
causing unbounded allocation; a tree that exceeds a ceiling is rejected as a
whole, never truncated or partially accepted. Large files use one ``F`` record
per path and ``C`` records per chunk, so chunking reduces object transfer but
does not eliminate manifest metadata or these limits.

For backward compatibility, the legacy four-field form
``F <hash> <size> <path>`` is still accepted and interpreted as a regular
file with mode ``644`` and timestamp zero. New manifests always use the
seven-field form.

The tree hash is the BLAKE2b-512 digest of the exact manifest bytes. Manifest
records are written to a temporary file and hashed incrementally, avoiding a
second whole-manifest memory copy. Manifests are stored at
``.mesh/store/trees/<tree-hash>``. The store is itself content-addressed:
saving an identical tree does not rewrite the file. Loading remains bounded by
the manifest and response ceilings above.

Example manifest::

   F 01ba4719c80b6fe911b091a7c05124b64eeece964e09c058ef8f9805daca546b01ba4719c80b6fe911b091a7c05124b64eeece964e09c058ef8f9805daca546b 6 1792515685 123456789 644 f hello.txt
   F 3916b7c767fde0ccf185fde7eca3b04b1b5c27cae81a84d41aeb7440312e946f29bb5d27c52e7342d9ff0a9ff2a7534f0b60ec9a518d22afd7e60a1f0b236975 182 1792515685 0 644 f .vscode/settings.json
   F c247f82cfa3c210a16c66c47cb992dd014ccacfa15a8fd65895025c2774b89ecd7052e0cb52698d802ab90066077436feae00e2b6ef1ef4ad19325ce5e892231 13609 1792515685 0 644 f mesh_icon.svg

The modification time is part of the snapshot contract. A save checks it
before and after hashing/storage to detect a file that changed during the
operation, and materialization restores it. Content is still hashed on every
save; timestamps never substitute for a content digest.


Snapshot History
----------------

``.mesh/history`` is an append-only text file with one line per snapshot. Mesh
has no history-pruning or rewrite command; existing parent links, refs, tags
and remote copies make rewriting a separate compatibility operation. The
legacy seven-field form is still accepted:

.. code-block:: text

   <node-hash>|<tree-hash>|<timestamp-ns>|<parent-1>|<parent-2>|<branch>|<message>

When a snapshot has a configured author, two fields are appended:

.. code-block:: text

   <node-hash>|<tree-hash>|<timestamp-ns>|<parent-1>|<parent-2>|<branch>|<message>|<author-name>|<author-email>

The node hash is not the hash of the line. It is the BLAKE2b-512 digest of a
canonical signature. Legacy snapshots use:

.. code-block:: text

   <tree>|<parent-1-or-dash>|<parent-2-or-dash>|<branch>|<timestamp-ns>|<message>

Snapshots with an author append ``|<author-name>|<author-email>`` to the same
signature. This makes identity depend on content, ancestry, branch, time,
message and author, not on file ordering or formatting. The timestamp is
nanoseconds since the Unix epoch, which prevents two saves in the same second
from colliding. Empty parents are represented by ``-`` in the signature and by
an empty field in the history line.

Message, branch, author name and author e-mail are validated before hashing and
writing. ``|``, newline, carriage return, tab and control bytes are replaced or
rejected, so a message cannot forge extra fields. The message is bounded by
``MESH_MSG_MAX`` (512 bytes including the terminator); the author name is
bounded by ``MESH_AUTHOR_MAX`` (128 bytes) and the e-mail by
``MESH_EMAIL_MAX`` (254 bytes). Config values are validated before they reach
``graph_commit()``; a malformed or incomplete identity is treated as absent,
which keeps the legacy format and warning behavior.

Loading reverses the file order into a linked list, so the head of the list is
the newest line. A sorted hash index is then built for the nodes, duplicate
hashes are rejected, and each node's parent hashes are resolved with binary
search. The optional author fields are split only after the message and must
pass the same name/e-mail validation as the configuration command. Loading is
fail-closed: a truncated or malformed line, duplicate node, invalid signature,
or missing parent rejects the history rather than silently presenting a
partial graph.

A lineage with ``n`` snapshots therefore loads in ``O(n log n)`` instead of
``O(n^2)``. ``graph_find()`` uses the same index, and a commit invalidates it
because the list changed. Appending a history line records the old file size
and truncates back to it if the write or ``fsync`` fails, so a failed save
cannot leave a partial final line. The index is rebuilt on the next load.

``HEAD`` contains the current 128-character node hash. ``refs/<branch>``
contains the same hash for the active branch. Updating a ref is an atomic
replacement. The local config file supplies the branch name and may override
the global ``user.name`` and ``user.email``.


Object Store
------------

Objects live at:

.. code-block:: text

   .mesh/store/objects/<first-two-hex>/<remaining-126-hex>

The two-character fan-out keeps directory sizes reasonable on file systems
that degrade with very large single directories. Object files are created with
mode ``0444`` and are never modified after publication.

Writing an object follows a strict sequence:

1. Validate the hash (128 lowercase hexadecimal characters).
2. Reuse an existing object only after its file type and BLAKE2b digest have
   been verified.
3. Create the fan-out directory if needed.
4. Write to a private ``mkstemp()`` file through a 128 KiB buffer, handling
   short writes and ``EINTR``.
5. ``fsync()`` the descriptor.
6. Verify the temporary file's digest, close, then ``rename()`` into the final
   name.
7. ``fsync()`` the containing directory so the rename survives a crash.

A crash leaves a temporary file, never a corrupt object under a valid hash.
Temporary files are not referenced by any manifest.

``mesh admin gc`` has a dry-run mode and an explicit ``--apply`` mode. It marks
objects referenced by every snapshot in the local history and removes only
unreachable regular object files; applying requires non-empty history and the
repository lock. It does not delete history lines, snapshots, tree manifests
or reachable objects. Thus object garbage collection exists, but snapshot
retention and history compaction do not, and space used by reachable history
remains monotonic. Remote-only refs are not local roots; do not rely on them
for retention. Run ``mesh admin verify`` and review the dry run before applying
collection.

Chunk objects and whole-file objects share the same store. A chunk is simply a
content object whose length is described by the manifest. Deduplication is
automatic: if two files contain the same byte range, the chunk hash matches and
the second write is skipped.


Content-Defined Chunking
------------------------

Files smaller than 50 MiB (``MESH_LARGE_FILE``) are stored as a single object.
The manifest has no ``C`` lines.

Files at or above 50 MiB are divided into nominal 10 MiB blocks
(``MESH_CHUNK_SIZE``). At each nominal boundary the implementation reads up to
4 KiB (``MESH_CHUNK_WINDOW``) starting at the boundary. If a byte in that
window is ``\n`` or ``\0``, the boundary moves to just after that byte, capped
at end-of-file. Otherwise the nominal boundary is used.

The algorithm is deliberately not a rolling hash:

* boundaries are deterministic and require no global parameters;
* the scan is bounded, so hashing a file costs one linear pass plus at most
  4 KiB of look-ahead per block;
* sequential I/O dominates, which is friendly to both spinning disks and
  flash;
* an insertion near a boundary may shift subsequent boundaries, which is an
  accepted worst case in exchange for simplicity.

The boundary routine uses ``pread()``, so it never disturbs a shared file
offset. Chunk ranges are stored as ``(offset, length)`` pairs and hashed in
parallel after the boundary list is built. The chunks are then written to the
object store; existing chunks are skipped.

A one-kilobyte edit inside a 100 MiB file typically changes one 10 MiB chunk.
The other nine chunks keep their hashes and are neither re-read for storage nor
re-sent during synchronization.

.. list-table:: Chunking parameters
   :header-rows: 1
   :widths: 28 18 54

   * - Constant
     - Value
     - Meaning
   * - ``MESH_CHUNK_SIZE``
     - 10 MiB
     - Nominal block size for large files.
   * - ``MESH_CHUNK_WINDOW``
     - 4 KiB
     - Forward scan for ``\n`` or ``\0`` at each boundary.
   * - ``MESH_LARGE_FILE``
     - 50 MiB
     - Threshold below which a file is one blob.
   * - ``MESH_IO_BUFFER``
     - 128 KiB
     - Copy buffer for object writes and file streaming.
   * - ``MESH_MAX_THREADS``
     - 64
     - Upper bound on hash workers.


Conflict Isolation
------------------

When synchronization finds that the same path has different local and remote
content, it stages both pure versions without touching the working file.

* ``.mesh/store/conflicts/`` receives
  ``<16-hex-path-hash>-<sanitized-basename>.alpha`` and the matching
  ``.beta`` file. The stem is derived from the path, never from the index
  text.
* ``.mesh/store/conflicts/index`` receives one escaped repository-relative
  path per line. The legacy ``<ignored-key>\t<escaped-path>`` form is accepted
  when reading, but the key is ignored.
* The path is escaped with the same rules as manifest paths, so tabs and
  newlines cannot break the index. The path is validated again when the
  resolver reads it; a forged entry cannot point at ``../../target`` or
  ``.mesh/credentials``.
* ``conflict_choose()`` rewrites the index through an in-memory stream, copies
  the selected variant over the working file using the secure ``openat`` /
  ``renameat`` path walker, unlinks both staged files and atomically replaces
  the index.

Two files with the same basename in different directories therefore remain
independent because the full path is hashed into the stem. The readable
basename is retained only as a diagnostic suffix. The index still records the
full path, so the resolver knows which working file to update.

The working tree is never polluted with ``<<<<<<<``, ``=======`` or
``>>>>>>>`` markers. The user's file remains exactly as it was until an
explicit choice is made. This is both a usability decision and a data-safety
decision: a merge tool that writes markers can silently break a build or a
binary file.


Git Patch Bridge
----------------

``mesh save`` compares the previous tree with the new tree and writes
``.mesh/monitor/last.patch``. The format is conventional unified diff:

.. code-block:: text

   diff --git a/path b/path
   new file mode 100644
   --- /dev/null
   +++ b/path
   @@ -0,0 +1,2 @@
   +first line
   +second line

Design choices:

* Text changes are emitted as a **full replacement hunk** for the file. This
  produces larger patches than a minimal diff, but it is always valid and
  ``git apply`` accepts it without fuzzy matching.
* Files containing a NUL byte, or any change larger than 4 MiB
  (``MESH_PATCH_LIMIT``), are omitted with a warning on standard error. A
  half-correct binary patch would cause ``git apply`` to reject the entire
  patch, which is worse than an explicit omission.
* Adding or deleting an empty file is represented by the mode header only,
  because there are no lines to include in a hunk.
* File modes are emitted as Git's ``100644``/``100755`` headers, and symlinks
  use ``120000``. A mode-only change still produces a valid patch.
* Paths containing newline, carriage return, tab, backslash or double quote
  are skipped with a warning, since the unified diff header cannot represent
  them safely.
* Missing trailing newlines are marked with
  ``\ No newline at end of file``.

The patch is a bridge, not the storage format. Mesh itself never reads it back.


Passive Monitor
---------------

``monitor_collect()`` fills a ``struct mesh_status`` from the current HEAD,
tree, conflict index and graph count. ``monitor_write()`` serializes it by hand
with ``snprintf`` into a heap buffer and replaces
``.mesh/monitor/status.json`` atomically. There is no JSON library and no
schema file.

The document is flat by design. It contains strings, integers and one small
nested object (``changed``). GUIs can parse it with a few hundred bytes of
code. The ``repo_icon_source`` key is always ``"mesh_icon.svg"`` and exists so
an editor extension can locate the icon without guessing.

See :doc:`gui_integration` for the field-by-field contract.


Memory Ownership
----------------

The rules are simple enough to state completely.

* Functions that allocate a string through ``mesh_read_text()``,
  ``object_read()`` or ``graph_find()`` return ownership to the caller, which
  must ``free()`` it.
* ``manifest_load()`` allocates a ``struct mesh_file_list``; release it with
  ``file_list_free()``.
* ``graph_load()`` allocates nodes and messages; release them with
  ``graph_free()``.
* ``chunk_scan_fd()`` initializes and allocates a chunk array; release it with
  ``chunk_list_free()``.
* ``mesh_http_init()`` zeroes a response; ``mesh_http_free()`` releases its
  buffer and zeroes it again.
* Error paths free everything they acquired. The code uses one ``done:`` label
  per function and saves ``errno`` before cleanup, so the reported error is
  the one that actually occurred.

The flat layout keeps these rules visible. A guard clause that returns early
also makes the ownership boundary obvious: no later branch can still be
holding a resource. The rewrite added ``mesh_write_all()`` as the single
place where short writes and ``EINTR`` are handled, plus small helpers such
as ``json_unescape_char()`` and ``append_block()`` that isolate one concern
each. See :doc:`complexity_guardrails`.

No function returns a pointer to a stack buffer. No global buffer is reused
across commands. The hash pool allocates its job array in the caller, not in a
worker.


Error Handling
--------------

Every system call that can fail is checked. The patterns are consistent:

* ``open()``, ``read()``, ``write()``, ``pread()``, ``fsync()``, ``close()``,
  ``rename()``, ``stat()``, ``lstat()``, ``mkdir()`` and ``opendir()`` are
  checked at the call site.
* Short writes are looped. ``EINTR`` is retried. Other errors propagate.
* Partial results are removed or left unreferenced; they are never published
  under a valid name.
* ``errno`` is preserved across cleanup blocks and converted to a message only
  at the CLI boundary.
* Network errors are reported with ``curl_easy_strerror()``. HTTP status codes
  outside the expected range are reported explicitly, because a transport
  success is not an application success.
* Remote paths are validated with ``mesh_relpath_safe()``: absolute paths,
  control bytes, empty components and ``..`` are rejected before any write.

The CLI returns 0 on success, 1 on runtime failure and 2 on usage errors. A
warning about an omitted patch or a skipped shell file does not turn a
successful save into a failure, but it is printed on standard error so it
cannot be missed in a log.

The Alves layout makes the error path auditable at one level. A check is not
buried in the else branch of a success pyramid; it returns or jumps to the
single cleanup label where the failure actually occurred. This is the same
discipline described in :doc:`complexity_guardrails`, applied to system
calls rather than to business logic.


Build System
------------

The Makefile is intentionally small and incremental.

.. code-block:: make

   CC      ?= cc
   CFLAGS  ?= -std=c17 -O3 -Wall -Wextra
   CPPFLAGS += -Isrc
   LDLIBS  ?= -lcurl -lpthread

Each source file compiles to ``src/<name>.o`` with ``-MMD -MP``, producing a
``.d`` file that lists header dependencies. Changing ``core.h`` rebuilds every
object. The link step only needs libcurl and pthreads.

Targets:

.. list-table::
   :header-rows: 1
   :widths: 18 82

   * - Target
     - Effect
   * - ``make``
     - Build ``mesh`` incrementally.
   * - ``make install``
     - Publish the Python installer from ``install_uninstall.py`` and run it.
   * - ``make uninstall``
     - Remove the installed binary, icons and managed PATH blocks.
   * - ``make purge``
     - Run the uninstaller and remove the global Mesh configuration too.
   * - ``make check``
     - Run ``./mesh --version``.
   * - ``make clean``
     - Remove objects, dependency files, the binary and ``mesh-install``.

The installer is a self-contained Python 3 program; ``make mesh-install``
copies ``install_uninstall.py`` to the executable ``mesh-install`` entry point. It probes
the toolchain by invoking the compiler on a temporary program that includes
``curl/curl.h`` and links ``-lcurl -lpthread``. Probes run only when a build is
required, so ``--no-build`` with an existing binary does not need a compiler or
development headers.

Missing build packages are resolved automatically. A static table maps each
supported package manager (``apt-get``, ``dnf``, ``yum``, ``zypper``,
``pacman``, ``apk``, ``xbps-install``, ``emerge``, ``eopkg``, FreeBSD ``pkg``,
Termux ``pkg`` and Homebrew) to the fixed package names needed by Mesh. The
installer prepends ``sudo``, ``doas`` or ``pkexec`` when it is not root,
executes the package manager directly through ``subprocess`` -- no shell and no
user-controlled arguments -- and re-probes the toolchain afterwards. Debian
family installs run with ``DEBIAN_FRONTEND=noninteractive`` and are retried
once after ``apt-get update`` if the initial transaction fails. ``--no-deps``
disables the mechanism and prints the manual command; ``--yes`` skips the
single confirmation prompt.

The installer builds with ``make -jN``, where N is the online processor count
capped at 64, copies the binary with mode 0755 and the icon with mode 0644,
and injects the same PATH markers used by ``mesh start``.

``--uninstall`` removes the binary, both icon copies and every marked PATH
block without needing a compiler or the source tree. It removes
``PREFIX/share/mesh`` when empty and leaves the shared ``PREFIX/bin``
directory in place. ``--purge`` additionally removes
``$XDG_CONFIG_HOME/mesh`` or ``$HOME/.config/mesh`` and implies
``--uninstall``. Neither mode walks project directories, so ``.mesh``
repositories, snapshots and credentials are preserved.

Formatting alone does not change object code; the structural part of the
Alves conversion was therefore validated by a clean build rather than assumed
to be harmless. The tree compiles with ``-std=c17 -O3 -Wall -Wextra`` and no
warnings. The Makefile itself is intentionally untouched by the layout: the
build contract is independent of how the source is displayed.


Limits and Complexity
---------------------

.. list-table::
   :header-rows: 1
   :widths: 34 18 48

   * - Limit
     - Value
     - Reason
   * - Path length
     - 4096 bytes
     - ``MESH_PATH_MAX``; checked at every join.
   * - Directory depth
     - 64
     - Bounds recursion and catches loops.
   * - Control nesting
     - 3 levels
     - Alves guardrail; deeper logic is extracted or flattened.
   * - Snapshot message
     - 512 bytes
     - Fits the history line and keeps JSON small.
   * - Author name
     - 128 bytes
     - ``MESH_AUTHOR_MAX``; validated before hashing and writing.
   * - Author e-mail
     - 254 bytes
     - ``MESH_EMAIL_MAX``; a single ``@`` with non-empty local and domain parts.
   * - History in ``send``
     - 64 nodes
     - Bounds request size; newest first.
   * - In-memory HTTP body
     - 64 MiB
     - Prevents a hostile peer from exhausting RAM.
   * - Patch input
     - 4 MiB
     - Larger changes are omitted with a warning.
   * - Hash workers
     - 64
     - Caps thread creation on large machines.
   * - Chunk size
     - 10 MiB
     - Predictable I/O and useful deduplication granularity.

Time complexity of a save is linear in total bytes for hashing and I/O, plus
``O(n log n)`` for sorting the file list. History loading and ``graph_find()``
use a sorted hash index, so both are ``O(n log n)`` and ``O(log n)``
respectively. Manifest comparison and patch generation are linear in the number
of entries. Sync parses each manifest once, uses binary search for local
lookups and materializes directly from the loaded entry, so it is linear in the
remote manifest size plus the bytes actually transferred. Memory use is
proportional to the number of files, not to the size of the largest file,
because content is streamed and chunks are hashed independently.


Portability Notes
-----------------

Mesh targets POSIX. The only Linux-specific call is ``readlink("/proc/self/exe")``
in ``mesh_self_dir()``, used to locate the icon next to the running binary. If
that call fails, the function falls back to ``getcwd()``. On systems without
``/proc``, ``mesh start`` still works; it simply looks for the icon in the
current directory.

``open_memstream()``, ``strtok_r()``, ``flock()``, ``pread()`` and
``pthread_*`` are POSIX interfaces. Untrusted paths are materialized with
``openat()``, ``mkdirat()``, ``renameat()`` and ``O_NOFOLLOW`` rather than by
concatenating strings and calling ``open()``. ``st_mtim`` is used where it is
available, with a ``st_mtimespec`` fallback on Apple and NetBSD. The code does
not use GNU extensions.
Integer formatting uses ``<inttypes.h>`` macros so 64-bit values remain
portable across LP64 and other data models. ``core.h`` and the Makefile define
``_FILE_OFFSET_BITS=64`` before system headers, so ``off_t`` and file-size
fields remain 64-bit even on a 32-bit userland. The Python installer relies on
Python's integer and filesystem APIs and needs no equivalent macro.

.. seealso::
   For the current implementation boundaries and safe operating mitigations, see
   :doc:`limitations`.
