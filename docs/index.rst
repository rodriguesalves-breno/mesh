Mesh - Introduction and Overview
================================

.. note::
   **Official Literary Announcement**

   The architectural foundation of Mesh is strictly based on the Alves Style, formalized in the upcoming book **"The Ideal Programming Style"**. Written in a direct, concise, and versatile tone across exactly 64 pages, this book establishes the definitive paradigm for high-density, zero-overhead coding.

   *Status: Coming soon. This volume will be available in the future for developers seeking maximum terminal ergonomics and logical stability.*

Mesh is a minimalist, ultra-fast version control and transfer tool written in
pure C17. The source is formatted in the Alves style: zero left margin, no
decorative blank lines, compact control syntax and a hard limit of three
control-nesting levels. It combines three jobs that traditionally require
three programs:

* a local snapshot engine with a content-addressed object store, similar in
  spirit to Git;
* a resumable file downloader, similar in spirit to Wget;
* a small HTTP client for JSON APIs and object transfer, similar in spirit to
  cURL.

The result is a single small binary, a compact on-disk format and a passive
JSON status file that graphical interfaces can read without a daemon, a socket
or a language runtime.

.. contents::
   :depth: 2
   :local:


Design Principles
-----------------

Mesh follows the Alves style rather than the conventions of an application
framework or the ceremonial layout of the Linux kernel. The style treats
vertical space and the left margin as scarce engineering resources: a
function should fit on one terminal screen, and its control flow should be
visible without counting indentation. The full argument is in
:doc:`alves_style_manifesto`.

**One binary, one job per function.**
Every source file has a narrow responsibility. ``cli.c`` parses and routes,
``graph.c`` owns snapshots, ``chunking.c`` owns the object store,
``network.c`` owns libcurl, and so on. There is no object-oriented facade and
no hidden global state.

**Density with discipline.**
Every function is written at column zero, without decorative blank lines.
Control flow is flattened with guard clauses and capped at three nesting
levels (:doc:`complexity_guardrails`). The conversion removed 1,041 physical
lines from the C source, a 12.6% reduction, while the Makefile build stayed
warning-free. The measurement and its limits are in
:doc:`metrics_and_line_optimization`.

**Predictable memory.**
Large files are streamed through bounded buffers. JSON responses use one
growable in-memory buffer with a hard ceiling. Hash workers share a compact
job array. Nothing is memory-mapped, nothing is swapped to a temporary file
just to be parsed again.

**No dangerous string functions.**
``strcpy`` and ``strcat`` are not used. Paths and messages are built with
``snprintf`` and bounded copies. Every path that reaches the file system is
validated against traversal.

**No shell scripts.**
Environment automation is performed with POSIX file APIs. ``mesh start`` and
the installer write a marked PATH block into ``.bashrc``, ``.zshrc`` and
``~/.config/fish/config.fish`` directly.

**Truthful failures.**
Disk errors, short writes, malformed hashes, unsafe remote paths and HTTP
status codes are checked at the point where they occur. A failed operation
returns a non-zero exit status and leaves the previous state readable.


What Mesh Is Not
----------------

Mesh is not a drop-in Git server and does not implement the Git wire protocol.
It is not a package manager, a build system or a text editor. It does not
provide an index, interactive rebase, hooks or submodules. The deliberate
omissions are what keep the implementation small enough to audit in one
sitting.

For teams that need the Git ecosystem, ``mesh save`` writes a Git-compatible
unified patch to ``.mesh/monitor/last.patch``. See :doc:`git_migration` for
the exact bridge and its limits.


Requirements
------------

* A C17 compiler (``cc``, ``gcc`` or ``clang``).
* ``make``.
* libcurl development headers and library.
* POSIX threads, which are part of the C library on current systems.
* Linux or a compatible POSIX system. ``mesh start`` uses
  ``/proc/self/exe`` to locate the installed icon; the rest of the program
  only needs POSIX.

BLAKE2b is compiled into the binary from ``src/blake2b.c``. No external hash
library is required, linked or probed by the build or the installer.


Build and Install
-----------------

.. code-block:: sh

   make
   ./mesh --version

The version output is exactly::

   Mesh 1.0

The standalone installer is a Python 3 program. It probes the compiler,
``make``, libcurl and pthreads when a build is needed. If a component is
missing it detects the system package manager (``apt-get``, ``dnf``, ``yum``,
``zypper``, ``pacman``, ``apk``, ``xbps-install``, ``emerge``, ``eopkg``,
``pkg`` or Homebrew), installs the required development packages through
``sudo``, ``doas`` or ``pkexec``, then re-probes the toolchain. It builds the
project, copies the binary and icon into a prefix and wires PATH for the current
user:

.. code-block:: sh

   make mesh-install
   ./mesh-install

Useful installer options:

.. list-table::
   :header-rows: 1
   :widths: 22 78

   * - Option
     - Effect
   * - ``--prefix DIR``
     - Install under ``DIR``. Defaults to ``~/.local`` for a normal user and
       ``/usr/local`` for root.
   * - ``--bin-dir DIR``
     - Put the binary in ``DIR`` instead of ``PREFIX/bin``.
   * - ``--build``
     - Force ``make`` before installing.
   * - ``--no-build``
     - Never invoke ``make``; fail if ``./mesh`` is missing.
   * - ``--no-shell``
     - Do not modify any shell startup file.
   * - ``--no-deps``
     - Never invoke a package manager. Report the missing components and
       print the exact command to run manually.
   * - ``--uninstall``
     - Remove the installed binary, both icon copies and the managed PATH
       blocks. Global configuration is preserved.
   * - ``--purge``
     - Also remove the global configuration directory. Implies
       ``--uninstall``.
   * - ``--yes``
     - Do not ask for confirmation before installing missing packages. The
       installer also proceeds without a prompt when stdin is not a terminal
       or when ``--quiet`` is used.
   * - ``--quiet``
     - Print only warnings and errors.

Dependency installation never invokes a shell: the package manager is executed
directly with a fixed list of package names. When an ``apt-get install`` fails,
the installer refreshes the package lists once with ``apt-get update`` and
retries. If no supported package manager exists, or if package installation
fails, the installer prints the exact manual command instead of guessing.

The installer installs ``mesh_icon.svg`` both next to the binary, where
``mesh start`` looks for it, and in ``PREFIX/share/mesh``. The PATH block is
idempotent: a marker prevents duplicate entries on repeated runs, and a stale
block that points at another directory is replaced atomically.

``mesh-install --uninstall`` reverses the installation: it removes the binary,
both icon copies and every managed PATH block, then removes
``PREFIX/share/mesh`` when it is empty. Pass the same ``--prefix`` and
``--bin-dir`` used during installation. Global configuration in
``$XDG_CONFIG_HOME/mesh`` (or ``$HOME/.config/mesh``) is preserved.

``mesh-install --purge`` performs the same removal and also deletes that global
configuration directory. It implies ``--uninstall``. Both modes are
idempotent, return status 0 when the requested state is already in place, and
never scan or modify project repositories. A repository's ``.mesh`` directory,
including snapshots and credentials, remains untouched.

The Makefile exposes the same operations:

.. code-block:: sh

   make uninstall    # keep global settings
   make purge        # remove global settings as well


Quick Start
-----------

.. code-block:: sh

   mkdir demo && cd demo
   mesh config --global user.name "Ada Lovelace"
   mesh config --global user.email "ada@example.com"
   mesh start
   printf 'hello mesh\n' > hello.txt
   mesh save "first snapshot"
   mesh check

``mesh start`` creates the ``.mesh`` directory, writes a default
configuration, ensures the official icon in the working tree and writes a
VS Code manifest when the editor settings do not already exist.

``mesh save`` scans the working directory, hashes every regular file with
BLAKE2b-512, stores new content in the object store, writes a manifest and
appends a snapshot node to the history. When ``user.name`` and ``user.email``
are configured, the snapshot also records its author; a repository without
identity keeps the legacy history format and still saves.

``mesh check`` reads ``.mesh/monitor/status.json`` and prints the current
state. It never rescans the working tree, so it is cheap enough to call from a
status bar or an editor extension.


Repository Layout
-----------------

