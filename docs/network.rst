Network Subsystem
=================

The network engine lives in ``src/network.c`` and is built on libcurl. It has
four jobs: forge archive import, resumable file transfer, in-memory JSON API
calls, and the object exchange used by ``mesh sync``. Every job shares the same
conservative timeouts, redirect policy, authentication and error reporting.

.. contents::
   :depth: 2
   :local:


Implementation Style
--------------------

``src/network.c`` is written in the Alves style: flat functions, guard
clauses for every libcurl and allocation failure, no decorative blank lines
and no indentation. ``net_download_ex()`` reads from top to bottom: validate the
URL and optional policy, open the destination, set the resume offset, stream the
body, verify the result. Each failure returns at the point where it occurs.
``net_download()`` is the default-policy wrapper. The sync-side
``fetch_object()`` in ``src/cli.c`` wraps ``net_download()`` in the
temporary-file discipline so a failed object fetch cannot publish a truncated
object. The ``missing_objects`` scan is similarly short and guarded.

The wire protocol is independent of the layout. Headers, status codes,
timeouts and resume semantics are unchanged by the rewrite. The style matters
because network code is reviewed under a terminal, often while a transfer is
failing; a flat error path is easier to audit than a nested success pyramid.
See :doc:`alves_style_manifesto` and :doc:`complexity_guardrails`.


Connection Policy
-----------------

.. list-table::
   :header-rows: 1
   :widths: 32 18 50

   * - Setting
     - Value
     - Rationale
   * - Connect timeout
     - 20 s
     - A dead endpoint should fail before a user gives up.
   * - Low-speed limit
     - 512 B/s
     - Detects a stalled transfer rather than waiting forever.
   * - Low-speed window
     - 60 s
     - Tolerates slow links and short pauses.
   * - Redirects
     - Disabled
     - Prevents credentials and upload bodies from crossing an untrusted origin.
   * - User agent
     - ``Mesh/1.0``
     - Identifies the client in server logs.
   * - Signals
     - Disabled
     - ``CURLOPT_NOSIGNAL`` keeps libcurl from touching process signal state.
   * - ``Expect``
     - Suppressed
     - Avoids an unnecessary 100-continue round trip on uploads.

There is no total transfer timeout by default. A large download may
legitimately take hours; the low-speed detector handles the case where it has
actually stalled. ``mesh obtain --max-time SEC`` can impose one explicitly.
TLS verification uses libcurl's defaults. Mesh does not disable certificate or
hostname checks, and it does not ship a custom CA bundle. Systems that need a
private CA configure it in the usual libcurl/OpenSSL environment.
``mesh obtain -k``/``--insecure`` is the explicit exception for one transfer:
it sets both ``CURLOPT_SSL_VERIFYPEER`` and ``CURLOPT_SSL_VERIFYHOST`` to zero
and prints a warning. Repository mode refuses ``-k`` when ``MESH_TOKEN`` or
matching repository credentials would be sent, because a certificate override
would expose a bearer token to a man-in-the-middle.

Only ``http://`` and ``https://`` URLs are accepted. A URL without either
scheme is rejected with ``EINVAL`` before a connection is attempted. Plain
HTTP to a host other than ``localhost``/``127.0.0.1``/``[::1]`` is allowed but
prints a warning, because hashes provide integrity, not authenticity: an active
attacker can replace both the manifest and the objects.

Redirects are disabled by default. ``mesh obtain -L`` enables them for that
transfer only. libcurl is restricted to HTTP/HTTPS redirect targets, follows at
most five hops, and an HTTPS source may not redirect down to HTTP. Repository
mode refuses ``-L`` when ``MESH_TOKEN`` or matching repository credentials
would be attached, so a bearer token is never forwarded to a redirect target.
Custom headers supplied with ``-H`` are an explicit user choice.

Cookie and proxy policy is intentionally delegated to libcurl. ``-b`` supplies
cookie data or a cookie file, ``-c`` writes a cookie jar, ``-x`` selects a
proxy, ``-U`` supplies proxy credentials and ``--noproxy`` bypasses a proxy for
listed hosts. These options affect only the current command. File downloads
still do not attach repository credentials, and a proxy cannot read an HTTPS
body while TLS verification is enforced.

