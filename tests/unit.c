/*
* Unit tests for the security and manifest contracts. No test framework:
* a failure prints a line and exits non-zero, which is enough for CI and
* keeps the build dependency list at "a C compiler".
*/
#include "core.h"
#include <sys/stat.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, msg) do { \
if(!(cond)) { \
fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); \
failures++; \
} \
} while(0)

static void hash_text(const char *text, char hex[MESH_HASH_HEX+1]) {
blake2b_hash_string(text, hex);
}

static void make_repo(struct mesh_repo *repo, char *base, size_t cap) {
char tmpl[] = "/tmp/mesh-unit-XXXXXX";
char *root = mkdtemp(tmpl);
char dir[MESH_PATH_MAX];
if (!root) {
perror("mkdtemp");
exit(1);
}
snprintf(base, cap, "%s", root);
memset(repo, 0, sizeof(*repo));
snprintf(repo->root, sizeof(repo->root), "%s", root);
if (mesh_path_join(dir, sizeof(dir), repo->root, ".mesh") < 0 ||
mesh_mkdir_p(dir) < 0)
exit(1);
snprintf(repo->mesh_dir, sizeof(repo->mesh_dir), "%s", dir);
char trees[MESH_PATH_MAX];
if (mesh_path_join(trees, sizeof(trees), dir, "store/trees") < 0 ||
mesh_mkdir_p(trees) < 0)
exit(1);
snprintf(repo->branch, sizeof(repo->branch), "main");
repo->valid = true;
}

static void test_paths(void) {
char boundary[MESH_PATH_MAX+1];
char joined[5];
size_t at=0;
for(size_t part=0;part<16;part++) {
memset(boundary+at,'a',254);
at+=254;
boundary[at++]='/';
}
memset(boundary+at,'a',15);
at+=15;
boundary[at]='\0';
CHECK(at==MESH_PATH_MAX-1, "path boundary has 4095 bytes");
CHECK(mesh_relpath_safe(boundary), "4095-byte relative path accepted");
boundary[at++]='a';
boundary[at]='\0';
CHECK(!mesh_relpath_safe(boundary), "4096-byte relative path rejected");
errno=0;
CHECK(mesh_path_join(joined,sizeof(joined),"base","long-path")<0&&
errno==ENAMETOOLONG&&joined[0]=='\0',
"path join rejects overflow without returning a truncated path");
CHECK(!mesh_relpath_safe(""), "empty path rejected");
CHECK(!mesh_relpath_safe("/etc/passwd"), "absolute path rejected");
CHECK(!mesh_relpath_safe("../x"), "parent component rejected");
CHECK(!mesh_relpath_safe("a/../b"), "embedded parent rejected");
CHECK(!mesh_relpath_safe("a//b"), "empty component rejected");
CHECK(!mesh_relpath_safe(".mesh/credentials"), ".mesh namespace rejected");
CHECK(!mesh_relpath_safe(".git/hooks/pre-commit"), ".git namespace rejected");
CHECK(!mesh_relpath_safe("src/.git/config"), "nested .git rejected");
CHECK(!mesh_relpath_safe("src/.GIT/config"), "case-folded .GIT rejected");
CHECK(!mesh_relpath_safe(".Mesh/credentials"), "case-folded .Mesh rejected");
CHECK(!mesh_relpath_safe("dir/"), "trailing slash rejected");
CHECK(!mesh_relpath_safe("a\nb"), "newline in path rejected");
CHECK(!mesh_relpath_safe("a\tb"), "tab in path rejected");
CHECK(!mesh_relpath_safe("a\x01b"), "control byte in path rejected");
CHECK(!mesh_relpath_safe("a\x1b[31mb"), "escape sequence in path rejected");
CHECK(!mesh_relpath_safe("a\x7f" "b"), "DEL in path rejected");
CHECK(mesh_relpath_safe("src/main.c"), "ordinary path accepted");
CHECK(mesh_relpath_safe(".github/workflows/ci.yml"), ".github accepted");
CHECK(mesh_relpath_safe("a/.mesh.txt"), "lookalike accepted");
}

