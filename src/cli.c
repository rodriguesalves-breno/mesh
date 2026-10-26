/*
* cli.c - string parser, command router and the operations that tie the
* subsystems together.
* Argument handling is deliberately boring: strncmp, snprintf and a
* table of command names. No getopt state machines, no allocation for
* flags that fit in a register.
*/
#include "core.h"
#include <sys/stat.h>
#include <ctype.h>
#include <strings.h>
static const char help_text[]=
"Mesh - The Simple, Ultra-Fast Version Control System (1.0)\n"
"\n"
"Usage:\n"
"  mesh [command] [options]\n"
"\n"
"Core Commands:\n"
"  start          Initialize or repair a repository, shell PATH and VS Code\n"
"                 integration.\n"
"  config [--global|--local] [--list] [key [value]]\n"
"                 Read or write user.name and user.email identity settings.\n"
"  branch [list|create|switch|delete] ...\n"
"                 Manage named branches and the current branch pointer.\n"
"  tag [name [snapshot]] | tag -d name\n"
"                 List, create or delete lightweight snapshot tags.\n"
"  reflog         Show recent HEAD and branch movements.\n"
"  ignore         List the active .meshignore rules.\n"
"  clean (-n|-f) [-x]\n"
"                 List or remove working-tree files not tracked by HEAD.\n"
"  save \"msg\"     Record the working tree as a new snapshot.\n"
"  log            Show snapshot history, newest first.\n"
"  diff [a [b]]   Compare snapshots, or a snapshot/HEAD against the working\n"
"                 tree.\n"
"  restore <snap> [--force] [--] [path...]\n"
"                 Restore files from a snapshot without deleting extra files.\n"
"  check          Show repository state, refreshing status.json when needed.\n"
"  choose [alpha|beta]\n"
"                 List conflicts or resolve them by choosing a variant.\n"
"  git-info       Report Git metadata beside Mesh (read-only).\n"
"  admin <cmd>    Inspect, verify and maintain repository administration state.\n"
"                 Run 'mesh admin help' for the available subcommands.\n"
"\n"
"Network Commands:\n"
"  obtain [options] URL [DEST]\n"
"                 Clone a forge repository or download a file\n"
"                 (wget/curl-style). See 'mesh obtain --help' for options.\n"
"  fetch [options] URL [DEST]\n"
"                 Deprecated alias for 'mesh obtain --file'.\n"
"  auth [--host HOST] [token]\n"
"                 Store or report repository-scoped credentials (mode 0600).\n"
"  send <url>     Send the current snapshot and bounded history as JSON.\n"
"  sync <url>     Synchronize manifests and objects with a remote API.\n"
"\n"
"Global Flags:\n"
"  -v, --version  Show current Mesh software version.\n"
"  -h, --help     Display this help menu with the list of available commands.\n";
static int repo_required(struct mesh_repo*repo) {
if(mesh_repo_open(repo)<0) {
fprintf(stderr,
"mesh: not a Mesh repository (run 'mesh start' first)\n");
return -1;
}
return 0;
}
static bool looks_like_hash(const char*s) {
return mesh_hash_valid(s);
}
/* Plain HTTP to a non-loopback peer lets an active attacker replace
* manifests and objects. Integrity is not authentication: warn loudly
* instead of silently pretending the transport is safe. */
static bool url_plain_http_non_loopback(const char*url) {
const char*host;
const char*end;
const char*at;
size_t len;
if(!url||strncmp(url,"http://",7)!=0)
return false;
host=url+7;
end=host;
while(*end&&*end!='/'&&*end!='?'&&*end!='#')
end++;
at=memchr(host,'@',(size_t)(end-host));
if(at)
host=at+1;
len=(size_t)(end-host);
if(len==0)
return false;
if(len>=9&&strncasecmp(host,"localhost",9)==0&&
(len==9||host[9]==':'))
return false;
if(len>=9&&strncmp(host,"127.0.0.1",9)==0&&
(len==9||host[9]==':'))
return false;
if(len>=5&&memcmp(host,"[::1]",5)==0&&
(len==5||host[5]==':'))
return false;
return true;
}
static void warn_plain_http(const char*command, const char*url) {
if(url_plain_http_non_loopback(url))
fprintf(stderr,
"mesh: %s: warning: unencrypted HTTP to a non-loopback host; "
"a network attacker can forge manifests and objects. Use "
"https:// or a trusted tunnel.\n", command);
}
/* Working tree scan and hashing */
static int entry_cmp(const void*a, const void*b) {
const struct mesh_file_entry*ea=a;
const struct mesh_file_entry*eb=b;
return strcmp(ea->path, eb->path);
}
static int scan_dir(const struct mesh_repo*repo, const char*rel,
struct mesh_file_list*list, const struct mesh_ignore*ignore,
unsigned depth) {
char full[MESH_PATH_MAX];
DIR*dir;
struct dirent*de;
int rc=0;
if(depth>64) {
fprintf(stderr,"mesh: save: directory nesting too deep at %s\n",
rel[0]?rel:".");
errno=ELOOP;
return -1;
}
if(mesh_path_join(full, sizeof(full), repo->root, rel)<0)
return -1;
dir=opendir(full);
if(!dir)
return -1;
while((errno=0, de=readdir(dir))!=NULL) {
char child[MESH_PATH_MAX];
char path[MESH_PATH_MAX];
struct stat st;
if(strcmp(de->d_name,".")==0||strcmp(de->d_name,"..")==0)
continue;
if(mesh_component_reserved(de->d_name,strlen(de->d_name)))
continue;
if(mesh_path_join(child, sizeof(child), rel, de->d_name)<0||
mesh_path_join(path, sizeof(path), repo->root, child)<0) {
rc=-1;
break;
}
if(!mesh_relpath_safe(child)) {
fprintf(stderr,
"mesh: scan: refusing a path with control characters or "
"an invalid component\n");
errno=EINVAL;
rc=-1;
break;
}
if(lstat(path,&st)<0) {
rc=-1;
break;
}
bool is_dir=S_ISDIR(st.st_mode);
if(ignore_match(ignore, child, is_dir)&&
(!is_dir||ignore_can_prune(ignore)))
continue;
if(is_dir) {
if(scan_dir(repo, child, list, ignore, depth+1)<0) {
rc=-1;
break;
}
} else if(S_ISREG(st.st_mode)||S_ISLNK(st.st_mode)) {
int64_t sec,nsec;
uint32_t kind=S_ISLNK(st.st_mode)?MESH_KIND_SYMLINK:
MESH_KIND_FILE;
uint64_t size=S_ISLNK(st.st_mode)?(uint64_t)st.st_size:
(uint64_t)st.st_size;
mesh_stat_mtime(&st,&sec,&nsec);
struct mesh_file_entry*entry=file_list_add(list, child, size,
sec, nsec, S_ISLNK(st.st_mode)?0777u:
(uint32_t)(st.st_mode&0777), kind);
if(!entry) {
rc=-1;
break;
}
entry->dev=(uint64_t)st.st_dev;
entry->ino=(uint64_t)st.st_ino;
}
}
if(rc==0&&errno!=0)
rc=-1;
if(closedir(dir)<0)
return -1;
return rc;
}
static bool entry_matches_stat(const struct mesh_file_entry*entry,
const struct stat*st) {
int64_t sec,nsec;
if(!entry||!st)
return false;
mesh_stat_mtime(st,&sec,&nsec);
if((uint64_t)st->st_size!=entry->size||sec!=entry->mtime_sec||
nsec!=entry->mtime_nsec||(uint32_t)(st->st_mode&0777)!=entry->mode||
(entry->kind==MESH_KIND_SYMLINK)!=(S_ISLNK(st->st_mode)!=0))
return false;
if(entry->dev!=0&&(uint64_t)st->st_dev!=entry->dev)
return false;
if(entry->ino!=0&&(uint64_t)st->st_ino!=entry->ino)
return false;
return true;
}
static int hash_symlink(const struct mesh_repo*repo,
struct mesh_file_entry*entry) {
char full[MESH_PATH_MAX];
char target[MESH_PATH_MAX];
struct stat before,after;
ssize_t len;
if(mesh_path_join(full,sizeof(full),repo->root,entry->path)<0)
return -1;
if(lstat(full,&before)<0||!S_ISLNK(before.st_mode)||
!entry_matches_stat(entry,&before)) {
errno=EAGAIN;
return -1;
}
if(entry->size>=sizeof(target)) {
errno=ENAMETOOLONG;
return -1;
}
len=readlink(full,target,sizeof(target));
if(len<0)
return -1;
if((uint64_t)len!=entry->size||lstat(full,&after)<0||
!entry_matches_stat(entry,&after)) {
errno=EAGAIN;
return -1;
}
uint8_t digest[MESH_HASH_LEN];
static const char digits[]="0123456789abcdef";
blake2b_hash_buffer(target,(size_t)len,digest);
for(size_t i=0;i<MESH_HASH_LEN;i++) {
entry->hash[2*i]=digits[digest[i]>>4];
entry->hash[2*i+1]=digits[digest[i]&0x0f];
}
entry->hash[MESH_HASH_HEX]='\0';
return 0;
}
static int hash_work_tree(const struct mesh_repo*repo,
struct mesh_file_list*list) {
struct mesh_hash_job*jobs=NULL;
size_t njobs=0;
if(list->n==0)
return 0;
jobs=calloc(list->n,sizeof(*jobs));
if(!jobs)
return -1;
for(size_t i=0;i<list->n;i++) {
char full[MESH_PATH_MAX];
if(list->v[i].kind==MESH_KIND_SYMLINK) {
if(hash_symlink(repo,&list->v[i])<0)
goto fail;
continue;
}
if(list->v[i].kind!=MESH_KIND_FILE) {
errno=EINVAL;
goto fail;
}
if(mesh_path_join(full,sizeof(full),repo->root,
list->v[i].path)<0)
goto fail;
jobs[njobs].path=strdup(full);
if(!jobs[njobs].path)
goto fail;
jobs[njobs].offset=0;
jobs[njobs].length=list->v[i].size;
njobs++;
}
if(blake2b_hash_jobs(jobs,njobs,mesh_threads())<0)
goto fail;
size_t j=0;
for(size_t i=0;i<list->n;i++) {
if(list->v[i].kind==MESH_KIND_FILE) {
memcpy(list->v[i].hash,jobs[j].hex,
sizeof(jobs[j].hex));
j++;
}
}
for(size_t i=0;i<njobs;i++)
free(jobs[i].path);
free(jobs);
return 0;
fail: {
int saved=errno;
for(size_t i=0;i<njobs;i++)
free(jobs[i].path);
free(jobs);
errno=saved;
return -1;
}
}
static int work_tree_stable(const struct mesh_repo*repo,
const struct mesh_file_list*list) {
for(size_t i=0;i<list->n;i++) {
char full[MESH_PATH_MAX];
struct stat st;
if(mesh_path_join(full, sizeof(full), repo->root,
list->v[i].path)<0||lstat(full,&st)<0||
!entry_matches_stat(&list->v[i],&st)) {
errno=EAGAIN;
return -1;
}
}
return 0;
}
static int chunk_work_tree(const struct mesh_repo*repo,
struct mesh_file_list*list) {
for(size_t i=0;i<list->n;i++) {
struct mesh_chunk_list chunks;
char full[MESH_PATH_MAX];
char full_hash[MESH_HASH_HEX+1];
struct stat st;
int fd;
if(list->v[i].kind!=MESH_KIND_FILE||
list->v[i].size<MESH_LARGE_FILE)
continue;
if(mesh_path_join(full,sizeof(full),repo->root,
list->v[i].path)<0)
return -1;
fd=open(full,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
if(fd<0)
return -1;
if(fstat(fd,&st)<0||!entry_matches_stat(&list->v[i],&st)) {
close(fd);
errno=EAGAIN;
return -1;
}
if(chunk_scan_fd(fd,list->v[i].size,&chunks,full_hash)<0) {
close(fd);
return -1;
}
if(fstat(fd,&st)<0||!entry_matches_stat(&list->v[i],&st)||
strcmp(full_hash,list->v[i].hash)!=0) {
chunk_list_free(&chunks);
close(fd);
errno=EAGAIN;
return -1;
}
if(close(fd)<0) {
chunk_list_free(&chunks);
return -1;
}
free(list->v[i].chunks);
list->v[i].chunks=chunks.v;
list->v[i].nchunks=chunks.n;
}
return 0;
}
static int store_work_tree(const struct mesh_repo*repo,
struct mesh_file_list*list) {
for(size_t i=0;i<list->n;i++) {
char full[MESH_PATH_MAX];
struct stat before,after;
int fd=-1;
if(mesh_path_join(full,sizeof(full),repo->root,
list->v[i].path)<0)
return -1;
if(list->v[i].kind==MESH_KIND_SYMLINK) {
char target[MESH_PATH_MAX];
ssize_t len;
if(lstat(full,&before)<0||
!entry_matches_stat(&list->v[i],&before)) {
errno=EAGAIN;
return -1;
}
len=readlink(full,target,sizeof(target));
if(len<0||(uint64_t)len!=list->v[i].size||
lstat(full,&after)<0||
!entry_matches_stat(&list->v[i],&after)) {
errno=EAGAIN;
return -1;
}
if(object_store_blob(repo,list->v[i].hash,target,
(size_t)len)<0)
return -1;
continue;
}
fd=open(full,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
if(fd<0)
return -1;
if(fstat(fd,&before)<0||!entry_matches_stat(&list->v[i],&before)) {
close(fd);
errno=EAGAIN;
return -1;
}
int rc;
if(list->v[i].nchunks==0) {
rc=object_store_fd(repo,list->v[i].hash,fd,0,
list->v[i].size);
} else {
rc=0;
for(size_t c=0;c<list->v[i].nchunks;c++) {
struct mesh_chunk*chunk=&list->v[i].chunks[c];
if(object_store_fd(repo,chunk->hash,fd,chunk->offset,
chunk->length)<0) {
rc=-1;
break;
}
}
}
if(fstat(fd,&after)<0||!entry_matches_stat(&list->v[i],&after))
rc=-1;
if(close(fd)<0)
rc=-1;
if(rc<0)
return -1;
}
return 0;
}
/* Commands */
static int config_usage(void) {
fprintf(stderr,"usage: mesh config [--global|--local] [--list] "
"[key [value]]\n");
return 2;
}
static int cmd_config(int argc, char**argv) {
struct mesh_repo repo;
bool global=false, local=false, list=false;
bool have_repo=false;
const char*key=NULL;
const char*value=NULL;
char out[MESH_EMAIL_MAX+1];
int rc;
for(int i=2;i<argc;i++) {
if(strcmp(argv[i],"--global")==0)
global=true;
else if(strcmp(argv[i],"--local")==0)
local=true;
else if(strcmp(argv[i],"--list")==0)
list=true;
else if(argv[i][0]=='-'&&argv[i][1]!='\0')
return config_usage();
else if(!key)
key=argv[i];
else if(!value)
value=argv[i];
else
return config_usage();
}
if((global&&local)||(list&&(key||value)))
return config_usage();
if(!key&&!list)
return config_usage();
if(!global&&!local) {
if(mesh_repo_open(&repo)==0) {
local=true;
have_repo=true;
} else {
fprintf(stderr,"mesh: config: not a Mesh repository "
"(use --global or run 'mesh start')\n");
return 1;
}
} else if(local) {
if(repo_required(&repo)<0)
return 1;
have_repo=true;
}
if(key&&!mesh_config_key_valid(key)) {
fprintf(stderr,"mesh: config: unsupported key '%s'\n", key);
return 2;
}
if(list) {
if(mesh_config_list(have_repo?&repo:NULL,global,stdout)<0) {
fprintf(stderr,"mesh: config: cannot read configuration: %s\n",
strerror(errno));
return 1;
}
return 0;
}
if(value) {
if(mesh_config_set(have_repo?&repo:NULL,global,key,value)<0) {
int saved=errno;
if(saved==EINVAL)
fprintf(stderr,"mesh: config: invalid value for %s\n",
key);
else
fprintf(stderr,"mesh: config: cannot write configuration: "
"%s\n", strerror(saved));
return saved==EINVAL?2:1;
}
return 0;
}
rc=mesh_config_get(have_repo?&repo:NULL,global,key,out,sizeof(out));
if(rc<0) {
fprintf(stderr,"mesh: config: cannot read configuration: %s\n",
strerror(errno));
return 1;
}
if(rc==0)
return 1;
printf("%s\n", out);
return 0;
}
static int cmd_start(int argc, char**argv) {
struct mesh_repo repo;
struct mesh_status status;
char bin_dir[MESH_PATH_MAX];
unsigned touched=0;
int failed=0;
int rc;
(void)argc;
(void)argv;
rc=mesh_repo_init(&repo);
if(rc<0) {
perror("mesh: start");
return 1;
}
if(rc==1)
printf("Repository already initialized: %s\n", repo.root);
else
printf("Initialized Mesh repository: %s\n", repo.root);
if(mesh_self_dir(bin_dir, sizeof(bin_dir))<0||
bin_dir[0]=='\0')
snprintf(bin_dir, sizeof(bin_dir),"%s", repo.root);
if(mesh_env_install(&repo, bin_dir,&touched)<0) {
fprintf(stderr,
"mesh: start: shell integration failed: %s\n",
strerror(errno));
failed=1;
} else
printf("Shell integration: %u startup file(s) updated (PATH: %s)\n",
touched, bin_dir);
rc=mesh_write_vscode(&repo);
if(rc<0) {
fprintf(stderr,"mesh: start: IDE manifest failed: %s\n",
strerror(errno));
failed=1;
} else if(rc==1)
printf("IDE manifest: existing .vscode/settings.json left "
"untouched\n");
else
printf("IDE manifest: %s/.vscode/settings.json\n", repo.root);
if(monitor_collect(&repo,&status,"start")==0) {
snprintf(status.message, sizeof(status.message),
"repository initialized");
if(monitor_write(&repo,&status)<0) {
fprintf(stderr,"mesh: start: status write failed: %s\n",
strerror(errno));
failed=1;
}
} else
failed=1;
printf("Next: mesh save \"initial snapshot\"\n");
return failed?1:0;
}
static int cmd_save(int argc, char**argv) {
struct mesh_repo repo;
struct mesh_file_list work, previous;
struct mesh_graph graph;
struct mesh_status status;
struct mesh_changed changed;
struct mesh_ignore*ignore=NULL;
char head[MESH_HASH_HEX+1];
char tree[MESH_HASH_HEX+1];
char parent_tree[MESH_HASH_HEX+1]="";
char commit[MESH_HASH_HEX+1];
char patch[MESH_PATH_MAX];
const char*message;
const struct mesh_node*parent=NULL;
uint64_t tracked=0;
size_t snapshots=0;
int lock_fd=-1;
int rc=1;
if(argc<3) {
fprintf(stderr,"usage: mesh save \"message\"\n");
return 2;
}
message=argv[2];
if(repo_required(&repo)<0)
return 1;
if(!repo.author_name[0]||!repo.author_email[0])
fprintf(stderr,
"mesh: save: warning: user.name/user.email not configured; "
"this snapshot has no author\n"
"  mesh config --global user.name \"Your Name\"\n"
"  mesh config --global user.email \"you@example.com\"\n");
lock_fd=mesh_repo_lock(&repo);
if(lock_fd<0) {
fprintf(stderr,"mesh: save: cannot lock repository: %s\n",
strerror(errno));
return 1;
}
memset(&status, 0, sizeof(status));
file_list_init(&work);
file_list_init(&previous);
graph_init(&graph);
if(ignore_load(&repo,&ignore)<0) {
fprintf(stderr,"mesh: save: cannot read .meshignore: %s\n",
strerror(errno));
goto done;
}
if(scan_dir(&repo,"",&work,ignore, 0)<0) {
perror("mesh: save: scan");
goto done;
}
if(work.n>1)
qsort(work.v, work.n, sizeof(work.v[0]), entry_cmp);
for(size_t i=0;i<work.n;i++)
tracked+=work.v[i].size;
if(hash_work_tree(&repo,&work)<0) {
fprintf(stderr,"mesh: save: cannot hash working tree: %s\n",
strerror(errno));
goto done;
}
if(work_tree_stable(&repo,&work)<0) {
fprintf(stderr,"mesh: save: working tree changed during hashing\n");
goto done;
}
if(chunk_work_tree(&repo,&work)<0) {
fprintf(stderr,"mesh: save: chunking failed: %s\n",
strerror(errno));
goto done;
}
if(store_work_tree(&repo,&work)<0) {
fprintf(stderr,"mesh: save: cannot store objects: %s\n",
strerror(errno));
goto done;
}
if(work_tree_stable(&repo,&work)<0) {
fprintf(stderr,"mesh: save: working tree changed during storage\n");
goto done;
}
if(manifest_save(&repo,&work, tree)<0) {
fprintf(stderr,"mesh: save: cannot write tree: %s\n",
strerror(errno));
goto done;
}
if(graph_head(&repo, head)<0) {
fprintf(stderr,"mesh: save: cannot read HEAD: %s\n",
strerror(errno));
goto done;
}
if(graph_load(&repo,&graph)<0) {
fprintf(stderr,"mesh: save: cannot load history: %s\n",
strerror(errno));
goto done;
}
if(head[0]) {
parent=graph_find(&graph, head);
if(!parent) {
fprintf(stderr,"mesh: save: HEAD points to a missing "
"snapshot\n");
goto done;
}
snprintf(parent_tree, sizeof(parent_tree),"%s", parent->tree);
if(manifest_load(&repo, parent_tree,&previous)<0) {
fprintf(stderr,"mesh: save: cannot load previous "
"tree: %s\n", strerror(errno));
goto done;
}
}
if(manifest_diff(&previous,&work,&changed)<0) {
fprintf(stderr,"mesh: save: cannot calculate changes: %s\n",
strerror(errno));
goto done;
}
if(graph_commit(&repo,&graph, tree, head,"", repo.branch, message,
commit)<0) {
fprintf(stderr,"mesh: save: commit failed: %s\n",
strerror(errno));
goto done;
}
if(graph_set_head(&repo, repo.branch, commit)<0) {
fprintf(stderr,"mesh: save: cannot update HEAD: %s\n",
strerror(errno));
goto done;
}
if(reflog_append(&repo, head, commit, message)<0)
fprintf(stderr,"mesh: save: warning: reflog append failed: %s\n",
strerror(errno));
if(snprintf(patch, sizeof(patch),"%s/monitor/last.patch",
repo.mesh_dir)<(int)sizeof(patch)) {
if(bridge_write_patch(&repo, parent_tree, tree, patch)<0)
fprintf(stderr,
"mesh: save: warning: patch export failed: %s\n",
strerror(errno));
}
if(monitor_collect(&repo,&status,"save")==0) {
status.changed=changed;
mesh_strlcpy(status.message, sizeof(status.message), message);
if(monitor_write(&repo,&status)<0)
fprintf(stderr,
"mesh: save: warning: status write failed: %s\n",
strerror(errno));
} else {
fprintf(stderr,
"mesh: save: warning: cannot refresh status: %s\n",
strerror(errno));
}
snapshots=graph.count;
char short_hash[13];
char pretty[32];
mesh_short_hash(short_hash, sizeof(short_hash), commit);
mesh_format_bytes(tracked, pretty, sizeof(pretty));
printf("saved %s  %s\n", short_hash, message);
printf("  files: %zu (%s)   +%zu ~%zu -%zu   snapshots: %zu\n",
work.n, pretty, changed.added, changed.modified,
changed.deleted, snapshots);
rc=0;
done:
file_list_free(&work);
file_list_free(&previous);
ignore_free(ignore);
graph_free(&graph);
if(lock_fd>=0)
close(lock_fd);
return rc;
}
static int cmd_check(int argc, char**argv) {
struct mesh_repo repo;
(void)argc;
(void)argv;
if(repo_required(&repo)<0)
return 1;
if(monitor_print(&repo)<0) {
fprintf(stderr,"mesh: check: cannot read state: %s\n",
strerror(errno));
return 1;
}
return 0;
}
static int cmd_choose(int argc, char**argv) {
struct mesh_repo repo;
size_t resolved=0;
if(repo_required(&repo)<0)
return 1;
if(argc<3)
return conflict_list(&repo)<0?1:0;
if(strcmp(argv[2],"alpha")!=0&&strcmp(argv[2],"beta")!=0) {
fprintf(stderr,"usage: mesh choose alpha|beta\n");
return 2;
}
if(conflict_choose(&repo, argv[2],&resolved)<0) {
fprintf(stderr,"mesh: choose: %s\n", strerror(errno));
if(resolved>0)
printf("resolved %zu conflict(s) with %s\n", resolved,
argv[2]);
struct mesh_status status;
if(monitor_collect(&repo,&status,"choose")==0) {
snprintf(status.message, sizeof(status.message),
"resolved %zu conflict(s) with %s, some "
"entries remain",
resolved, argv[2]);
monitor_write(&repo,&status);
}
return 1;
}
if(resolved==0)
printf("No conflicts to resolve.\n");
else
printf("resolved %zu conflict(s) with %s\n", resolved,
argv[2]);
struct mesh_status status;
if(monitor_collect(&repo,&status,"choose")==0) {
snprintf(status.message, sizeof(status.message),
"resolved %zu conflict(s) with %s", resolved,
argv[2]);
if(monitor_write(&repo,&status)<0)
fprintf(stderr,
"mesh: choose: warning: status write "
"failed: %s\n",
strerror(errno));
}
return 0;
}
static int auth_host_ok(const char*host) {
if(!host||!*host||strlen(host)>=MESH_PATH_MAX)
return 0;
for(const unsigned char*p=(const unsigned char*)host;*p;p++) {
if(*p<=0x20||*p==0x7f||*p=='/'||*p=='?'||*p=='#'||*p=='@')
return 0;
}
return 1;
}
static int cmd_auth(int argc, char**argv) {
struct mesh_repo repo;
char path[MESH_PATH_MAX];
char header[MESH_AUTH_MAX+64];
const char*host="*";
const char*token=NULL;
char*old=NULL;
char*buf=NULL;
size_t buf_len=0;
FILE*fp=NULL;
for(int i=2;i<argc;i++) {
if(strcmp(argv[i],"--host")==0) {
if(i+1>=argc) {
fprintf(stderr,"usage: mesh auth [--host HOST] [token]\n");
return 2;
}
host=argv[++i];
} else if(argv[i][0]=='-'&&argv[i][1]!='\0') {
fprintf(stderr,"usage: mesh auth [--host HOST] [token]\n");
return 2;
} else if(!token) {
token=argv[i];
} else {
fprintf(stderr,"usage: mesh auth [--host HOST] [token]\n");
return 2;
}
}
if(repo_required(&repo)<0)
return 1;
if(!token) {
int rc=net_auth_header(&repo,NULL,header,sizeof(header));
if(rc<0) {
fprintf(stderr,"mesh: auth: %s\n", strerror(errno));
return 1;
}
printf("%s\n", rc==0?"Remote credentials stored."
:"No credentials stored.");
return 0;
}
if(!auth_host_ok(host)) {
fprintf(stderr,"mesh: auth: invalid host\n");
return 2;
}
size_t len=strlen(token);
if(len==0||len>MESH_AUTH_MAX) {
fprintf(stderr,"mesh: auth: invalid token length\n");
return 2;
}
for(size_t i=0;i<len;i++) {
unsigned char c=(unsigned char)token[i];
if(c<0x21||c>0x7e) {
fprintf(stderr,
"mesh: auth: token must be printable "
"without whitespace\n");
return 2;
}
}
if(mesh_path_join(path, sizeof(path), repo.mesh_dir,"credentials")<
0) {
fprintf(stderr,"mesh: auth: cannot store credentials: %s\n",
strerror(errno));
return 1;
}
if(mesh_read_text(path,&old,&buf_len)<0&&errno!=ENOENT) {
fprintf(stderr,"mesh: auth: cannot read credentials: %s\n",
strerror(errno));
return 1;
}
fp=open_memstream(&buf,&buf_len);
if(!fp) {
free(old);
fprintf(stderr,"mesh: auth: cannot store credentials: %s\n",
strerror(errno));
return 1;
}
if(old) {
char*save=NULL;
for(char*line=strtok_r(old,"\n",&save);line;
line=strtok_r(NULL,"\n",&save)) {
size_t hlen;
size_t llen;
mesh_trim_newline(line);
if(!*line)
continue;
hlen=strlen(host);
llen=strlen(line);
if(llen>=hlen&&strncasecmp(line,host,hlen)==0&&
(llen==hlen||line[hlen]==' '))
continue;
fprintf(fp,"%s\n",line);
}
free(old);
}
if(fprintf(fp,"%s %s\n",host,token)<0||fclose(fp)<0) {
fp=NULL;
free(buf);
fprintf(stderr,"mesh: auth: cannot store credentials: %s\n",
strerror(errno));
return 1;
}
fp=NULL;
if(mesh_write_atomic(path,buf,buf_len,0600)<0) {
int saved=errno;
free(buf);
fprintf(stderr,"mesh: auth: cannot store credentials: %s\n",
strerror(saved));
return 1;
}
free(buf);
if(chmod(path,0600)<0) {
fprintf(stderr,"mesh: auth: cannot store credentials: %s\n",
strerror(errno));
return 1;
}
printf("Remote credentials stored (mode 0600).\n");
return 0;
}
static int url_basename(const char*url, char*out, size_t cap) {
const char*start=strstr(url,"://");
const char*end=strpbrk(url,"?#");
const char*slash=NULL;
size_t len;
start=start?start+3:url;
if(!end)
end=url+strlen(url);
for(const char*p=start;p<end;p++) {
if(*p=='/')
slash=p;
}
if(!slash) {
/* A bare host carries no usable filename. */
snprintf(out, cap,"download.bin");
return 0;
}
if(slash+1<end)
start=slash+1;
len=(size_t)(end-start);
if(len==0||(len==1&&start[0]=='.')||
(len==2&&start[0]=='.'&&start[1]=='.')) {
snprintf(out, cap,"download.bin");
return 0;
}
if(len>=cap) {
errno=ENAMETOOLONG;
return -1;
}
char name[MESH_PATH_MAX];
memcpy(name, start, len);
name[len]='\0';
if(strchr(name,'/')) {
errno=EINVAL;
return -1;
}
snprintf(out, cap,"%s", name);
return 0;
}
/* JSON payloads */
static int build_snapshot_json(const struct mesh_repo*repo,
const struct mesh_graph*graph,
const char*head, const char*tree,
const char*message, char**out,
size_t*out_len) {
struct mesh_file_list files;
const struct mesh_node*node;
char*manifest=NULL;
char*manifest_esc=NULL;
char*json=NULL;
size_t json_len=0;
char msg_esc[MESH_MSG_MAX*6+1];
char branch_esc[64*6+1];
char author_name_esc[MESH_AUTHOR_MAX*6+1];
char author_email_esc[MESH_EMAIL_MAX*6+1];
FILE*fp=NULL;
uint64_t bytes=0;
int rc=-1;
if(!repo||!graph||!out||!out_len) {
errno=EINVAL;
return -1;
}
*out=NULL;
*out_len=0;
file_list_init(&files);
node=head&&*head?graph_find(graph, head):NULL;
if(mesh_json_escape(message?message:(node?node->message:""),
msg_esc, sizeof(msg_esc))<0||
mesh_json_escape(repo->branch[0]?repo->branch:"main",
branch_esc, sizeof(branch_esc))<0||
mesh_json_escape(node?node->author_name:"",
author_name_esc, sizeof(author_name_esc))<0||
mesh_json_escape(node?node->author_email:"",
author_email_esc, sizeof(author_email_esc))<0)
goto done;
if(tree&&*tree) {
char path[MESH_PATH_MAX];
if(manifest_load(repo, tree,&files)<0)
goto done;
for(size_t i=0;i<files.n;i++)
bytes+=files.v[i].size;
char name[MESH_HASH_HEX+32];
snprintf(name, sizeof(name),"store/trees/%s", tree);
if(mesh_path_join(path, sizeof(path), repo->mesh_dir, name)==0&&
mesh_read_text(path,&manifest, NULL)==0&&
strlen(manifest)<=MESH_PATCH_LIMIT) {
manifest_esc=malloc(strlen(manifest)*6+1);
if(!manifest_esc)
goto done;
if(mesh_json_escape(manifest, manifest_esc,
strlen(manifest)*6+1)<0)
goto done;
}
}
fp=open_memstream(&json,&json_len);
if(!fp) {
fp=NULL;
goto done;
}
fprintf(fp,"{\"mesh_version\":\""MESH_VERSION "\","
"\"action\":\"snapshot\","
"\"head\":\"%s\",\"tree\":\"%s\",\"parent\":\"%s\","
"\"branch\":\"%s\",\"message\":\"%s\","
"\"author_name\":\"%s\",\"author_email\":\"%s\","
"\"snapshots\":%zu,\"files\":%zu,\"bytes\":%"PRIu64 ",",
head?head:"", tree?tree:"",
node?node->parent_1:"", branch_esc, msg_esc,
author_name_esc, author_email_esc, graph->count,
files.n, bytes);
if(manifest_esc)
fprintf(fp,"\"manifest\":\"%s\",", manifest_esc);
fprintf(fp,"\"history\":[");
const struct mesh_node*n=graph->head;
unsigned emitted=0;
while(n&&emitted<64) {
char msg[MESH_MSG_MAX*6+1];
char name[MESH_AUTHOR_MAX*6+1];
char email[MESH_EMAIL_MAX*6+1];
if(mesh_json_escape(n->message, msg, sizeof(msg))<0||
mesh_json_escape(n->author_name, name, sizeof(name))<0||
mesh_json_escape(n->author_email, email, sizeof(email))<0)
goto done;
fprintf(fp,"%s{\"hash\":\"%s\",\"tree\":\"%s\","
"\"parent\":\"%s\",\"timestamp\":%lld,"
"\"message\":\"%s\",\"author_name\":\"%s\","
"\"author_email\":\"%s\"}",
emitted?",":"", n->hash, n->tree,
n->parent_1, (long long)n->timestamp, msg, name, email);
emitted++;
n=n->next;
}
fprintf(fp,"],\"repo_icon_source\":\""MESH_ICON "\"}");
if(fflush(fp)==EOF||ferror(fp))
goto done;
if(fclose(fp)!=0) {
fp=NULL;
goto done;
}
fp=NULL;
*out=json;
*out_len=json_len;
json=NULL;
rc=0;
done: {
int saved=errno;
if(fp)
fclose(fp);
free(json);
free(manifest);
free(manifest_esc);
file_list_free(&files);
errno=saved;
}
return rc;
}
static void print_response(const struct mesh_http*resp) {
char buf[401];
size_t total;
size_t at=0;
if(!resp||!resp->data)
return;
total=resp->len?resp->len:strlen(resp->data);
for(size_t i=0;i<total&&at<sizeof(buf)-1;i++) {
unsigned char c=(unsigned char)resp->data[i];
buf[at++]=(c<0x20||c==0x7f)?'?':(char)c;
}
buf[at]='\0';
if(at==0)
printf("  response: (empty)\n");
else if(at<total)
printf("  response: %s...\n", buf);
else
printf("  response: %s\n", buf);
}
static int cmd_send(int argc, char**argv) {
struct mesh_repo repo;
struct mesh_graph graph;
struct mesh_status status;
struct mesh_http resp;
char head[MESH_HASH_HEX+1];
char tree[MESH_HASH_HEX+1]="";
char*payload=NULL;
size_t payload_len=0;
int rc=1;
if(argc<3) {
fprintf(stderr,"usage: mesh send <url>\n");
return 2;
}
if(repo_required(&repo)<0)
return 1;
warn_plain_http("send", argv[2]);
if(graph_head(&repo, head)<0||head[0]=='\0') {
fprintf(stderr,"mesh: send: repository has no snapshots\n");
return 1;
}
graph_init(&graph);
if(graph_load(&repo,&graph)<0) {
fprintf(stderr,"mesh: send: cannot load history: %s\n",
strerror(errno));
return 1;
}
struct mesh_node*node=graph_find(&graph, head);
if(!node) {
fprintf(stderr,
"mesh: send: HEAD points to a snapshot missing from history\n");
graph_free(&graph);
return 1;
}
snprintf(tree, sizeof(tree),"%s", node->tree);
if(build_snapshot_json(&repo,&graph, head, tree, NULL,&payload,
&payload_len)<0) {
fprintf(stderr,"mesh: send: cannot build payload: %s\n",
strerror(errno));
goto done;
}
mesh_http_init(&resp);
if(net_post_json(&repo, argv[2], payload,&resp)<0) {
fprintf(stderr,"mesh: send: POST failed: %s\n",
strerror(errno));
goto done;
}
if(resp.status<200||resp.status>=300) {
fprintf(stderr,"mesh: send: remote returned HTTP %ld\n",
resp.status);
print_response(&resp);
mesh_http_free(&resp);
goto done;
}
printf("sent snapshot %.12s to %s (HTTP %ld, %zu bytes JSON)\n", head,
argv[2], resp.status, payload_len);
print_response(&resp);
if(monitor_collect(&repo,&status,"send")==0) {
snprintf(status.message, sizeof(status.message),
"POST %.400s (HTTP %ld)", argv[2], resp.status);
monitor_write(&repo,&status);
}
mesh_http_free(&resp);
rc=0;
done:
free(payload);
graph_free(&graph);
return rc;
}
/* Bidirectional sync */
static int join_url(char*out, size_t cap, const char*base, const char*suffix) {
size_t len=strlen(base);
int n;
while(len>0&&base[len-1]=='/')
len--;
n=snprintf(out, cap,"%.*s%s", (int)len, base, suffix);
if(n<0||(size_t)n>=cap) {
errno=ENAMETOOLONG;
return -1;
}
return 0;
}
static int fetch_object(const struct mesh_repo*repo, const char*base,
const char*hash) {
char final[MESH_PATH_MAX], part[MESH_PATH_MAX], url[MESH_PATH_MAX];
char suffix[MESH_HASH_HEX+32];
char got[MESH_HASH_HEX+1];
int n;
if(!looks_like_hash(hash)) {
errno=EINVAL;
return -1;
}
if(mesh_object_exists(repo, hash)&&object_verify(repo, hash, NULL)==0)
return 0;
if(mesh_object_path(repo, hash, final, sizeof(final))<0)
return -1;
if(snprintf(part, sizeof(part),"%s.part", final)>=(int)sizeof(part)) {
errno=ENAMETOOLONG;
return -1;
}
char dir[MESH_PATH_MAX];
char*slash;
snprintf(dir, sizeof(dir),"%s", final);
slash=strrchr(dir,'/');
if(slash) {
*slash='\0';
if(mesh_mkdir_p(dir)<0)
return -1;
}
snprintf(suffix, sizeof(suffix),"/objects/%s", hash);
if(join_url(url, sizeof(url), base, suffix)<0)
return -1;
if(net_download(repo, url, part, NULL, NULL, got)<0)
return -1;
if(strcmp(got, hash)!=0) {
fprintf(stderr,
"mesh: sync: object %s failed BLAKE2b verification\n",
hash);
unlink(part);
errno=EIO;
return -1;
}
n=rename(part, final);
if(n<0) {
int saved=errno;
unlink(part);
errno=saved;
} else {
int fd=open(final,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
if(fd<0||fchmod(fd,0444)<0) {
int saved=errno;
if(fd>=0)
close(fd);
unlink(final);
errno=saved;
n=-1;
} else {
close(fd);
if(mesh_fsync_parent(final)<0)
n=-1;
}
}
return n;
}
static int upload_object(const struct mesh_repo*repo, const char*base,
const char*hash) {
char path[MESH_PATH_MAX], url[MESH_PATH_MAX];
char suffix[MESH_HASH_HEX+32];
if(!looks_like_hash(hash))
return -1;
if(!mesh_object_exists(repo, hash)) {
fprintf(stderr,
"mesh: sync: warning: local object %s is missing\n",
hash);
return 0;
}
if(object_verify(repo, hash, NULL)<0) {
fprintf(stderr,
"mesh: sync: local object %s failed integrity verification\n",
hash);
return -1;
}
if(mesh_object_path(repo, hash, path, sizeof(path))<0)
return -1;
snprintf(suffix, sizeof(suffix),"/objects/%s", hash);
if(join_url(url, sizeof(url), base, suffix)<0)
return -1;
return net_put_file(repo, url, path);
}
struct hash_set {
char (*v)[MESH_HASH_HEX+1];
size_t n;
};
static int hash_set_cmp(const void*a, const void*b) {
return strcmp(a, b);
}
static void hash_set_free(struct hash_set*set) {
if(!set)
return;
free(set->v);
set->v=NULL;
set->n=0;
}
static int hash_set_build(const struct mesh_file_list*list,
struct hash_set*set) {
size_t total=0;
memset(set, 0, sizeof(*set));
for(size_t i=0;i<list->n;i++) {
if(list->v[i].nchunks>SIZE_MAX-total-1) {
errno=E2BIG;
return -1;
}
total+=list->v[i].nchunks+1;
}
if(total>0) {
if(total>SIZE_MAX/sizeof(*set->v)) {
errno=E2BIG;
return -1;
}
set->v=malloc(total*sizeof(*set->v));
if(!set->v)
return -1;
}
if(total==0)
return 0;
size_t at=0;
for(size_t i=0;i<list->n;i++) {
memcpy(set->v[at++], list->v[i].hash, MESH_HASH_HEX+1);
for(size_t c=0;c<list->v[i].nchunks;c++)
memcpy(set->v[at++], list->v[i].chunks[c].hash,
MESH_HASH_HEX+1);
}
qsort(set->v, total, sizeof(*set->v), hash_set_cmp);
size_t unique=0;
for(size_t i=0;i<total;i++) {
if(unique==0||strcmp(set->v[unique-1], set->v[i])!=0) {
if(unique!=i)
memcpy(set->v[unique], set->v[i], MESH_HASH_HEX+1);
unique++;
}
}
set->n=unique;
return 0;
}
static bool hash_set_contains(const struct hash_set*set, const char*hash) {
if(!set||!set->v||set->n==0)
return false;
return bsearch(hash, set->v, set->n, sizeof(*set->v),
hash_set_cmp)!=NULL;
}
static int upload_missing(const struct mesh_repo*repo, const char*base,
const char*json, const char*tree, size_t*uploaded) {
struct mesh_file_list files;
struct hash_set allowed;
const char*p;
const char*array_end;
size_t count=0;
int rc=-1;
file_list_init(&files);
memset(&allowed, 0, sizeof(allowed));
if(!json)
return 0;
if(!tree||!*tree)
return 0;
if(manifest_load(repo, tree, &files)<0) {
fprintf(stderr,
"mesh: sync: cannot load local tree for upload: %s\n",
strerror(errno));
goto done;
}
if(hash_set_build(&files, &allowed)<0)
goto done;
p=strstr(json,"\"missing_objects\"");
if(!p) {
rc=0;
goto done;
}
p=strchr(p,'[');
if(!p) {
rc=0;
goto done;
}
array_end=strchr(p+1,']');
if(!array_end) {
errno=EINVAL;
goto done;
}
while(p<array_end&&(p=strchr(p,'"'))!=NULL&&p<array_end) {
char hash[MESH_HASH_HEX+1];
p++;
if(p+MESH_HASH_HEX>=array_end)
break;
memcpy(hash, p, MESH_HASH_HEX);
hash[MESH_HASH_HEX]='\0';
if(!looks_like_hash(hash)) {
p+=1;
continue;
}
if(p[MESH_HASH_HEX]!='"') {
p+=MESH_HASH_HEX;
continue;
}
if(!hash_set_contains(&allowed, hash)) {
fprintf(stderr,
"mesh: sync: warning: peer requested object %s outside "
"the current tree; not uploading\n", hash);
p+=MESH_HASH_HEX+1;
continue;
}
if(upload_object(repo, base, hash)<0)
goto done;
count++;
if(count>4096) {
errno=E2BIG;
goto done;
}
p+=MESH_HASH_HEX+1;
}
if(uploaded)
*uploaded+=count;
rc=0;
done: {
int saved=errno;
hash_set_free(&allowed);
file_list_free(&files);
errno=saved;
}
return rc;
}
static int sync_read_symlink(const struct mesh_repo*repo,const char*rel,
char*target,size_t cap,size_t*target_len) {
char leaf[MESH_PATH_MAX];
int root=-1,parent=-1,rc=-1;
if(!repo||!rel||!target||cap<2||!target_len) {
errno=EINVAL;
return -1;
}
root=open(repo->root,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
if(root<0)
goto done;
if(mesh_open_parent_at(root,rel,false,&parent,leaf,sizeof(leaf))<0)
goto done;
ssize_t n=readlinkat(parent,leaf,target,cap-1);
if(n<0)
goto done;
if(n==0||(size_t)n>=cap-1) {
errno=ENAMETOOLONG;
goto done;
}
target[n]='\0';
*target_len=(size_t)n;
rc=0;
done: {
int saved=errno;
if(parent>=0)close(parent);
if(root>=0)close(root);
errno=saved;
}
return rc;
}
static int sync_symlink_payload(const struct mesh_repo*repo,
const char*rel,const char*dest) {
char target[MESH_PATH_MAX+1];
size_t len=0;
int fd=-1,rc=-1;
if(sync_read_symlink(repo,rel,target,sizeof(target),&len)<0)
return -1;
if(mesh_path_no_symlink(dest)<0)
return -1;
fd=open(dest,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,0600);
if(fd<0)
return -1;
if(mesh_write_all(fd,target,len)<0||fsync(fd)<0)
goto done;
rc=0;
done: {
int saved=errno;
if(close(fd)<0&&rc==0) {
rc=-1;
saved=errno;
}
errno=saved;
}
return rc;
}
static int sync_symlink_hash(const struct mesh_repo*repo,const char*rel,
char out[MESH_HASH_HEX+1]) {
char target[MESH_PATH_MAX+1];
size_t len=0;
struct mesh_hash_ctx hash;
if(sync_read_symlink(repo,rel,target,sizeof(target),&len)<0)
return -1;
mesh_hash_init(&hash);
mesh_hash_update(&hash,target,len);
mesh_hash_final(&hash,out);
return 0;
}
static int sync_pull(const struct mesh_repo*repo, const char*base,
const char*local_tree, const char*remote_tree,
size_t*pulled, size_t*conflicts) {
struct mesh_file_list local, remote;
bool have_local=false;
int rc=-1;
file_list_init(&local);
file_list_init(&remote);
if(manifest_load(repo, remote_tree,&remote)<0)
goto done;
if(local_tree&&local_tree[0]) {
if(manifest_load(repo, local_tree,&local)<0)
goto done;
have_local=true;
}
/* Fetch every object the remote manifest needs first. */
for(size_t i=0;i<remote.n;i++) {
const struct mesh_file_entry*e=&remote.v[i];
size_t nchunks=e->nchunks?e->nchunks:1;
for(size_t c=0;c<nchunks;c++) {
const char*hash=e->nchunks?e->chunks[c].hash:e->hash;
if(fetch_object(repo, base, hash)<0) {
fprintf(stderr,
"mesh: sync: cannot fetch %s %s: %s\n",
e->nchunks?"chunk":"object", hash,
strerror(errno));
goto done;
}
}
}
for(size_t i=0;i<remote.n;i++) {
const struct mesh_file_entry*re=&remote.v[i];
const struct mesh_file_entry*le;
char dest[MESH_PATH_MAX];
struct stat dest_st;
bool work_exists;
if(!mesh_relpath_safe(re->path)) {
fprintf(stderr,
"mesh: sync: refusing unsafe remote path\n");
goto done;
}
le=have_local?manifest_find(&local, re->path):NULL;
if(mesh_path_join(dest, sizeof(dest), repo->root,
re->path)<0)
goto done;
work_exists=lstat(dest,&dest_st)==0;
if(!work_exists&&errno!=ENOENT)
goto done;
if(le&&strcmp(le->hash, re->hash)==0&&
le->kind==re->kind) {
if(!work_exists||
(re->kind==MESH_KIND_FILE&&!S_ISREG(dest_st.st_mode))||
(re->kind==MESH_KIND_SYMLINK&&!S_ISLNK(dest_st.st_mode))) {
if(manifest_materialize_entry(repo, re)<0) {
fprintf(stderr,
"mesh: sync: cannot restore %s: %s\n",
re->path, strerror(errno));
goto done;
}
(*pulled)++;
} else if(le->kind==MESH_KIND_FILE&&
le->mode!=re->mode) {
char work_hash[MESH_HASH_HEX+1];
if(blake2b_hash_file(dest,work_hash)==0&&
strcmp(work_hash,re->hash)==0) {
int fd=open(dest,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
if(fd<0||fchmod(fd,(mode_t)re->mode)<0) {
if(fd>=0)
close(fd);
goto done;
}
close(fd);
}
}
continue;
}
if(!le&&!work_exists) {
if(manifest_materialize_entry(repo, re)<0) {
fprintf(stderr,
"mesh: sync: cannot write %s: %s\n",
re->path, strerror(errno));
goto done;
}
(*pulled)++;
continue;
}
/*
* A working file that already carries the remote bytes is not a
* conflict, even when it is not part of the local snapshot yet.
* This keeps repeated syncs idempotent.
*/
char working_hash[MESH_HASH_HEX+1];
if(work_exists&&re->kind==MESH_KIND_FILE&&
S_ISREG(dest_st.st_mode)&&
blake2b_hash_file(dest, working_hash)==0&&
strcmp(working_hash, re->hash)==0)
continue;
if(work_exists&&re->kind==MESH_KIND_SYMLINK&&
S_ISLNK(dest_st.st_mode)&&
sync_symlink_hash(repo,re->path,working_hash)==0&&
strcmp(working_hash,re->hash)==0)
continue;
if(work_exists&&!S_ISREG(dest_st.st_mode)&&!S_ISLNK(dest_st.st_mode)) {
fprintf(stderr,"mesh: sync: refusing non-file conflict for %s\n",re->path);
errno=EISDIR;
goto done;
}
/* Collision: stage pure variants, never touch the file. */
char alpha[MESH_PATH_MAX], beta[MESH_PATH_MAX];
int n1, n2;
n1=snprintf(alpha, sizeof(alpha),
"%s/store/tmp/alpha.%ld", repo->mesh_dir,
(long)getpid());
n2=snprintf(beta, sizeof(beta),
"%s/store/tmp/beta.%ld", repo->mesh_dir,
(long)getpid());
if(n1<0||(size_t)n1>=sizeof(alpha)||n2<0||
(size_t)n2>=sizeof(beta)) {
errno=ENAMETOOLONG;
goto done;
}
mode_t alpha_mode=le?(mode_t)le->mode:0644;
uint32_t alpha_kind=le?le->kind:MESH_KIND_FILE;
if(work_exists&&S_ISREG(dest_st.st_mode)) {
alpha_kind=MESH_KIND_FILE;
alpha_mode=(mode_t)(dest_st.st_mode&0777);
if(mesh_copy_path_to_path(dest,alpha,alpha_mode)<0)
goto done;
} else if(work_exists&&S_ISLNK(dest_st.st_mode)) {
alpha_kind=MESH_KIND_SYMLINK;
alpha_mode=le&&le->kind==MESH_KIND_SYMLINK?
(mode_t)le->mode:0777;
if(sync_symlink_payload(repo,re->path,alpha)<0)
goto done;
} else if(le) {
if(manifest_stream_path(repo,local_tree,re->path,alpha,alpha_mode)<0)
goto done;
} else {
errno=ENOENT;
goto done;
}
if(manifest_stream_path(repo, remote_tree, re->path,
beta, (mode_t)re->mode)<0) {
unlink(alpha);
goto done;
}
if(conflict_stage_typed(repo,re->path,alpha,beta,alpha_kind,
alpha_mode,re->kind,(mode_t)re->mode)<0) {
unlink(alpha);
unlink(beta);
goto done;
}
unlink(alpha);
unlink(beta);
(*conflicts)++;
}
rc=0;
done: {
int saved=errno;
file_list_free(&local);
file_list_free(&remote);
errno=saved;
}
return rc;
}
static int cmd_sync(int argc, char**argv) {
struct mesh_repo repo;
struct mesh_graph graph;
struct mesh_status status;
struct mesh_http resp, push;
char head[MESH_HASH_HEX+1];
char tree[MESH_HASH_HEX+1]="";
char remote_tree[MESH_HASH_HEX+1]="";
char remote_etag[256]="";
char manifest_path[MESH_PATH_MAX];
char header_hash[MESH_HASH_HEX+32];
char header_tree[MESH_HASH_HEX+32];
const char*extra[4];
char*payload=NULL;
size_t payload_len=0;
size_t pulled=0, conflicts=0, uploaded=0;
size_t pending=0;
bool pushed=false;
bool supports_cas=false;
int lock_fd=-1;
int rc=1;
if(argc<3) {
fprintf(stderr,"usage: mesh sync <url>\n");
return 2;
}
if(repo_required(&repo)<0)
return 1;
warn_plain_http("sync", argv[2]);
lock_fd=mesh_repo_lock(&repo);
if(lock_fd<0) {
fprintf(stderr,"mesh: sync: cannot lock repository: %s\n",
strerror(errno));
return 1;
}
graph_init(&graph);
mesh_http_init(&resp);
mesh_http_init(&push);
if(graph_head(&repo, head)<0) {
fprintf(stderr,"mesh: sync: cannot read HEAD: %s\n",
strerror(errno));
goto done;
}
if(graph_load(&repo,&graph)<0) {
fprintf(stderr,"mesh: sync: cannot load history: %s\n",
strerror(errno));
goto done;
}
if(head[0]) {
struct mesh_node*node=graph_find(&graph, head);
if(!node) {
fprintf(stderr,
"mesh: sync: HEAD points to a snapshot missing from history\n");
goto done;
}
snprintf(tree, sizeof(tree),"%s", node->tree);
}
snprintf(header_hash, sizeof(header_hash),"X-Mesh-Hash: %s", head);
snprintf(header_tree, sizeof(header_tree),"X-Mesh-Tree: %s", tree);
extra[0]=header_hash;
extra[1]=header_tree;
extra[2]="Accept: text/plain";
extra[3]=NULL;
pending=conflict_count(&repo);
if(pending>0) {
fprintf(stderr,
"mesh: sync: %zu conflict(s) remain; resolve and save before retrying\n",
pending);
goto done;
}
if(net_get(&repo, argv[2], extra,&resp)<0) {
fprintf(stderr,"mesh: sync: GET failed: %s\n",
strerror(errno));
goto done;
}
if(resp.status!=304&&(resp.status<200||resp.status>=300)) {
fprintf(stderr,"mesh: sync: remote returned HTTP %ld\n",
resp.status);
goto done;
}
	if(resp.status==304||
	(head[0]&&resp.remote_hash[0]&&
	strcmp(resp.remote_hash, head)==0)) {
printf("already up to date: %.12s\n", head);
if(monitor_collect(&repo,&status,"sync")==0) {
snprintf(status.message, sizeof(status.message),
"up to date");
monitor_write(&repo,&status);
} else {
fprintf(stderr,
"mesh: sync: warning: cannot refresh status: %s\n",
strerror(errno));
}
rc=0;
	goto done;
	}
	if(resp.supports_cas) {
	if(!resp.remote_etag[0]) {
	fprintf(stderr,"mesh: sync: peer advertises CAS but sent no ETag\n");
	goto done;
	}
	mesh_strlcpy(remote_etag,sizeof(remote_etag),resp.remote_etag);
	supports_cas=true;
	}
	if(resp.remote_tree[0]) {
if(!looks_like_hash(resp.remote_tree)) {
fprintf(stderr,"mesh: sync: malformed X-Mesh-Tree "
"header\n");
goto done;
}
snprintf(remote_tree, sizeof(remote_tree),"%s",
resp.remote_tree);
}
if(remote_tree[0]&&strcmp(remote_tree, tree)!=0) {
char actual[MESH_HASH_HEX+1];
uint8_t digest[MESH_HASH_LEN];
static const char digits[]="0123456789abcdef";
blake2b_hash_buffer(resp.data, resp.len, digest);
for(size_t i=0;i<MESH_HASH_LEN;i++) {
actual[2*i]=digits[digest[i]>>4];
actual[2*i+1]=digits[digest[i]&0x0f];
}
actual[MESH_HASH_HEX]='\0';
if(strcmp(actual, remote_tree)!=0) {
fprintf(stderr,
"mesh: sync: remote manifest failed tree-hash verification\n");
goto done;
}
char name[MESH_HASH_HEX+32];
snprintf(name, sizeof(name),"store/trees/%s",
remote_tree);
if(mesh_path_join(manifest_path,
sizeof(manifest_path),
repo.mesh_dir, name)<0)
goto done;
if(mesh_path_no_symlink(manifest_path)<0)
goto done;
if(mesh_write_atomic(manifest_path, resp.data, resp.len, 0644)<
0) {
fprintf(stderr,
"mesh: sync: cannot store remote manifest: %s\n",
strerror(errno));
goto done;
}
if(sync_pull(&repo, argv[2], tree, remote_tree,&pulled,
&conflicts)<0) {
fprintf(stderr,"mesh: sync: pull failed: %s\n",
strerror(errno));
goto done;
}
}
if(conflicts>0||conflict_count(&repo)>0) {
pending=conflict_count(&repo);
fprintf(stderr,
"mesh: sync: snapshot not published; resolve conflicts and save before retrying\n");
if(monitor_collect(&repo,&status,"sync")==0) {
snprintf(status.message, sizeof(status.message),
"%zu conflict(s); snapshot not published", pending);
monitor_write(&repo,&status);
}
goto done;
}
if(head[0]) {
if(build_snapshot_json(&repo,&graph, head, tree, NULL,&payload,
&payload_len)<0) {
fprintf(stderr,
"mesh: sync: cannot build payload: %s\n",
strerror(errno));
goto done;
}
	if((supports_cas?
	net_post_json_if_match(&repo,argv[2],payload,remote_etag,&push):
	net_post_json(&repo, argv[2], payload,&push))<0) {
	fprintf(stderr,"mesh: sync: POST failed: %s\n",
	strerror(errno));
	goto done;
	}
	if(push.status<200||push.status>=300) {
	if(supports_cas&&(push.status==409||push.status==412))
	fprintf(stderr,"mesh: sync: remote changed since GET; no snapshot was published (HTTP %ld)\n",
	push.status);
	else
	fprintf(stderr,"mesh: sync: push returned HTTP %ld\n",
	push.status);
	goto done;
}
pushed=true;
if(upload_missing(&repo, argv[2], push.data, tree,&uploaded)<0) {
fprintf(stderr,
"mesh: sync: object upload failed: %s\n",
strerror(errno));
goto done;
}
}
if(monitor_collect(&repo,&status,"sync")==0) {
snprintf(status.message, sizeof(status.message),
"pulled %zu, conflicts %zu, uploaded %zu", pulled,
conflicts, uploaded);
monitor_write(&repo,&status);
} else {
fprintf(stderr,
"mesh: sync: warning: cannot refresh status: %s\n",
strerror(errno));
}
printf("sync complete: pulled %zu file(s), %zu conflict(s)", pulled,
conflicts);
if(pushed)
printf(", pushed %.12s", head);
if(uploaded>0)
printf(", uploaded %zu object(s)", uploaded);
printf("\n");
if(pulled>0)
printf("Working tree changed: run 'mesh save \"sync\"' to record "
"the new state.\n");
if(conflicts>0)
printf("Resolve with 'mesh choose alpha' or 'mesh choose beta'.\n");
rc=0;
done:
free(payload);
mesh_http_free(&resp);
mesh_http_free(&push);
graph_free(&graph);
if(lock_fd>=0)
close(lock_fd);
return rc;
}
static bool entries_same(const struct mesh_file_entry*a,
const struct mesh_file_entry*b) {
return a->kind==b->kind&&a->mode==b->mode&&a->size==b->size&&
strcmp(a->hash,b->hash)==0;
}
static void hash_buffer_hex(const void*data, size_t len,
char out[MESH_HASH_HEX+1]) {
uint8_t digest[MESH_HASH_LEN];
static const char digits[]="0123456789abcdef";
blake2b_hash_buffer(data, len, digest);
for(size_t i=0;i<MESH_HASH_LEN;i++) {
out[2*i]=digits[digest[i]>>4];
out[2*i+1]=digits[digest[i]&0x0f];
}
out[MESH_HASH_HEX]='\0';
}
static bool work_matches_entry(const struct mesh_repo*repo,
const struct mesh_file_entry*entry) {
int root=-1, parent=-1, fd=-1;
char leaf[MESH_PATH_MAX];
char hex[MESH_HASH_HEX+1];
struct stat st;
bool match=false;
if(!repo||!repo->valid||!entry)
return false;
root=open(repo->root,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
if(root<0)
return false;
if(mesh_open_parent_at(root, entry->path, false,&parent, leaf,
sizeof(leaf))<0)
goto done;
if(fstatat(parent, leaf,&st,AT_SYMLINK_NOFOLLOW)<0)
goto done;
if(entry->kind==MESH_KIND_SYMLINK) {
char target[MESH_PATH_MAX];
ssize_t len;
if(!S_ISLNK(st.st_mode)||(uint64_t)st.st_size!=entry->size)
goto done;
len=readlinkat(parent, leaf, target, sizeof(target));
if(len<0||(uint64_t)len!=entry->size)
goto done;
hash_buffer_hex(target,(size_t)len,hex);
match=strcmp(hex,entry->hash)==0;
} else {
if(!S_ISREG(st.st_mode)||(uint64_t)st.st_size!=entry->size||
(uint32_t)(st.st_mode&0777)!=entry->mode)
goto done;
fd=openat(parent, leaf, O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
if(fd<0)
goto done;
if(fstat(fd,&st)<0||(uint64_t)st.st_size!=entry->size||
(uint32_t)(st.st_mode&0777)!=entry->mode)
goto done;
if(blake2b_hash_fd(fd,0,entry->size,hex)<0)
goto done;
match=strcmp(hex,entry->hash)==0;
}
done: {
int saved=errno;
if(fd>=0)
close(fd);
if(parent>=0)
close(parent);
if(root>=0)
close(root);
errno=saved;
}
return match;
}
static int restore_one(const struct mesh_repo*repo,
const struct mesh_file_entry*entry, bool force,
size_t*restored, size_t*skipped) {
char full[MESH_PATH_MAX];
struct stat st;
if(mesh_path_join(full,sizeof(full),repo->root,entry->path)<0)
return -1;
if(lstat(full,&st)==0) {
if(work_matches_entry(repo,entry)) {
(*skipped)++;
return 0;
}
if(!force) {
fprintf(stderr,
"mesh: restore: refusing to overwrite %s; use --force\n",
entry->path);
errno=EEXIST;
return -1;
}
} else if(errno!=ENOENT) {
return -1;
}
if(manifest_materialize_entry(repo,entry)<0)
return -1;
(*restored)++;
return 0;
}
static int cmd_log(int argc, char**argv) {
struct mesh_repo repo;
struct mesh_graph graph;
(void)argv;
if(argc>2) {
fprintf(stderr,"usage: mesh log\n");
return 2;
}
if(repo_required(&repo)<0)
return 1;
graph_init(&graph);
if(graph_load(&repo,&graph)<0) {
fprintf(stderr,"mesh: log: cannot load history: %s\n",
strerror(errno));
graph_free(&graph);
return 1;
}
graph_print(&graph, stdout);
graph_free(&graph);
return 0;
}
static int cmd_diff(int argc, char**argv) {
struct mesh_repo repo;
struct mesh_graph graph;
struct mesh_file_list from, to;
struct mesh_ignore*ignore=NULL;
char head[MESH_HASH_HEX+1];
char from_hash[MESH_HASH_HEX+1];
char to_hash[MESH_HASH_HEX+1];
size_t added=0, modified=0, deleted=0;
int rc=1;
if(argc>4) {
fprintf(stderr,"usage: mesh diff [snapshot [snapshot]]\n");
return 2;
}
if(repo_required(&repo)<0)
return 1;
graph_init(&graph);
file_list_init(&from);
file_list_init(&to);
if(graph_head(&repo, head)<0||graph_load(&repo,&graph)<0) {
fprintf(stderr,"mesh: diff: cannot load history: %s\n",
strerror(errno));
goto done;
}
if(argc==2) {
if(head[0]) {
struct mesh_node*hn=graph_find(&graph,head);
if(!hn) {
fprintf(stderr,
"mesh: diff: HEAD points to a snapshot missing from history\n");
goto done;
}
if(manifest_load(&repo, hn->tree,&from)<0) {
fprintf(stderr,"mesh: diff: cannot load snapshot: %s\n",
strerror(errno));
goto done;
}
}
} else {
if(graph_resolve(&graph,argv[2],head,from_hash)<0) {
fprintf(stderr,
"mesh: diff: unknown or ambiguous snapshot '%s'\n",
argv[2]);
goto done;
}
if(manifest_load(&repo, graph_find(&graph,from_hash)->tree,&from)<0) {
fprintf(stderr,"mesh: diff: cannot load snapshot: %s\n",
strerror(errno));
goto done;
}
}
if(argc==4) {
if(graph_resolve(&graph,argv[3],head,to_hash)<0) {
fprintf(stderr,
"mesh: diff: unknown or ambiguous snapshot '%s'\n",
argv[3]);
goto done;
}
if(manifest_load(&repo, graph_find(&graph,to_hash)->tree,&to)<0) {
fprintf(stderr,"mesh: diff: cannot load snapshot: %s\n",
strerror(errno));
goto done;
}
} else {
if(ignore_load(&repo,&ignore)<0) {
fprintf(stderr,"mesh: diff: cannot read .meshignore: %s\n",
strerror(errno));
goto done;
}
if(scan_dir(&repo,"",&to,ignore,0)<0) {
fprintf(stderr,"mesh: diff: cannot scan working tree: %s\n",
strerror(errno));
goto done;
}
if(to.n>1)
qsort(to.v, to.n, sizeof(to.v[0]), entry_cmp);
if(hash_work_tree(&repo,&to)<0||
work_tree_stable(&repo,&to)<0||
chunk_work_tree(&repo,&to)<0||
work_tree_stable(&repo,&to)<0) {
fprintf(stderr,"mesh: diff: cannot hash working tree: %s\n",
strerror(errno));
goto done;
}
}
size_t i=0, j=0;
while(i<from.n||j<to.n) {
int cmp;
if(i>=from.n)
cmp=1;
else if(j>=to.n)
cmp=-1;
else
cmp=strcmp(from.v[i].path,to.v[j].path);
if(cmp<0) {
printf("D %s\n", from.v[i].path);
deleted++;
i++;
} else if(cmp>0) {
printf("A %s\n", to.v[j].path);
added++;
j++;
} else {
if(!entries_same(&from.v[i],&to.v[j])) {
printf("M %s\n", to.v[j].path);
modified++;
}
i++;
j++;
}
}
if(added==0&&modified==0&&deleted==0)
printf("no changes\n");
else
printf("%zu added, %zu modified, %zu deleted\n",
added, modified, deleted);
rc=0;
done:
file_list_free(&from);
file_list_free(&to);
ignore_free(ignore);
graph_free(&graph);
return rc;
}
static int cmd_restore(int argc, char**argv) {
struct mesh_repo repo;
struct mesh_graph graph;
struct mesh_file_list files;
char head[MESH_HASH_HEX+1];
char snapshot[MESH_HASH_HEX+1];
const struct mesh_node*node;
bool force=false;
bool after_dd=false;
bool all;
size_t selected=0, restored=0, skipped=0;
int lock_fd=-1;
int rc=1;
if(argc<3) {
fprintf(stderr,
"usage: mesh restore <snapshot> [--force] [--] [path...]\n");
return 2;
}
for(int i=3;i<argc;i++) {
if(!after_dd&&strcmp(argv[i],"--force")==0) {
force=true;
continue;
}
if(!after_dd&&strcmp(argv[i],"--")==0) {
after_dd=true;
continue;
}
if(!after_dd&&argv[i][0]=='-') {
fprintf(stderr,"mesh: restore: unknown option '%s'\n",
argv[i]);
return 2;
}
selected++;
}
all=selected==0;
if(repo_required(&repo)<0)
return 1;
lock_fd=mesh_repo_lock(&repo);
if(lock_fd<0) {
fprintf(stderr,"mesh: restore: cannot lock repository: %s\n",
strerror(errno));
return 1;
}
graph_init(&graph);
file_list_init(&files);
if(graph_head(&repo, head)<0||graph_load(&repo,&graph)<0) {
fprintf(stderr,"mesh: restore: cannot load history: %s\n",
strerror(errno));
goto done;
}
if(graph_resolve(&graph,argv[2],head,snapshot)<0) {
fprintf(stderr,
"mesh: restore: unknown or ambiguous snapshot '%s'\n",
argv[2]);
goto done;
}
node=graph_find(&graph,snapshot);
if(!node||manifest_load(&repo,node->tree,&files)<0) {
fprintf(stderr,"mesh: restore: cannot load snapshot: %s\n",
strerror(errno));
goto done;
}
if(all) {
for(size_t i=0;i<files.n;i++) {
if(restore_one(&repo,&files.v[i],force,&restored,
&skipped)<0)
goto done;
}
} else {
after_dd=false;
for(int i=3;i<argc;i++) {
const struct mesh_file_entry*entry;
if(!after_dd&&strcmp(argv[i],"--force")==0)
continue;
if(!after_dd&&strcmp(argv[i],"--")==0) {
after_dd=true;
continue;
}
entry=manifest_find(&files,argv[i]);
if(!entry) {
fprintf(stderr,"mesh: restore: path not in snapshot: %s\n",
argv[i]);
errno=ENOENT;
goto done;
}
if(restore_one(&repo,entry,force,&restored,&skipped)<0)
goto done;
}
}
struct mesh_status status;
if(monitor_collect(&repo,&status,"restore")==0) {
snprintf(status.message, sizeof(status.message),
"restored %zu path(s), skipped %zu", restored, skipped);
monitor_write(&repo,&status);
} else {
fprintf(stderr,
"mesh: restore: warning: cannot refresh status: %s\n",
strerror(errno));
}
printf("restored %zu path(s), skipped %zu already current\n",
restored, skipped);
rc=0;
done:
file_list_free(&files);
graph_free(&graph);
if(lock_fd>=0)
close(lock_fd);
return rc;
}
static int branch_usage(void) {
fprintf(stderr,
"usage: mesh branch [list]\n"
"       mesh branch create <name> [snapshot]\n"
"       mesh branch switch <name>\n"
"       mesh branch delete <name>\n");
return 2;
}
static int cmd_branch(int argc, char**argv) {
struct mesh_repo repo;
if(argc==2) {
if(repo_required(&repo)<0)
return 1;
return branch_list(&repo,stdout)<0?1:0;
}
if(argc==3&&strcmp(argv[2],"list")==0) {
if(repo_required(&repo)<0)
return 1;
return branch_list(&repo,stdout)<0?1:0;
}
if(strcmp(argv[2],"create")==0||strcmp(argv[2],"new")==0) {
if(argc<4||argc>5)
return branch_usage();
if(repo_required(&repo)<0)
return 1;
if(branch_create(&repo,argv[3],argc==5?argv[4]:NULL)<0) {
fprintf(stderr,"mesh: branch: %s\n", strerror(errno));
return 1;
}
return 0;
}
if(strcmp(argv[2],"switch")==0||strcmp(argv[2],"checkout")==0) {
if(argc!=4)
return branch_usage();
if(repo_required(&repo)<0)
return 1;
if(branch_switch(&repo,argv[3])<0) {
fprintf(stderr,"mesh: branch: %s\n", strerror(errno));
return 1;
}
return 0;
}
if(strcmp(argv[2],"delete")==0||strcmp(argv[2],"remove")==0||
strcmp(argv[2],"-d")==0) {
if(argc!=4)
return branch_usage();
if(repo_required(&repo)<0)
return 1;
if(branch_delete(&repo,argv[3])<0) {
if(errno==EBUSY)
fprintf(stderr,
"mesh: branch: cannot delete the current branch '%s'\n",
argv[3]);
else
fprintf(stderr,"mesh: branch: %s\n", strerror(errno));
return 1;
}
return 0;
}
return branch_usage();
}
static int tag_usage(void) {
fprintf(stderr,
"usage: mesh tag\n"
"       mesh tag <name> [snapshot]\n"
"       mesh tag -d <name>\n");
return 2;
}
static int cmd_tag(int argc, char**argv) {
struct mesh_repo repo;
if(argc==2||(argc==3&&strcmp(argv[2],"list")==0)) {
if(repo_required(&repo)<0)
return 1;
return tag_list(&repo,stdout)<0?1:0;
}
if(strcmp(argv[2],"-d")==0||strcmp(argv[2],"--delete")==0) {
if(argc!=4)
return tag_usage();
if(repo_required(&repo)<0)
return 1;
if(tag_delete(&repo,argv[3])<0) {
fprintf(stderr,"mesh: tag: %s\n", strerror(errno));
return 1;
}
return 0;
}
if(argc<3||argc>4)
return tag_usage();
if(repo_required(&repo)<0)
return 1;
if(tag_create(&repo,argv[2],argc==4?argv[3]:NULL)<0) {
fprintf(stderr,"mesh: tag: %s\n", strerror(errno));
return 1;
}
return 0;
}
static int cmd_reflog(int argc, char**argv) {
struct mesh_repo repo;
(void)argv;
if(argc>2) {
fprintf(stderr,"usage: mesh reflog\n");
return 2;
}
if(repo_required(&repo)<0)
return 1;
return reflog_print(&repo,stdout)<0?1:0;
}
static int cmd_ignore(int argc, char**argv) {
struct mesh_repo repo;
(void)argv;
if(argc>2) {
fprintf(stderr,"usage: mesh ignore\n");
return 2;
}
if(repo_required(&repo)<0)
return 1;
return ignore_list(&repo,stdout)<0?1:0;
}
static int cmd_clean(int argc, char**argv) {
struct mesh_repo repo;
bool dry=false, force=false, include_ignored=false;
size_t removed=0, failed=0;
uint64_t bytes=0;
char pretty[32];
for(int i=2;i<argc;i++) {
if(strcmp(argv[i],"-n")==0||strcmp(argv[i],"--dry-run")==0)
dry=true;
else if(strcmp(argv[i],"-f")==0||strcmp(argv[i],"--force")==0)
force=true;
else if(strcmp(argv[i],"-x")==0||strcmp(argv[i],"--ignored")==0)
include_ignored=true;
else {
fprintf(stderr,
"usage: mesh clean (-n|--dry-run|-f|--force) [-x|--ignored]\n");
return 2;
}
}
if(dry==force) {
fprintf(stderr,
"usage: mesh clean (-n|--dry-run|-f|--force) [-x|--ignored]\n");
return 2;
}
if(repo_required(&repo)<0)
return 1;
if(clean_run(&repo,force,include_ignored,&removed,&bytes,&failed)<0) {
fprintf(stderr,"mesh: clean: %s\n", strerror(errno));
return 1;
}
mesh_format_bytes(bytes, pretty, sizeof(pretty));
printf("%s %zu untracked file(s) (%s)\n",
dry?"would remove":"removed", removed, pretty);
if(failed>0) {
fprintf(stderr,"mesh: clean: %zu file(s) could not be removed\n",
failed);
return 1;
}
return 0;
}
#define OBTAIN_MAX_HEADERS 32u
struct obtain_options {
const char*url;
const char*dest;
const char*ref;
const char*archive;
const char*output;
const char*output_dir;
const char*user_agent;
const char*referer;
const char*cookie;
const char*cookie_file;
const char*cookie_jar;
const char*proxy;
const char*proxy_user;
const char*noproxy;
const char*write_out;
const char*header[OBTAIN_MAX_HEADERS+1];
size_t header_count;
long max_time;
long connect_timeout;
uint64_t limit_rate;
uint64_t resume_from;
bool file;
bool repo;
bool help;
bool remote_name;
bool location;
bool silent;
bool compressed;
bool create_dirs;
bool resume;
bool resume_from_set;
bool head;
bool insecure;
bool fail;
};
static void obtain_usage(FILE*out) {
fprintf(out,"usage: mesh obtain [options] URL [DEST]\n");
}
static void obtain_help(FILE*out,bool fetch_alias) {
if(fetch_alias) {
fprintf(out,
"usage: mesh fetch [options] URL [DEST]\n"
"\n"
"Deprecated alias for 'mesh obtain --file'.\n"
"\n"
"Download options:\n");
} else {
fprintf(out,
"usage: mesh obtain [options] URL [DEST]\n"
"\n"
"Repository mode (forge archive import):\n"
"  mesh obtain https://github.com/OWNER/REPO [DIR]\n"
"  mesh obtain --ref REF https://gitlab.com/GROUP/PROJECT [DIR]\n"
"  mesh obtain --archive URL https://forge.example/OWNER/REPO [DIR]\n"
"\n"
"File mode (wget/curl):\n"
"  mesh obtain https://example.com/file.bin [-o FILE]\n"
"  mesh obtain -O https://example.com/file.bin\n"
"  mesh obtain --file https://example.com/download [-o FILE]\n"
"  mesh obtain -o - https://example.com/file.bin > file.bin\n"
"  mesh obtain -I https://example.com/file.bin\n"
"\n"
"Repository options:\n"
"  --ref REF        Branch, tag or revision for repository mode.\n"
"  --archive URL    Tarball to use instead of a forge endpoint.\n"
"  --repo           Force repository mode for an ambiguous URL.\n"
"  --file           Force file mode for an ambiguous URL.\n"
"\n"
"Download options:\n");
}
fprintf(out,
"  -o, --output FILE\n"
"                   Destination; '-' streams the body to stdout.\n"
"  -O, --remote-name\n"
"                   Derive the destination from the URL instead of DEST.\n"
"  --output-dir DIR Store the output under DIR.\n"
"  -I, --head       Fetch response headers only (writes them to the output).\n"
"  -L, --location   Follow HTTP redirects (HTTP/HTTPS only).\n"
"  -C -, --continue-at -\n"
"                   Resume from the current destination size.\n"
"  -C N, --continue-at N\n"
"                   Truncate the existing file to N bytes, then resume.\n"
"  -H, --header LINE\n"
"                   Add an HTTP request header; may be repeated.\n"
"  -A, --user-agent STRING\n"
"                   Override the HTTP User-Agent.\n"
"  -e, --referer URL\n"
"                   Set the HTTP Referer header.\n"
"  -b, --cookie DATA|FILE\n"
"                   Send cookies, or read a cookie file when DATA has no '='.\n"
"  -c, --cookie-jar FILE\n"
"                   Write cookies received during the transfer to FILE.\n"
"  -x, --proxy URL  Use an HTTP proxy (also honours proxy environment vars).\n"
"  -U, --proxy-user USER:PASSWORD\n"
"                   Proxy credentials.\n"
"  --noproxy LIST   Comma-separated hosts that bypass the proxy.\n"
"  -k, --insecure   Disable TLS certificate verification (dangerous).\n"
"  --compressed     Request and decode compressed responses.\n"
"  --create-dirs    Create missing destination parent directories.\n"
"  --max-time SEC   Limit the whole transfer to SEC seconds.\n"
"  --connect-timeout SEC\n"
"                   Limit connection setup to SEC seconds.\n"
"  --limit-rate RATE\n"
"                   Cap download speed (bytes; K, M and G suffixes).\n"
"  --url URL        Provide the URL as an option instead of positionally.\n"
"  -w, --write-out FORMAT\n"
"                   Print transfer metrics after the transfer. Supports\n"
"                   %%{http_code}, %%{size_download}, %%{time_total},\n"
"                   %%{time_connect}, %%{time_starttransfer}, %%{url_effective},\n"
"                   %%{filename_effective}, %%{remote_ip}, %%{num_redirects},\n"
"                   %%{content_type}, %%{exitcode} and %%{errormsg}.\n"
"  -f, --fail       Fail on HTTP errors; with -I it also rejects error\n"
"                   responses after printing their headers.\n"
"  -s, --silent     Suppress successful file download output.\n"
"  -S, --show-error Accepted for curl compatibility; errors are always shown.\n"
"  -h, --help       Show this help.\n");
}
static int obtain_positional(struct obtain_options*opt,const char*arg) {
if(!opt->url) {
opt->url=arg;
return 0;
}
if(!opt->dest) {
opt->dest=arg;
return 0;
}
obtain_usage(stderr);
return -1;
}
static int obtain_take_value(int argc,char**argv,int*index,
const char**value) {
if(*index+1>=argc) {
obtain_usage(stderr);
return -1;
}
*value=argv[++(*index)];
return 0;
}
static bool obtain_text_ok(const char*text) {
if(!text)
return false;
for(const unsigned char*p=(const unsigned char*)text;*p;p++)
if(*p<0x20||*p==0x7f)
return false;
return true;
}
static int obtain_add_header(struct obtain_options*opt,const char*value) {
if(!value||value[0]=='\0'||!obtain_text_ok(value)) {
fprintf(stderr,"mesh: obtain: invalid HTTP header\n");
return -1;
}
if(opt->header_count>=OBTAIN_MAX_HEADERS) {
fprintf(stderr,"mesh: obtain: too many HTTP headers (max %u)\n",
OBTAIN_MAX_HEADERS);
return -1;
}
opt->header[opt->header_count++]=value;
opt->header[opt->header_count]=NULL;
return 0;
}
static int obtain_seconds(const char*text,long*out) {
char*end=NULL;
long value;
if(!text||text[0]=='\0'||text[0]=='-') {
fprintf(stderr,"mesh: obtain: invalid time value '%s'\n",
text?text:"");
return -1;
}
errno=0;
value=strtol(text,&end,10);
if(errno!=0||end==text||*end!='\0'||value<0) {
fprintf(stderr,"mesh: obtain: invalid time value '%s'\n",text);
return -1;
}
*out=value;
return 0;
}
static int obtain_rate(const char*text,uint64_t*out) {
char*end=NULL;
uint64_t value, multiplier=1;
if(!text||text[0]=='\0'||text[0]=='-') {
fprintf(stderr,"mesh: obtain: invalid rate '%s'\n",text?text:"");
return -1;
}
errno=0;
value=strtoull(text,&end,10);
if(errno!=0||end==text) {
fprintf(stderr,"mesh: obtain: invalid rate '%s'\n",text);
return -1;
}
if(*end=='k'||*end=='K') {
multiplier=1024;
end++;
} else if(*end=='m'||*end=='M') {
multiplier=1024u*1024u;
end++;
} else if(*end=='g'||*end=='G') {
multiplier=1024u*1024u*1024u;
end++;
}
if(*end!='\0'||value>UINT64_MAX/multiplier) {
fprintf(stderr,"mesh: obtain: invalid rate '%s'\n",text);
return -1;
}
*out=value*multiplier;
return 0;
}
static int obtain_offset(const char*text,uint64_t*out) {
char*end=NULL;
unsigned long long value;
if(!text||text[0]=='\0'||text[0]<'0'||text[0]>'9') {
fprintf(stderr,"mesh: obtain: invalid offset '%s'\n",text?text:"");
return -1;
}
errno=0;
value=strtoull(text,&end,10);
if(errno!=0||end==text||*end!='\0') {
fprintf(stderr,"mesh: obtain: invalid offset '%s'\n",text);
return -1;
}
*out=(uint64_t)value;
return 0;
}
static int obtain_cookie(struct obtain_options*opt,const char*value) {
if(!value||value[0]=='\0'||!obtain_text_ok(value)) {
fprintf(stderr,"mesh: obtain: invalid cookie\n");
return -1;
}
if(strchr(value,'='))
opt->cookie=value;
else
opt->cookie_file=value;
return 0;
}
static int obtain_flag(const char*name,const char*value,bool*out) {
if(value) {
fprintf(stderr,"mesh: obtain: %s does not take a value\n",name);
return -1;
}
*out=true;
return 0;
}
static int obtain_ignored(const char*name,const char*value) {
if(value) {
fprintf(stderr,"mesh: obtain: %s does not take a value\n",name);
return -1;
}
return 0;
}
static int obtain_long(struct obtain_options*opt,int argc,char**argv,
int*index) {
const char*arg=argv[*index];
const char*eq=strchr(arg,'=');
const char*value=NULL;
char name[64];
size_t len=eq?(size_t)(eq-arg):strlen(arg);
if(len==0||len>=sizeof(name)) {
fprintf(stderr,"mesh: obtain: invalid option '%s'\n",arg);
return -1;
}
memcpy(name,arg,len);
name[len]='\0';
if(eq)
value=eq+1;
if(strcmp(name,"--ref")==0) {
if(!value&&obtain_take_value(argc,argv,index,&value)<0)
return -1;
opt->ref=value;
return 0;
}
if(strcmp(name,"--archive")==0) {
if(!value&&obtain_take_value(argc,argv,index,&value)<0)
return -1;
opt->archive=value;
return 0;
}
if(strcmp(name,"--output")==0) {
if(!value&&obtain_take_value(argc,argv,index,&value)<0)
return -1;
opt->output=value;
return 0;
}
if(strcmp(name,"--output-dir")==0) {
if(!value&&obtain_take_value(argc,argv,index,&value)<0)
return -1;
opt->output_dir=value;
return 0;
}
if(strcmp(name,"--header")==0) {
if(!value&&obtain_take_value(argc,argv,index,&value)<0)
return -1;
return obtain_add_header(opt,value);
}
if(strcmp(name,"--user-agent")==0) {
if(!value&&obtain_take_value(argc,argv,index,&value)<0)
return -1;
if(value[0]=='\0'||!obtain_text_ok(value)) {
fprintf(stderr,"mesh: obtain: invalid user agent\n");
return -1;
}
opt->user_agent=value;
return 0;
}
if(strcmp(name,"--referer")==0) {
if(!value&&obtain_take_value(argc,argv,index,&value)<0)
return -1;
if(value[0]=='\0'||!obtain_text_ok(value)) {
fprintf(stderr,"mesh: obtain: invalid referer\n");
return -1;
}
opt->referer=value;
return 0;
}
if(strcmp(name,"--max-time")==0) {
if(!value&&obtain_take_value(argc,argv,index,&value)<0)
return -1;
return obtain_seconds(value,&opt->max_time);
}
if(strcmp(name,"--connect-timeout")==0) {
if(!value&&obtain_take_value(argc,argv,index,&value)<0)
return -1;
return obtain_seconds(value,&opt->connect_timeout);
}
if(strcmp(name,"--limit-rate")==0) {
if(!value&&obtain_take_value(argc,argv,index,&value)<0)
return -1;
return obtain_rate(value,&opt->limit_rate);
}
if(strcmp(name,"--url")==0) {
if(opt->url) {
fprintf(stderr,"mesh: obtain: multiple URLs are not supported\n");
return -1;
}
if(!value&&obtain_take_value(argc,argv,index,&value)<0)
return -1;
opt->url=value;
return 0;
}
if(strcmp(name,"--continue-at")==0) {
if(!value&&obtain_take_value(argc,argv,index,&value)<0)
return -1;
if(strcmp(value,"-")==0) {
opt->resume=true;
return 0;
}
if(obtain_offset(value,&opt->resume_from)<0)
return -1;
opt->resume=true;
opt->resume_from_set=true;
return 0;
}
if(strcmp(name,"--cookie")==0) {
if(!value&&obtain_take_value(argc,argv,index,&value)<0)
return -1;
return obtain_cookie(opt,value);
}
if(strcmp(name,"--cookie-jar")==0) {
if(!value&&obtain_take_value(argc,argv,index,&value)<0)
return -1;
if(value[0]=='\0'||!obtain_text_ok(value)) {
fprintf(stderr,"mesh: obtain: invalid cookie jar\n");
return -1;
}
opt->cookie_jar=value;
return 0;
}
if(strcmp(name,"--proxy")==0) {
if(!value&&obtain_take_value(argc,argv,index,&value)<0)
return -1;
if(!obtain_text_ok(value)) {
fprintf(stderr,"mesh: obtain: invalid proxy\n");
return -1;
}
opt->proxy=value;
return 0;
}
if(strcmp(name,"--proxy-user")==0) {
if(!value&&obtain_take_value(argc,argv,index,&value)<0)
return -1;
if(!obtain_text_ok(value)) {
fprintf(stderr,"mesh: obtain: invalid proxy user\n");
return -1;
}
opt->proxy_user=value;
return 0;
}
if(strcmp(name,"--noproxy")==0) {
if(!value&&obtain_take_value(argc,argv,index,&value)<0)
return -1;
if(!obtain_text_ok(value)) {
fprintf(stderr,"mesh: obtain: invalid no-proxy list\n");
return -1;
}
opt->noproxy=value;
return 0;
}
if(strcmp(name,"--write-out")==0) {
if(!value&&obtain_take_value(argc,argv,index,&value)<0)
return -1;
if(!obtain_text_ok(value)) {
fprintf(stderr,"mesh: obtain: invalid write-out format\n");
return -1;
}
opt->write_out=value;
return 0;
}
if(strcmp(name,"--file")==0)
return obtain_flag(name,value,&opt->file);
if(strcmp(name,"--repo")==0)
return obtain_flag(name,value,&opt->repo);
if(strcmp(name,"--remote-name")==0)
return obtain_flag(name,value,&opt->remote_name);
if(strcmp(name,"--location")==0)
return obtain_flag(name,value,&opt->location);
if(strcmp(name,"--head")==0)
return obtain_flag(name,value,&opt->head);
if(strcmp(name,"--insecure")==0)
return obtain_flag(name,value,&opt->insecure);
if(strcmp(name,"--silent")==0)
return obtain_flag(name,value,&opt->silent);
if(strcmp(name,"--compressed")==0)
return obtain_flag(name,value,&opt->compressed);
if(strcmp(name,"--create-dirs")==0)
return obtain_flag(name,value,&opt->create_dirs);
if(strcmp(name,"--help")==0)
return obtain_flag(name,value,&opt->help);
if(strcmp(name,"--fail")==0)
return obtain_flag(name,value,&opt->fail);
if(strcmp(name,"--show-error")==0)
return obtain_ignored(name,value);
fprintf(stderr,"mesh: obtain: unknown option '%s'\n",arg);
return -1;
}
static int obtain_short(struct obtain_options*opt,int argc,char**argv,
int*index) {
const char*arg=argv[*index];
for(size_t j=1;arg[j]!='\0';j++) {
char c=arg[j];
const char*value=NULL;
if(c=='o'||c=='H'||c=='A'||c=='e'||c=='C'||c=='b'||c=='c'||
c=='x'||c=='U'||c=='w') {
if(arg[j+1]!='\0') {
value=arg+j+1;
j=strlen(arg)-1;
} else if(obtain_take_value(argc,argv,index,&value)<0) {
return -1;
}
}
if(c=='o') {
opt->output=value;
continue;
}
if(c=='H') {
if(obtain_add_header(opt,value)<0)
return -1;
continue;
}
if(c=='A') {
if(value[0]=='\0'||!obtain_text_ok(value)) {
fprintf(stderr,"mesh: obtain: invalid user agent\n");
return -1;
}
opt->user_agent=value;
continue;
}
if(c=='e') {
if(value[0]=='\0'||!obtain_text_ok(value)) {
fprintf(stderr,"mesh: obtain: invalid referer\n");
return -1;
}
opt->referer=value;
continue;
}
if(c=='C') {
if(strcmp(value,"-")==0) {
opt->resume=true;
continue;
}
if(obtain_offset(value,&opt->resume_from)<0)
return -1;
opt->resume=true;
opt->resume_from_set=true;
continue;
}
if(c=='b') {
if(obtain_cookie(opt,value)<0)
return -1;
continue;
}
if(c=='c') {
if(value[0]=='\0'||!obtain_text_ok(value)) {
fprintf(stderr,"mesh: obtain: invalid cookie jar\n");
return -1;
}
opt->cookie_jar=value;
continue;
}
if(c=='x') {
if(!obtain_text_ok(value)) {
fprintf(stderr,"mesh: obtain: invalid proxy\n");
return -1;
}
opt->proxy=value;
continue;
}
if(c=='U') {
if(!obtain_text_ok(value)) {
fprintf(stderr,"mesh: obtain: invalid proxy user\n");
return -1;
}
opt->proxy_user=value;
continue;
}
if(c=='w') {
if(!obtain_text_ok(value)) {
fprintf(stderr,"mesh: obtain: invalid write-out format\n");
return -1;
}
opt->write_out=value;
continue;
}
if(c=='h') {
opt->help=true;
continue;
}
if(c=='O') {
opt->remote_name=true;
continue;
}
if(c=='L') {
opt->location=true;
continue;
}
if(c=='I') {
opt->head=true;
continue;
}
if(c=='k') {
opt->insecure=true;
continue;
}
if(c=='s') {
opt->silent=true;
continue;
}
if(c=='f') {
opt->fail=true;
continue;
}
if(c=='S')
continue;
fprintf(stderr,"mesh: obtain: unknown option '-%c'\n",c);
return -1;
}
return 0;
}
static int obtain_parse(int argc,char**argv,bool force_file,
struct obtain_options*opt) {
memset(opt, 0, sizeof(*opt));
for(int i=2;i<argc;i++) {
const char*arg=argv[i];
if(strcmp(arg,"--")==0) {
for(i++;i<argc;i++)
if(obtain_positional(opt,argv[i])<0)
return -1;
break;
}
if(arg[0]=='-'&&arg[1]=='-'&&arg[2]!='\0') {
if(obtain_long(opt,argc,argv,&i)<0)
return -1;
continue;
}
if(arg[0]=='-'&&arg[1]!='\0') {
if(obtain_short(opt,argc,argv,&i)<0)
return -1;
continue;
}
if(obtain_positional(opt,arg)<0)
return -1;
}
if(force_file)
opt->file=true;
if(opt->help)
return 0;
if(!opt->url||opt->url[0]=='\0') {
obtain_usage(stderr);
return -1;
}
bool repo_mode=opt->repo||opt->ref||opt->archive;
if(opt->file&&opt->repo) {
fprintf(stderr,"mesh: obtain: --file and --repo are exclusive\n");
return -1;
}
if(opt->file&&(opt->ref||opt->archive)) {
fprintf(stderr,"mesh: obtain: --file cannot be combined with "
"--ref or --archive\n");
return -1;
}
if(opt->head) {
if(repo_mode) {
fprintf(stderr,"mesh: obtain: --head is only valid for downloads\n");
return -1;
}
if(opt->resume||opt->resume_from_set) {
fprintf(stderr,"mesh: obtain: --head conflicts with --continue-at\n");
return -1;
}
}
if(repo_mode&&opt->write_out) {
fprintf(stderr,"mesh: obtain: --write-out is only valid for downloads\n");
return -1;
}
const char*explicit_dest=opt->output?opt->output:opt->dest;
if(explicit_dest&&strcmp(explicit_dest,"-")==0&&
(opt->resume||opt->resume_from_set)) {
fprintf(stderr,"mesh: obtain: --continue-at cannot be used with stdout\n");
return -1;
}
if(repo_mode&&opt->output) {
fprintf(stderr,"mesh: obtain: --output is only valid for downloads\n");
return -1;
}
if(repo_mode&&opt->output_dir) {
fprintf(stderr,"mesh: obtain: --output-dir is only valid for downloads\n");
return -1;
}
if(repo_mode&&opt->remote_name) {
fprintf(stderr,"mesh: obtain: --remote-name is only valid for downloads\n");
return -1;
}
if(repo_mode&&opt->resume) {
fprintf(stderr,"mesh: obtain: --continue-at is only valid for downloads\n");
return -1;
}
if(repo_mode&&opt->create_dirs) {
fprintf(stderr,"mesh: obtain: --create-dirs is only valid for downloads\n");
return -1;
}
if(opt->output&&opt->dest) {
fprintf(stderr,"mesh: obtain: use either --output or DEST, not both\n");
return -1;
}
if(opt->remote_name&&(opt->output||opt->dest)) {
fprintf(stderr,"mesh: obtain: --remote-name conflicts with an explicit "
"destination\n");
return -1;
}
return 0;
}
static int obtain_output_path(const struct obtain_options*opt,const char*url,
char*out,size_t cap,bool allow_stdout) {
char base[MESH_PATH_MAX];
const char*leaf;
const char*explicit_dest=opt->output?opt->output:opt->dest;
if(!out||cap==0) {
errno=EINVAL;
return -1;
}
if(explicit_dest) {
if(explicit_dest[0]=='\0'||strlen(explicit_dest)>=sizeof(base)||
!obtain_text_ok(explicit_dest)) {
fprintf(stderr,"mesh: obtain: invalid output path\n");
errno=EINVAL;
return -1;
}
snprintf(base, sizeof(base),"%s", explicit_dest);
} else if(url_basename(url, base, sizeof(base))<0) {
fprintf(stderr,"mesh: obtain: cannot derive output name\n");
return -1;
}
if(strcmp(base,"-")==0) {
if(!allow_stdout) {
fprintf(stderr,"mesh: obtain: writing the body to stdout is not "
"supported here\n");
errno=EINVAL;
return -1;
}
if(cap<2) {
errno=ENAMETOOLONG;
return -1;
}
snprintf(out, cap,"%s", base);
return 0;
}
if(base[0]=='\0'||base[strlen(base)-1]=='/') {
fprintf(stderr,"mesh: obtain: invalid output path\n");
errno=EINVAL;
return -1;
}
if(opt->output_dir) {
if(opt->output_dir[0]=='\0'||strlen(opt->output_dir)>=sizeof(base)||
!obtain_text_ok(opt->output_dir)) {
fprintf(stderr,"mesh: obtain: invalid output directory\n");
errno=EINVAL;
return -1;
}
leaf=strrchr(base,'/');
leaf=leaf?leaf+1:base;
if(leaf[0]=='\0'||strcmp(leaf,".")==0||strcmp(leaf,"..")==0) {
fprintf(stderr,"mesh: obtain: invalid output path\n");
errno=EINVAL;
return -1;
}
if(mesh_path_join(out, cap, opt->output_dir, leaf)<0) {
fprintf(stderr,"mesh: obtain: output path is too long\n");
return -1;
}
} else {
if(strlen(base)>=cap) {
fprintf(stderr,"mesh: obtain: output path is too long\n");
errno=ENAMETOOLONG;
return -1;
}
snprintf(out, cap,"%s", base);
}
return 0;
}
static int obtain_create_parent(const char*path) {
char parent[MESH_PATH_MAX];
const char*slash=strrchr(path,'/');
size_t len;
if(!slash||slash==path)
return 0;
len=(size_t)(slash-path);
if(len>=sizeof(parent)) {
errno=ENAMETOOLONG;
return -1;
}
memcpy(parent, path, len);
parent[len]='\0';
return mesh_mkdir_p(parent);
}
static void obtain_write_out(const char*format,
const struct mesh_transfer_stats*stats,const char*url,const char*filename,
int exitcode,const char*errmsg) {
char name[64], number[64];
if(!format)
return;
for(const char*p=format;*p;p++) {
if(*p=='\\'&&p[1]) {
p++;
if(*p=='n')
fputc('\n',stdout);
else if(*p=='r')
fputc('\r',stdout);
else if(*p=='t')
fputc('\t',stdout);
else
fputc((unsigned char)*p,stdout);
continue;
}
if(*p=='%'&&p[1]=='%') {
fputc('%',stdout);
p++;
continue;
}
if(*p=='%'&&p[1]=='{') {
const char*close=strchr(p+2,'}');
size_t len;
const char*value=NULL;
if(!close) {
fputc('%',stdout);
continue;
}
len=(size_t)(close-(p+2));
if(len<sizeof(name)) {
memcpy(name, p+2, len);
name[len]='\0';
} else {
name[0]='\0';
}
if(strcmp(name,"http_code")==0||strcmp(name,"response_code")==0) {
snprintf(number, sizeof(number), "%03ld",
stats?stats->response_code:0L);
value=number;
} else if(strcmp(name,"size_download")==0) {
snprintf(number, sizeof(number), "%"PRIu64,
stats?stats->bytes:0);
value=number;
} else if(strcmp(name,"time_total")==0) {
snprintf(number, sizeof(number), "%.6f",
stats?stats->time_total:0.0);
value=number;
} else if(strcmp(name,"time_connect")==0) {
snprintf(number, sizeof(number), "%.6f",
stats?stats->time_connect:0.0);
value=number;
} else if(strcmp(name,"time_starttransfer")==0) {
snprintf(number, sizeof(number), "%.6f",
stats?stats->time_starttransfer:0.0);
value=number;
} else if(strcmp(name,"url_effective")==0) {
value=(stats&&stats->url_effective[0])?stats->url_effective:
(url?url:"");
} else if(strcmp(name,"filename_effective")==0) {
value=filename?filename:"-";
} else if(strcmp(name,"remote_ip")==0) {
value=stats?stats->remote_ip:"";
} else if(strcmp(name,"num_redirects")==0) {
snprintf(number, sizeof(number), "%ld",
stats?stats->redirects:0L);
value=number;
} else if(strcmp(name,"content_type")==0) {
value=stats?stats->content_type:"";
} else if(strcmp(name,"exitcode")==0) {
snprintf(number, sizeof(number), "%d", exitcode);
value=number;
} else if(strcmp(name,"errormsg")==0) {
value=errmsg?errmsg:"";
}
if(value)
fputs(value, stdout);
p=close;
continue;
}
fputc((unsigned char)*p,stdout);
}
}
static void obtain_download_options(const struct obtain_options*opt,
struct mesh_download_options*download,
struct mesh_transfer_stats*stats) {
memset(download, 0, sizeof(*download));
download->headers=opt->header;
download->user_agent=opt->user_agent;
download->referer=opt->referer;
download->cookie=opt->cookie;
download->cookie_file=opt->cookie_file;
download->cookie_jar=opt->cookie_jar;
download->proxy=opt->proxy;
download->proxy_user=opt->proxy_user;
download->noproxy=opt->noproxy;
download->stats=stats;
download->connect_timeout=opt->connect_timeout;
download->max_time=opt->max_time;
download->max_recv_speed=opt->limit_rate;
download->resume_from=opt->resume_from;
download->resume_from_set=opt->resume_from_set;
download->follow_redirects=opt->location;
download->compressed=opt->compressed;
download->insecure=opt->insecure;
download->fail=opt->fail;
}
static int cmd_obtain_file(const struct obtain_options*opt) {
struct mesh_repo repo;
struct mesh_status status;
struct mesh_download_options download;
struct mesh_transfer_stats stats;
char dest[MESH_PATH_MAX], hash[MESH_HASH_HEX+1], pretty[32];
uint64_t bytes=0, resumed=0;
bool have_repo;
int rc;
have_repo=mesh_repo_open(&repo)==0;
if(obtain_output_path(opt,opt->url,dest,sizeof(dest),false)<0)
return 2;
if(opt->create_dirs&&obtain_create_parent(dest)<0) {
fprintf(stderr,"mesh: obtain: cannot create output directory: %s\n",
strerror(errno));
return 1;
}
warn_plain_http("obtain", opt->url);
/* File downloads never carry repository credentials. */
obtain_download_options(opt,&download,&stats);
rc=net_download_ex(NULL,opt->url,dest,&download,&bytes,&resumed,hash);
if(rc<0) {
int saved=errno;
fprintf(stderr,"mesh: obtain: transfer failed: %s\n",
strerror(saved));
if(opt->write_out)
obtain_write_out(opt->write_out,&stats,opt->url,dest,1,
strerror(saved));
return 1;
}
if(!opt->silent) {
FILE*meta=opt->write_out?stderr:stdout;
mesh_format_bytes(bytes, pretty, sizeof(pretty));
fprintf(meta,"obtained %s\n", dest);
fprintf(meta,"  size: %s", pretty);
if(resumed>0)
fprintf(meta,"   resumed from: %"PRIu64 " bytes", resumed);
fprintf(meta,"\n  blake2b: %s\n", hash);
}
if(opt->write_out)
obtain_write_out(opt->write_out,&stats,opt->url,dest,0,NULL);
if(have_repo&&monitor_collect(&repo,&status,"obtain")==0) {
snprintf(status.message, sizeof(status.message),
"downloaded %.400s", dest);
mesh_strlcpy(status.last_path, sizeof(status.last_path), dest);
mesh_strlcpy(status.download_hash, sizeof(status.download_hash),
hash);
status.download_bytes=bytes;
status.resumed_from=resumed;
if(monitor_write(&repo,&status)<0)
fprintf(stderr,
"mesh: obtain: warning: status write failed: %s\n",
strerror(errno));
}
return 0;
}
static int cmd_obtain_stream(const struct obtain_options*opt) {
struct mesh_repo repo;
struct mesh_status status;
struct mesh_download_options download;
struct mesh_transfer_stats stats;
char hash[MESH_HASH_HEX+1], pretty[32];
uint64_t bytes=0;
bool have_repo;
int rc;
have_repo=mesh_repo_open(&repo)==0;
warn_plain_http("obtain", opt->url);
obtain_download_options(opt,&download,&stats);
rc=net_stream_ex(NULL,opt->url,&download,&bytes,hash);
if(rc<0) {
int saved=errno;
fprintf(stderr,"mesh: obtain: transfer failed: %s\n",
strerror(saved));
if(opt->write_out)
obtain_write_out(opt->write_out,&stats,opt->url,"-",1,
strerror(saved));
return 1;
}
if(!opt->silent) {
mesh_format_bytes(bytes, pretty, sizeof(pretty));
fprintf(stderr,"obtained stdout\n");
fprintf(stderr,"  size: %s\n", pretty);
fprintf(stderr,"  blake2b: %s\n", hash);
}
if(opt->write_out)
obtain_write_out(opt->write_out,&stats,opt->url,"-",0,NULL);
if(have_repo&&monitor_collect(&repo,&status,"obtain")==0) {
snprintf(status.message, sizeof(status.message),
"streamed %.400s", opt->url);
mesh_strlcpy(status.last_path, sizeof(status.last_path), "-");
mesh_strlcpy(status.download_hash, sizeof(status.download_hash),
hash);
status.download_bytes=bytes;
status.resumed_from=0;
if(monitor_write(&repo,&status)<0)
fprintf(stderr,
"mesh: obtain: warning: status write failed: %s\n",
strerror(errno));
}
return 0;
}
static int cmd_obtain_head(const struct obtain_options*opt) {
struct mesh_download_options download;
struct mesh_transfer_stats stats;
char dest[MESH_PATH_MAX];
const char*filename="-";
FILE*out=stdout;
bool wants_file;
int fd=-1, rc;
dest[0]='\0';
wants_file=opt->output||opt->dest||opt->output_dir||opt->remote_name;
if(wants_file) {
if(obtain_output_path(opt,opt->url,dest,sizeof(dest),true)<0)
return 2;
if(strcmp(dest,"-")!=0) {
filename=dest;
if(opt->create_dirs&&obtain_create_parent(dest)<0) {
fprintf(stderr,
"mesh: obtain: cannot create output directory: %s\n",
strerror(errno));
return 1;
}
if(mesh_path_no_symlink(dest)<0) {
fprintf(stderr,"mesh: obtain: invalid output path: %s\n",
strerror(errno));
return 2;
}
fd=open(dest, O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,
0644);
if(fd<0) {
fprintf(stderr,"mesh: obtain: cannot open %s: %s\n",
dest, strerror(errno));
return 1;
}
out=fdopen(fd,"wb");
if(!out) {
int saved=errno;
close(fd);
fprintf(stderr,"mesh: obtain: cannot open %s: %s\n",
dest, strerror(saved));
return 1;
}
}
}
warn_plain_http("obtain", opt->url);
obtain_download_options(opt,&download,&stats);
rc=net_head_ex(NULL,opt->url,&download,out);
if(out!=stdout) {
if(fclose(out)!=0&&rc==0) {
fprintf(stderr,"mesh: obtain: cannot close %s: %s\n",
dest, strerror(errno));
rc=-1;
}
}
if(rc<0) {
int saved=errno;
fprintf(stderr,"mesh: obtain: transfer failed: %s\n",
strerror(saved));
if(opt->write_out)
obtain_write_out(opt->write_out,&stats,opt->url,filename,1,
strerror(saved));
return 1;
}
if(opt->write_out)
obtain_write_out(opt->write_out,&stats,opt->url,filename,0,NULL);
return 0;
}
static int cmd_obtain_repo(const struct obtain_options*opt) {
char planned[MESH_PATH_MAX];
char name[256], authority[MESH_PATH_MAX];
char stage[MESH_PATH_MAX], final[MESH_PATH_MAX];
char message[MESH_MSG_MAX];
char pretty[32];
struct mesh_download_options download;
uint64_t bytes=0;
int cwd_fd=-1;
int rc;
if(mesh_obtain_plan(opt->url, opt->ref, opt->archive, planned,
sizeof(planned), name, sizeof(name), authority,
sizeof(authority))<0) {
fprintf(stderr,"mesh: obtain: invalid source or reference\n");
return 2;
}
(void)name;
(void)authority;
warn_plain_http("obtain", planned);
obtain_download_options(opt,&download,NULL);
if(mesh_obtain_stage_ex(opt->url, opt->ref, opt->archive, opt->dest,
&download, stage, sizeof(stage), final, sizeof(final),
&bytes)<0) {
fprintf(stderr,"mesh: obtain: %s\n", strerror(errno));
return 1;
}
cwd_fd=open(".", O_RDONLY|O_DIRECTORY|O_CLOEXEC);
if(cwd_fd<0||chdir(stage)<0) {
fprintf(stderr,"mesh: obtain: cannot enter staging directory: %s\n",
strerror(errno));
if(cwd_fd>=0)
close(cwd_fd);
mesh_obtain_abort(stage);
return 1;
}
snprintf(message, sizeof(message),"obtain %s", opt->url);
char*save_argv[]={"mesh","save",message};
rc=cmd_save(3, save_argv);
if(fchdir(cwd_fd)<0)
fprintf(stderr,"mesh: obtain: warning: cannot restore the working "
"directory: %s\n", strerror(errno));
close(cwd_fd);
if(rc!=0) {
mesh_obtain_abort(stage);
return rc;
}
if(mesh_obtain_commit(stage, final)<0) {
fprintf(stderr,"mesh: obtain: cannot publish %s: %s\n",
final, strerror(errno));
mesh_obtain_abort(stage);
return 1;
}
if(!opt->silent) {
mesh_format_bytes(bytes, pretty, sizeof(pretty));
printf("obtained %s\n", final);
printf("  archive: %s (%s)\n", planned, pretty);
}
return 0;
}
static int cmd_obtain_mode(int argc,char**argv,bool force_file) {
struct obtain_options opt;
int looks;
if(obtain_parse(argc,argv,force_file,&opt)<0)
return 2;
if(opt.help) {
obtain_help(stdout,force_file);
return 0;
}
if(opt.insecure)
fprintf(stderr,"mesh: obtain: warning: TLS certificate verification "
"is disabled\n");
if(opt.head)
return cmd_obtain_head(&opt);
const char*explicit_dest=opt.output?opt.output:opt.dest;
if(explicit_dest&&strcmp(explicit_dest,"-")==0)
return cmd_obtain_stream(&opt);
if(opt.file||opt.output||opt.output_dir||opt.remote_name||opt.resume||
opt.resume_from_set||opt.create_dirs||opt.write_out)
return cmd_obtain_file(&opt);
if(opt.repo||opt.ref||opt.archive)
return cmd_obtain_repo(&opt);
looks=mesh_obtain_is_repo_url(opt.url);
if(looks<0) {
fprintf(stderr,"mesh: obtain: invalid source URL\n");
return 2;
}
if(looks)
return cmd_obtain_repo(&opt);
return cmd_obtain_file(&opt);
}
static int cmd_obtain(int argc,char**argv) {
return cmd_obtain_mode(argc,argv,false);
}
static int cmd_fetch(int argc,char**argv) {
return cmd_obtain_mode(argc,argv,true);
}
static int cmd_git_info(int argc,char**argv) {
struct mesh_repo repo;
(void)argv;
if(argc!=2) {
fprintf(stderr,"usage: mesh git-info\n");
return 2;
}
if(repo_required(&repo)<0)
return 1;
if(git_coexist_report(&repo)<0) {
fprintf(stderr,"mesh: git-info: %s\n",strerror(errno));
return 1;
}
return 0;
}
/* Router */
int cli_main(int argc, char**argv) {
if(argc<2) {
fputs(help_text, stdout);
return 0;
}
if(strcmp(argv[1],"-v")==0||strcmp(argv[1],"--version")==0) {
printf("Mesh "MESH_VERSION "\n");
return 0;
}
if(strcmp(argv[1],"-h")==0||strcmp(argv[1],"--help")==0) {
fputs(help_text, stdout);
return 0;
}
if(strcmp(argv[1],"start")==0)
return cmd_start(argc, argv);
if(strcmp(argv[1],"config")==0)
return cmd_config(argc, argv);
if(strcmp(argv[1],"branch")==0)
return cmd_branch(argc, argv);
if(strcmp(argv[1],"tag")==0)
return cmd_tag(argc, argv);
if(strcmp(argv[1],"reflog")==0)
return cmd_reflog(argc, argv);
if(strcmp(argv[1],"ignore")==0)
return cmd_ignore(argc, argv);
if(strcmp(argv[1],"clean")==0)
return cmd_clean(argc, argv);
if(strcmp(argv[1],"save")==0)
return cmd_save(argc, argv);
if(strcmp(argv[1],"log")==0)
return cmd_log(argc, argv);
if(strcmp(argv[1],"diff")==0)
return cmd_diff(argc, argv);
if(strcmp(argv[1],"restore")==0)
return cmd_restore(argc, argv);
if(strcmp(argv[1],"check")==0)
return cmd_check(argc, argv);
if(strcmp(argv[1],"choose")==0)
return cmd_choose(argc, argv);
if(strcmp(argv[1],"admin")==0)
return admin_main(argc, argv);
if(strcmp(argv[1],"obtain")==0)
return cmd_obtain(argc, argv);
if(strcmp(argv[1],"fetch")==0)
return cmd_fetch(argc, argv);
if(strcmp(argv[1],"auth")==0)
return cmd_auth(argc, argv);
if(strcmp(argv[1],"send")==0)
return cmd_send(argc, argv);
if(strcmp(argv[1],"sync")==0)
return cmd_sync(argc, argv);
if(strcmp(argv[1],"git-info")==0)
return cmd_git_info(argc,argv);
if(argv[1][0]=='-') {
fprintf(stderr,"mesh: unknown flag '%s'\n", argv[1]);
return 2;
}
fprintf(stderr,"mesh: unknown command '%s'\n", argv[1]);
fprintf(stderr,"Try 'mesh --help'.\n");
return 2;
}