``net_stream_ex()`` shares the same validation, redirect and authentication
rules as ``net_download_ex()`` but streams the body to stdout and hashes it
incrementally. It discards HTTP error bodies, like file mode. ``net_head_ex()``
sends a HEAD request, writes response headers to a caller-supplied stream and
records the same transfer metrics. ``--write-out`` formats those metrics in
``src/cli.c``; the network layer exposes the raw counters and timings through
``struct mesh_transfer_stats``.


Authentication
--------------

``mesh auth TOKEN`` stores a legacy wildcard token in ``.mesh/credentials``
with mode ``0600``. ``mesh auth --host HOST TOKEN`` stores a token scoped to
that authority. The file is UTF-8 text with one entry per line:

.. code-block:: text

   mesh.example.com:8443 token-for-that-peer
   * wildcard-token

A line containing only a token is also accepted as the legacy wildcard form.
The authority includes an explicit port when the URL has one; hostname
comparison is case-insensitive. Exact host matches take precedence over the
wildcard. Every request built by ``build_headers()`` asks
``net_auth_header()`` with its own URL, so a scoped token is never sent to a
different origin.

When a matching token is present and valid, the request includes:

.. code-block:: text

   Authorization: Bearer <token>

Token validation is strict: non-empty, at most 4096 bytes, printable ASCII
from ``0x21`` through ``0x7e``, no spaces. A token containing a newline or a
control byte is rejected rather than truncated, because a truncated credential
is a confusing authentication failure.

Credentials are per repository. ``mesh save`` never stores them in a snapshot
because ``.mesh`` is excluded from the scan. They are not encrypted at rest;
the protection is the file mode. Use full-disk encryption if the threat model
requires it. When a matching credential exists, Mesh refuses ``http://`` rather
than sending the bearer token in clear text; use ``https://``. Generic
file downloads through ``mesh obtain`` pass no repository to the network layer
and therefore never attach a token automatically.


Forge Archive Imports
---------------------

``mesh obtain`` is a network consumer with a different trust boundary from the
Mesh peer protocol. It reads a tar.gz archive generated by a forge, so the
archive format itself is untrusted input. The command resolves a repository
URL, downloads the archive with ``net_download_ex()``, extracts it into a
private staging directory and only then publishes the tree with one ``rename``.

The URL mapping is deliberately small:

.. list-table::
   :header-rows: 1
   :widths: 28 72

   * - Source
     - Archive URL
   * - GitHub
     - ``https://codeload.github.com/O/R/tar.gz/REF``
   * - GitLab.com
     - ``https://gitlab.com/G/P/-/archive/REF/P-archive.tar.gz``
   * - Gitea, Forgejo, Codeberg and generic hosts
     - ``ORIGIN/PATH/archive/REF.tar.gz``
   * - Any host with ``--archive URL``
     - The URL is used verbatim.

``--ref`` selects a branch, tag or revision; the default ``HEAD`` resolves the
default branch on all three forge families. Redirects remain disabled unless
``-L``/``--location`` is explicit. Following redirects is restricted to
HTTP/HTTPS, an HTTPS transfer may not downgrade to HTTP, and repository mode
refuses redirects when a bearer credential would be attached. Self-hosted
GitLab, GitHub Enterprise or a non-standard archive route should use
``--archive`` rather than relying on host detection.

The gzip and DEFLATE stream is decoded by ``src/inflate.c`` without zlib. The
decoder handles stored, fixed-Huffman and dynamic-Huffman blocks, verifies the
gzip CRC32 and ISIZE trailer, and rejects malformed or truncated streams. The
tar reader in ``src/archive.c`` consumes the decoded stream incrementally:
payloads are written to temporary files below the staging root, directories
and symlinks are created with ``openat()``/``O_NOFOLLOW``, and names are
validated with ``mesh_relpath_safe()`` after removing the archive's single root
component. PAX ``path``/``linkpath``/``size``/``mtime`` records, GNU long
names, USTAR prefixes and executable modes are supported.

Limits are explicit: one archive may decompress to at most 64 GiB and contain
at most one million entries; PAX/GNU metadata is limited to 1 MiB per record
set. Hard links and device/fifo entries are rejected. Extraction never follows
a symlink component, so an archive cannot create ``link -> ../..`` and then
write through it. A trailing CRC or truncated tar stream fails the whole
operation and the staging directory is removed.

