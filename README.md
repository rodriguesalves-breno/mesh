# Mesh

Mesh is a small, fast, single-step version control system for POSIX systems.
It snapshots a working tree directly into content-addressed history, without an
index, a staging area or a separate commit phase.

Mesh is deliberately conservative:

- C17 and POSIX APIs; the core uses libcurl, pthreads and ICU4C Unicode APIs.
- BLAKE2b-512 content addressing for files, manifests and history nodes.
- Resumable HTTP downloads and a simple manifest/object synchronization
  protocol.
- Snapshot import from GitHub, GitLab, Gitea, Forgejo and compatible archives.
- Symbolic links, executable modes and nanosecond timestamps are preserved.
- Untrusted remote paths are validated and materialized with
  `openat()`/`renameat()` and `O_NOFOLLOW`.

Version 1.0 is a complete local VCS and a reference HTTP peer protocol. It is
not a hosted forge, a multi-tenant server or a replacement for a distributed
consensus system.

## Build

Requirements:

- A C17 compiler (GCC or Clang)
- POSIX headers and pthreads
- libcurl development headers and library
- ICU4C `icu-uc` development headers/library and `pkg-config`
- `make` and a POSIX shell to run the build
- Python 3 with its standard library for the installer and test suite

```sh
make
make test
```

The binary is `./mesh`. The standalone installer source is `install_uninstall.py`, published as
`./mesh-install` by the Makefile; it needs Python 3 but no C compilation.

To run the installer:

```sh
make install
# or, explicitly:
make mesh-install
./mesh-install --prefix "$HOME/.local"
```

The installer verifies the toolchain, builds Mesh when needed, installs the
binary, places `mesh_icon.svg` next to it and adds an idempotent PATH block to
the user's shell startup files. Existing shell symlinks are preserved.

To uninstall, use the same prefix and binary directory that were used during
installation:

```sh
make uninstall
# or, explicitly:
./mesh-install --uninstall --prefix "$HOME/.local"
```

The common uninstall removes the binary, both icon copies and the managed PATH
blocks, while preserving global settings. The Makefile targets use the default
prefix; for a custom location run `mesh-install` directly with the same
`--prefix` and `--bin-dir`. The definitive removal also deletes the global
configuration directory:

```sh
make purge
# or, explicitly:
./mesh-install --uninstall --purge --prefix "$HOME/.local"
```

`--purge` implies `--uninstall`. Project repositories are never touched:
`.mesh` directories and their snapshots, credentials and working files remain
intact. Use `--no-shell` to leave shell startup files unchanged, or `--quiet`
for scripted removal.

## Quick start

```sh
mkdir notes
cd notes

mesh config --global user.name "Ada Lovelace"
mesh config --global user.email "ada@example.com"
mesh start

printf 'alpha\n' > a.txt
mesh save "initial snapshot"

printf 'beta\n' >> a.txt
mesh save "second snapshot"

mesh check
```

Typical output from `mesh save`:

```text
saved 5d08affd9cb2  second snapshot
  files: 3 (5.7 KiB)   +0 ~1 -0   snapshots: 2
```

There is no `add` step. `mesh save` scans, hashes, stores and records the
snapshot in one operation. An unchanged tree still creates a snapshot.

To start from a repository hosted on a Git forge:

```sh
mesh obtain https://github.com/owner/repo
mesh obtain --ref v1.2.0 https://gitlab.com/group/project custom-directory
```

`mesh obtain` downloads the forge source archive, extracts the current tree,
initializes `.mesh` and records the first snapshot. It imports the tree, not
the Git history or refs. The source URL is kept in the snapshot message.

The same command is also the wget/curl-style downloader. When the URL points
to a file, `mesh obtain` resumes from the bytes already on disk and reports the
BLAKE2b digest. Common curl download options are accepted, including redirects,
custom headers, a custom user agent, compression and rate limits:

