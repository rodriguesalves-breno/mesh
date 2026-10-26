Benchmarks
==========

Mesh makes performance claims, so it must publish a protocol that can falsify
them. This document defines the workloads, the metrics, the environmental
controls and the tables used to report results. It deliberately contains no
invented numbers. A table is a place to record a measurement, not a
substitute for one.

.. contents::
   :depth: 2
   :local:


What Is Being Measured
----------------------

Mesh optimizes four things:

1. **Snapshot latency** - the wall-clock time to record a working tree.
2. **Incremental cost** - the extra time and bytes caused by a small edit in a
   large file.
3. **Transfer efficiency** - the bytes sent over the network for a given
   change.
4. **Resident memory** - the maximum RSS while processing large inputs.

It does not optimize interactive merge, history browsing or repository
maintenance, because those features are intentionally small or absent in
version 1.0. A benchmark that measures them would be measuring something else.

The comparison target is Git for local snapshots and a plain HTTP client for
transfers. Comparisons must state what is included. A Git command that updates
an index, writes a commit and runs hooks is not equivalent to ``mesh save``,
which scans, hashes, chunks, stores, writes a manifest, appends history and
exports a patch. Report both the end-to-end command and the isolated phase when
possible.

Do not claim that Mesh is "faster than Git" without naming the workload. On a
tree of 100,000 unchanged files, Git's index can answer "what changed?" without
re-hashing content. Mesh hashes the tree on every save by design. On a large
binary that changed in one region, Mesh's chunk reuse can avoid work that a
whole-file snapshot performs. The honest statement is workload-specific.


Source-Density Measurement
--------------------------

Mesh also tracks one source-level metric: physical lines of C. It is not a
runtime benchmark, and it must never be reported as one. It answers a review
question: how much vertical space does a reader have to traverse to verify a
change? The Alves style treats that space as a cost, alongside wall time and
resident memory.

The baseline was 8,272 lines across ``src/*.c``, ``src/*.h`` and the former
standalone C installer. The Alves rewrite produced 7,231 lines, a reduction of
1,041 lines or 12.6%. This is a historical measurement: the C installer has
since been replaced by ``install_uninstall.py``. The Makefile and the documentation are
excluded. Blank lines
fell from 743 to zero; the remaining 298 lines came from brace reflow,
declaration merging and guard-clause extraction.

.. list-table:: Source line count
   :header-rows: 1
   :widths: 30 18 18 34

   * - Metric
     - Before
     - After
     - Delta
   * - Physical lines
     - 8,272
     - 7,231
     - -1,041 (-12.6%)
   * - Blank lines
     - 743
     - 0
     - -743
   * - Non-blank lines
     - 7,529
     - 7,231
     - -298
   * - Source bytes
     - 197,377
     - 166,349
     - -31,028 (-15.7%)
   * - Lines over 100 columns
     - 1
     - 0
     - -1

The number is reproducible with ``wc -l`` and ``wc -c`` over the same file
set. It is a proxy for review surface, not a proof of quality: a shorter file
can still be wrong, and a longer file can still be clear. The claim is
narrower and testable: given the same behaviour and the same warning-free
build, the dense layout removes rows that carried no information. The
per-file accounting and the structural analysis are in
:doc:`metrics_and_line_optimization`.

Do not mix this table with the latency, memory and transfer tables below.
Source lines do not consume RAM at run time and do not change the object
code by themselves. The runtime claims are established only by the workloads
and protocol in the rest of this document.


Test Environment
----------------

Record the following for every published result.

.. list-table::
   :header-rows: 1
   :widths: 28 72

   * - Item
     - Record
   * - CPU model, cores, threads
     - e.g. "8 cores / 16 threads, 3.8 GHz boost"
   * - RAM size and speed
     - e.g. "32 GiB DDR5-5600"
   * - Storage model
     - NVMe, SATA SSD, HDD or tmpfs; state the filesystem (ext4, xfs, btrfs,
       zfs).
   * - Kernel and libc
     - ``uname -a`` and ``ldd --version``.
   * - Mesh version and build flags
     - ``mesh --version``; ``-O3 -Wall -Wextra -std=c17``.
   * - Git version, if comparing
     - ``git --version`` and the relevant configuration.
   * - CPU governor
     - ``performance`` for latency tests; note if not changed.
   * - Filesystem mount options
     - ``noatime``, compression, encryption; these matter.
   * - Network path
     - loopback, LAN or WAN; link rate, RTT, loss and shaping.
   * - Server implementation
     - name, version and storage backend.

Run each measurement at least five times. Report the median and the full range.
A single run is an anecdote. If the variance is larger than the effect you are
trying to show, the effect is not established.

For cold-cache tests, drop caches between runs (requires root):

.. code-block:: sh

   sync
   echo 3 | sudo tee /proc/sys/vm/drop_caches

For warm-cache tests, run the workload once to populate the page cache, then
measure. State which one you are reporting. Most developer work is warm-cache;
most CI runners are cold-cache. Both are legitimate, but they answer different
questions.

Pin the CPU governor and avoid background builds, indexing daemons and
containers during the run. Record whether the machine is a laptop on battery
or a server on a controlled power profile.


Workloads
---------

W1 - Small source tree
~~~~~~~~~~~~~~~~~~~~~~

A realistic source tree: 2,000 files, 20 MiB total, mixed text and small
binaries, depth up to 8, paths with spaces in at least 10 files.

Purpose: baseline snapshot latency and per-file overhead.

.. code-block:: sh

   mkdir -p /tmp/mesh-bench/w1 && cd /tmp/mesh-bench/w1
   mesh start
   # populate the tree with the generator of your choice
   time mesh save "initial"
   time mesh save "unchanged"
   printf '\n/* edit */\n' >> src/main.c
   time mesh save "one edit"