Authentication uses ``MESH_TOKEN`` when set. The token is validated with the
same rules as ``mesh auth``, written to the staging repository's
``.mesh/credentials`` scoped to the archive authority, and sent only over
HTTPS. Without ``MESH_TOKEN``, an existing repository in the current directory
may supply matching credentials. File mode of ``mesh obtain`` still never
attaches credentials.


In-Memory Responses
-------------------

API responses are read by ``memory_cb()`` into a single growable buffer. The
buffer starts at 4096 bytes, doubles as needed, and is capped at 64 MiB
(``MESH_HTTP_MAX_BODY``). The buffer is always NUL-terminated so callers can
use string searches safely.

If a peer sends more than the ceiling, the callback returns zero, libcurl
reports a write error and the command fails. The partial buffer is freed. This
is a deliberate denial-of-service bound: Mesh will not let a remote endpoint
force an unbounded allocation.

The same callback is used for ``net_get()`` and ``net_post_json()``. Neither
writes a temporary response file. The disk is reserved for user data and for
objects that are actually stored.

Response headers are parsed by ``header_cb()``. Two headers are recognized:

.. code-block:: text

   X-Mesh-Hash: <128 lowercase hex characters>
   X-Mesh-Tree: <128 lowercase hex characters>

Header names are compared case-insensitively. Values are trimmed of leading
spaces and tabs and copied with a bound.


Resumable Downloads
-------------------

File mode of ``mesh obtain`` and every object download use
``net_download_ex()``. ``net_download()`` remains a compatibility wrapper for
callers that want the default, credential-free download policy.

The algorithm is driven by the file system, not by a counter in memory.

1. ``lstat()`` the destination. If it is a regular file larger than zero
   bytes, record its size as the resume offset. A non-regular destination or a
   symlink is rejected.
2. Build request headers, including a matching bearer token when the caller
   supplied a repository context.
3. Open the destination with ``O_NOFOLLOW`` in append mode.
4. Set ``CURLOPT_RESUME_FROM_LARGE`` to the offset. libcurl emits
   ``Range: bytes=<offset>-``.
5. Stream the body through ``fwrite(3)``. There is no whole-file buffer.
6. On success, flush and close, ``stat()`` the final file, report its size and
   hash it with BLAKE2b-512.

Two failure modes receive special treatment.

**The server ignores the range.**
A server that does not understand ``Range`` may answer ``200`` with the entire
body. Appending that body to a partial file would corrupt it. Mesh detects
``200`` with a non-zero resume offset, closes and truncates the file, and
retries once from zero. The same recovery happens if libcurl reports
``CURLE_RANGE_ERROR``.

**The server rejects the range.**
A server may answer ``416 Range Not Satisfiable`` when the requested offset is
at or beyond the end of the resource. Mesh cannot prove completeness from that
status alone, so it removes the partial destination and retries once from byte
zero. This is safer than accepting a stale or oversized local file.

Downloads retry at most once, and only for the range fallback. Transport errors
such as DNS failure, TLS failure or a stalled connection are reported and the
command exits with status 1. The next invocation resumes from whatever bytes
made it to disk.

Object downloads are driven by ``fetch_object()`` in ``src/cli.c``. It
downloads to ``<object>.part``, flushes and ``fsync``s the stream, verifies the
BLAKE2b digest, renames the file into the object store and applies mode
``0444``. An existing partial file is resumed with a Range request when
possible. A mismatch deletes the partial file; a failed object fetch never
publishes a truncated object under a content hash.


HTTP GET
--------

``net_get()`` performs a GET and stores the body in memory. Optional extra
headers are passed as a NULL-terminated array of ``"Name: value"`` strings.
The status code is returned in ``resp->status``; transport errors return -1.

The function does not use ``CURLOPT_FAILONERROR``. That is intentional: a 304
or a 404 still carries useful headers and should be interpreted by the caller
rather than collapsed into a generic transport error.

``mesh sync`` uses GET for the base resource and for individual objects. The
base request carries:

.. code-block:: text

   X-Mesh-Hash: <local HEAD or empty>
   X-Mesh-Tree: <local tree or empty>
   Accept: text/plain

The server answers with the remote manifest as the body and announces its own
identity in response headers. The client verifies that the BLAKE2b-512 digest
of the exact body equals ``X-Mesh-Tree`` before storing or parsing it. An empty
body is valid for an empty tree when its digest matches; malformed records are
rejected by the strict manifest parser.


HTTP POST
---------

``net_post_json()`` sends a JSON document with:

