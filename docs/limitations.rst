Known Limitations and Mitigations
=================================

This page separates implemented mitigations from boundaries that remain. Mesh
prefers refusing unsafe or ambiguous input over silently truncating paths,
rewriting snapshot identities, or overwriting a user's work.

.. contents::
   :depth: 2
   :local:


Repository Paths and Large Files
--------------------------------

The maximum repository-relative path accepted by Mesh remains 4095 bytes. This
is a byte limit, not a Unicode-character limit. It is a theoretical ceiling:
absolute paths, fixed-size path buffers, the operating-system path limit, and
per-component filesystem limits can all reduce the usable length. An overlong
join fails with ``ENAMETOOLONG``; Mesh must not silently return a truncated
path. Mesh also rejects invalid UTF-8, control characters, and reserved
``.mesh``/``.git`` path components.

The build defines ``_FILE_OFFSET_BITS=64`` and file/chunk sizes and offsets use
64-bit values. This permits files larger than 2 GiB on suitable POSIX systems;
it is not a promise of unlimited size. The operating system, filesystem,
available storage, chunk ceilings, and manifest ceiling still apply.

Large regular files (at least 50 MiB) are split into content-addressed chunks.
The manifest remains a flat document: one ``F`` record per path and one ``C``
record per chunk. Limits remain 262144 entries, 1048576 chunks total, 262144
chunks per file, and a 64 MiB manifest/HTTP response. Exceeding a ceiling
rejects the whole operation. Manifest creation now writes and hashes records
incrementally rather than keeping a second full copy in memory, and chunk
parsing grows allocations geometrically and validates offsets in constant time
per chunk. Loading and HTTP JSON response handling are still bounded and are
not fully streaming.

**Mitigation.** Keep repository roots reasonably short, avoid excessively deep
or verbose generated paths, and split very large generated trees across
repositories when they approach the published ceilings. Run
``python3 tools/unicode_audit.py PATH`` before importing a tree or moving it to a
case-insensitive filesystem. The hard path, entry, chunk, and response ceilings
remain explicit rather than dynamically allocating unbounded memory for
untrusted peer input.


History Retention and Object Collection
---------------------------------------

``.mesh/history`` remains append-only during ordinary operation. ``mesh admin
gc`` is an object-store collector: dry-run is the default, and ``--apply``
removes unreachable object blobs only. It does not prune history records, tree
manifests, branch/tag refs, or objects reachable from any recorded snapshot.
Consequently, ordinary GC does not compact snapshot history.

A supported, explicit history restart tool is now provided:
``tools/history_restart.py``. It defaults to a dry run and requires a new backup
path. With ``--apply`` it locks the repository, verifies the current store,
checks that the working tree did not change during preparation, copies the full
old ``.mesh`` directory to the chosen backup, builds and verifies a new
repository containing the current working tree as a new root snapshot, and
atomically swaps the active metadata. This reduces active history to a fresh
root, but deliberately does **not** preserve ancestry, old snapshot IDs, refs,
tags, or local remote state in the active repository; those remain in the
backup. Treat it as a history reset, not an in-place compaction.

Example (choose a backup location with sufficient private storage):

.. code-block:: sh

   MESH_BIN="$PWD/mesh" python3 tools/history_restart.py \\
     --root . --backup ../mesh-history-backup
   MESH_BIN="$PWD/mesh" python3 tools/history_restart.py \\
     --root . --backup ../mesh-history-backup --apply

The operation refuses to overwrite an existing backup. Keep the backup until
the new repository has been independently verified and any required remote
configuration has been restored.


Synchronization and Concurrent Writers
---------------------------------------

``mesh sync`` exchanges manifests and content-addressed objects. It still does
not compute a common ancestor, create merge snapshots, or provide a complete
general deletion-aware three-way merge. Peers that advertise
``X-Mesh-CAS: if-match`` and provide an ``ETag`` now receive a conditional
``If-Match`` POST. HTTP 409/412 is reported as a stale remote version, and the
snapshot is not reported as published. This closes the lost-update window for
peers that correctly implement that conditional contract.

Legacy peers that do not advertise the capability are still supported for
compatibility, but they cannot guarantee protection from simultaneous writers.
Use a single writer or serialize publication when talking to such peers. Mesh
itself does not provide a hosted consensus service or server-side locking.

Regular-file and file/symlink conflicts are staged as typed ``alpha`` and
``beta`` variants without inserting conflict markers into the working file.
The conflict index records each variant's kind and mode; choosing a symlink
creates it through a temporary sibling and atomic rename. A new or pre-existing
conflict prevents sync from POSTing the local snapshot. ``mesh choose`` applies
the selected side to every staged conflict, so review the full batch before
choosing and run ``mesh save`` afterward. Directories and special filesystem
objects are not variants and fail closed.

A pull may already have materialized other non-conflicting paths before a
conflict is found; inspect the working tree before retrying. A failed sync does
not imply that no local files changed.

**Mitigation.** Resolve one writer at a time for legacy peers, inspect staged
variants, and complete ``choose`` plus ``save`` before retrying. If the remote
supports CAS, treat 409/412 as a signal to fetch and review the newer state;
do not blindly retry the old publication.


Forge Imports and Git History
-----------------------------

``mesh obtain`` imports the source archive for the selected forge reference and
creates an initial Mesh snapshot; it does not import Git ancestry. The new
``tools/git_import.py`` imports the selected ref's first-parent tree sequence
into a new Mesh repository by using Git archives and the Mesh CLI. It validates
archive paths and Unicode collisions and verifies the resulting repository.

The importer preserves the tree sequence and commit subjects, but does not
preserve merge parents, Git OIDs, all branches/tags, signatures, exact author
and committer metadata, or original timestamps. It is a bounded migration aid,
not a lossless Git conversion or a Git transport implementation. The
``mesh obtain`` operation remains an archive-only import.

Example:

.. code-block:: sh

   MESH_BIN="$PWD/mesh" python3 tools/git_import.py \\
     /path/to/git-clone /path/to/new-mesh --ref main

The destination must not already exist. Keep the source Git clone and its refs
until the imported Mesh tree and linear history have been reviewed.


Filesystem Portability
----------------------

Mesh targets POSIX. Manifest paths must be valid UTF-8. The C core now rejects
name collisions under Unicode NFC normalization plus full, locale-independent
case-folding, so canonically equivalent and common case-insensitive collisions
are detected before a manifest is saved or loaded. Original path spellings are
preserved in the manifest; only comparison keys are normalized. ICU4C provides
the Unicode tables used by the C implementation. The Python preflight tool uses
Python's Unicode database, which can differ by Unicode release; the C core is
authoritative.

POSIX and individual filesystems still impose their own path, component,
timestamp, permission, and file-size semantics. A 4095-byte Mesh path limit may
be lower than or higher than a particular filesystem's practical total-path
limit.

**Mitigation.** Run the read-only preflight tool before migration, avoid names
that differ only by normalization or case, and retain a verified backup when
moving to a filesystem with different naming or metadata semantics.


Compatibility Policy
--------------------

Manifest and snapshot serialization remain content-addressed and byte-stable;
Unicode comparison does not rewrite stored path spellings. The version 1 HTTP
exchange remains compatible, with CAS as an opt-in capability advertised by
the peer. The limits that remain—path length, manifest and HTTP response size,
legacy-peer concurrency, non-file conflicts, and lossless Git metadata—cannot
be removed by a local-only client change. They require operating-system-aware
path work, streaming on both sides of the manifest exchange, server support,
or a versioned history/transport design. The tools documented here provide
safe, explicit migrations where a transparent in-place rewrite would risk data
loss or break identity.
