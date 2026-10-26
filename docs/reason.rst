Reason - A Manifesto for Mesh
=============================

Software tools carry assumptions. A version control system assumes that
history is a local, rational structure. A downloader assumes that the network
is an empirical, unreliable source of bytes. An HTTP client assumes that the
outside world speaks in requests and responses. Each assumption is useful
alone and incomplete alone.

Mesh exists because these assumptions are not rivals. They are moments of a
single practice. The two sections below state the philosophical argument and
then translate it into concrete engineering decisions.

.. contents::
   :depth: 2
   :local:


The Kantian Synthesis (Unifying Git, Wget, and cURL)
----------------------------------------------------

Immanuel Kant argued that knowledge arises neither from pure concepts alone
nor from raw sensation alone. Concepts without intuitions are empty;
intuitions without concepts are blind. A rational structure must be applied to
empirical material for either to become knowledge. The structure is not
derived from the material, and the material is not produced by the structure.
Both are required, and the act of joining them is synthesis.

The history of developer tooling can be read as a long separation of these two
faculties.

Git is a monument to pure rationalism. It builds an internal, self-referential
world of blobs, trees and commits. Content is hashed, objects are immutable,
and history is a directed graph that can be traversed without ever touching
the network. That world is coherent and powerful, but by itself it is blind to
the empirical world outside the repository. Git knows how to compare two
trees; it does not know how to fetch a file from an arbitrary URL, resume a
broken transfer, or speak a simple JSON API.

Wget and cURL are monuments to pure empiricism. They receive bytes from the
network, honour range requests, report status codes and must treat redirects
as an explicit trust decision.
Their contact with reality is direct and unsentimental. But they have no
memory of meaning. A downloaded file is a sequence of bytes at a path. The
tool does not know whether the bytes are a new version, an old version, a
partial copy or a duplicate. It cannot decide what changed, and it cannot
preserve history.

Each tool is correct within its own domain and incomplete outside it. The
common failure is not a bug in either tool. It is the absence of a synthesis.
Developers bridge the gap with shell pipelines, checksum files, ad hoc naming
conventions and scripts that re-download data already present on disk.

Mesh performs the synthesis at the level of the repository.

The rational categories are local and explicit:

* **Identity.** Every file and every chunk is named by its BLAKE2b-512 digest.
  Two byte sequences with the same digest are treated as the same object. This
  is the category of unity: multiplicity is reduced to identity without
  consulting the network.
* **Causality.** Snapshots form a directed acyclic graph. A snapshot names its
  tree and up to two parents. History is not a list of timestamps pasted onto
  files; it is a structure of dependence.
* **Relation.** A tree is a manifest that relates paths, sizes, modification
  times, whole-file hashes and chunk boundaries. The manifest is the
  intelligible form of the working directory.
* **Modality.** The object store distinguishes what exists (content already
  stored), what is possible (content announced by a remote manifest) and what
  is necessary for a requested operation (the objects that must be fetched).

The empirical material arrives through libcurl:

* an HTTP response body carrying a remote manifest;
* headers such as ``X-Mesh-Hash`` and ``X-Mesh-Tree``;
* object downloads addressed by hash;
* JSON requests and responses carrying snapshot metadata.

Neither side is allowed to absorb the other. The network does not dictate the
local object format. A remote server cannot write arbitrary paths into the
working tree; remote paths are validated before use. Conversely, local
rational structure does not pretend that the network is reliable. A transfer
can fail at any byte, and the next attempt resumes from what is actually on
disk, measured by ``stat``, not by an optimistic in-memory counter.

The synthesis appears in three concrete places.

**Download plus identity.**
File mode of ``mesh obtain`` streams bytes to disk and then hashes the file with
BLAKE2b. The resulting digest is printed and recorded in the monitor status.
The empirical act of downloading is immediately subsumed under the rational
category of identity. The file is no longer merely "the thing at this path";
it is a specific content object with a verifiable name.

**Transfer plus history.**
``mesh sync`` does not merely copy files. It retrieves a remote manifest,
compares it with the local tree, fetches only missing objects, stages genuine
collisions as pure variants, and pushes a snapshot description back to the
peer. The network exchange is interpreted through the local categories of
tree, snapshot and parent. Bandwidth is saved because identity is known before
the bytes are requested.