.. code-block:: text

   Content-Type: application/json
   Authorization: Bearer <token>   (when a matching credential exists)

The body is sent from memory with an explicit length, so no chunked transfer
encoding is required. The response is read into memory and parsed only enough
to find the ``missing_objects`` array. A response outside ``2xx`` is reported
as an application failure.

``mesh send`` uses POST for a one-way snapshot notification. ``mesh sync`` uses
POST to publish the local snapshot and to ask which objects the peer still
needs.


HTTP PUT
--------

``net_put_file()`` uploads one object. It opens the source read-only, uses
``CURLOPT_UPLOAD`` with an explicit size and sends:

.. code-block:: text

   Content-Type: application/octet-stream

The response body is discarded through a sink callback. Any status outside
``2xx`` fails the upload. Objects are uploaded only when the peer explicitly
lists their hashes in ``missing_objects``; Mesh does not push its whole store
on every sync.


Sync Protocol
-------------

The protocol is intentionally small. A peer is any HTTP server that can serve
manifests and objects and accept a snapshot document.

.. list-table::
   :header-rows: 1
   :widths: 12 24 22 42

   * - Step
     - Method and path
     - Request body
     - Response
   * - Announce
     - ``GET <base>``
     - none
     - remote manifest; ``X-Mesh-Hash`` and ``X-Mesh-Tree`` headers
   * - Fetch object
     - ``GET <base>/objects/<hash>``
     - none
     - raw bytes of the object
   * - Publish
     - ``POST <base>``
     - snapshot JSON
     - JSON, optionally with ``missing_objects``
   * - Upload object
     - ``PUT <base>/objects/<hash>``
     - raw bytes
     - any ``2xx``

The base URL is the argument to ``mesh sync``. ``<base>`` is used verbatim;
``/objects/`` is appended without normalizing the path beyond joining the two
parts. A trailing slash on the base is handled by the join helper.

Announce
~~~~~~~~

Client request::

   GET /repo HTTP/1.1
   Host: mesh.example.com
   User-Agent: Mesh/1.0
   Accept: text/plain
   X-Mesh-Hash: 5d08affd9cb2e6d7...
   X-Mesh-Tree: 76fe60df789791d4...

Server response::

   HTTP/1.1 200 OK
   X-Mesh-Hash: a479d5380757b636...
   X-Mesh-Tree: 108ba5695b69f48c...
   Content-Type: text/plain
   Content-Length: 647

   F 3916b7c767fde0cc... 182 1792515685 0 644 f .vscode/settings.json
   F c247f82cfa3c210a... 13609 1792515685 0 644 f mesh_icon.svg
   F 54a3925ca244d6c2... 15 1792515685 0 644 f remote_only.txt
   F c05bb7e811bd6290... 21 1792515685 0 644 f shared.txt

The abbreviated hashes above stand for the full 128 lowercase hexadecimal
digits required on the wire. Each ``F`` line carries
``hash size mtime_sec mtime_nsec mode kind path``. ``kind`` is ``f`` for a
regular file or ``l`` for a symlink; a symlink hash covers its target bytes.
The legacy four-field form ``F hash size path`` is accepted as regular mode
``644`` with timestamp zero.

If the server returns ``304 Not Modified``, or if ``X-Mesh-Hash`` equals the
local HEAD, the client stops and prints ``already up to date``. A server may
also omit the body when the trees are equal; the client still checks the
header before parsing.

The remote manifest is stored locally at
``.mesh/store/trees/<remote-tree>`` even though it is not part of local
history. This makes object references resolvable during the pull and gives a
later ``mesh sync`` a cached description of the peer's state.

Fetch and merge
~~~~~~~~~~~~~~~

Before merging, the client requests every object referenced by the remote
manifest that it does not already have. Whole files are one object. Large files
are represented by their ``C`` lines, and each chunk is requested separately.
An existing object is skipped without a request, which is the main bandwidth
saving of the protocol.

The merge then walks the remote manifest path by path:

.. list-table::
   :header-rows: 1
   :widths: 34 66

   * - Local condition
     - Action
   * - Local tree has the same hash and kind
     - Restore the file when it is missing; apply a mode-only change when the
       working content still matches. Otherwise skip.
   * - Path absent locally and absent on disk
     - Materialize the remote file, symlink, mode and timestamp.
   * - Working file exists and hashes to the remote hash
     - Accept it; no conflict, no write. This makes repeated syncs
       idempotent.
   * - Anything else
     - For regular-file variants, stage ``alpha`` (local working file, or the
       local tree content when the file is missing) and ``beta`` (remote
       content). Do not modify that working file. If either manifest side is
       a symlink in a mismatched conflict, fail closed; the current conflict
       store and resolver cannot preserve or choose mixed file types.