static void test_json(void) {
char out[128];
const char in[] = { 'a', '\x01', '"', '\\', (char)0xff, '\0' };
CHECK(mesh_json_escape(in, out, sizeof(out)) == 0, "json escape succeeds");
CHECK(strstr(out, "\\u0001") != NULL, "control byte escaped");
CHECK(strstr(out, "\\\"") != NULL, "quote escaped");
CHECK(strstr(out, "\\\\") != NULL, "backslash escaped");
CHECK(strstr(out, "\\u00ff") != NULL, "invalid UTF-8 escaped");
CHECK(strchr(out, (char)0xff) == NULL, "no raw invalid byte");
const char overlong3[] = { (char)0xe0, (char)0x80, (char)0x80, '\0' };
const char surrogate[] = { (char)0xed, (char)0xa0, (char)0x80, '\0' };
const char too_high[] = { (char)0xf4, (char)0x90, (char)0x80, (char)0x80, '\0' };
CHECK(mesh_json_escape(overlong3, out, sizeof(out)) == 0,
"overlong UTF-8 does not fail escaping");
CHECK(strchr(out, (char)0x80) == NULL, "overlong UTF-8 is escaped");
CHECK(mesh_json_escape(surrogate, out, sizeof(out)) == 0,
"surrogate UTF-8 does not fail escaping");
CHECK(strchr(out, (char)0xa0) == NULL, "surrogate UTF-8 is escaped");
CHECK(mesh_json_escape(too_high, out, sizeof(out)) == 0,
"out-of-range UTF-8 does not fail escaping");
CHECK(strchr(out, (char)0x90) == NULL, "out-of-range UTF-8 is escaped");
}

static void test_layout(void) {
char path[] = "/tmp/mesh-layout-XXXXXX";
int fd = mkstemp(path);
uint8_t data[100];
char full1[MESH_HASH_HEX+1], full2[MESH_HASH_HEX+1];
struct mesh_chunk chunks[2];
for (size_t i = 0; i < sizeof(data); i++)
data[i] = (uint8_t)(i * 17u + 3u);
CHECK(fd >= 0, "temp file created");
if (fd < 0)
return;
CHECK(write(fd, data, sizeof(data)) == (ssize_t)sizeof(data), "write sample");
memset(chunks, 0, sizeof(chunks));
chunks[0].offset = 0;
chunks[0].length = 40;
chunks[1].offset = 40;
chunks[1].length = 60;
CHECK(blake2b_hash_fd_layout(fd, sizeof(data), chunks, 2, full1) == 0,
"layout hash succeeds");
CHECK(blake2b_hash_fd(fd, 0, sizeof(data), full2) == 0,
"plain hash succeeds");
CHECK(strcmp(full1, full2) == 0, "layout full hash matches plain hash");
CHECK(mesh_hash_valid(chunks[0].hash), "chunk 0 hash filled");
CHECK(mesh_hash_valid(chunks[1].hash), "chunk 1 hash filled");
close(fd);
unlink(path);
}

