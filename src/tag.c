/*
* tag.c - lightweight tags pointing at immutable snapshots.
* A tag is one regular file under .mesh/refs/tags. The file contains the
* full snapshot hash, exactly like a branch ref. Tags are intentionally
* lightweight: Mesh history already stores the snapshot timestamp, author
* and message, so a second annotated object would duplicate them.
*/
#include "core.h"
static int tag_ref_path(const struct mesh_repo*repo, const char*name,
char*out, size_t cap) {
char rel[160];
int n;
if(!repo||!repo->valid||!out||cap==0||!mesh_ref_name_valid(name)) {
errno=EINVAL;
return -1;
}
n=snprintf(rel, sizeof(rel),"refs/tags/%s", name);
if(n<0||(size_t)n>=sizeof(rel)) {
errno=ENAMETOOLONG;
return -1;
}
return mesh_path_join(out, cap, repo->mesh_dir, rel);
}
static int tag_read_ref(const struct mesh_repo*repo, const char*name,
char out[MESH_HASH_HEX+1]) {
char path[MESH_PATH_MAX];
char*buf=NULL;
size_t len=0;
int rc=-1;
if(!out) {
errno=EINVAL;
return -1;
}
out[0]='\0';
if(tag_ref_path(repo,name,path,sizeof(path))<0)
return -1;
if(mesh_read_text(path,&buf,&len)<0)
return -1;
if(memchr(buf,'\0',len)!=NULL) {
errno=EINVAL;
goto done;
}
mesh_trim_newline(buf);
if(!mesh_hash_valid(buf)) {
errno=EINVAL;
goto done;
}
snprintf(out, MESH_HASH_HEX+1,"%s", buf);
rc=0;
done: {
int saved=errno;
free(buf);
errno=saved;
}
return rc;
}
static int tag_write_ref(const struct mesh_repo*repo, const char*name,
const char*hash) {
char path[MESH_PATH_MAX];
char dir[MESH_PATH_MAX];
char line[MESH_HASH_HEX+2];
int n;
if(!mesh_hash_valid(hash)||tag_ref_path(repo,name,path,sizeof(path))<0)
return -1;
if(mesh_path_join(dir, sizeof(dir), repo->mesh_dir,"refs/tags")<0)
return -1;
if(mesh_mkdir_p(dir)<0)
return -1;
n=snprintf(line, sizeof(line),"%s\n", hash);
if(n<0||(size_t)n>=sizeof(line)) {
errno=ENAMETOOLONG;
return -1;
}
return mesh_write_atomic(path, line, (size_t)n, 0644);
}
static int tag_target(const struct mesh_repo*repo, const char*target,
char out[MESH_HASH_HEX+1]) {
struct mesh_graph graph;
char head[MESH_HASH_HEX+1];
int rc=-1;
graph_init(&graph);
if(graph_head(repo,head)<0)
return -1;
if(graph_load(repo,&graph)<0)
goto done;
if(graph_resolve(&graph,target&&*target?target:"HEAD",head,out)<0)
goto done;
rc=0;
done:
graph_free(&graph);
return rc;
}
static int tag_name_cmp(const void*a, const void*b) {
return strcmp(*(const char*const*)a, *(const char*const*)b);
}
static void tag_names_free(char**names, size_t n) {
for(size_t i=0;i<n;i++)
free(names[i]);
free(names);
}
static int tag_names_add(char***names, size_t*n, size_t*cap,
const char*name) {
char**v;
char*duplicate;
if(*n>=*cap) {
size_t next=*cap?*cap*2:16;
if(next<*cap||next>SIZE_MAX/sizeof(*v)) {
errno=ENOMEM;
return -1;
}
v=realloc(*names, next*sizeof(*v));
if(!v)
return -1;
*names=v;
*cap=next;
}
duplicate=strdup(name);
if(!duplicate)
return -1;
(*names)[(*n)++]=duplicate;
return 0;
}
static int tag_collect(const struct mesh_repo*repo, char***out,
size_t*out_n) {
char path[MESH_PATH_MAX];
DIR*dir;
struct dirent*entry;
char**names=NULL;
size_t n=0, cap=0;
if(!repo||!repo->valid||!out||!out_n) {
errno=EINVAL;
return -1;
}
*out=NULL;
*out_n=0;
if(mesh_path_join(path, sizeof(path), repo->mesh_dir,"refs/tags")<0)
return -1;
dir=opendir(path);
if(!dir) {
if(errno==ENOENT)
return 0;
return -1;
}
while((errno=0, entry=readdir(dir))!=NULL) {
char ref[MESH_PATH_MAX];
struct stat st;
if(strcmp(entry->d_name,".")==0||strcmp(entry->d_name,"..")==0)
continue;
if(!mesh_ref_name_valid(entry->d_name))
continue;
if(mesh_path_join(ref, sizeof(ref), path, entry->d_name)<0||
lstat(ref,&st)<0||!S_ISREG(st.st_mode))
continue;
if(tag_names_add(&names,&n,&cap,entry->d_name)<0)
goto fail;
}
if(errno!=0)
goto fail;
if(closedir(dir)<0) {
dir=NULL;
goto fail;
}
dir=NULL;
if(n>1)
qsort(names, n, sizeof(*names), tag_name_cmp);
*out=names;
*out_n=n;
return 0;
fail: {
int saved=errno;
if(dir)
closedir(dir);
tag_names_free(names,n);
errno=saved;
return -1;
}
}
int tag_list(const struct mesh_repo*repo, FILE*out) {
char**names=NULL;
size_t n=0;
if(!repo||!out) {
errno=EINVAL;
return -1;
}
if(tag_collect(repo,&names,&n)<0)
return -1;
if(n==0) {
fprintf(out,"No tags.\n");
tag_names_free(names,n);
return 0;
}
fprintf(out,"Tags (%zu):\n", n);
for(size_t i=0;i<n;i++) {
char hash[MESH_HASH_HEX+1];
char short_hash[13];
if(tag_read_ref(repo,names[i],hash)<0) {
int saved=errno;
tag_names_free(names,n);
errno=saved;
return -1;
}
mesh_short_hash(short_hash, sizeof(short_hash), hash);
fprintf(out,"%s %s\n", names[i], short_hash);
}
tag_names_free(names,n);
return 0;
}
int tag_create(const struct mesh_repo*repo, const char*name,
const char*target) {
char path[MESH_PATH_MAX];
char hash[MESH_HASH_HEX+1];
char short_hash[13];
struct stat st;
int lock_fd=-1;
int rc=-1;
if(!repo||!repo->valid||!mesh_ref_name_valid(name)) {
errno=EINVAL;
return -1;
}
if(tag_ref_path(repo,name,path,sizeof(path))<0)
return -1;
lock_fd=mesh_repo_lock(repo);
if(lock_fd<0)
return -1;
if(lstat(path,&st)==0) {
errno=EEXIST;
goto done;
}
if(errno!=ENOENT)
goto done;
if(tag_target(repo,target,hash)<0)
goto done;
if(tag_write_ref(repo,name,hash)<0)
goto done;
mesh_short_hash(short_hash, sizeof(short_hash), hash);
printf("created tag %s at %s\n", name, short_hash);
rc=0;
done: {
int saved=errno;
close(lock_fd);
errno=saved;
}
return rc;
}
int tag_delete(const struct mesh_repo*repo, const char*name) {
char path[MESH_PATH_MAX];
int lock_fd=-1;
int rc=-1;
if(!repo||!repo->valid||!mesh_ref_name_valid(name)) {
errno=EINVAL;
return -1;
}
if(tag_ref_path(repo,name,path,sizeof(path))<0)
return -1;
lock_fd=mesh_repo_lock(repo);
if(lock_fd<0)
return -1;
if(unlink(path)<0)
goto done;
printf("deleted tag %s\n", name);
rc=0;
done: {
int saved=errno;
close(lock_fd);
errno=saved;
}
return rc;
}
