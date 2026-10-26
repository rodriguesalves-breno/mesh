#!/usr/bin/env python3
"""Mesh installer and uninstaller.

This program probes the local toolchain, builds the project through make(1),
copies the binary and the official icon into a prefix and injects a marked
PATH block into the user's shell startup files. When a build is required, it
probes the compiler, make(1) and the libcurl development files. Missing
packages are installed through the system package manager (apt, dnf, yum,
zypper, pacman, apk, xbps, emerge, eopkg, pkg or brew) using sudo, doas or
pkexec. Pass --no-deps to keep the machine untouched and print the manual
command instead.

The same program can uninstall Mesh. --uninstall removes the installed binary,
icons and PATH blocks while preserving global configuration. --purge also
removes the user's global configuration. Project repositories are never
modified.

Usage: ./mesh-install [--prefix DIR] [--bin-dir DIR] [--build]
                      [--no-build] [--no-shell] [--no-deps] [--yes]
                      [--quiet]
       ./mesh-install --uninstall [--purge] [--prefix DIR] [--bin-dir DIR]
                      [--no-shell] [--yes] [--quiet]
"""
from __future__ import annotations
import errno
import fcntl
import os
import shutil
import stat
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import NoReturn, TextIO

MESH_VERSION = "1.0"
MESH_ICON = "mesh_icon.svg"
MARK_BEGIN = b"# >>> mesh path >>>"
MARK_END = b"# <<< mesh path <<<"
RUN_VERBOSE = 0
RUN_QUIET = 1
RUN_SILENT = 2


class InstallerError(Exception):
 """A fatal installation error that was already reported."""


class UsageError(Exception):
 """Invalid command-line usage."""


@dataclass(slots=True)
class Options:
 prefix: str = ""
 bin_dir: str = ""
 have_prefix: bool = False
 have_bin_dir: bool = False
 force_build: bool = False
 no_build: bool = False
 no_shell: bool = False
 no_deps: bool = False
 assume_yes: bool = False
 quiet: bool = False
 uninstall: bool = False
 purge: bool = False


@dataclass(frozen=True, slots=True)
class PkgPlan:
 manager: str
 debian_env: bool
 args: tuple[str, ...]


PKG_PLANS = (
 PkgPlan("apt-get", True, ("install", "-y", "--no-install-recommends",
                           "build-essential", "libcurl4-openssl-dev")),
 PkgPlan("dnf", False, ("install", "-y", "gcc", "make", "libcurl-devel")),
 PkgPlan("yum", False, ("install", "-y", "gcc", "make", "libcurl-devel")),
 PkgPlan("zypper", False, ("--non-interactive", "install", "gcc", "make",
                           "libcurl-devel")),
 PkgPlan("pacman", False, ("-S", "--needed", "--noconfirm", "base-devel",
                           "curl")),
 PkgPlan("apk", False, ("add", "--no-cache", "build-base", "curl-dev")),
 PkgPlan("xbps-install", False, ("-y", "base-devel", "libcurl-devel")),
 PkgPlan("emerge", False, ("--quiet", "--noreplace", "sys-devel/gcc",
                           "sys-devel/make", "net-misc/curl")),
 PkgPlan("eopkg", False, ("install", "-y", "system.devel", "curl-devel")),
 PkgPlan("pkg", False, ("install", "-y", "gcc", "gmake", "curl")),
 PkgPlan("brew", False, ("install", "curl")),
)
TERMUX_PLAN = PkgPlan("pkg", False, ("install", "-y", "build-essential",
                                     "libcurl"))


def say(opt: Options | None, message: str) -> None:
 if opt is not None and opt.quiet:
  return
 sys.stdout.write(message)
 sys.stdout.flush()


def warn(message: str) -> None:
 sys.stdout.flush()
 sys.stderr.write(f"mesh-install: warning: {message}\n")
 sys.stderr.flush()


def fail(message: str) -> None:
 sys.stdout.flush()
 sys.stderr.write(f"mesh-install: error: {message}\n")
 sys.stderr.flush()


def stop(message: str) -> NoReturn:
 fail(message)
 raise InstallerError


def path_join(first: str, second: str) -> str:
 if not first:
  return second
 if not second:
  return first
 if first.endswith("/"):
  return first + second
 return first + "/" + second


def trim_trailing_slashes(path: str) -> str:
 while len(path) > 1 and path.endswith("/"):
  path = path[:-1]
 return path