**Conflict plus judgement.**
When local and remote versions of the same path differ, Mesh refuses to
simulate a resolution by writing conflict markers into the user's source. The
original file remains untouched. The two pure versions are placed in
``.mesh/store/conflicts/`` under a readable basename or a path-derived storage
key when basenames collide. The user then makes a judgement with
``mesh choose alpha`` or ``mesh choose beta``. The tool
presents the intuitions; the user supplies the decision. No synthetic text
pretends to be a synthesis that has not occurred.

Kant's formula is often summarized as: thoughts without content are empty,
intuitions without concepts are blind. Git without network is a system of
empty concepts. Wget without history is a stream of blind intuitions. Mesh is
the attempt to make each inform the other without dissolving either into the
other. The local categories give the network data meaning; the network data
gives the local categories reach.

This is not a metaphor pasted onto an architecture diagram. It is a design
constraint. Whenever a feature would require Mesh to choose between local
rationality and empirical honesty, the implementation keeps both and makes
the boundary explicit. That is why manifests are text, why objects are
immutable, why remote paths are checked, why downloads resume from measured
disk state and why conflicts are isolated instead of disguised.


Cartesian Analysis (Pragmatic Content-Defined Chunking)
-------------------------------------------------------

René Descartes proposed a method of disciplined doubt whose second rule is
usually stated as: divide each difficulty into as many parts as possible and
as necessary for better resolution. The rule is not an invitation to infinite
decomposition. It is an instruction to cut at joints that make the problem
tractable.

Large-file storage is such a difficulty. A naive version control system treats
a file as one indivisible object. Change one byte in a ten-gigabyte file and
the system stores, transfers and hashes ten gigabytes again. The difficulty
has not been divided; it has been swallowed whole.

Academic content-defined chunking answers with rolling hashes and sliding
windows. A polynomial or Rabin fingerprint rolls across every byte, and a
boundary is declared when the fingerprint satisfies a mask. The approach is
elegant and content-sensitive. It is also expensive in exactly the dimension
that matters for a tool meant to run on laptops, CI machines and small
servers: it touches every byte with a non-trivial arithmetic loop, often more
than once, and it makes chunk boundaries depend on a global parameter set that
must be tuned and documented.

Mesh applies the Cartesian rule with engineering restraint.

**Cut at fixed, predictable intervals.**
Files of 50 MiB or more are divided into nominal blocks of 10 MiB. The
interval is a compile-time constant, visible in ``core.h`` as
``MESH_CHUNK_SIZE``. A reader can predict where a block begins without
running a hash function over the preceding data. Sequential I/O remains
sequential. The page cache and the disk scheduler see large, aligned reads
instead of a storm of small probes.

**Repair only the seam.**
A pure fixed-size split has one weakness: inserting a byte near the beginning
shifts every later boundary, so every later chunk changes. Mesh addresses this
with a bounded realignment scan. At each nominal boundary it looks ahead at
most 4 KiB (``MESH_CHUNK_WINDOW``) for a NUL byte or a newline. If one is
found, the boundary moves just past it. If not, the nominal boundary stands.
The scan is linear, local and bounded. It does not roll over the whole file,
and it does not require a tuned mask.

The consequence is pragmatic rather than mathematically perfect. A local edit
usually changes one chunk and possibly the next boundary. A large insertion
can still shift boundaries for the remainder of the file. That is an accepted
trade. The common case, editing a region of a large log, database dump or
media container, avoids re-storing the entire file. The worst case remains
correct, only less efficient.

**Treat small files as single objects.**
Below 50 MiB (``MESH_LARGE_FILE``) the difficulty is not worth dividing.
The file is hashed and stored as one blob. Creating a chunk table for a
20 KiB source file would add metadata, indirection and system calls without
saving a meaningful number of bytes. The threshold is deliberately generous:
modern source trees contain many small files, and the per-file overhead of a
manifest line is already the dominant cost.

