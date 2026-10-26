#!/usr/bin/env python3
"""Installer idempotence, shell integration, uninstall and CLI contract.

The installer is a standalone Python program.  This test never touches the
real HOME; it drives the public ``mesh-install`` entry point through the same
subprocess contract used by the other Python integration tests.
"""
from __future__ import annotations
import os
import re
import stat
import sys
from meshtest import (
 check,
 fail,
 project_root,
 read_text,
 run_command,
 workspace,
 write_text,
)
def main() -> int:
 root = project_root()
 installer = os.environ.get("INSTALLER") or os.path.join(root, "mesh-install")
 check(os.access(installer, os.X_OK), f"mesh-install not built: {installer}")
 temp = workspace("mesh-installer-test")
 home = os.path.join(temp, "home")
 prefix = os.path.join(temp, "prefix")
 os.makedirs(home, exist_ok=True)
 write_text(os.path.join(home, "dot-bashrc"), "export KEEP=1\n")
 try:
  os.symlink("dot-bashrc", os.path.join(home, ".bashrc"))
 except OSError as exc:
  fail(f"cannot create .bashrc symlink: {exc}")
 arguments = ("--prefix", prefix, "--no-build", "--no-deps", "--yes")
 first_status, _, first_err = run_command(root, {"HOME": home}, installer, *arguments)
 check(first_status==0, f"installer failed: {first_err}")
 second_status, second_out, second_err = run_command(
  root, {"HOME": home}, installer, *arguments
 )
 check(second_status==0, f"second installer run failed: {second_err}")
 binary = os.path.join(prefix, "bin", "mesh")
 icon = os.path.join(prefix, "bin", "mesh_icon.svg")
 shared_icon = os.path.join(prefix, "share", "mesh", "mesh_icon.svg")
 check(os.access(binary, os.X_OK), "binary was not installed")
 check(stat.S_IMODE(os.stat(binary).st_mode)==0o755,
       "binary mode is not 0755")
 check(os.path.isfile(icon), "icon was not installed next to the binary")
 check(os.path.isfile(shared_icon), "icon was not installed under share/mesh")
 check(stat.S_IMODE(os.stat(icon).st_mode)==0o644,
       "icon mode is not 0644")
 check("PATH block ensured in 0 shell file(s)." in second_out,
       "idempotent run did not report an unchanged shell file")
 check(os.path.islink(os.path.join(home, ".bashrc")),
       "installer replaced the .bashrc symlink")
 target = read_text(os.path.join(home, "dot-bashrc"))
 check(target is not None, "installer lost the symlink target")
 assert target is not None
 check("KEEP=1" in target, "installer overwrote the symlink target")
 blocks = re.findall(r"^# >>> mesh path >>>$", target, re.MULTILINE)
 check(len(blocks)==1, "installer duplicated the shell block")
 check(f'PATH="{prefix}/bin:$PATH"' in target,
       "shell block does not reference the binary directory")

 # A stale block is replaced in place instead of being duplicated.
 write_text(
  os.path.join(home, "dot-bashrc"),
  "export KEEP=1\n"
  "# >>> mesh path >>>\n"
  "# stale block\n"
  "# <<< mesh path <<<\n"
  "export AFTER=1\n",
 )
 stale_status, _, stale_err = run_command(
  root, {"HOME": home}, installer, *arguments
 )
 check(stale_status==0, f"stale-block install failed: {stale_err}")
 target = read_text(os.path.join(home, "dot-bashrc"))
 check(target is not None, "installer lost the shell file")
 assert target is not None
 check(target.count("# >>> mesh path >>>")==1,
       "installer duplicated a stale block")
 check("# stale block" not in target, "stale block was not removed")
 check("export AFTER=1" in target, "content after the stale block was lost")

 # Fish gets its native syntax, not the POSIX case block.
 fish_dir = os.path.join(home, ".config", "fish")
 os.makedirs(fish_dir, exist_ok=True)
 fish = os.path.join(fish_dir, "config.fish")
 write_text(fish, "set -gx KEEP 1\n")
 fish_status, _, fish_err = run_command(
  root, {"HOME": home}, installer, *arguments
 )
 check(fish_status==0, f"fish install failed: {fish_err}")
 fish_text = read_text(fish)
 check(fish_text is not None, "fish startup file is missing")
 assert fish_text is not None
 check("if not contains" in fish_text, "fish PATH syntax is missing")
 check("set -gx KEEP 1" in fish_text, "fish content was overwritten")

 # Special characters in --bin-dir must be escaped for the shell.
 custom = os.path.join(temp, "custom$ dir")
 custom_status, _, custom_err = run_command(
  root, {"HOME": home}, installer, "--prefix", os.path.join(temp, "custom-prefix"),
  "--bin-dir", custom, "--no-build", "--no-deps", "--yes"
 )
 check(custom_status==0, f"custom bin-dir install failed: {custom_err}")
 target = read_text(os.path.join(home, "dot-bashrc"))
 assert target is not None
 escaped = custom.replace("$", "\\$")
 check(f'PATH="{escaped}:$PATH"' in target,
       "custom bin-dir was not escaped in the shell block")

 # --no-shell must not touch any startup file.
 before = (
  read_text(os.path.join(home, "dot-bashrc")),
  read_text(fish),
 )
 no_shell_status, no_shell_out, no_shell_err = run_command(
  root, {"HOME": home}, installer, "--prefix", os.path.join(temp, "quiet-prefix"),
  "--no-build", "--no-deps", "--yes", "--no-shell", "--quiet"
 )
 check(no_shell_status==0, f"--no-shell install failed: {no_shell_err}")
 check(no_shell_out=="", "--quiet printed normal output")
 after = (
  read_text(os.path.join(home, "dot-bashrc")),
  read_text(fish),
 )
 check(before==after, "--no-shell modified a startup file")

 # Common uninstall removes program files and PATH blocks but keeps settings.
 global_config = os.path.join(home, ".config", "mesh")
 os.makedirs(global_config, exist_ok=True)
 write_text(os.path.join(global_config, "config"), "user.name=Keep Me\n")
 project = os.path.join(temp, "project")
 sentinel = os.path.join(project, ".mesh", "sentinel")
 os.makedirs(os.path.dirname(sentinel), exist_ok=True)
 write_text(sentinel, "keep\n")
 uninstall_env = {"HOME": home, "XDG_CONFIG_HOME": os.path.join(home, ".config")}
 uninstall_status, _, uninstall_err = run_command(
  root, uninstall_env, installer, "--uninstall", "--prefix", prefix, "--yes"
 )
 check(uninstall_status==0, f"uninstall failed: {uninstall_err}")
 check(not os.path.exists(binary), "uninstall kept the binary")
 check(not os.path.exists(icon), "uninstall kept the icon next to the binary")
 check(not os.path.exists(shared_icon), "uninstall kept the shared icon")
 check(not os.path.exists(os.path.join(prefix, "share", "mesh")),
       "uninstall kept an empty share/mesh directory")
 check(os.path.isdir(os.path.join(prefix, "bin")),
       "uninstall removed the shared bin directory")
 target = read_text(os.path.join(home, "dot-bashrc"))
 check(target is not None, "uninstall lost the shell target")
 assert target is not None
 check("# >>> mesh path >>>" not in target, "uninstall kept the PATH block")
 check("KEEP=1" in target, "uninstall lost unrelated shell content")
 check("export AFTER=1" in target, "uninstall lost content after the block")
 fish_text = read_text(fish)
 check(fish_text is not None, "uninstall lost the fish startup file")
 assert fish_text is not None
 check("# >>> mesh path >>>" not in fish_text,
       "uninstall kept the fish PATH block")
 check("set -gx KEEP 1" in fish_text,
       "uninstall lost unrelated fish content")
 check(os.path.isfile(os.path.join(global_config, "config")),
       "common uninstall removed the global configuration")
 check(os.path.isfile(sentinel), "uninstall modified a project repository")
 repeat_status, _, repeat_err = run_command(
  root, uninstall_env, installer, "--uninstall", "--prefix", prefix, "--yes"
 )
 check(repeat_status==0, f"repeated uninstall failed: {repeat_err}")

 # Reinstall, then let --purge alone remove files and configuration.
 reinstall_status, _, reinstall_err = run_command(
  root, {"HOME": home}, installer, *arguments
 )
 check(reinstall_status==0, f"reinstall failed: {reinstall_err}")
 check(os.path.isfile(binary), "reinstall did not restore the binary")
 purge_status, _, purge_err = run_command(
  root, uninstall_env, installer, "--purge", "--prefix", prefix, "--yes"
 )
 check(purge_status==0, f"purge failed: {purge_err}")
 check(not os.path.exists(binary), "purge kept the binary")
 check(not os.path.exists(global_config),
       "purge kept the global configuration")
 target = read_text(os.path.join(home, "dot-bashrc"))
 assert target is not None
 check("# >>> mesh path >>>" not in target,
       "purge kept the bash PATH block")
 fish_text = read_text(fish)
 assert fish_text is not None
 check("# >>> mesh path >>>" not in fish_text,
       "purge kept the fish PATH block")
 check(os.path.isdir(os.path.join(home, ".config", "fish")),
       "purge removed unrelated user configuration")
 check(os.path.isfile(fish), "purge removed the fish startup file")
 check(os.path.isfile(sentinel), "purge modified a project repository")

 # A custom --bin-dir is removed only when the same flags are supplied.
 custom_prefix = os.path.join(temp, "custom-prefix")
 custom_uninstall_status, _, custom_uninstall_err = run_command(
  root, {"HOME": home}, installer, "--uninstall", "--prefix", custom_prefix,
  "--bin-dir", custom, "--yes"
 )
 check(custom_uninstall_status==0,
       f"custom uninstall failed: {custom_uninstall_err}")
 check(not os.path.exists(os.path.join(custom, "mesh")),
       "custom uninstall kept the binary")
 check(not os.path.exists(os.path.join(custom, "mesh_icon.svg")),
       "custom uninstall kept the icon")
 check(not os.path.exists(os.path.join(custom_prefix, "share", "mesh")),
       "custom uninstall kept the shared icon directory")

 # Usage errors must use status 2 and help must use status 0.
 usage_status, _, usage_err = run_command(
  root, {"HOME": home}, installer, "--build", "--no-build"
 )
 check(usage_status==2, f"conflicting build flags returned {usage_status}")
 check("mutually exclusive" in usage_err, usage_err)
 relative_status, _, relative_err = run_command(
  root, {"HOME": home}, installer, "--prefix", "relative"
 )
 check(relative_status==2, f"relative prefix returned {relative_status}")
 check("absolute path" in relative_err, relative_err)
 uninstall_build_status, _, uninstall_build_err = run_command(
  root, {"HOME": home}, installer, "--uninstall", "--build"
 )
 check(uninstall_build_status==2,
       f"uninstall accepted --build: {uninstall_build_status}")
 check("cannot be used" in uninstall_build_err, uninstall_build_err)
 help_status, help_out, help_err = run_command(
  root, {"HOME": home}, installer, "--help"
 )
 check(help_status==0, f"--help failed: {help_err}")
 check("Usage: mesh-install" in help_out, help_out)
 check("--uninstall" in help_out and "--purge" in help_out,
       "help does not document uninstall options")
 print("installer tests: ok")
 return 0
if __name__=="__main__":
 sys.exit(main())
