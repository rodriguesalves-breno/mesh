#!/usr/bin/env python3
"""Core integration regressions for shell wiring, manifests, history and auth."""
from __future__ import annotations
import atexit
import os
import re
import shutil
import stat
import sys
from meshtest import (
 blake2b_hex,
 check,
 fail,
 read_text,
 run_mesh,
 run_mesh_ok,
 workspace,
 write_text,
)
def main() -> int:
 os.environ.pop("XDG_CONFIG_HOME", None)
 os.environ.pop("EMAIL", None)
 temp = workspace("mesh-test")
 home1 = os.path.join(temp, "home1")
 home2 = os.path.join(temp, "home2")
 repo = os.path.join(temp, "repo")
 longroot = f"/tmp/m{os.getpid()}"
 atexit.register(shutil.rmtree, longroot, True)
 for directory in (home1, home2, repo):
  os.makedirs(directory, exist_ok=True)
 help_text = run_mesh_ok("help", repo, {"HOME": home1}, "--help")
 short_help = run_mesh_ok("short help", repo, {"HOME": home1}, "-h")
 check(help_text==short_help, "-h and --help disagree")
 for name in (
  "start", "config", "branch", "tag", "reflog", "ignore", "clean", "save",
  "log", "diff", "restore", "check", "choose", "admin", "obtain", "fetch",
  "auth", "send", "sync",
 ):
  check(
   re.search(rf"^  {name}(?:\s|$)", help_text, re.MULTILINE) is not None,
   f"help omits the {name} command",
  )
 check("git clone" not in help_text, "help still describes obtain as git clone")
 check(
  "send <url>" in help_text and "sync <url>" in help_text,
  "help does not mark send/sync URLs as required",
 )
 run_mesh_ok("start home 1", home1, {"HOME": home1}, "start")
 first_bashrc = read_text(os.path.join(home1, ".bashrc"))
 check(first_bashrc is not None, "start did not create .bashrc")
 assert first_bashrc is not None
 run_mesh_ok("start home 1 again", home1, {"HOME": home1}, "start")
 second_bashrc = read_text(os.path.join(home1, ".bashrc"))
 check(second_bashrc is not None, "start lost .bashrc")
 assert second_bashrc is not None
 check(len(first_bashrc)==len(second_bashrc), "start grew .bashrc")
 home_blocks = re.findall(r"^# >>> mesh path >>>$", second_bashrc, re.MULTILINE)
 check(len(home_blocks)==1, "shell block duplicated")
 write_text(os.path.join(home2, "dot-bashrc"), "export KEEP_ME=1\n")
 try:
  os.symlink("dot-bashrc", os.path.join(home2, ".bashrc"))
 except OSError as exc:
  fail(f"cannot create .bashrc symlink: {exc}")
 run_mesh_ok("start home 2", home2, {"HOME": home2}, "start")
 run_mesh_ok("start home 2 again", home2, {"HOME": home2}, "start")
 check(os.path.islink(os.path.join(home2, ".bashrc")), "start replaced the .bashrc symlink")
 symlink_target = read_text(os.path.join(home2, "dot-bashrc"))
 check(symlink_target is not None, "start lost the symlink target")
 assert symlink_target is not None
 check("KEEP_ME" in symlink_target, "start overwrote the symlink target")
 target_blocks = re.findall(r"^# >>> mesh path >>>$", symlink_target, re.MULTILINE)
 check(len(target_blocks)==1, "symlink target duplicated the shell block")
 run_mesh_ok("start repository", repo, {"HOME": home1}, "start")
 run_mesh_ok(
  "config global name",
  home1,
  {"HOME": home1},
  "config",
  "--global",
  "user.name",
  "Ada Lovelace",
 )
 run_mesh_ok(
  "config global email",
  home1,
  {"HOME": home1},
  "config",
  "--global",
  "user.email",
  "ada@example.com",
 )
 global_name = run_mesh_ok(
  "config read global name", home1, {"HOME": home1}, "config", "--global", "user.name"
 )
 check(global_name=="Ada Lovelace\n", "global user.name was not stored")
 global_list = run_mesh_ok(
  "config list global", home1, {"HOME": home1}, "config", "--global", "--list"
 )
 check(
  re.search(r"^user\.name=Ada Lovelace$", global_list, re.MULTILINE) is not None
  and re.search(r"^user\.email=ada@example\.com$", global_list, re.MULTILINE) is not None,
  "global configuration listing is incomplete",
 )
 run_mesh_ok(
  "config local name", repo, {"HOME": home1}, "config", "user.name", "Local User"
 )
 run_mesh_ok(
  "config local email",
  repo,
  {"HOME": home1},
  "config",
  "user.email",
  "local@example.com",
 )
 local_list = run_mesh_ok("config list local", repo, {"HOME": home1}, "config", "--list")
 check(
  re.search(r"^user\.name=Local User$", local_list, re.MULTILINE) is not None
  and re.search(r"^user\.email=local@example\.com$", local_list, re.MULTILINE) is not None,
  "local configuration listing is incomplete",
 )
 bad_email_status, _, _ = run_mesh(
  repo, {"HOME": home1}, "config", "user.email", "not-an-email"
 )
 check(bad_email_status==2, "invalid e-mail was accepted")
 bad_key_status, _, _ = run_mesh(repo, {"HOME": home1}, "config", "user.unknown", "value")
 check(bad_key_status==2, "unknown configuration key was accepted")
 write_text(os.path.join(repo, "a.txt"), "hello\n")
 write_text(os.path.join(repo, "run.sh"), "#!/bin/sh\necho hi\n")
 os.chmod(os.path.join(repo, "run.sh"), 0o755)
 try:
  os.symlink("a.txt", os.path.join(repo, "link"))
 except OSError as exc:
  fail(f"cannot create link symlink: {exc}")
 run_mesh_ok("save first", repo, {"HOME": home1}, "save", "first")
 run_mesh_ok("save again", repo, {"HOME": home1}, "save", "again")
 history = read_text(os.path.join(repo, ".mesh", "history"))
 check(history is not None, "history was not written")
 assert history is not None
 history_lines = [line for line in history.split("\n") if line]
 check(len(history_lines)==2, "snapshots collided")
 timestamps = {line.split("|")[2] for line in history_lines}
 check(len(timestamps)==2, "history timestamps are not unique")
 last_fields = history_lines[-1].split("|")
 check(len(last_fields)==9, "new history line lacks author fields")
 check(last_fields[7]=="Local User", "history author name is wrong")
 check(last_fields[8]=="local@example.com", "history author e-mail is wrong")
 tree = last_fields[1]
 manifest = read_text(os.path.join(repo, ".mesh", "store", "trees", tree))
 check(manifest is not None, "manifest was not written")
 assert manifest is not None
 check(re.search(r" 755 f run\.sh$", manifest, re.MULTILINE) is not None,
   "executable mode missing")
 check(re.search(r" 777 l link$", manifest, re.MULTILINE) is not None,
   "symlink kind or mode missing")
 check_output = run_mesh_ok("check", repo, {"HOME": home1}, "check")
 check(re.search(r"^Message\s+: again$", check_output, re.MULTILINE) is not None,
   "check did not read the status message")
 check(re.search(r"^Conflicts\s+: 0$", check_output, re.MULTILINE) is not None,
   "check did not read the conflict count")
 check(
  re.search(
   r"^Author\s+: Local User <local@example\.com>$",
   check_output,
   re.MULTILINE,
  )
  is not None,
  "check did not read the author identity",
 )
 log_output = run_mesh_ok("log", repo, {"HOME": home1}, "log")
 check(re.search(r"^History \(2 snapshots\):", log_output, re.MULTILINE) is not None,
   "log did not report the history")
 check("first" in log_output and "again" in log_output, "log omitted snapshot messages")
 check("by Local User <local@example.com>" in log_output,
   "log omitted the author identity")
 first_match = re.search(
  r"^\s+([0-9a-f]{12})\s+.*\bfirst\b", log_output, re.MULTILINE
 )
 check(first_match is not None, "log did not expose the first snapshot hash")
 assert first_match is not None
 first_short = first_match.group(1)
 write_text(os.path.join(repo, "a.txt"), "changed\n")
 write_text(os.path.join(repo, "new.txt"), "new\n")
 os.unlink(os.path.join(repo, "run.sh"))
 diff_output = run_mesh_ok("diff working", repo, {"HOME": home1}, "diff")
 check(re.search(r"^M a\.txt$", diff_output, re.MULTILINE) is not None,
   "diff missed a modified file")
 check(re.search(r"^A new\.txt$", diff_output, re.MULTILINE) is not None,
   "diff missed an added file")
 check(re.search(r"^D run\.sh$", diff_output, re.MULTILINE) is not None,
   "diff missed a deleted file")
 run_mesh_ok("save third", repo, {"HOME": home1}, "save", "third")
 log_third = run_mesh_ok("log third", repo, {"HOME": home1}, "log")
 third_match = re.search(
  r"^\s+([0-9a-f]{12})\s+.*\bthird\b", log_third, re.MULTILINE
 )
 check(third_match is not None, "log did not expose the third snapshot hash")
 assert third_match is not None
 third_short = third_match.group(1)
 diff_two = run_mesh_ok(
  "diff two snapshots", repo, {"HOME": home1}, "diff", first_short, third_short
 )
 check(re.search(r"^M a\.txt$", diff_two, re.MULTILINE) is not None,
   "two-snapshot diff missed a modification")
 check(re.search(r"^A new\.txt$", diff_two, re.MULTILINE) is not None,
   "two-snapshot diff missed an addition")
 check(re.search(r"^D run\.sh$", diff_two, re.MULTILINE) is not None,
   "two-snapshot diff missed a deletion")
 write_text(os.path.join(repo, "a.txt"), "junk\n")
 refuse_status, _, _ = run_mesh(
  repo, {"HOME": home1}, "restore", first_short, "a.txt"
 )
 check(refuse_status!=0, "restore overwrote a file without --force")
 still_junk = read_text(os.path.join(repo, "a.txt"))
 check(still_junk=="junk\n", "failed restore changed the file anyway")
 run_mesh_ok(
  "restore forced", repo, {"HOME": home1}, "restore", first_short, "--force", "a.txt"
 )
 restored_a = read_text(os.path.join(repo, "a.txt"))
 check(restored_a=="hello\n", "restore did not recover the file contents")
 run_mesh_ok("restore tree", repo, {"HOME": home1}, "restore", first_short, "--force")
 check(os.access(os.path.join(repo, "run.sh"), os.X_OK),
   "restore did not recover executable mode")
 check(os.path.exists(os.path.join(repo, "new.txt")),
   "restore deleted a file that was not in the snapshot")
 os.unlink(os.path.join(repo, "new.txt"))
 component = "a"*254
 long_dirs = [component]*16
 long_rel = "/".join(long_dirs)+"/f"
 long_path = os.path.join(longroot, *long_dirs)
 shutil.rmtree(longroot, ignore_errors=True)
 os.makedirs(long_path, exist_ok=True)
 run_mesh_ok("long start", longroot, {"HOME": home1}, "start")
 write_text(os.path.join(longroot, long_rel), "long\n")
 run_mesh_ok("long save 1", longroot, {"HOME": home1}, "save", "long one")
 run_mesh_ok("long save 2", longroot, {"HOME": home1}, "save", "long two")
 long_check = run_mesh_ok("long check", longroot, {"HOME": home1}, "check")
 check(re.search(r"^Head\s+: [0-9a-f]{128}$", long_check, re.MULTILINE) is not None,
   "long path snapshot was not recorded in status")
 shutil.rmtree(longroot, ignore_errors=True)
 legacy = os.path.join(temp, "legacy")
 os.makedirs(legacy, exist_ok=True)
 run_mesh_ok("legacy start", legacy, {"HOME": home2}, "start")
 write_text(os.path.join(legacy, "file.txt"), "legacy\n")
 run_mesh_ok("legacy save", legacy, {"HOME": home2}, "save", "legacy snapshot")
 legacy_history = read_text(os.path.join(legacy, ".mesh", "history"))
 check(legacy_history is not None, "legacy history was not written")
 assert legacy_history is not None
 legacy_lines = [line for line in legacy_history.split("\n") if line]
 legacy_fields = legacy_lines[-1].split("|")
 check(len(legacy_fields)==7, "unconfigured save did not use the legacy format")
 legacy_log = run_mesh_ok("legacy log", legacy, {"HOME": home2}, "log")
 check(re.search(r"\blegacy snapshot\b", legacy_log) is not None,
   "legacy history without author fields was not loaded")
 target = os.path.join(repo, "target.txt")
 write_text(target, "DO NOT TOUCH\n")
 os.makedirs(os.path.join(repo, ".mesh", "store", "conflicts"), exist_ok=True)
 index = os.path.join(repo, ".mesh", "store", "conflicts", "index")
 write_text(index, "forged-key\t../../target.txt\n")
 forged_status, _, _ = run_mesh(repo, {"HOME": home1}, "choose", "alpha")
 check(forged_status!=0, "forged conflict index was accepted")
 untouched = read_text(target)
 check(untouched=="DO NOT TOUCH\n", "forged index escaped the root")
 os.makedirs(os.path.join(repo, "conflict-dir"), exist_ok=True)
 stem = blake2b_hex("conflict-dir/file.txt")[:16]+"-file.txt"
 write_text(os.path.join(repo, ".mesh", "store", "conflicts", stem+".alpha"),
    "alpha bytes\n")
 write_text(os.path.join(repo, ".mesh", "store", "conflicts", stem+".beta"),
    "beta bytes\n")
 write_text(index, "conflict-dir/file.txt\n")
 run_mesh_ok("choose beta", repo, {"HOME": home1}, "choose", "beta")
 chosen = read_text(os.path.join(repo, "conflict-dir", "file.txt"))
 check(chosen=="beta bytes\n", "choose wrote the wrong variant")
 try:
  index_size = os.path.getsize(index)
 except OSError:
  index_size = 0
 check(index_size==0, "conflict index was not cleared")
 run_mesh_ok("auth wildcard", repo, {"HOME": home1}, "auth", "token-abc")
 credentials_path = os.path.join(repo, ".mesh", "credentials")
 credentials = read_text(credentials_path)
 check(credentials is not None, "credentials were not written")
 assert credentials is not None
 credentials_mode = stat.S_IMODE(os.stat(credentials_path).st_mode)
 check(credentials_mode==0o600, "credentials mode is not 0600")
 check(credentials.endswith("\n"), "credentials lack a trailing newline")
 run_mesh_ok(
  "auth host", repo, {"HOME": home1}, "auth", "--host", "example.com", "host-token"
 )
 credentials = read_text(credentials_path)
 check(credentials is not None, "credentials disappeared after host auth")
 assert credentials is not None
 check(re.search(r"^\* token-abc$", credentials, re.MULTILINE) is not None,
   "wildcard credential was lost")
 check(re.search(r"^example\.com host-token$", credentials, re.MULTILINE) is not None,
   "scoped credential is missing")
 run_mesh_ok(
  "auth replacement",
  repo,
  {"HOME": home1},
  "auth",
  "--host",
  "example.com",
  "replacement",
 )
 credentials = read_text(credentials_path)
 check(credentials is not None, "credentials disappeared after replacement")
 assert credentials is not None
 replacements = re.findall(r"^example\.com replacement$", credentials, re.MULTILINE)
 check(len(replacements)==1, "scoped credential was not replaced")
 check(re.search(r"^\* token-abc$", credentials, re.MULTILINE) is not None,
   "replacing a host discarded the wildcard entry")
 print("integration tests: ok")
 return 0
if __name__=="__main__":
 sys.exit(main())