```sh
mesh obtain https://example.com/data.bin
mesh obtain https://example.com/data.bin -o copy.bin
mesh obtain --file https://example.com/download
mesh obtain -L -O https://example.com/releases/latest.tar.gz
mesh obtain -C - -o image.iso https://example.com/image.iso
mesh obtain -C 104857600 -o image.iso https://example.com/image.iso
mesh obtain -o - https://example.com/data.bin > data.bin
mesh obtain -I https://example.com/data.bin
mesh obtain -w '%{http_code} %{size_download}\n' -o data.bin https://example.com/data.bin
mesh obtain -b cookies.txt -c cookies.txt -o page.html https://example.com/page
mesh obtain -x http://proxy.example:8080 -o data.bin https://example.com/data.bin
mesh obtain -H "X-Request-ID: 42" -A mesh/1.0 https://example.com/data.bin
```

`--repo` and `--file` resolve ambiguous URLs explicitly. `-L` is opt-in;
redirects are disabled by default and are restricted to HTTP/HTTPS. `-o -`
writes the body to stdout with metadata on stderr; `-I` prints response headers;
and `-w` exposes a curl-style subset of transfer metrics. Cookies and proxies
are supported through libcurl. TLS verification is always on unless `-k` is
given, and repository mode refuses `-k` when a bearer token would be sent.

Identity is optional but Git-style. `user.name` and `user.email` can be set
globally or per repository; a local setting overrides the global one. When both
are present, the author is stored in the snapshot history and exposed by
`mesh log`, `mesh check` and the JSON metadata. A repository without identity
still saves, but prints a warning and writes the legacy history format.

Global settings live in `$XDG_CONFIG_HOME/mesh/config` when that variable is an
absolute path, or `~/.config/mesh/config` otherwise. Local settings live in
`.mesh/config`. If neither file supplies an e-mail, `EMAIL` is used as a
last-resort fallback.

## Commands

| Command | Purpose |
| --- | --- |
| `mesh start` | Initialize a repository and provision the shell/IDE environment. |
| `mesh config [--global] KEY [value]` | Read or write `user.name` and `user.email` identity settings. |
| `mesh branch [list\|create\|switch\|delete]` | Manage named branches and the current branch pointer. |
| `mesh tag [NAME [SNAPSHOT]]` | List, create or delete lightweight snapshot tags. |
| `mesh reflog` | Show recent HEAD and branch movements. |
| `mesh ignore` | List the active `.meshignore` rules. |
| `mesh clean (-n\|-f) [-x]` | List or remove working-tree files not tracked by HEAD. |
| `mesh save "msg"` | Snapshot the current working tree. |
| `mesh log` | Show the snapshot history, newest first. |
| `mesh diff [a [b]]` | Compare two snapshots, or a snapshot/HEAD against the working tree. |
| `mesh restore <snap> [--force] [path...]` | Restore files from a snapshot. Extra files are never deleted. |
| `mesh check` | Print the state in `.mesh/monitor/status.json`. |
| `mesh choose alpha\|beta` | Resolve staged sync conflicts. |
| `mesh git-info` | Report `.git` beside `.mesh` without opening or modifying Git metadata. |
| `mesh admin <command>` | Inspect, verify and maintain repository administration state. |
| `mesh obtain [opts] URL [DEST]` | Clone a forge repository or download a file with resumable, curl-compatible transfers. |
| `mesh fetch [opts] URL [DEST]` | Deprecated alias for `mesh obtain --file`. |
| `mesh auth [--host HOST] [TOKEN]` | Store or report host-scoped bearer credentials. |
| `mesh send URL` | POST the snapshot manifest and bounded history as JSON. |
| `mesh sync URL` | Bidirectional manifest and object synchronization. |