If this pull staged one or more conflicts, the client does not POST its local
snapshot. It exits non-zero and asks the user to choose a variant and save
before retrying. A sync that starts with conflicts already in the local index
also stops before GET. Non-conflicting paths may already have been materialized
before a later collision is discovered; a failed sync is therefore not
necessarily a no-op. The peer may have received GET/object requests, but it
does not receive the local snapshot POST while conflicts are pending.

Publish
~~~~~~~

After the merge, the client POSTs the local snapshot:

.. code-block:: json

   {
     "mesh_version": "1.0",
     "action": "snapshot",
     "head": "<local head>",
     "tree": "<local tree>",
     "parent": "<parent hash or empty>",
     "branch": "main",
     "message": "local state",
     "snapshots": 1,
     "files": 4,
     "bytes": 13822,
     "manifest": "F ...\n...",
     "history": [
       {"hash": "...", "tree": "...", "parent": "", "timestamp": 1792515685123456789, "message": "local state"}
     ],
     "repo_icon_source": "mesh_icon.svg"
   }

The response may be any ``2xx``. A response containing:

.. code-block:: json

   {"ok": true, "missing_objects": ["<hash>", "<hash>"]}

causes the client to PUT each listed object. The parser looks for the
``missing_objects`` key, then scans quoted 128-character lowercase hex strings
inside the following array. Unknown keys are ignored. A malformed or absent
array means "nothing to upload".

Idempotence
~~~~~~~~~~~

Running ``mesh sync`` twice against an unchanged peer performs one GET and
stops on the header comparison. Running it after a pull but before a save
re-evaluates the working files; files whose content already matches the remote
hash are accepted without creating a conflict. This is what makes the command
safe to call from a timer or an editor save hook when no conflict is pending.

Concurrency and Merge Boundaries
--------------------------------

The version 1.0 exchange is manifest/object synchronization, not a complete
multi-writer merge protocol. The client does not compute a common ancestor,
create a two-parent merge snapshot, or propagate deletions as a general
three-way merge. A peer may opt into conditional publication by returning
``X-Mesh-CAS: if-match`` and an ``ETag`` on the manifest GET. Mesh then sends
that ETag as ``If-Match`` on the snapshot POST. A conforming peer must reject a
stale precondition (normally with HTTP 409 or 412); Mesh reports the stale
version and does not report the snapshot as published. Peers that omit the
capability remain compatible, but simultaneous writers can race. Serialize
updates when using those legacy peers; successful transport alone does not
prove concurrent updates were merged.

Symlinks are supported as manifest entries and can be transferred when paths
are new, identical, or in a staged file/symlink conflict. Conflict index v1
records kind and mode for each endpoint. The resolver stores variant payloads
as regular files inside the private conflict store, then materializes a chosen
symlink through a temporary sibling and atomic rename. Directories and special
filesystem objects are not supported conflict variants and fail closed.


Bandwidth Strategy
------------------

Mesh saves bandwidth at four levels.

**Header-level short circuit.**
The ``X-Mesh-Hash`` comparison avoids downloading a manifest when the peers
already agree. A 304 response is even cheaper.

**Object-level deduplication.**
Objects are requested by BLAKE2b hash. Content already present locally is
never requested. This applies to whole files and to individual chunks.

**Chunk-level reuse.**
A large file that changed in one region shares all unchanged chunks with the
peer. Only the changed chunk and any boundary-shifted neighbours are
transferred.

**Resume from disk.**
A dropped download costs only the bytes after the last successful write.
``stat()`` is the source of truth, so a killed process or a reboot does not
lose progress.

Mesh does not compress request bodies or responses. Large objects are usually
already compressed, and adding a compression layer would spend CPU to save
bytes that the transport may already compress. A peer is free to apply HTTP
content encoding; libcurl decodes it transparently when the server announces
it.

Mesh does not request delta patches between two versions of a file. The chunk
store provides block-level reuse without a separate delta protocol. A future
version could add a ``GET <base>/delta/<old>/<new>`` endpoint without changing
the object model.