W2 - Many files
~~~~~~~~~~~~~~~

100,000 files of 1 KiB each, distributed across 1,000 directories.

Purpose: manifest size, sorting cost, memory per entry and status file size.

.. code-block:: sh

   mkdir -p /tmp/mesh-bench/w2 && cd /tmp/mesh-bench/w2
   mesh start
   # create the files with a deterministic generator
   /usr/bin/time -v mesh save "100k files"

Watch maximum RSS. The file list is proportional to the number of entries, not
to their total size.

W3 - Large binary, local edit
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

A 1 GiB file of pseudo-random bytes. Save it, then overwrite 1 KiB at offset
512 MiB and save again.

Purpose: chunk reuse. The second save should create one new content object
(the edited chunk) and leave the other chunks untouched. Boundary realignment
may add a neighbour if the edit crosses a seam.

.. code-block:: sh

   mkdir -p /tmp/mesh-bench/w3 && cd /tmp/mesh-bench/w3
   mesh start
   dd if=/dev/urandom of=big.bin bs=1M count=1024 status=none
   /usr/bin/time -v mesh save "big initial"
   printf 'X' | dd of=big.bin bs=1 seek=$((512*1024*1024)) conv=notrunc status=none
   /usr/bin/time -v mesh save "one kilobyte edit"

Count objects before and after:

.. code-block:: sh

   find .mesh/store/objects -type f | wc -l

W4 - Append-heavy log
~~~~~~~~~~~~~~~~~~~~~

A 2 GiB text log, then append 10 MiB at the end.

Purpose: boundary behaviour at end-of-file and the cost of re-hashing the
unchanged prefix. Mesh does not maintain an index, so the prefix is read and
hashed again; only the changed tail produces new objects. Report both time and
bytes read, and do not hide the re-hash cost.

W5 - Resume
~~~~~~~~~~~

A 4 GiB file served over a shaped link. Kill the client at 40%, restart it and
measure the bytes transferred on the second run.

Purpose: verify that ``stat()``-driven ``Range`` requests work and that no
whole-file restart occurs.

.. code-block:: sh

   # terminal 1: serve the file with a range-capable server
   # terminal 2:
   mesh obtain http://127.0.0.1:8000/big.iso
   # interrupt with Ctrl-C at roughly 40%
   mesh obtain http://127.0.0.1:8000/big.iso

Measure server bytes sent, not just wall time. A resumed transfer should send
approximately the remaining bytes plus protocol overhead.

W6 - Sync after a small change
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Two repositories with a 1 GiB file. Change 1 KiB on one side, sync, and measure
uplink and downlink bytes.

Purpose: manifest comparison, object deduplication and ``missing_objects``
upload. The expected shape is a small manifest exchange plus one chunk in each
direction, not a gigabyte.

W7 - Conflict staging
~~~~~~~~~~~~~~~~~~~~~

Two repositories with divergent changes in 100 small text files. Sync and
measure the time to stage 100 conflicts, the size of the conflict index and the
time to resolve them.

Purpose: conflict path cost and index scaling. The working files must remain
byte-identical during staging; verify with checksums.

W8 - Patch export
~~~~~~~~~~~~~~~~~

A commit that modifies 100 text files, plus one binary file and one 10 MiB
file. Measure patch generation time and verify that ``git apply`` accepts the
text portion.

Purpose: bridge overhead and the cost of the explicit omissions.


Metrics
-------