static void test_manifest(void) {
struct mesh_repo repo;
struct mesh_file_list list;
char base[MESH_PATH_MAX];
char tree[MESH_HASH_HEX+1];
char hash1[MESH_HASH_HEX+1], hash2[MESH_HASH_HEX+1];
char*unicode_a=NULL,*unicode_b=NULL;
const char invalid_utf8[]={'x',(char)0xc0,(char)0xaf,'\0'};
CHECK(mesh_unicode_valid_utf8("caf\xC3\xA9"),"valid UTF-8 accepted");
CHECK(!mesh_unicode_valid_utf8(invalid_utf8),"overlong UTF-8 rejected");
CHECK(mesh_unicode_key("\xC3\xA9",&unicode_a)==0&&
mesh_unicode_key("e\xCC\x81",&unicode_b)==0&&
strcmp(unicode_a,unicode_b)==0,"NFC-equivalent names share a key");
free(unicode_a);free(unicode_b);unicode_a=unicode_b=NULL;
CHECK(mesh_unicode_key("Stra\xC3\x9F" "e",&unicode_a)==0&&
mesh_unicode_key("STRASSE",&unicode_b)==0&&
strcmp(unicode_a,unicode_b)==0,"full casefold expands sharp s");
free(unicode_a);free(unicode_b);
make_repo(&repo, base, sizeof(base));
file_list_init(&list);
hash_text("one", hash1);
hash_text("two", hash2);
struct mesh_file_entry *e = file_list_add(&list, "src/main.c", 3,
1, 2, 0644, MESH_KIND_FILE);
CHECK(e != NULL, "file entry added");
if (e)
memcpy(e->hash, hash1, sizeof(hash1));
e = file_list_add(&list, "link", 3, 1, 2, 0777, MESH_KIND_SYMLINK);
CHECK(e != NULL, "symlink entry added");
if (e)
memcpy(e->hash, hash2, sizeof(hash2));
char longpath[MESH_PATH_MAX];
size_t long_at = 0;
while (long_at + 3 < sizeof(longpath) - 1) {
longpath[long_at++] = 'd';
longpath[long_at++] = '/';
}
longpath[long_at++] = 'f';
longpath[long_at] = '\0';
CHECK(strlen(longpath) > 4000, "long path is actually long");
e = file_list_add(&list, longpath, 3, 1, 2, 0644, MESH_KIND_FILE);
CHECK(e != NULL, "long path entry added");
if (e)
memcpy(e->hash, hash1, sizeof(hash1));
CHECK(manifest_save(&repo, &list, tree) == 0, "manifest saved");
file_list_free(&list);

struct mesh_file_list loaded;
CHECK(manifest_load(&repo, tree, &loaded) == 0, "manifest loaded");
const struct mesh_file_entry *file = manifest_find(&loaded, "src/main.c");
const struct mesh_file_entry *link = manifest_find(&loaded, "link");
CHECK(file && file->kind == MESH_KIND_FILE && file->mode == 0644 &&
file->mtime_nsec == 2, "file metadata round-trips");
CHECK(link && link->kind == MESH_KIND_SYMLINK && link->mode == 0777,
"symlink metadata round-trips");
CHECK(manifest_find(&loaded, longpath) != NULL,
"long path round-trips through the manifest");
file_list_free(&loaded);

struct mesh_file_list dup;
file_list_init(&dup);
e = file_list_add(&dup, "Case.txt", 1, 0, 0, 0644, MESH_KIND_FILE);
if (e)
memcpy(e->hash, hash1, sizeof(hash1));
e = file_list_add(&dup, "D.txt", 1, 0, 0, 0644, MESH_KIND_FILE);
if (e)
memcpy(e->hash, hash2, sizeof(hash2));
e = file_list_add(&dup, "case.txt", 1, 0, 0, 0644, MESH_KIND_FILE);
if (e)
memcpy(e->hash, hash2, sizeof(hash2));
CHECK(manifest_save(&repo, &dup, tree) != 0,
"manifest save rejects case-folded duplicate paths");
file_list_free(&dup);

struct mesh_file_list unicode_dup;
file_list_init(&unicode_dup);
e=file_list_add(&unicode_dup,"caf\xC3\xA9.txt",1,0,0,0644,MESH_KIND_FILE);
if(e)memcpy(e->hash,hash1,sizeof(hash1));
e=file_list_add(&unicode_dup,"cafe\xCC\x81.txt",1,0,0,0644,MESH_KIND_FILE);
if(e)memcpy(e->hash,hash2,sizeof(hash2));
CHECK(manifest_save(&repo,&unicode_dup,tree)!=0,
"manifest save rejects Unicode normalization collision");
file_list_free(&unicode_dup);

struct mesh_file_list bad;
file_list_init(&bad);
e = file_list_add(&bad, ".git/pwned", 1, 0, 0, 0644, MESH_KIND_FILE);
CHECK(e != NULL, "bad entry allocated");
if (e)
memcpy(e->hash, hash1, sizeof(hash1));
CHECK(manifest_save(&repo, &bad, tree) != 0,
"manifest save rejects reserved namespace");
file_list_free(&bad);

char text[512];
char badtree[MESH_HASH_HEX+1];
uint8_t digest[MESH_HASH_LEN];
static const char digits[] = "0123456789abcdef";
snprintf(text, sizeof(text), "F %s 1 0 0 644 f .git/pwned\n", hash1);
blake2b_hash_buffer(text, strlen(text), digest);
for (size_t i = 0; i < MESH_HASH_LEN; i++) {
badtree[2*i] = digits[digest[i] >> 4];
badtree[2*i+1] = digits[digest[i] & 0x0f];
}
badtree[MESH_HASH_HEX] = '\0';
char treedir[MESH_PATH_MAX], treepath[MESH_PATH_MAX];
mesh_path_join(treedir, sizeof(treedir), repo.mesh_dir, "store/trees");
mesh_mkdir_p(treedir);
mesh_path_join(treepath, sizeof(treepath), treedir, badtree);
CHECK(mesh_write_atomic(treepath, text, strlen(text), 0644) == 0,
"crafted manifest written");
CHECK(manifest_load(&repo, badtree, &loaded) != 0,
"manifest load rejects .git path");
file_list_free(&loaded);

char duptext[512];
char duptree[MESH_HASH_HEX+1];
snprintf(duptext, sizeof(duptext),
"F %s 1 0 0 644 f Case.txt\n"
"F %s 1 0 0 644 f D.txt\n"
"F %s 1 0 0 644 f case.txt\n",
hash1, hash2, hash1);
blake2b_hash_string(duptext, duptree);
mesh_path_join(treepath, sizeof(treepath), treedir, duptree);
CHECK(mesh_write_atomic(treepath, duptext, strlen(duptext), 0644) == 0,
"case-folded manifest written");
CHECK(manifest_load(&repo, duptree, &loaded) != 0,
"manifest load rejects case-folded duplicate paths");
file_list_free(&loaded);

char cmd[MESH_PATH_MAX*2];
snprintf(cmd, sizeof(cmd), "rm -rf '%s'", base);
if (system(cmd) != 0)
fprintf(stderr, "warning: could not remove %s\n", base);
}


