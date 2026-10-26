/*
* admin_doctor.c - read-only checks for repository layout and metadata.
* Doctor never repairs anything. It reports the first broken contract and
* leaves the repository untouched so an operator can inspect it safely.
*/
#include "core.h"
static int report_path(const char*label, const char*path, mode_t type,
bool optional, size_t*failures) {
struct stat st;
if(lstat(path,&st)<0) {
if(errno==ENOENT&&optional)
return 0;
printf("FAIL %-24s missing: %s\n", label, path);
(*failures)++;
return 0;
}
if(S_ISLNK(st.st_mode)) {
printf("FAIL %-24s symlink is not allowed: %s\n", label, path);
(*failures)++;
return 0;
}
if(type==S_IFDIR&&!S_ISDIR(st.st_mode)) {
printf("FAIL %-24s not a directory: %s\n", label, path);
(*failures)++;
return 0;
}
if(type==S_IFREG&&!S_ISREG(st.st_mode)) {
printf("FAIL %-24s not a regular file: %s\n", label, path);
(*failures)++;
return 0;
}
printf("ok   %-24s %s\n", label, path);
return 0;
}
static void check_layout(const struct mesh_repo*repo,size_t*failures) {
static const char*dirs[]={
"store","store/objects","store/trees","store/conflicts",
"store/remote","store/tmp","monitor","refs","refs/tags","logs",
};
static const char*files[]={"config","HEAD","history"};
char path[MESH_PATH_MAX];
for(size_t i=0;i<sizeof(dirs)/sizeof(dirs[0]);i++) {
if(mesh_path_join(path, sizeof(path), repo->mesh_dir, dirs[i])<0) {
printf("FAIL layout path too long: %s\n", dirs[i]);
(*failures)++;
continue;
}
report_path(dirs[i], path, S_IFDIR, false, failures);
}
for(size_t i=0;i<sizeof(files)/sizeof(files[0]);i++) {
if(mesh_path_join(path, sizeof(path), repo->mesh_dir, files[i])<0) {
printf("FAIL layout path too long: %s\n", files[i]);
(*failures)++;
continue;
}
report_path(files[i], path, S_IFREG, false, failures);
}
}
static void check_branch_ref(const struct mesh_repo*repo,size_t*failures) {
char branch[64];
char rel[96];
char path[MESH_PATH_MAX];
if(mesh_config_branch(repo,branch,sizeof(branch))<0) {
printf("FAIL %-24s cannot read branch setting\n", "branch");
(*failures)++;
return;
}
if(!mesh_ref_name_valid(branch)) {
printf("FAIL %-24s invalid branch name '%s'\n", "branch", branch);
(*failures)++;
return;
}
snprintf(rel, sizeof(rel),"refs/%s", branch);
if(mesh_path_join(path, sizeof(path), repo->mesh_dir, rel)<0) {
printf("FAIL %-24s path too long\n", "branch ref");
(*failures)++;
return;
}
report_path("branch ref", path, S_IFREG, false, failures);
}
static void check_head(const struct mesh_repo*repo,size_t*failures) {
struct mesh_graph graph;
char head[MESH_HASH_HEX+1];
if(graph_head(repo,head)<0) {
printf("FAIL %-24s cannot read HEAD\n", "HEAD");
(*failures)++;
return;
}
if(head[0]=='\0') {
printf("ok   %-24s empty repository\n", "HEAD");
return;
}
graph_init(&graph);
if(graph_load(repo,&graph)<0) {
printf("FAIL %-24s history is unreadable\n", "HEAD");
(*failures)++;
graph_free(&graph);
return;
}
if(!graph_find(&graph, head)) {
printf("FAIL %-24s target is not in history\n", "HEAD");
(*failures)++;
} else {
printf("ok   %-24s %s\n", "HEAD", head);
}
graph_free(&graph);
}
static void check_credentials(const struct mesh_repo*repo,size_t*failures) {
char path[MESH_PATH_MAX];
struct stat st;
if(mesh_path_join(path, sizeof(path), repo->mesh_dir,
"credentials")<0) {
printf("FAIL %-24s path too long\n", "credentials");
(*failures)++;
return;
}
if(lstat(path,&st)<0) {
if(errno==ENOENT)
printf("ok   %-24s not configured\n", "credentials");
else {
printf("FAIL %-24s cannot stat: %s\n", "credentials",
strerror(errno));
(*failures)++;
}
return;
}
if(!S_ISREG(st.st_mode)||(st.st_mode&0777)!=0600) {
printf("FAIL %-24s must be a regular 0600 file\n", "credentials");
(*failures)++;
return;
}
printf("ok   %-24s regular, mode 0600\n", "credentials");
}
int admin_doctor_run(const struct mesh_repo*repo) {
size_t failures=0;
if(!repo||!repo->valid) {
errno=EINVAL;
return 1;
}
printf("Mesh admin doctor\n");
check_layout(repo,&failures);
check_branch_ref(repo,&failures);
check_head(repo,&failures);
check_credentials(repo,&failures);
if(failures) {
printf("doctor: %zu failure(s)\n", failures);
return 1;
}
printf("doctor: repository is healthy\n");
return 0;
}