.. list-table::
   :header-rows: 1
   :widths: 22 16 62

   * - Metric
     - Unit
     - How to collect
   * - Wall time
     - seconds
     - ``/usr/bin/time -f '%e'`` or a monotonic timer around the command.
   * - User and system time
     - seconds
     - ``/usr/bin/time -f '%U %S'``.
   * - Maximum RSS
     - KiB
     - ``/usr/bin/time -v`` (``Maximum resident set size``).
   * - Bytes read
     - bytes
     - ``/proc/self/io`` (``read_bytes``) or the block device counters.
   * - Bytes written
     - bytes
     - ``/proc/self/io`` (``write_bytes``); note that page cache delays the
       physical write.
   * - Objects created
     - count
     - ``find .mesh/store/objects -type f`` before and after.
   * - Chunks created
     - count
     - Compare the ``C`` line count in the two manifests.
   * - Manifest size
     - bytes
     - ``wc -c .mesh/store/trees/<hash>``.
   * - Status size
     - bytes
     - ``wc -c .mesh/monitor/status.json``.
   * - Network bytes
     - bytes
     - Server access log or interface counters; separate request and response
       bodies from headers.
   * - Requests
     - count
     - Server log; count GET, POST and PUT separately.
   * - Peak threads
     - count
     - ``/proc/<pid>/status`` (``Threads``) sampled during a large hash.
   * - CPU utilization
     - percent
     - Derived from user+system divided by wall time, capped at core count.

For network tests, record the request count as well as bytes. A protocol that
saves bytes but doubles round trips can be slower on a high-latency link. For
local tests, record inode and metadata operations if the filesystem exposes
them; a snapshot tool is often limited by metadata, not bandwidth.


Reporting Tables
----------------

Local snapshots
~~~~~~~~~~~~~~~

.. list-table::
   :header-rows: 1
   :widths: 12 30 12 12 12 22

   * - Run
     - Workload
     - Wall
     - User
     - Sys
     - Max RSS
   * - W1-a
     - 2k files, first save
     - 
     - 
     - 
     - 
   * - W1-b
     - 2k files, unchanged resave
     - 
     - 
     - 
     - 
   * - W1-c
     - 2k files, one edit
     - 
     - 
     - 
     - 
   * - W2
     - 100k files, first save
     - 
     - 
     - 
     - 
   * - W3-a
     - 1 GiB binary, first save
     - 
     - 
     - 
     - 
   * - W3-b
     - 1 GiB binary, 1 KiB edit
     - 
     - 
     - 
     - 

Chunk and object reuse
~~~~~~~~~~~~~~~~~~~~~~

.. list-table::
   :header-rows: 1
   :widths: 30 16 16 18 20

   * - Workload
     - Objects before
     - Objects after
     - New objects
     - New chunks
   * - W3 first save
     - 
     - 
     - 
     - 
   * - W3 1 KiB edit
     - 
     - 
     - 
     - 
   * - W4 append 10 MiB
     - 
     - 
     - 
     - 

Transfer
~~~~~~~~

.. list-table::
   :header-rows: 1
   :widths: 26 14 14 14 16 16

   * - Workload
     - Uplink
     - Downlink
     - GETs
     - PUTs
     - POSTs
   * - W5 resume at 40%
     - 
     - 
     - 
     - 
     - 
   * - W6 sync, 1 KiB change
     - 
     - 
     - 
     - 
     - 
   * - W6 sync, no change
     - 
     - 
     - 
     - 
     - 

Conflicts and patches
~~~~~~~~~~~~~~~~~~~~~

.. list-table::
   :header-rows: 1
   :widths: 30 18 18 18 16

   * - Workload
     - Wall
     - Index bytes
     - Variants staged
     - Max RSS
   * - W7 stage 100 conflicts
     - 
     - 
     - 
     - 
   * - W7 resolve 100 conflicts
     - 
     - 
     - 
     - 
   * - W8 patch export
     - 
     - 
     - 
     - 

Fill the cells with medians and put the range in a footnote or a second table.
Do not round a 12% difference into "about the same". Do not report a speedup
that disappears when the cache is cold.


Expected Shapes
---------------

These are qualitative expectations derived from the implementation, not
measurements. They tell a reviewer what a plausible result looks like.