`mesh --help` and `mesh --version` print the current command table and version.
`mesh obtain` selects repository mode for forge URLs and file mode for file
URLs; `--repo` and `--file` override that choice. File downloads never attach
repository credentials automatically; `-L` is opt-in and repository mode
refuses redirects or `-k` when a bearer credential would be sent. `mesh send`
and `mesh sync` use credentials that match the target host. Repository mode
accepts `MESH_TOKEN` for private forge archives and also uses credentials from
the current repository.

## Administration plane

`mesh admin` adds a read-mostly maintenance surface without changing the
snapshot format. The commands are:

```sh
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
```

`gc` is a dry run by default. `--apply` requires a non-empty history and the
repository lock; it removes only objects unreachable from every snapshot in
local history, not snapshots or history lines. `verify` checks every object
reachable from a manifest, and `export` writes graph metadata as JSON for
dashboards and scripts. The admin plane is implemented by 13 C modules below
`src/admin*.c` and documented in `docs/admin_modules.rst`.

The `tools` directory provides standard-library Python scripts for module
inventory, style checking, dependency graphing, scaffolding, smoke testing,
documentation generation, metrics, artifact cleanup, Unicode preflight, Git
history migration/restart and read-only Git/Mesh coexistence audits.

```sh
make tools-check
make tools-test
make tools-docs
```

## Repository layout

```text
.mesh/
|-- HEAD                 current snapshot hash
|-- config               version, branch, icon name and local identity
|-- history              append-only snapshot log
|-- credentials          host-scoped bearer tokens, mode 0600
|-- lock                 repository operation lock
|-- refs/main            branch ref
|-- refs/tags/           lightweight tag refs
|-- logs/HEAD            local reflog of HEAD movements
|-- monitor/
|   |-- status.json      passive state for GUIs and scripts
|   `-- last.patch       exported Git-style patch
`-- store/
    |-- objects/         BLAKE2b-addressed content
    |-- trees/           manifests, one per tree hash
    |-- conflicts/       isolated alpha/beta variants
    |-- remote/          reserved remote metadata
    `-- tmp/             short-lived staging files
```

A root `.meshignore` file filters the working-tree scan for `save` and `diff`.
Tag refs, branch refs and the reflog are local metadata; they are not part of
the snapshot graph and are not transferred by `send` or `sync`.

A manifest uses one `F` line per object:

```text
F <hash> <size> <mtime-sec> <mtime-nsec> <mode> <kind> <path>
```

`kind` is `f` for a regular file and `l` for a symlink. A symlink's hash covers
its target bytes. The legacy four-field form `F <hash> <size> <path>` is still
accepted as a regular file with mode `644` and timestamp zero.

## Synchronization and conflicts

`mesh sync URL` announces the local head, downloads and verifies missing
objects, merges paths that are new or already identical, and pushes the local
snapshot metadata when there are no unresolved conflicts. If both sides have
different regular-file content for the same path, Mesh never writes conflict
markers into the working file. It stages two variants under
`.mesh/store/conflicts/` and leaves that working file untouched. A detected or
pre-existing conflict prevents the POST; `mesh sync` exits non-zero until the
conflicts are chosen and saved. A pull may already have materialized other,
non-conflicting paths before a conflict is found.

This is not a complete multi-writer merge protocol: there is no common-ancestor
merge or general deletion-aware three-way merge. Peers advertising
`X-Mesh-CAS: if-match` and an `ETag` receive a conditional `If-Match` publish;
stale updates (HTTP 409/412) fail instead of being reported as published.
Legacy peers remain supported but cannot prevent simultaneous-writer lost
updates. File/symlink conflicts are staged with their types and modes, and
`mesh choose` materializes the selected kind safely. See [the limitations and
mitigations](docs/limitations.rst) for protocol and operational boundaries.

Resolve a conflict with:

```sh
mesh choose alpha   # local variant
mesh choose beta    # remote variant
mesh save "resolve"
```

The conflict index stores escaped paths plus typed variant metadata. Variant
names are derived from a hash of the full path, so an index entry cannot
redirect the resolver to another file.

## Unicode preflight

Manifests preserve original UTF-8 path spelling, but the C core rejects
collisions under Unicode NFC normalization and full case-folding. Run the
read-only Python preflight before importing or migrating a tree:

```sh
python3 tools/unicode_audit.py /path/to/tree
```

The C implementation is authoritative; Python's Unicode database may be a
different Unicode release. The 4095-byte path limit and operating-system path
limits remain.

## Git history import and history restart

Import a Git ref's first-parent tree sequence into a new Mesh directory with:

```sh
MESH_BIN="$PWD/mesh" python3 tools/git_import.py \
  /path/to/git-clone /path/to/new-mesh --ref main