def file_exists(path: str) -> bool:
 try:
  return stat.S_ISREG(os.stat(path).st_mode)
 except OSError:
  return False


def same_inode(first: str, second: str) -> bool:
 try:
  left = os.stat(first)
  right = os.stat(second)
 except OSError:
  return False
 return left.st_dev == right.st_dev and left.st_ino == right.st_ino


def mkdir_p(path: str, mode: int) -> None:
 if not path:
  raise OSError(errno.EINVAL, os.strerror(errno.EINVAL))
 work = path[:-1] if len(path) > 1 and path.endswith("/") else path
 for index, char in enumerate(work):
  if char != "/" or index == 0:
   continue
  part = work[:index]
  try:
   os.mkdir(part, mode)
  except FileExistsError:
   pass
 try:
  os.mkdir(work, mode)
 except FileExistsError:
  pass
 if not stat.S_ISDIR(os.stat(work).st_mode):
  raise OSError(errno.ENOTDIR, os.strerror(errno.ENOTDIR))


def write_all(fd: int, data: bytes) -> None:
 view = memoryview(data)
 while view:
  written = os.write(fd, view)
  view = view[written:]


def copy_file(source: str, destination: str, mode: int) -> None:
 temporary = ""
 fd = -1
 try:
  for attempt in range(32):
   temporary = f"{destination}.tmp.{os.getpid()}.{attempt}"
   try:
    fd = os.open(temporary,
                 os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC,
                 0o600)
    break
   except FileExistsError:
    continue
  else:
   raise OSError(errno.EEXIST, os.strerror(errno.EEXIST))
  with open(source, "rb") as handle:
   while True:
    chunk = handle.read(65536)
    if not chunk:
     break
    write_all(fd, chunk)
  os.fsync(fd)
  os.fchmod(fd, mode)
  os.close(fd)
  fd = -1
  os.replace(temporary, destination)
  temporary = ""
 finally:
  if fd >= 0:
   os.close(fd)
  if temporary:
   try:
    os.unlink(temporary)
   except FileNotFoundError:
    pass


def write_file_atomic(path: str, data: bytes, mode: int) -> None:
 temporary = ""
 fd = -1
 try:
  for attempt in range(32):
   temporary = f"{path}.tmp.{os.getpid()}.{attempt}"
   try:
    fd = os.open(temporary,
                 os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC,
                 0o600)
    break
   except FileExistsError:
    continue
  else:
   raise OSError(errno.EEXIST, os.strerror(errno.EEXIST))
  write_all(fd, data)
  os.fsync(fd)
  os.fchmod(fd, mode)
  os.close(fd)
  fd = -1
  os.replace(temporary, path)
  temporary = ""
 finally:
  if fd >= 0:
   os.close(fd)
  if temporary:
   try:
    os.unlink(temporary)
   except FileNotFoundError:
    pass


def append_block(path: str, block: bytes) -> None:
 fd = os.open(path,
              os.O_RDWR | os.O_CREAT | os.O_APPEND | os.O_CLOEXEC |
              os.O_NOFOLLOW, 0o644)
 try:
  fcntl.flock(fd, fcntl.LOCK_EX)
  size = os.fstat(fd).st_size
  if size > 0 and os.pread(fd, 1, size - 1) != b"\n":
   write_all(fd, b"\n")
  write_all(fd, block)
  os.fsync(fd)
  fcntl.flock(fd, fcntl.LOCK_UN)
 finally:
  os.close(fd)


def escaped_bin_dir(bin_dir: str) -> bytes:
 result = bytearray()
 for byte in os.fsencode(bin_dir):
  if byte < 0x20 or byte == 0x7F or byte == ord(":"):
   raise OSError(errno.EINVAL, os.strerror(errno.EINVAL))
  if byte in b'\\"$`':
   result.append(ord("\\"))
  result.append(byte)
 return bytes(result)


def shell_block(bin_dir: str, fish: bool) -> bytes:
 escaped = escaped_bin_dir(bin_dir)
 version = MESH_VERSION.encode()
 if fish:
  return (
   MARK_BEGIN + b"\n" +
   b"# Managed by Mesh " + version + b" - remove this block to uninstall.\n" +
   b"if not contains \"" + escaped + b"\" $PATH\n" +
   b"    set -gx PATH \"" + escaped + b"\" $PATH\n" +
   b"end\n" +
   MARK_END + b"\n"
  )
 return (
  MARK_BEGIN + b"\n" +
  b"# Managed by Mesh " + version + b" - remove this block to uninstall.\n" +
  b"case \":$PATH:\" in\n" +
  b"    *\":" + escaped + b":\"*) ;;\n" +
  b"    *) PATH=\"" + escaped + b":$PATH\" ;;\n" +
  b"esac\n" +
  b"export PATH\n" +
  MARK_END + b"\n"
 )


