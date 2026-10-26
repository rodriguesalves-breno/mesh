/*
* LibFuzzer harness for the untrusted manifest parser. Build with Clang:
*
*   make fuzz
*
* Each input is written as a tree manifest under a temporary repository and
* loaded through the same verified path used by sync. The harness never links
* a network client into the parser logic; it only gives the parser arbitrary
* bytes and lets ASan/UBSan catch memory and undefined-behaviour mistakes.
*/
#include "core.h"
#include <sys/stat.h>
#include <unistd.h>

/* Keep the harness bounded like the HTTP layer; the constant lives in
* network.c and is not part of the shared header. */
#define FUZZ_MAX_INPUT (64u*1024u*1024u)

static struct mesh_repo repo;
static int ready;

static void setup_repo(void) {
char tmpl[] = "/tmp/mesh-fuzz-XXXXXX";
char *root;
char mesh_dir[MESH_PATH_MAX];
char trees[MESH_PATH_MAX];
root = mkdtemp(tmpl);
if (!root) {
ready = -1;
return;
}
memset(&repo, 0, sizeof(repo));
snprintf(repo.root, sizeof(repo.root), "%s", root);
if (mesh_path_join(mesh_dir, sizeof(mesh_dir), repo.root, ".mesh") < 0 ||
mesh_mkdir_p(mesh_dir) < 0) {
ready = -1;
return;
}
snprintf(repo.mesh_dir, sizeof(repo.mesh_dir), "%s", mesh_dir);
if (mesh_path_join(trees, sizeof(trees), mesh_dir, "store/trees") < 0 ||
mesh_mkdir_p(trees) < 0) {
ready = -1;
return;
}
snprintf(repo.branch, sizeof(repo.branch), "main");
repo.valid = true;
ready = 1;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
char tree[MESH_HASH_HEX + 1];
char name[MESH_HASH_HEX + 32];
char path[MESH_PATH_MAX];
uint8_t digest[MESH_HASH_LEN];
static const char digits[] = "0123456789abcdef";
struct mesh_file_list list;
int fd;
size_t off = 0;

if (ready == 0)
setup_repo();
if (ready < 0 || size > FUZZ_MAX_INPUT)
return 0;
blake2b_hash_buffer(data, size, digest);
for (size_t i = 0; i < MESH_HASH_LEN; i++) {
tree[2 * i] = digits[digest[i] >> 4];
tree[2 * i + 1] = digits[digest[i] & 0x0f];
}
tree[MESH_HASH_HEX] = '\0';
snprintf(name, sizeof(name), "store/trees/%s", tree);
if (mesh_path_join(path, sizeof(path), repo.mesh_dir, name) < 0)
return 0;
fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
if (fd < 0)
return 0;
while (off < size) {
ssize_t n = write(fd, data + off, size - off);
if (n < 0 && errno == EINTR)
continue;
if (n <= 0)
break;
off += (size_t)n;
}
close(fd);
file_list_init(&list);
(void)manifest_load(&repo, tree, &list);
file_list_free(&list);
unlink(path);
return 0;
}