```

This retains tree snapshots and subjects, not merge parents, Git OIDs,
signatures, exact dates, or every branch and tag. Keep the original Git clone
until the result is reviewed.

Ordinary `mesh admin gc` removes unreachable objects but does not compact
`.mesh/history`. To restart the active history at the current working tree,
the explicit tool below creates a full backup first; it is a history reset,
not a lossless rebase. It defaults to a dry run and refuses to overwrite the
backup path:

```sh
MESH_BIN="$PWD/mesh" python3 tools/history_restart.py \
  --root . --backup ../mesh-history-backup --apply
```

The active repository gets a new root snapshot. Old IDs, refs, tags and remote
configuration remain in the backup and are not carried into the new history.

## Security model

- Repository-relative paths are rejected when they are absolute, contain
  empty/`.`/`..` components, exceed the 4095-byte bound, contain ASCII control
  bytes, or enter the reserved `.mesh`/`.git` namespaces.
- Manifests reject invalid UTF-8 and Unicode NFC/full-casefold collisions;
  the original path spelling is preserved while normalized comparison keys
  are temporary.
- Remote manifests are untrusted. Malformed records, embedded NUL bytes,
  bad hashes, non-contiguous chunks and structures above the entry/chunk
  ceilings reject the whole manifest before any write.
- Materialization walks the repository with `openat()`/`renameat()` and
  `O_NOFOLLOW`. A symlink component cannot escape the repository root.
- `mesh obtain` extracts forge archives into a private staging directory.
  Tar names pass the same repository-relative validation as every other remote
  path. Absolute names, `..`, `.mesh`, `.git`, special files, hard links and
  writes through symlink components reject the whole archive before the
  staging directory is published.
- The internal gzip/DEFLATE decoder verifies the gzip CRC and size trailer.
  Archive sizes, metadata records and entry counts are bounded.
- Objects are verified by BLAKE2b before publication, materialization and
  upload. Objects downloaded by `mesh sync` are made read-only (`0444`), like
  objects created by a local save.
- `mesh save`, `mesh sync`, `mesh restore` and `mesh choose` hold an exclusive
  `.mesh/lock`. Atomic documents are written through a temporary file,
  `fsync`-ed, renamed and followed by a parent-directory `fsync`.
- Credentials are stored with mode `0600`. Host-scoped entries are matched
  before a header is built; file downloads through `mesh obtain` never send
  them. Repository mode reads `MESH_TOKEN` or the current repository
  credentials for its archive request.
- HTTP is refused when a matching credential would be sent in clear text.
  Redirects are disabled and TLS verification is never disabled. Plain HTTP to
  a non-loopback host is allowed but prints a warning.

Credentials are not encrypted at rest. File mode and full-disk encryption are
the available protections.

## Tests

```sh
make test       # unit + local integration + HTTP sync + installer
make ci         # strict GCC build with -Werror and the full suite
make sanitize   # Clang ASan/UBSan build and the full suite
make analyze    # GCC static analyzer over the production build
make fuzz       # Clang/libFuzzer harness for the manifest parser
```

The integration tests are Python 3 scripts. They create temporary
repositories, run local HTTP peers from `tests/http_server.py` and
`tests/obtain_server.py` and exercise path safety, resumable downloads,
redirects, custom request metadata, compression, symlinks, executable modes,
conflict staging, log/diff/restore, branches, tags, the reflog, ignore rules,
clean, archive extraction and installer idempotence.
`tests/unit.c` remains the compiled C unit test; `make test` runs the C unit
binary first and then the Python integration tests. The Python peers use only
the standard library, including `hashlib` for BLAKE2b vectors and `gzip` for
the archive fixture, so no external language runtime or hash utility is
required. `PYTHON` and `PYTHONFLAGS` can be overridden for a different
interpreter or flags. `make fuzz` requires Clang with the libFuzzer and
ASan/UBSan runtimes; `FUZZ_RUNS` and `FUZZ_MAX_LEN` tune the smoke run.

## Mesh alongside Git

Mesh is an independent, lightweight snapshot and content-addressed storage
system for teams or individuals who want a simple local history and a small
HTTP exchange protocol. It is not a Git implementation, Git replacement, Git
server, or compatibility layer for Git's object database. Git remains the
source of truth for Git commits, branches, tags, index/staging, hooks, remotes,
submodules, merges, rebases, and Git transport.

Mesh can coexist in the same working directory as Git: `.git` and `.mesh` are
separate metadata stores. Mesh ignores the reserved `.git` namespace while
scanning files, and its commands do not read or write Git refs, objects, the
index, config, hooks, or remotes. Conversely, Git does not interpret Mesh
snapshots as Git commits. Keep using normal `git` commands for all Git work.

To inspect the relationship without changing either repository:

```sh
mesh git-info
python3 tools/git_coexist.py --json .
```

`mesh git-info` only reports whether `.git` is present and whether it looks like
a directory or a worktree pointer file; it deliberately does not follow a
pointer. The Python audit delegates Git status/branch interpretation to the
installed Git CLI and calls `mesh git-info`; it is read-only and reports the two
states separately. It is useful for verifying co-location, not for reconciling
histories or synchronizing Git and Mesh automatically.

A conservative shared-directory workflow is:

1. Keep Git initialized and continue using `git status`, `git add`, `git commit`,
   and your established Git remotes as usual.
2. Initialize Mesh only when you explicitly want a separate snapshot history:
   run `mesh start`, then `mesh save "message"`. Mesh creates `.mesh/` and
   leaves `.git/` alone.
3. Use `mesh diff`/`mesh log` for Mesh snapshots and Git's own tools for Git
   commits. The histories are independent; neither system imports the other's
   commit graph automatically.
4. Exchange changes through an explicit artifact when needed: Mesh can export a
   Git-compatible patch via `mesh diff`/its patch output, while Git can apply or
   review that patch using ordinary Git commands. Review paths and patch content
   before applying; Mesh does not write into Git's index or commit for you.
5. For a historical migration, `tools/git_import.py` creates a separate Mesh
   repository from a Git ref's first-parent tree sequence. It is opt-in, does
   not alter the source clone, and is not a lossless conversion. Keep the Git
   clone and verify the result before relying on the Mesh history.

This separation is intentional: Mesh adds a compact snapshot workflow where it
is useful, while Git remains fully installed, usable, and authoritative for all
Git operations. See [Git migration and coexistence](docs/git_migration.rst) and
[the architecture](docs/architecture.rst) for implementation details.

## Documentation

The Sphinx sources live in `docs/`:

- `docs/architecture.rst` — internals, formats and invariants
- `docs/commands.rst` — command reference and exit codes
- `docs/network.rst` — HTTP protocol and security notes
- `docs/gui_integration.rst` — status JSON and conflict UI contract
- `docs/limitations.rst` — documented boundaries and safe operating mitigations

## License

Mesh is free software licensed under the GNU General Public License, version 2
only. See [LICENSE](LICENSE).

Copyright (C) 2026 Breno Rodrigues Alves.