static void test_legacy_manifest(void) {
struct mesh_repo repo;
struct mesh_file_list list;
char base[MESH_PATH_MAX];
char hash[MESH_HASH_HEX+1];
char text[512];
char tree[MESH_HASH_HEX+1];
char treedir[MESH_PATH_MAX], treepath[MESH_PATH_MAX];
make_repo(&repo, base, sizeof(base));
hash_text("legacy", hash);
snprintf(text, sizeof(text), "F %s 6 legacy.txt\n", hash);
blake2b_hash_string(text, tree);
mesh_path_join(treedir, sizeof(treedir), repo.mesh_dir, "store/trees");
mesh_mkdir_p(treedir);
mesh_path_join(treepath, sizeof(treepath), treedir, tree);
CHECK(mesh_write_atomic(treepath, text, strlen(text), 0644) == 0,
"legacy manifest written");
CHECK(manifest_load(&repo, tree, &list) == 0, "legacy manifest accepted");
const struct mesh_file_entry *entry = manifest_find(&list, "legacy.txt");
CHECK(entry && entry->mode == 0644 && entry->kind == MESH_KIND_FILE,
"legacy defaults applied");
file_list_free(&list);
char cmd[MESH_PATH_MAX*2];
snprintf(cmd, sizeof(cmd), "rm -rf '%s'", base);
if (system(cmd) != 0)
fprintf(stderr, "warning: could not remove %s\n", base);
}