.. code-block:: text

   project/
   |-- .mesh/
   |   |-- HEAD                 current snapshot hash
   |   |-- config               version, branch, icon name, local identity
   |   |-- history              append-only snapshot log
   |   |-- credentials          host-scoped bearer tokens, mode 0600
   |   |-- refs/
   |   |   |-- main             branch ref
   |   |   `-- tags/            lightweight tag refs
   |   |-- logs/
   |   |   `-- HEAD             local reflog of HEAD movements
   |   |-- monitor/
   |   |   |-- status.json      passive state for GUIs
   |   |   `-- last.patch       last exported Git-style patch
   |   `-- store/
   |       |-- objects/         BLAKE2b-addressed content
   |       |-- trees/           manifests, one file per tree hash
   |       |-- conflicts/       isolated alpha/beta variants
   |       |-- remote/          reserved for remote metadata
   |       `-- tmp/             short-lived staging files
   |-- .vscode/
   |   `-- settings.json        editor associations and icon path
   |-- mesh_icon.svg            official repository icon
   `-- ...                      user files

The object store is immutable. Objects are written to a temporary name,
flushed, and renamed into place. Manifests and status files use the same
write-then-rename discipline, so a crash leaves either the old document or the
new one, never a half-written file.


Command Summary
---------------

.. list-table::
   :header-rows: 1
   :widths: 18 82

   * - Command
     - Purpose
   * - ``mesh start``
     - Initialize a repository, install the icon and wire the shell PATH.
   * - ``mesh config [--global] KEY [value]``
     - Read or write ``user.name`` and ``user.email`` identity settings.
   * - ``mesh branch [list|create|switch|delete]``
     - Manage named branches and the current branch pointer.
   * - ``mesh tag [NAME [SNAPSHOT]]``
     - List, create or delete lightweight snapshot tags.
   * - ``mesh reflog``
     - Show recent HEAD and branch movements.
   * - ``mesh ignore``
     - List the active ``.meshignore`` rules.
   * - ``mesh clean (-n|-f) [-x]``
     - List or remove working-tree files not tracked by HEAD.
   * - ``mesh save "msg"``
     - Snapshot the working tree in one step.
   * - ``mesh check``
     - Print the current state from ``status.json``.
   * - ``mesh choose alpha|beta``
     - Resolve staged conflicts without editing user files.
   * - ``mesh obtain [opts] URL [DEST]``
     - Clone a forge repository or download a file with resumable transfers.
   * - ``mesh fetch [opts] URL [DEST]``
     - Deprecated alias for ``mesh obtain --file``.
   * - ``mesh auth [--host HOST] TOKEN``
     - Store a bearer token, optionally scoped to one host.
   * - ``mesh send URL``
     - POST the current snapshot and history as JSON.
   * - ``mesh sync URL``
     - Bidirectional manifest and object synchronization.

The complete reference, including exit codes and examples, is in
:doc:`commands`.


Documentation Map
-----------------

.. list-table::
   :header-rows: 1
   :widths: 28 72

   * - Document
     - Contents
   * - :doc:`reason`
     - The philosophical manifesto: Kantian synthesis and Cartesian analysis.
   * - :doc:`commands`
     - Every command, flag, output and exit code.
   * - :doc:`architecture`
     - C17 engine internals, data structures and storage formats.
   * - :doc:`limitations`
     - Hard bounds, deliberate non-goals, safe mitigations and compatibility risks.
   * - :doc:`admin_modules`
     - Administration command plane and the Python module tools.
   * - :doc:`network`
     - libcurl engine, resume logic, JSON API and sync protocol.
   * - :doc:`contributing`
     - Alves-style contribution rules, patch discipline and review checklist.
   * - :doc:`gui_integration`
     - Passive status contract for editors and graphical interfaces.
   * - :doc:`benchmarks`
     - Stress protocol, metrics and conceptual result tables.
   * - :doc:`git_migration`
     - Transitional equivalence between Git and Mesh.
   * - :doc:`alves_style_manifesto`
     - The density manifesto, SSH readability and one minimal ``main`` sample.
   * - :doc:`complexity_guardrails`
     - The three-level nesting cap, guard clauses and the refactored cases.
   * - :doc:`idiomatic_c_optimization`
     - Pure-C idiom versus Torvalds-style ceremony.
   * - :doc:`metrics_and_line_optimization`
     - The 1,041-line reduction and the structural accounting behind it.
   * - :doc:`ssh_ergonomics`
     - Terminal, Vim, Neovim and Emacs usability under a narrow viewport.
   * - :doc:`onboarding_and_veterans`
     - Optional adoption, team governance and veteran psychology.
   * - :doc:`ideal_programming_book_ref`
     - Cross-reference to the 64-page book and its reading paths.


Documentation Tree
------------------

.. toctree::
   :maxdepth: 2
   :caption: Mesh Documentation

   reason
   commands
   architecture
   limitations
   admin_modules
   network
   contributing
   gui_integration
   benchmarks
   git_migration
   alves_style_manifesto
   complexity_guardrails
   idiomatic_c_optimization
   metrics_and_line_optimization
   ssh_ergonomics
   onboarding_and_veterans
   ideal_programming_book_ref


License and Spirit
------------------

Mesh is distributed as a compact, self-contained source tree. You are
encouraged to read it, measure it, cut what is unnecessary and keep what is
true. The project values clear code, honest benchmarks and small diffs over
feature checklists. The Alves layout is part of that economy: it is not a
preference imposed on the reader, but a way to make the structure of the
program survive a narrow terminal and a short review window.