Server Expectations
-------------------

A minimal peer must satisfy the following contract.

.. list-table::
   :header-rows: 1
   :widths: 28 72

   * - Requirement
     - Detail
   * - Base GET
     - Return the manifest for the current remote tree. Set ``X-Mesh-Hash`` to
       the remote HEAD and ``X-Mesh-Tree`` to the tree hash. Return 304 when
       the client's ``X-Mesh-Hash`` is current.
   * - Object GET
     - Serve raw bytes at ``/objects/<hash>``. Return 404 for unknown hashes.
       Content length should be exact.
   * - Object PUT
     - Store bytes at the hash named in the path. Verify the hash if the server
       wants to reject corruption. Return any 2xx on success.
   * - POST
     - Accept ``application/json``. Optionally persist the snapshot. Respond
       with JSON and list required objects in ``missing_objects``.
   * - Ranges
     - Honour ``Range: bytes=N-`` with ``206 Partial Content`` for resumable
       object and file transfer. A server that ignores ranges still works, but
       the client restarts from zero.
   * - Hash format
     - Use 128 lowercase hexadecimal characters. Uppercase or shortened hashes
       are rejected by the client.
   * - Path safety
     - Never publish a manifest with absolute paths, empty components or
       ``..``. The client validates, but a correct server should not generate
       them.

The reference implementation used during development is a small Python HTTP
server that reads a Mesh repository directly. It is not part of the shipped
source tree; the protocol above is the stable interface. Any server that
speaks it can peer with Mesh.


Failure Reporting
-----------------

Network failures produce one line on standard error, prefixed with
``mesh: network:`` for libcurl errors or ``mesh: <command>:`` for command-level
errors. Examples::

   mesh: network: Could not resolve hostname
   mesh: obtain: transfer failed: Connection timed out
   mesh: sync: remote returned HTTP 500
   mesh: sync: GET failed: SSL certificate problem

The exit status is 1. Partial downloads remain on disk and are resumable.
Partial object downloads never enter the store until their hash is verified.
A failed POST or PUT leaves local history and the object store unchanged; the
next sync retries the exchange.

Warnings that do not abort an operation are printed without a failure status.
For example, a local object listed for upload but missing from the store
produces ``mesh: sync: warning: local object <hash> is missing`` and the
remaining uploads continue. This is intentional: one missing object should not
discard a successful manifest exchange, but the operator must see it.


Security Notes
--------------

* Bearer tokens are stored with mode 0600, scoped by host when the user asks
  for it, and never printed. File downloads through ``mesh obtain`` never send
  one.
* Remote manifests are untrusted input. Paths are validated before any write;
  hashes are validated before any path construction. Embedded NUL bytes, ASCII
  control bytes, reserved namespaces, case-folded duplicate paths and
  non-contiguous chunks reject the whole manifest. The parser also enforces
  ceilings on entries and chunk records before allocating for them.
* Object content is addressed by hash and is re-hashed before publication and
  before materialization or upload. A peer serving bytes that do not match the
  requested hash is rejected. Objects fetched by sync are published with mode
  ``0444``.
* The in-memory response ceiling is 64 MiB. This bounds memory but does not
  bound CPU spent escaping JSON.
* Forge archives are untrusted. ``mesh obtain`` removes one root component,
  validates every remaining path, rejects reserved namespaces and special
  entries, never follows a symlink while extracting and verifies the gzip
  trailer. The staging directory is not published until extraction and the
  first snapshot both succeed.
* Redirects are disabled and TLS verification is never disabled. Plain HTTP to
  a non-loopback host produces an explicit warning; a request that would send a
  bearer token over HTTP is refused.
* ``mesh sync`` will not write outside the repository root. A remote path with
  ``..``, an absolute prefix, a symlink component, or an invalid manifest
  record rejects the pull before materialization. Materialization uses
  ``openat()``/``renameat()`` with ``O_NOFOLLOW``.
* Conflict variants are staged under ``.mesh/store/conflicts`` with names
  derived from the full path, and are never executed or automatically applied.
  The user chooses explicitly.

The design assumes a peer you have reason to trust, reached over TLS, with
credentials scoped to that peer. It does not attempt to be a hostile multi-tenant
object server.

.. seealso::
   For the current implementation boundaries and safe operating mitigations, see
   :doc:`limitations`.