def shell_file_target(path: str) -> str | None:
 try:
  link_stat = os.lstat(path)
 except FileNotFoundError:
  return None
 except OSError:
  raise
 if stat.S_ISLNK(link_stat.st_mode):
  try:
   return os.path.realpath(path, strict=True)
  except OSError as exc:
   raise OSError(errno.ELOOP, os.strerror(errno.ELOOP)) from exc
 if not stat.S_ISREG(link_stat.st_mode):
  raise OSError(errno.EINVAL, os.strerror(errno.EINVAL))
 return path


def strip_shell_blocks(existing: bytes) -> tuple[bytes, int]:
 pieces: list[bytes] = []
 position = 0
 count = 0
 while True:
  begin = existing.find(MARK_BEGIN, position)
  if begin < 0:
   break
  end = existing.find(MARK_END, begin)
  if end < 0:
   raise OSError(errno.EINVAL, os.strerror(errno.EINVAL))
  pieces.append(existing[position:begin])
  position = end + len(MARK_END)
  count += 1
  if existing[position:position + 1] == b"\n":
   position += 1
 pieces.append(existing[position:])
 return b"".join(pieces), count


def remove_shell_blocks(path: str) -> int:
 work = shell_file_target(path)
 if work is None:
  return 0
 try:
  existing = Path(work).read_bytes()
 except FileNotFoundError:
  return 0
 rewritten, count = strip_shell_blocks(existing)
 if count == 0:
  return 0
 try:
  mode = stat.S_IMODE(os.stat(work).st_mode)
 except OSError:
  mode = 0o644
 write_file_atomic(work, rewritten, mode)
 return count


def inject_shell_file(path: str, bin_dir: str, fish: bool) -> int:
 work = shell_file_target(path)
 if work is None:
  work = path
 block = shell_block(bin_dir, fish)
 try:
  existing = Path(work).read_bytes()
 except FileNotFoundError:
  append_block(work, block)
  return 1
 begin = existing.find(MARK_BEGIN)
 if begin >= 0 and existing.find(block, begin) >= 0:
  return 0
 if begin < 0:
  append_block(work, block)
  return 1
 pieces: list[bytes] = []
 position = 0
 while True:
  begin = existing.find(MARK_BEGIN, position)
  if begin < 0:
   break
  end = existing.find(MARK_END, begin)
  if end < 0:
   raise OSError(errno.EINVAL, os.strerror(errno.EINVAL))
  pieces.append(existing[position:begin])
  position = end + len(MARK_END)
  if existing[position:position + 1] == b"\n":
   position += 1
 pieces.append(existing[position:])
 rewritten = b"".join(pieces) + block
 try:
  mode = stat.S_IMODE(os.stat(work).st_mode)
 except OSError:
  mode = 0o644
 write_file_atomic(work, rewritten, mode)
 return 1


def find_in_path(name: str) -> str | None:
 path = os.environ.get("PATH")
 if not path or not name:
  return None
 for entry in path.split(":"):
  candidate = path_join(entry or ".", name)
  if os.access(candidate, os.X_OK):
   return candidate
 return None