.. list-table::
   :header-rows: 1
   :widths: 28 72

   * - Workload
     - Expected shape
   * - W1 first save
     - Linear in total bytes for hashing and I/O; sorting dominates only for
       very large file counts.
   * - W1 unchanged resave
     - Similar to the first save, because content is re-hashed. Manifests and
       objects are reused, so write volume is small.
   * - W1 one edit
     - First save cost plus one new object and one new manifest. The old
       manifest remains.
   * - W2 100k files
     - Wall time grows with metadata operations; RSS grows with the entry
       array and path strings. The status file stays small.
   * - W3 first save
     - One whole-file object plus a chunk table and one object per chunk. I/O
       is sequential with a 4 KiB look-ahead per boundary.
   * - W3 1 KiB edit
     - One new chunk object and one manifest; other chunk objects are reused.
       Time is still linear in file size because the file is re-read and
       re-hashed.
   * - W4 append
     - The prefix is re-hashed; the tail produces new chunks. Time grows with
       file size even though write volume does not.
   * - W5 resume
     - Second run transfers roughly the missing bytes; no full restart if the
       server honours ``Range``.
   * - W6 no change
     - One GET, header comparison, no object transfer.
   * - W6 small change
     - Manifest exchange plus one chunk up and one down; request count should
       be constant, not proportional to file size.
   * - W7 conflicts
     - One variant pair per path; working files unchanged; index grows linearly
       with the conflict count.
   * - W8 patch
     - Text files are emitted as full replacement hunks; binary and oversized
       files are omitted with warnings, keeping ``git apply`` valid.

If a measurement contradicts an expected shape, investigate before publishing.
A second save that creates hundreds of objects for a one-byte edit suggests a
boundary or hash bug. A sync that transfers the whole file for a one-kilobyte
edit suggests object deduplication is not working. A resume that starts at zero
suggests the server is ignoring ``Range`` or the client is truncating the
partial file.


Reproducibility
---------------

Publish the generator, not just the results. The exact byte pattern matters for
chunk boundaries: pseudo-random data, zeros and newline-rich text produce
different boundary decisions. Use a fixed seed and document it.

A minimal reproducible record contains:

.. code-block:: text

   mesh commit: <source revision or archive digest>
   build: cc (version), -std=c17 -O3 -Wall -Wextra
   kernel: <uname -a>
   filesystem: <mount options>
   cpu: <model, cores>
   ram: <size>
   storage: <model and interface>
   workload: <generator command and seed>
   repetitions: <n>
   cache state: cold or warm
   network: <loopback/LAN/WAN, shaping parameters>

Store raw per-run output next to the summary. A table of medians without raw
data is not reproducible.

For network tests, run the peer locally when measuring client CPU, and over a
shaped link when measuring bytes and latency. A loopback test cannot show the
benefit of avoiding a round trip, because loopback round trips are nearly free.
A WAN test cannot isolate client CPU, because the network dominates. Report
both when the claim involves both dimensions.


Interpreting Results
--------------------

Three traps are common in storage benchmarks.

**Page cache.**
A "fast" second run may be reading from RAM. State whether the cache was
dropped. A cold-cache result is usually more relevant to CI; a warm-cache
result is usually more relevant to a developer's inner loop.

**Write-back.**
``write()`` returning does not mean the data is on stable storage. Mesh calls
``fsync()`` on objects and history appends, so its wall time includes durability
cost. A competitor that skips ``fsync`` will look faster until the machine
loses power. Compare durability semantics before comparing numbers.

**Compression and deduplication at another layer.**
ZFS, btrfs compression, SSD write amplification and the page cache can all
change the observed cost. Report the filesystem and mount options. If the
storage compresses zeros, a zero-filled 1 GiB file is not a 1 GiB I/O test.

For CPU-bound hashing, report the number of threads and the CPU utilization. A
result with 12 threads on a 12-core machine at 40% utilization is not a
hashing limit; it is an I/O limit. A result at 100% utilization across all
cores is a hashing limit. The distinction tells a maintainer where to optimize.

For memory, report peak RSS and the largest input. A 100k-file tree and a 1 GiB
file stress different structures: the first stresses the entry array, the
second stresses chunk arrays and streaming buffers. Neither should make RSS
grow with file size; if it does, a buffer is being held whole.


Publishing a Result
-------------------

A complete report has six parts:

1. **Claim.** One sentence, with the workload and direction. Example: "On a
   1 GiB file with a 1 KiB edit, the second save creates one new chunk object."
2. **Environment.** The table above.
3. **Method.** Commands, repetitions, cache state, network shaping.
4. **Raw data.** Per-run values, not only medians.
5. **Summary table.** Median and range.
6. **Threats to validity.** What the test does not show. Example: "This does
   not measure a 100k-file tree, where metadata dominates."

A benchmark that cannot be reproduced is marketing. Mesh prefers a modest,
reproducible number to an impressive, unexplained one. The code is fast because
it avoids unnecessary work; the benchmark exists to show which work was
avoided, not to win an argument. The Alves layout is part of the same
discipline: it removes rows that carry no information, so the reviewer can see
which work the code actually performs.

.. seealso::
   For the current implementation boundaries and safe operating mitigations, see
   :doc:`limitations`.