static void test_auth_scope(void) {
struct mesh_repo repo;
char base[MESH_PATH_MAX];
char path[MESH_PATH_MAX];
char out[MESH_AUTH_MAX+64];
static const char creds[] =
"example.com token-a\nother.example token-b\n";
make_repo(&repo, base, sizeof(base));
mesh_path_join(path, sizeof(path), repo.mesh_dir, "credentials");
CHECK(mesh_write_atomic(path, creds, strlen(creds), 0600) == 0,
"credentials written");
CHECK(net_auth_header(&repo, "https://example.com/repo", out,
sizeof(out)) == 0 &&
strstr(out, "token-a") != NULL, "scoped credential selected");
CHECK(net_auth_header(&repo, "https://other.example/repo", out,
sizeof(out)) == 0 &&
strstr(out, "token-b") != NULL, "second scoped credential selected");
CHECK(net_auth_header(&repo, "https://evil.example/repo", out,
sizeof(out)) == 1, "unscoped host gets no credential");
CHECK(mesh_write_atomic(path, "legacy-token\n", 13, 0600) == 0,
"legacy credential written");
CHECK(net_auth_header(&repo, "https://anything.example/", out,
sizeof(out)) == 0 &&
strstr(out, "legacy-token") != NULL, "legacy wildcard credential works");
char cmd[MESH_PATH_MAX*2];
snprintf(cmd, sizeof(cmd), "rm -rf '%s'", base);
if (system(cmd) != 0)
fprintf(stderr, "warning: could not remove %s\n", base);
}
static void test_identity(void) {
CHECK(mesh_author_name_valid("Ada Lovelace"), "ordinary author name accepted");
CHECK(mesh_author_name_valid("Zoë"), "UTF-8 author name accepted");
CHECK(!mesh_author_name_valid(""), "empty author name rejected");
CHECK(!mesh_author_name_valid("   "), "blank author name rejected");
CHECK(!mesh_author_name_valid("Ada|Lovelace"), "pipe in author name rejected");
CHECK(!mesh_author_name_valid("Ada\nLovelace"), "newline in author name rejected");
CHECK(mesh_email_valid("ada@example.com"), "ordinary e-mail accepted");
CHECK(mesh_email_valid("ada+mesh@example.co.uk"), "tagged e-mail accepted");
CHECK(mesh_email_valid("local@localhost"), "dotless domain accepted");
CHECK(!mesh_email_valid("ada"), "missing @ rejected");
CHECK(!mesh_email_valid("@example.com"), "empty local part rejected");
CHECK(!mesh_email_valid("ada@"), "empty domain rejected");
CHECK(!mesh_email_valid("ada@@example.com"), "double @ rejected");
CHECK(!mesh_email_valid("ada lovelace@example.com"), "space in e-mail rejected");
CHECK(!mesh_email_valid("ada|lovelace@example.com"), "pipe in e-mail rejected");
CHECK(!mesh_email_valid("ada@example.com\n"), "newline in e-mail rejected");
}
static void test_refs(void) {
CHECK(mesh_ref_name_valid("main"), "ordinary branch name accepted");
CHECK(mesh_ref_name_valid("feature-1"), "dash branch name accepted");
CHECK(!mesh_ref_name_valid(""), "empty ref name rejected");
CHECK(!mesh_ref_name_valid("."), "dot ref name rejected");
CHECK(!mesh_ref_name_valid(".."), "dot-dot ref name rejected");
CHECK(!mesh_ref_name_valid("a/b"), "slash ref name rejected");
CHECK(!mesh_ref_name_valid("-dash"), "leading dash rejected");
CHECK(!mesh_ref_name_valid("name.lock"), "lock suffix rejected");
CHECK(!mesh_ref_name_valid("a@{b"), "reflog syntax rejected");
CHECK(!mesh_ref_name_valid("a b"), "space ref name rejected");
CHECK(!mesh_ref_name_valid("a|b"), "pipe ref name rejected");
}
static void test_ignore(void) {
struct mesh_repo repo;
struct mesh_ignore*rules=NULL;
char base[MESH_PATH_MAX];
char path[MESH_PATH_MAX];
static const char text[]=
"*.tmp\n"
"build/\n"
"!keep.o\n"
"/top.txt\n"
"src/*.o\n"
"src/**/deep\n";
make_repo(&repo, base, sizeof(base));
CHECK(ignore_load(&repo,&rules)==0&&rules==NULL,
"missing .meshignore loads no rules");
mesh_path_join(path, sizeof(path), repo.root,".meshignore");
CHECK(mesh_write_atomic(path,text,strlen(text),0644)==0,
".meshignore written");
CHECK(ignore_load(&repo,&rules)==0&&rules!=NULL,
".meshignore loaded");
CHECK(!ignore_match(rules,"a.txt",false), "ordinary file not ignored");
CHECK(ignore_match(rules,"a.tmp",false), "basename glob ignored");
CHECK(ignore_match(rules,"dir/a.tmp",false), "nested basename glob ignored");
CHECK(ignore_match(rules,"build/x.o",false), "ignored directory suppresses child");
CHECK(!ignore_match(rules,"keep.o",false), "last negation wins");
CHECK(!ignore_match(rules,".meshignore",false), "rule file is never ignored");
CHECK(ignore_match(rules,"top.txt",false), "anchored rule matches root");
CHECK(!ignore_match(rules,"sub/top.txt",false), "anchored rule stays at root");
CHECK(ignore_match(rules,"src/a.o",false), "slash glob matches");
CHECK(!ignore_match(rules,"other/src/a.o",false), "slash glob stays relative");
CHECK(ignore_match(rules,"src/x/y/deep",false), "double star matches");
CHECK(!ignore_can_prune(rules), "negation disables directory pruning");
ignore_free(rules);
char cmd[MESH_PATH_MAX*2];
snprintf(cmd, sizeof(cmd), "rm -rf '%s'", base);
if (system(cmd) != 0)
fprintf(stderr, "warning: could not remove %s\n", base);
}
static void test_obtain_mode(void) {
CHECK(mesh_obtain_is_repo_url(
"https://github.com/octocat/Hello-World") == 1,
"GitHub repository detected");
CHECK(mesh_obtain_is_repo_url(
"https://github.com/octocat/Hello-World/archive/main.tar.gz") == 0,
"GitHub archive treated as a file");
CHECK(mesh_obtain_is_repo_url(
"https://gitlab.com/group/sub/project") == 1,
"nested GitLab repository detected");
CHECK(mesh_obtain_is_repo_url(
"https://gitlab.com/group/project/-/archive/HEAD/project-archive.tar.gz")
== 0, "GitLab archive treated as a file");
CHECK(mesh_obtain_is_repo_url(
"https://codeberg.org/user/repo") == 1,
"Gitea repository detected");
CHECK(mesh_obtain_is_repo_url(
"https://codeberg.org/user/repo/raw/branch/main/README.md") == 0,
"Gitea raw file treated as a file");
CHECK(mesh_obtain_is_repo_url(
"https://example.com/file.bin") == 0,
"single-component file detected");
CHECK(mesh_obtain_is_repo_url(
"https://example.com/files/data.bin") == 0,
"two-component file with extension detected");
CHECK(mesh_obtain_is_repo_url(
"https://example.com/owner/repo") == 1,
"generic two-component repository detected");
CHECK(mesh_obtain_is_repo_url(
"https://example.com/owner/repo.git") == 1,
"generic .git repository detected");
CHECK(mesh_obtain_is_repo_url(
"https://example.com/owner/repo.js") == 0,
"generic dotted repository name treated as a file");
CHECK(mesh_obtain_is_repo_url(
"https://example.com/one/two/three") == 0,
"generic deep path treated as a file");
CHECK(mesh_obtain_is_repo_url("ftp://example.com/owner/repo") == -1,
"non-HTTP source rejected by mode detection");
}
static void test_obtain_plan(void) {
char archive[MESH_PATH_MAX], dest[256], authority[MESH_PATH_MAX];
CHECK(mesh_obtain_plan("https://github.com/octocat/Hello-World.git",
NULL, NULL, archive, sizeof(archive), dest, sizeof(dest),
authority, sizeof(authority)) == 0, "GitHub URL planned");
CHECK(strcmp(archive,
"https://codeload.github.com/octocat/Hello-World/tar.gz/HEAD") == 0,
"GitHub archive URL mapped");
CHECK(strcmp(dest, "Hello-World") == 0, "GitHub destination derived");
CHECK(strcmp(authority, "codeload.github.com") == 0,
"GitHub archive authority derived");
CHECK(mesh_obtain_plan("https://gitlab.com/group/sub/project.git",
NULL, NULL, archive, sizeof(archive), dest, sizeof(dest),
authority, sizeof(authority)) == 0, "GitLab URL planned");
CHECK(strcmp(archive,
"https://gitlab.com/group/sub/project/-/archive/HEAD/"
"project-archive.tar.gz") == 0, "GitLab archive URL mapped");
CHECK(strcmp(dest, "project") == 0, "GitLab destination derived");
CHECK(mesh_obtain_plan("https://codeberg.org/user/repo",
"feature/x", NULL, archive, sizeof(archive), dest, sizeof(dest),
authority, sizeof(authority)) == 0, "Gitea URL with ref planned");
CHECK(strcmp(archive,
"https://codeberg.org/user/repo/archive/feature/x.tar.gz") == 0,
"Gitea archive URL keeps ref path");
CHECK(mesh_obtain_plan("https://github.com/o/r", NULL,
"https://example.com/source.tar.gz", archive, sizeof(archive),
dest, sizeof(dest), authority, sizeof(authority)) == 0,
"explicit archive accepted");
CHECK(strcmp(archive, "https://example.com/source.tar.gz") == 0,
"explicit archive used verbatim");
CHECK(strcmp(authority, "example.com") == 0,
"explicit archive authority derived");
CHECK(mesh_obtain_plan("https://github.com/onlyowner", NULL, NULL,
archive, sizeof(archive), dest, sizeof(dest), authority,
sizeof(authority)) < 0, "owner without repository rejected");
CHECK(mesh_obtain_plan("ftp://github.com/o/r", NULL, NULL,
archive, sizeof(archive), dest, sizeof(dest), authority,
sizeof(authority)) < 0, "non-HTTP URL rejected");
CHECK(mesh_obtain_plan("https://user@github.com/o/r", NULL, NULL,
archive, sizeof(archive), dest, sizeof(dest), authority,
sizeof(authority)) < 0, "URL credentials rejected");
CHECK(mesh_obtain_plan("https://github.com/o/r?tab=readme", NULL, NULL,
archive, sizeof(archive), dest, sizeof(dest), authority,
sizeof(authority)) < 0, "URL query rejected");
CHECK(mesh_obtain_plan("https://github.com/o/r", "../escape", NULL,
archive, sizeof(archive), dest, sizeof(dest), authority,
sizeof(authority)) < 0, "unsafe ref rejected");
}
static void test_stream_hash(void) {
static const char text[]="incremental hashing crosses the block boundary";
char one[MESH_HASH_HEX+1], two[MESH_HASH_HEX+1];
struct mesh_hash_ctx ctx;
CHECK(blake2b_hash_string(text, one) == 0, "hash string");
mesh_hash_init(&ctx);
mesh_hash_update(&ctx, text, 7);
mesh_hash_update(&ctx, text + 7, 11);
mesh_hash_update(&ctx, text + 18, strlen(text) - 18);
mesh_hash_final(&ctx, two);
CHECK(strcmp(one, two) == 0, "incremental hash matches one-shot hash");
}
int main(void) {
test_paths();
test_identity();
test_refs();
test_ignore();
test_json();
test_layout();
test_manifest();
test_legacy_manifest();
test_auth_scope();
test_obtain_mode();
test_obtain_plan();
test_stream_hash();
if (failures) {
fprintf(stderr, "%d unit test(s) failed\n", failures);
return 1;
}
printf("unit tests: ok\n");
return 0;
}