def run_wait(argv: list[str], cwd: str | None,
             mode: int) -> int:
 sys.stdout.flush()
 sys.stderr.flush()
 try:
  if mode == RUN_VERBOSE:
   result = subprocess.run(argv, cwd=cwd, check=False)
  elif mode == RUN_QUIET:
   result = subprocess.run(argv, cwd=cwd, check=False,
                           stdout=subprocess.DEVNULL)
  else:
   result = subprocess.run(argv, cwd=cwd, check=False,
                           stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
 except OSError:
  return 127
 if result.returncode < 0:
  return -1
 return result.returncode


def make_temp_dir() -> str:
 temporary = os.environ.get("TMPDIR")
 if not temporary or not temporary.startswith("/"):
  temporary = "/tmp"
 try:
  return tempfile.mkdtemp(prefix="mesh-install.", dir=temporary)
 except OSError as exc:
  stop(f"cannot create probe directory: {exc.strerror}")


def probe_compile(compiler: str, directory: str, source: str,
                  libs: str) -> bool:
 source_path = path_join(directory, "probe.c")
 binary_path = path_join(directory, "probe.bin")
 try:
  Path(source_path).write_text(source, encoding="utf-8")
 except OSError:
  return False
 argv = [compiler, "-std=c17", "-O2", source_path, "-o", binary_path]
 argv.extend(libs.split())
 try:
  return run_wait(argv, None, RUN_SILENT) == 0
 finally:
  for path in (source_path, binary_path):
   try:
    os.unlink(path)
   except FileNotFoundError:
    pass


def is_termux() -> bool:
 prefix = os.environ.get("PREFIX")
 return ("TERMUX_VERSION" in os.environ or
         (prefix is not None and "com.termux" in prefix))


def plan_user_owned(plan: PkgPlan) -> bool:
 return plan.manager == "brew" or is_termux()


def select_pkg_plan() -> tuple[PkgPlan, str] | None:
 if is_termux():
  manager = find_in_path("pkg")
  if manager is None:
   return None
  return TERMUX_PLAN, manager
 for plan in PKG_PLANS:
  manager = find_in_path(plan.manager)
  if manager is not None:
   return plan, manager
 return None


def format_pkg_command(plan: PkgPlan, elevator: str | None,
                       override: str | None) -> str:
 pieces: list[str] = []
 if elevator:
  pieces.extend((elevator, "--"))
 if plan.debian_env:
  pieces.extend(("env", "DEBIAN_FRONTEND=noninteractive",
                 "APT_LISTCHANGES_FRONTEND=none"))
 pieces.append(plan.manager)
 if override:
  pieces.append(override)
 else:
  pieces.extend(plan.args)
 return " ".join(pieces)


def build_pkg_argv(manager: str, plan: PkgPlan, elevator: str | None,
                   override: str | None) -> list[str]:
 argv: list[str] = []
 if elevator:
  argv.extend((elevator, "--"))
 if plan.debian_env:
  argv.extend(("env", "DEBIAN_FRONTEND=noninteractive",
               "APT_LISTCHANGES_FRONTEND=none"))
 argv.append(manager)
 if override:
  argv.append(override)
 else:
  argv.extend(plan.args)
 return argv


def find_elevator() -> str | None:
 for name in ("sudo", "doas"):
  path = find_in_path(name)
  if path is not None:
   return path
 if os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY"):
  return find_in_path("pkexec")
 return None


def print_manual_hint() -> None:
 selected = select_pkg_plan()
 if selected is None:
  fail("install a C compiler, make(1) and libcurl development "
       "headers manually")
  return
 plan, _manager = selected
 if geteuid() != 0 and not plan_user_owned(plan):
  elevator = find_elevator()
  as_root = elevator is None
 else:
  elevator = None
  as_root = False
 command = format_pkg_command(plan, elevator, None)
 label = "as root" if as_root else "manually"
 sys.stderr.write(f"mesh-install: hint: run {label}:\n  {command}\n")
 sys.stderr.flush()


def install_dependencies(opt: Options) -> bool:
 selected = select_pkg_plan()
 if selected is None:
  fail("no supported package manager found")
  fail("install a C compiler, make(1) and libcurl development "
       "headers manually")
  return False
 plan, manager = selected
 apt = plan.manager == "apt-get"
 elevator: str | None = None
 if geteuid() != 0 and not plan_user_owned(plan):
  elevator = find_elevator()
  if elevator is None:
   fail("missing build dependencies and no sudo, doas or "
        "pkexec is available")
   print_manual_hint()
   return False
 elif geteuid() == 0 and plan.manager == "brew":
  fail("Homebrew refuses to run as root; run the installer as "
       "a normal user")
  return False
 command = format_pkg_command(plan, elevator, None)
 if not opt.assume_yes and not opt.quiet and sys.stdin.isatty():
  say(opt, "Missing build dependencies. The installer will run:\n"
           f"  {command}\n")
  say(opt, "Install them now? [Y/n] ")
  answer = sys.stdin.readline()
  if not answer or answer[0] in ("n", "N"):
   say(opt, "\n")
   fail("dependency installation declined")
   sys.stderr.write(f"mesh-install: hint: run manually:\n  {command}\n")
   sys.stderr.flush()
   return False
 else:
  say(opt, f"Installing build dependencies:\n  {command}\n")
 argv = build_pkg_argv(manager, plan, elevator, None)
 mode = RUN_QUIET if opt.quiet else RUN_VERBOSE
 rc = run_wait(argv, None, mode)
 if rc > 0 and apt:
  say(opt, "Package lists may be stale; refreshing them...\n")
  say(opt, f"  {format_pkg_command(plan, elevator, 'update')}\n")
  update_argv = build_pkg_argv(manager, plan, elevator, "update")
  if run_wait(update_argv, None, mode) == 0:
   rc = run_wait(argv, None, mode)
 if rc != 0:
  fail("automatic dependency installation failed")
  sys.stderr.write(f"mesh-install: hint: run manually:\n  {command}\n")
  sys.stderr.flush()
  return False
 say(opt, "Dependencies installed.\n")
 return True


def find_compiler() -> str | None:
 environment_cc = os.environ.get("CC", "")
 if environment_cc:
  end = 0
  while (end < len(environment_cc) and
         environment_cc[end] not in (" ", "\t")):
   end += 1
  first = environment_cc[:end]
  if "/" in first:
   if os.access(first, os.X_OK):
    return first
  else:
   found = find_in_path(first)
   if found is not None:
    return found
 for candidate in ("cc", "gcc", "clang"):
  found = find_in_path(candidate)
  if found is not None:
   return found
 return None


def find_make() -> str | None:
 if sys.platform.startswith("freebsd"):
  found = find_in_path("gmake")
  if found is not None:
   return found
 found = find_in_path("make")
 if found is not None:
  return found
 return find_in_path("gmake")


def env_append_unique(name: str, value: str) -> None:
 old = os.environ.get(name)
 if old is not None and value in old:
  return
 os.environ[name] = f"{old} {value}" if old else value


def apply_brew_flags(opt: Options) -> None:
 for keg in ("/opt/homebrew/opt/curl", "/usr/local/opt/curl"):
  header = path_join(keg, "include/curl/curl.h")
  if not os.access(header, os.R_OK):
   continue
  include = path_join(keg, "include")
  library = path_join(keg, "lib")
  if not os.access(library, os.X_OK):
   continue
  env_append_unique("CPPFLAGS", f"-I{include}")
  env_append_unique("LDFLAGS", f"-L{library}")
  env_append_unique("LDFLAGS", f"-Wl,-rpath,{library}")
  env_append_unique("CPATH", include)
  env_append_unique("LIBRARY_PATH", library)
  say(opt, f"  brew curl: {keg}\n")
  return


def probe_curl(compiler: str, directory: str) -> bool:
 source = (
  "#include <curl/curl.h>\n"
  "#include <pthread.h>\n"
  "int main(void) {\n"
  "    CURL *c = curl_easy_init();\n"
  "    if (c) curl_easy_cleanup(c);\n"
  "    return c ? 0 : 1;\n"
  "}\n"
 )
 return probe_compile(compiler, directory, source, "-lcurl -lpthread")


def prepare_toolchain(opt: Options) -> tuple[str, str]:
 directory = make_temp_dir()
 try:
  say(opt, "Checking build dependencies...\n")
  attempt = 0
  while True:
   apply_brew_flags(opt)
   compiler = find_compiler()
   make = find_make()
   curl_ok = compiler is not None and probe_curl(compiler, directory)
   say(opt, "  compiler: "
            f"{compiler if compiler is not None else 'not found'}\n")
   say(opt, "  make:     "
            f"{make if make is not None else 'not found'}\n")
   say(opt, "  libcurl:  "
            f"{'headers and library found' if curl_ok else 'not found'}\n")
   if compiler is not None and make is not None and curl_ok:
    break
   if opt.no_deps:
    fail("build dependencies are missing and --no-deps was given")
    print_manual_hint()
    raise InstallerError
   if attempt > 0:
    fail("build dependencies are still missing after installation")
    print_manual_hint()
    raise InstallerError
   if not install_dependencies(opt):
    raise InstallerError
   say(opt, "Re-checking build dependencies...\n")
   attempt += 1
  return compiler, make
 finally:
  shutil.rmtree(directory, ignore_errors=True)


def geteuid() -> int:
 return os.geteuid() if hasattr(os, "geteuid") else 1


def usage(stream: TextIO) -> None:
 stream.write(
  f"Mesh {MESH_VERSION} installer\n\n"
  "Usage: mesh-install [options]\n"
  "       mesh-install --uninstall [--purge] [options]\n\n"
  "Options:\n"
  "  --prefix DIR    Use DIR as the installation prefix (default: ~/.local,\n"
  "                  or /usr/local as root).\n"
  "  --bin-dir DIR   Use DIR for the mesh binary instead of PREFIX/bin.\n"
  "  --build         Force a rebuild with make before installing.\n"
  "  --no-build      Never invoke make; require an existing ./mesh.\n"
  "  --no-shell      Do not modify .bashrc, .zshrc or config.fish.\n"
  "  --no-deps       Do not install missing build packages; print the\n"
  "                  manual command instead.\n"
  "  --uninstall     Remove the installed binary, icons and PATH blocks.\n"
  "  --purge         Remove global configuration too; implies --uninstall.\n"
  "  --yes           Do not ask before installing build packages.\n"
  "  --quiet         Print only warnings and errors.\n"
  "  -h, --help      Show this help.\n\n"
  "Project repositories (.mesh directories) are never removed.\n"
  "Exit status: 0 on success, 1 on installation or removal failure, 2 on\n"
  "usage errors.\n"
 )
 stream.flush()


def usage_error(message: str, show_usage: bool = False) -> NoReturn:
 fail(message)
 if show_usage:
  usage(sys.stderr)
 raise UsageError


def parse_args(argc: int, argv: list[str]) -> Options | None:
 opt = Options()
 index = 1
 while index < argc:
  argument = argv[index]
  if argument in ("-h", "--help"):
   usage(sys.stdout)
   return None
  if argument == "--prefix":
   if index + 1 >= argc:
    usage_error("--prefix requires a directory")
   index += 1
   opt.prefix = argv[index]
   opt.have_prefix = True
  elif argument == "--bin-dir":
   if index + 1 >= argc:
    usage_error("--bin-dir requires a directory")
   index += 1
   opt.bin_dir = argv[index]
   opt.have_bin_dir = True
  elif argument == "--build":
   opt.force_build = True
  elif argument == "--no-build":
   opt.no_build = True
  elif argument == "--no-shell":
   opt.no_shell = True
  elif argument == "--no-deps":
   opt.no_deps = True
  elif argument == "--uninstall":
   opt.uninstall = True
  elif argument == "--purge":
   opt.purge = True
  elif argument == "--yes":
   opt.assume_yes = True
  elif argument == "--quiet":
   opt.quiet = True
  else:
   usage_error(f"unknown argument '{argument}'", True)
  index += 1
 if opt.purge:
  opt.uninstall = True
 if opt.force_build and opt.no_build:
  usage_error("--build and --no-build are mutually exclusive")
 if opt.uninstall and (opt.force_build or opt.no_build or opt.no_deps):
  usage_error("--build, --no-build and --no-deps cannot be used with "
              "--uninstall")
 if opt.have_prefix and not opt.prefix.startswith("/"):
  usage_error("--prefix must be an absolute path")
 if opt.have_bin_dir and not opt.bin_dir.startswith("/"):
  usage_error("--bin-dir must be an absolute path")
 return opt


def source_root() -> str:
 try:
  return str(Path(__file__).resolve().parent)
 except OSError:
  try:
   return os.getcwd()
  except OSError as exc:
   stop(f"cannot determine the source directory: {exc.strerror}")


def global_config_dir() -> str:
 xdg = os.environ.get("XDG_CONFIG_HOME")
 if xdg and xdg.startswith("/"):
  return path_join(xdg, "mesh")
 home = os.environ.get("HOME")
 if home and home.startswith("/"):
  return path_join(home, ".config/mesh")
 return ""


def remove_file(path: str, opt: Options) -> bool:
 try:
  os.unlink(path)
 except FileNotFoundError:
  return True
 except OSError as exc:
  warn(f"cannot remove {path}: {exc.strerror}")
  return False
 say(opt, f"Removed {path}\n")
 return True


def remove_empty_dir(path: str, opt: Options) -> bool:
 try:
  os.rmdir(path)
 except FileNotFoundError:
  return True
 except OSError as exc:
  if exc.errno in (errno.ENOTEMPTY, errno.EEXIST):
   return True
  warn(f"cannot remove {path}: {exc.strerror}")
  return False
 say(opt, f"Removed {path}\n")
 return True


def remove_tree(path: str) -> bool:
 try:
  entry_stat = os.lstat(path)
 except FileNotFoundError:
  return True
 except OSError as exc:
  warn(f"cannot remove {path}: {exc.strerror}")
  return False
 try:
  if stat.S_ISDIR(entry_stat.st_mode):
   shutil.rmtree(path)
  else:
   os.unlink(path)
 except OSError as exc:
  warn(f"cannot remove {path}: {exc.strerror}")
  return False
 return True


def uninstall(opt: Options) -> int:
 say(opt, f"Mesh {MESH_VERSION} uninstaller\n")
 home = os.environ.get("HOME")
 if opt.have_prefix:
  prefix = opt.prefix
 elif geteuid() == 0:
  prefix = "/usr/local"
 elif home is not None and home.startswith("/"):
  prefix = path_join(home, ".local")
 else:
  stop("HOME is not set; pass --prefix explicitly")
 prefix = trim_trailing_slashes(prefix)
 if opt.have_bin_dir:
  bin_dir = opt.bin_dir
 else:
  bin_dir = path_join(prefix, "bin")
 bin_dir = trim_trailing_slashes(bin_dir)
 share_dir = path_join(prefix, "share/mesh")
 binary = path_join(bin_dir, "mesh")
 icon = path_join(bin_dir, MESH_ICON)
 shared_icon = path_join(share_dir, MESH_ICON)
 failed = 0
 say(opt, f"Removing Mesh from {prefix}...\n")
 for path in (binary, icon, shared_icon):
  if not remove_file(path, opt):
   failed += 1
 if not remove_empty_dir(share_dir, opt):
  failed += 1
 if opt.no_shell:
  say(opt, "Shell integration left unchanged (--no-shell).\n")
 elif home is None or not home.startswith("/"):
  warn("HOME is not set; PATH blocks were not removed")
 else:
  for shell_path in (
   path_join(home, ".bashrc"),
   path_join(home, ".zshrc"),
   path_join(home, ".config/fish/config.fish"),
  ):
   try:
    count = remove_shell_blocks(shell_path)
   except OSError as exc:
    warn(f"cannot update {shell_path}: {exc.strerror}")
    failed += 1
    continue
   if count > 0:
    say(opt, f"Removed {count} PATH block(s) from {shell_path}\n")
 config_dir = global_config_dir()
 config_present = bool(config_dir) and os.path.lexists(config_dir)
 if opt.purge:
  if not config_dir:
   warn("cannot determine the global configuration path")
   failed += 1
  elif not config_present:
   say(opt, f"Global configuration not present: {config_dir}\n")
  elif remove_tree(config_dir):
   say(opt, f"Global configuration removed: {config_dir}\n")
  else:
   failed += 1
 elif config_present:
  say(opt, f"Global configuration preserved: {config_dir}\n")
  say(opt, "Use --purge to remove it as well.\n")
 elif config_dir:
  say(opt, f"No global configuration found at {config_dir}.\n")
 say(opt, "Project repositories (.mesh directories) were not modified.\n")
 if failed:
  say(opt, f"\nMesh {MESH_VERSION} uninstall completed with "
           f"{failed} error(s).\n")
  return 1
 if opt.purge:
  say(opt, f"\nMesh {MESH_VERSION} fully removed.\n")
 else:
  say(opt, f"\nMesh {MESH_VERSION} uninstalled successfully.\n")
 return 0


def install(opt: Options) -> int:
 say(opt, f"Mesh {MESH_VERSION} installer\n")
 root = source_root()
 say(opt, f"Source directory: {root}\n")
 binary_source = path_join(root, "mesh")
 icon_source = path_join(root, MESH_ICON)
 if not file_exists(icon_source):
  stop(f"official icon not found: {icon_source}")
 need_build = opt.force_build or not file_exists(binary_source)
 if opt.no_build and not file_exists(binary_source):
  stop(f"--no-build was given but {binary_source} does not exist")
 make = ""
 if need_build:
  compiler, make = prepare_toolchain(opt)
  del compiler
  cpus = os.cpu_count() or 1
  cpus = max(1, min(64, cpus))
  jobs = f"-j{cpus}"
  say(opt, f"Building with {make} {jobs}...\n")
  mode = RUN_QUIET if opt.quiet else RUN_VERBOSE
  if run_wait([make, jobs], root, mode) != 0:
   stop("build failed")
 else:
  say(opt, f"Using the existing binary: {binary_source}\n")
 if not file_exists(binary_source):
  stop(f"build completed without producing {binary_source}")
 home = os.environ.get("HOME")
 if opt.have_prefix:
  prefix = opt.prefix
 elif geteuid() == 0:
  prefix = "/usr/local"
 elif home is not None and home.startswith("/"):
  prefix = path_join(home, ".local")
 else:
  stop("HOME is not set; pass --prefix explicitly")
 prefix = trim_trailing_slashes(prefix)
 if opt.have_bin_dir:
  bin_dir = opt.bin_dir
 else:
  bin_dir = path_join(prefix, "bin")
 bin_dir = trim_trailing_slashes(bin_dir)
 share_dir = path_join(prefix, "share/mesh")
 binary_destination = path_join(bin_dir, "mesh")
 icon_destination = path_join(bin_dir, MESH_ICON)
 icon_share = path_join(share_dir, MESH_ICON)
 try:
  mkdir_p(bin_dir, 0o755)
 except OSError as exc:
  stop(f"cannot create {bin_dir}: {exc.strerror}")
 say(opt, f"Installing binary: {binary_destination}\n")
 if same_inode(binary_source, binary_destination):
  try:
   os.chmod(binary_destination, 0o755)
  except OSError as exc:
   stop(f"cannot chmod {binary_destination}: {exc.strerror}")
 else:
  try:
   copy_file(binary_source, binary_destination, 0o755)
  except OSError as exc:
   stop(f"cannot install {binary_destination}: {exc.strerror}")
 if same_inode(icon_source, icon_destination):
  try:
   os.chmod(icon_destination, 0o644)
  except OSError as exc:
   warn(f"cannot chmod {icon_destination}: {exc.strerror}")
 else:
  try:
   copy_file(icon_source, icon_destination, 0o644)
  except OSError as exc:
   warn(f"cannot copy the icon next to the binary: {exc.strerror}")
 try:
  mkdir_p(share_dir, 0o755)
  if not same_inode(icon_source, icon_share):
   try:
    copy_file(icon_source, icon_share, 0o644)
   except OSError as exc:
    warn(f"cannot copy the icon to {share_dir}: {exc.strerror}")
 except OSError as exc:
  warn(f"cannot create {share_dir}: {exc.strerror}")
 if opt.no_shell:
  say(opt, "Shell integration skipped (--no-shell).\n")
 elif home is None or not home.startswith("/"):
  warn("HOME is not set; PATH was not modified")
 else:
  bashrc = path_join(home, ".bashrc")
  zshrc = path_join(home, ".zshrc")
  fishrc = path_join(home, ".config/fish/config.fish")
  have_bash = file_exists(bashrc)
  have_zsh = file_exists(zshrc)
  have_fish = file_exists(fishrc)
  touched = 0
  if have_bash:
   try:
    touched += inject_shell_file(bashrc, bin_dir, False)
   except OSError as exc:
    stop(f"cannot update {bashrc}: {exc.strerror}")
  if have_zsh:
   try:
    touched += inject_shell_file(zshrc, bin_dir, False)
   except OSError as exc:
    stop(f"cannot update {zshrc}: {exc.strerror}")
  if not have_bash and not have_zsh:
   try:
    touched += inject_shell_file(bashrc, bin_dir, False)
   except OSError as exc:
    stop(f"cannot create {bashrc}: {exc.strerror}")
  if have_fish:
   try:
    touched += inject_shell_file(fishrc, bin_dir, True)
   except OSError as exc:
    stop(f"cannot update {fishrc}: {exc.strerror}")
  say(opt, f"PATH block ensured in {touched} shell file(s).\n")
 say(opt, f"Verifying {binary_destination}...\n")
 mode = RUN_QUIET if opt.quiet else RUN_VERBOSE
 if run_wait([binary_destination, "--version"], None, mode) != 0:
  stop("the installed binary did not run correctly")
 say(opt, f"\nMesh {MESH_VERSION} installed successfully.\n")
 say(opt, f"  binary: {binary_destination}\n")
 say(opt, f"  icon:   {icon_destination}\n")
 say(opt, "  obtain: mesh obtain URL [DEST]  (clone or download)\n")
 if not opt.no_shell:
  say(opt, "Open a new terminal (or source your shell startup "
           "file) to pick up the PATH.\n")
 return 0


def main(argv: list[str] | None = None) -> int:
 arguments = sys.argv if argv is None else argv
 try:
  opt = parse_args(len(arguments), arguments)
 except UsageError:
  return 2
 if opt is None:
  return 0
 try:
  return uninstall(opt) if opt.uninstall else install(opt)
 except InstallerError:
  return 1


if __name__ == "__main__":
 sys.exit(main())