**Hash chunks in parallel.**
Once the file is divided, chunk hashing is embarrassingly parallel. Mesh
builds an array of ``struct mesh_hash_job`` describing path, offset and
length, then hands it to a worker pool. The pool size is the number of online
processors, capped at 64. Workers pull indices under a mutex and hash
independent ranges. No chunk depends on another chunk's digest, so no
ordering constraint is introduced.

**Store only what is new.**
Each chunk digest names an immutable object. Before writing, the store checks
whether the object already exists. A one-kilobyte edit in a 100 MiB file
therefore produces one new chunk object and leaves the other nine untouched.
The saving is visible both locally and on the wire: ``mesh sync`` asks for
objects by hash, so the peer never sends content the client can already name.

The Cartesian rule says to divide difficulties into as many parts as are
necessary. It does not say to maximize the number of parts. Mesh divides large
files because the division pays for itself in storage and bandwidth. It
refuses to divide small files because the division would cost more than it
saves. It uses a fixed interval because predictability is a feature. It scans
a 4 KiB window because a small amount of content sensitivity removes the
worst seam behaviour at negligible cost.

This is the difference between an academic demonstration and a tool that
respects I/O. The demonstration optimizes a boundary-quality metric. The tool
optimizes the user's time, disk lifetime and network bill. Mesh chooses the
second without pretending that the first is irrelevant.

The same discipline appears elsewhere. Manifests are plain text because a
human can inspect them and a script can parse them. Objects are written with
write-then-rename because atomicity is cheaper than recovery logic. Status is
a single JSON file because a GUI should not need a protocol to ask "what is
happening?". Each decision divides a large difficulty into a part small enough
to solve completely.

Descartes doubted everything that could be doubted and rebuilt knowledge from
clear, distinct parts. Mesh does not doubt the value of history or the reality
of the network. It doubts the necessity of complexity. Where a simple part
solves the problem, the simple part is chosen and documented. Where a simple
part would be a lie, the complexity is kept and isolated. That is the
Cartesian method applied to a version control system: not more parts for their
own sake, but the right parts, cut where the material actually separates.


The Aesthetics of Density
-------------------------

The two arguments above justify the architecture. They also justify the
layout of the source. A program is read many more times than it is written,
and much of that reading happens through an SSH session, a terminal split, a
Vim window or an Emacs buffer. Vertical space and the left margin are
therefore engineering resources, not decoration.

The Alves style treats them that way. Control flow begins at column zero.
Blank lines are not used as punctuation. Braces bind to the construct they
close. A function that would need a fourth level of nesting is divided into
guards or helpers. None of this changes what the compiler emits; all of it
changes what a reviewer must hold in working memory.

This is the Cartesian rule applied to code layout: divide the function at the
points where control actually separates, and do not add parts that serve only
appearance. It is also a Kantian discipline: the rational structure of the
program must be visible to the empirical reader who receives it through a
narrow terminal. A layout that hides the structure behind indentation and
blank lines is a failure of synthesis at the smallest scale.

The result is measurable but not merely numeric. The rewrite removed 1,041
lines, 12.6% of the C source, left no decorative blank lines and kept the
build warning-free. The number is evidence, not the goal. The goal is that a
reader can see the error path, the ownership rule and the next branch without
scrolling. See :doc:`alves_style_manifesto`,
:doc:`complexity_guardrails` and :doc:`metrics_and_line_optimization`.

The same argument explains why the style is optional for individuals and
mandatory for the trunk. A veteran who has internalised another layout is not
wrong about C; the veteran is being asked to accept a different interface for
review. The next document, :doc:`onboarding_and_veterans`, states the
governance compromise: no forced conversion, no drive-by reformatting, and a
trunk that converges one reviewed patch at a time.


Closing Note
------------

The two arguments meet. Kant supplies the structure that makes network data
meaningful: identity, causality, relation and modality. Descartes supplies the
discipline that keeps the structure affordable: divide only where division
pays, and keep every part small enough to verify.

Mesh is the resulting wager. A tool can be fast without being careless, simple
without being naive, and connected without surrendering its memory. The code
is the argument. The benchmarks are the evidence. The layout is the grammar
that makes the argument readable. The rest is commentary.

.. seealso::
   For the current implementation boundaries and safe operating mitigations, see
   :doc:`limitations`.
