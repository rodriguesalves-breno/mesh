#!/usr/bin/env python3
"""Integration regressions for branches, tags, reflog, ignore and clean."""
from __future__ import annotations
import os
import re
import sys
from meshtest import (
 check,
 read_text,
 run_mesh,
 run_mesh_ok,
 workspace,
 write_text,
)
def head(root: str) -> str:
 value = read_text(os.path.join(root, ".mesh", "HEAD"))
 check(value is not None, "HEAD is missing")
 assert value is not None
 return value.rstrip()
def manifest_for(root: str, snapshot: str) -> str:
 history = read_text(os.path.join(root, ".mesh", "history"))
 check(history is not None, "history is missing")
 assert history is not None
 line = next(
  (entry for entry in history.split("\n") if entry.startswith(snapshot+"|")),
  None,
 )
 check(line is not None, "history has no line for HEAD")
 assert line is not None
 fields = line.split("|")
 tree = fields[1]
 manifest = read_text(os.path.join(root, ".mesh", "store", "trees", tree))
 check(manifest is not None, f"manifest is missing for {tree}")
 assert manifest is not None
 return manifest
def main() -> int:
 os.environ.pop("XDG_CONFIG_HOME", None)
 os.environ.pop("EMAIL", None)
 temp = workspace("mesh-extras")
 home = os.path.join(temp, "home")
 repo = os.path.join(temp, "repo")
 os.makedirs(home, exist_ok=True)
 os.makedirs(repo, exist_ok=True)
 run_mesh_ok(
  "config name", home, {"HOME": home}, "config", "--global", "user.name", "Extension Tester"
 )
 run_mesh_ok(
  "config email",
  home,
  {"HOME": home},
  "config",
  "--global",
  "user.email",
  "extensions@example.com",
 )
 run_mesh_ok("start repository", repo, {"HOME": home}, "start")
 write_text(os.path.join(repo, "a.txt"), "one\n")
 os.makedirs(os.path.join(repo, "build"), exist_ok=True)
 write_text(os.path.join(repo, "build", "out.o"), "ignored object\n")
 write_text(os.path.join(repo, "build", "keep.txt"), "kept by negation\n")
 write_text(os.path.join(repo, "scratch.tmp"), "ignored scratch\n")
 write_text(os.path.join(repo, ".meshignore"), "build/\n!build/keep.txt\n*.tmp\n")
 run_mesh_ok("first save", repo, {"HOME": home}, "save", "first")
 first = head(repo)
 manifest = manifest_for(repo, first)
 check(re.search(r"^F .* a\.txt$", manifest, re.MULTILINE) is not None,
   "tracked file missing from manifest")
 check(re.search(r"^F .* \.meshignore$", manifest, re.MULTILINE) is not None,
   "ignore file missing from manifest")
 check(re.search(r"^F .* build/keep\.txt$", manifest, re.MULTILINE) is not None,
   "negated ignore rule did not re-include the file")
 check("build/out.o" not in manifest, "ignored directory entered the manifest")
 check("scratch.tmp" not in manifest, "ignored file entered the manifest")
 rules = run_mesh_ok("ignore list", repo, {"HOME": home}, "ignore")
 check(re.search(r"^Ignore rules \(3\):$", rules, re.MULTILINE) is not None,
   "ignore listing has no header")
 check(re.search(r"^  build/$", rules, re.MULTILINE) is not None,
   "directory rule was not listed")
 check(re.search(r"^  !build/keep\.txt$", rules, re.MULTILINE) is not None,
   "negated rule was not listed")
 check(re.search(r"^  \*\.tmp$", rules, re.MULTILINE) is not None,
   "glob rule was not listed")
 write_text(os.path.join(repo, "build", "out.o"), "changed but ignored\n")
 write_text(os.path.join(repo, "scratch.tmp"), "changed but ignored\n")
 ignored_diff = run_mesh_ok("ignored diff", repo, {"HOME": home}, "diff")
 check(ignored_diff=="no changes\n", "ignored files appeared in diff")
 branches = run_mesh_ok("branch list", repo, {"HOME": home}, "branch")
 check(re.search(r"^\* main [0-9a-f]{12}$", branches, re.MULTILINE) is not None,
   "main branch is not current")
 run_mesh_ok("create feature", repo, {"HOME": home}, "branch", "create", "feature")
 feature_ref = read_text(os.path.join(repo, ".mesh", "refs", "feature"))
 check(
  feature_ref is not None and re.fullmatch(r"[0-9a-f]{128}\n", feature_ref) is not None,
  "branch ref was not written",
 )
 branch_list = run_mesh_ok("branch list two", repo, {"HOME": home}, "branch")
 check(
  re.search(r"^\* main ", branch_list, re.MULTILINE) is not None
  and re.search(r"^  feature ", branch_list, re.MULTILINE) is not None,
  "branch listing lost the current marker",
 )
 bad_branch_status, _, _ = run_mesh(repo, {"HOME": home}, "branch", "create", "bad/name")
 check(bad_branch_status!=0, "invalid branch name was accepted")
 run_mesh_ok("switch feature", repo, {"HOME": home}, "branch", "switch", "feature")
 feature_check = run_mesh_ok("check feature", repo, {"HOME": home}, "check")
 check(re.search(r"^Branch\s+: feature$", feature_check, re.MULTILINE) is not None,
   "status did not follow the branch switch")
 write_text(os.path.join(repo, "a.txt"), "two\n")
 run_mesh_ok("second save", repo, {"HOME": home}, "save", "second")
 second = head(repo)
 check(second!=first, "second save reused the first snapshot")
 run_mesh_ok("switch main", repo, {"HOME": home}, "branch", "switch", "main")
 check(head(repo)==first, "switching to main did not restore its ref")
 main_check = run_mesh_ok("check main", repo, {"HOME": home}, "check")
 check(re.search(r"^Branch\s+: main$", main_check, re.MULTILINE) is not None,
   "status kept the old branch name")
 main_ref = read_text(os.path.join(repo, ".mesh", "refs", "main"))
 check(main_ref is not None and main_ref.startswith(first),
   "main ref moved when feature was saved")
 feature_ref_after = read_text(os.path.join(repo, ".mesh", "refs", "feature"))
 check(feature_ref_after is not None and feature_ref_after.startswith(second),
   "feature ref did not move to the new snapshot")
 run_mesh_ok("tag head", repo, {"HOME": home}, "tag", "v1")
 short = second[:12]
 run_mesh_ok("tag prefix", repo, {"HOME": home}, "tag", "feature-tip", short)
 tags = run_mesh_ok("tag list", repo, {"HOME": home}, "tag")
 check(re.search(r"^v1 [0-9a-f]{12}$", tags, re.MULTILINE) is not None,
   "tag listing lost v1")
 check(re.search(r"^feature-tip [0-9a-f]{12}$", tags, re.MULTILINE) is not None,
   "tag prefix was not resolved")
 duplicate_status, _, _ = run_mesh(repo, {"HOME": home}, "tag", "v1")
 check(duplicate_status!=0, "duplicate tag was accepted")
 run_mesh_ok("delete tag", repo, {"HOME": home}, "tag", "-d", "feature-tip")
 tags_after = run_mesh_ok("tag list after delete", repo, {"HOME": home}, "tag")
 check("feature-tip" not in tags_after, "deleted tag is still listed")
 reflog = run_mesh_ok("reflog", repo, {"HOME": home}, "reflog")
 check(re.search(r"^Reflog \([0-9]+ entries\):$", reflog, re.MULTILINE) is not None,
   "reflog header is missing")
 check(
  re.search(r"\bfirst\b", reflog) is not None
  and re.search(r"\bsecond\b", reflog) is not None,
  "reflog lost a save message",
 )
 check(
  "branch: switch to feature" in reflog and "branch: switch to main" in reflog,
  "reflog lost a branch movement",
 )
 delete_current_status, _, _ = run_mesh(repo, {"HOME": home}, "branch", "delete", "main")
 check(delete_current_status!=0, "current branch was deleted")
 run_mesh_ok("delete feature", repo, {"HOME": home}, "branch", "delete", "feature")
 branches_after = run_mesh_ok("branch list after delete", repo, {"HOME": home}, "branch")
 check(re.search(r"^\s+feature ", branches_after, re.MULTILINE) is None,
   "deleted branch is still listed")
 write_text(os.path.join(repo, "u.txt"), "untracked\n")
 write_text(os.path.join(repo, "ignored.tmp"), "ignored untracked\n")
 dry = run_mesh_ok("clean dry run", repo, {"HOME": home}, "clean", "-n")
 check(re.search(r"^Would remove u\.txt$", dry, re.MULTILINE) is not None,
   "dry run missed an untracked file")
 check("ignored.tmp" not in dry, "dry run listed an ignored file")
 check("build/out.o" not in dry, "dry run entered an ignored directory")
 check(os.path.exists(os.path.join(repo, "u.txt")), "dry run removed a file")
 dry_all = run_mesh_ok("clean dry run all", repo, {"HOME": home}, "clean", "-n", "-x")
 check(
  re.search(r"^Would remove u\.txt$", dry_all, re.MULTILINE) is not None
  and re.search(r"^Would remove ignored\.tmp$", dry_all, re.MULTILINE) is not None
  and re.search(r"^Would remove build/out\.o$", dry_all, re.MULTILINE) is not None,
  "include-ignored dry run missed a path",
 )
 run_mesh_ok("clean force", repo, {"HOME": home}, "clean", "-f")
 check(not os.path.exists(os.path.join(repo, "u.txt")), "forced clean kept an untracked file")
 check(os.path.exists(os.path.join(repo, "ignored.tmp")),
   "plain clean removed an ignored file")
 check(os.path.exists(os.path.join(repo, "build", "out.o")),
   "plain clean entered an ignored directory")
 check(os.path.exists(os.path.join(repo, "a.txt")), "clean removed a tracked file")
 run_mesh_ok("clean ignored force", repo, {"HOME": home}, "clean", "-f", "-x")
 check(not os.path.exists(os.path.join(repo, "ignored.tmp")),
   "include-ignored clean kept a file")
 check(not os.path.exists(os.path.join(repo, "build", "out.o")),
   "include-ignored clean kept an ignored directory")
 check(os.path.exists(os.path.join(repo, "build", "keep.txt")),
   "include-ignored clean removed a tracked file")
 usage_status, _, _ = run_mesh(repo, {"HOME": home}, "clean")
 check(usage_status==2, "clean without an action did not exit 2")
 print("extension tests: ok")
 return 0
if __name__=="__main__":
 sys.exit(main())
