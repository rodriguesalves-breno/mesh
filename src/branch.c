/*
* branch.c - named branch refs and the current-branch pointer.
* Branches are one regular file under .mesh/refs. The current branch is the
* branch key in .mesh/config, so old repositories remain readable without a
* migration. HEAD is always the resolved hash of the current branch.
*/
#include "core.h"
static int branch_ref_path(const struct mesh_repo*repo, const char*name,
char*out, size_t cap) {
char rel[160];
int n;
if(!repo||!repo->valid||!out||cap==0||!mesh_ref_name_valid(name)) {
errno=EINVAL;
return -1;
}
n=snprintf(rel, sizeof(rel),"refs/%s", name);
if(n<0||(size_t)n>=sizeof(rel)) {
errno=ENAMETOOLONG;
return -1;
}
return mesh_path_join(out, cap, repo->mesh_dir, rel);
}
static int branch_read_ref(const struct mesh_repo*repo, const char*name,
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
if(branch_ref_path(repo,name,path,sizeof(path))<0)
return -1;
if(mesh_read_text(path,&buf,&len)<0)
return -1;
if(memchr(buf,'\0',len)!=NULL) {
errno=EINVAL;
goto done;
}
mesh_trim_newline(buf);
if(buf[0]=='\0') {
rc=0;
goto done;
}
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
static int branch_write_ref(const struct mesh_repo*repo, const char*name,
const char*hash) {
char path[MESH_PATH_MAX];
char line[MESH_HASH_HEX+2];
int n;
if(!mesh_hash_valid(hash)||branch_ref_path(repo,name,path,
sizeof(path))<0)
return -1;
n=snprintf(line, sizeof(line),"%s\n", hash);
if(n<0||(size_t)n>=sizeof(line)) {
errno=ENAMETOOLONG;
return -1;
}
return mesh_write_atomic(path, line, (size_t)n, 0644);
}
static int branch_config_append(char*out, size_t cap, size_t*off,
const char*name) {
int n=snprintf(out+*off, cap-*off,"branch=%s\n", name);
if(n<0||(size_t)n>=cap-*off) {
errno=ENOSPC;
return -1;
}
*off+=(size_t)n;
return 0;
}
static int branch_config_set(const struct mesh_repo*repo, const char*name) {
char path[MESH_PATH_MAX];
char*existing=NULL;
char*rewritten=NULL;
const char*p, *end;
size_t len=0, name_len, cap_size, out_len=0;
bool replaced=false;
int rc=-1;
if(!repo||!repo->valid||!mesh_ref_name_valid(name)) {
errno=EINVAL;
return -1;
}
name_len=strlen(name);
if(mesh_path_join(path, sizeof(path), repo->mesh_dir,"config")<0)
return -1;
if(mesh_read_text(path,&existing,&len)<0&&errno!=ENOENT)
return -1;
if(existing&&memchr(existing,'\0',len)!=NULL) {
errno=EINVAL;
goto done;
}
if(len>SIZE_MAX-name_len-64) {
errno=EOVERFLOW;
goto done;
}
cap_size=len+name_len+64;
rewritten=malloc(cap_size);
if(!rewritten)
goto done;
p=existing?existing:"";
end=p+len;
while(p<end) {
const char*nl=memchr(p,'\n',(size_t)(end-p));
size_t line_len=nl?(size_t)(nl-p):(size_t)(end-p);
bool is_branch=line_len>=7&&memcmp(p,"branch=",7)==0;
if(is_branch) {
if(!replaced) {
if(branch_config_append(rewritten,cap_size,&out_len,name)<0)
goto done;
replaced=true;
}
} else {
if(out_len+line_len+1>cap_size) {
errno=ENOSPC;
goto done;
}
memcpy(rewritten+out_len,p,line_len);
out_len+=line_len;
if(nl)
rewritten[out_len++]='\n';
}
p=nl?nl+1:end;
}
if(!replaced) {
if(out_len>0&&rewritten[out_len-1]!='\n'&&out_len+1<cap_size)
rewritten[out_len++]='\n';
if(branch_config_append(rewritten,cap_size,&out_len,name)<0)
goto done;
}
rc=mesh_write_atomic(path, rewritten, out_len, 0644);
done: {
int saved=errno;
free(existing);
free(rewritten);
errno=saved;
}
return rc;
}
static int branch_target(const struct mesh_repo*repo, const char*start,
char out[MESH_HASH_HEX+1]) {
struct mesh_graph graph;
char head[MESH_HASH_HEX+1];
int rc=-1;
graph_init(&graph);
if(graph_head(repo,head)<0)
return -1;
if(graph_load(repo,&graph)<0)
goto done;
if(graph_resolve(&graph,start&&*start?start:"HEAD",head,out)<0)
goto done;
rc=0;
done:
graph_free(&graph);
return rc;
}
static int name_cmp(const void*a, const void*b) {
return strcmp(*(const char*const*)a, *(const char*const*)b);
}
static void names_free(char**names, size_t n) {
for(size_t i=0;i<n;i++)
free(names[i]);
free(names);
}
static int names_add(char***names, size_t*n, size_t*cap, const char*name) {
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
static int branch_collect(const struct mesh_repo*repo, char***out,
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
if(mesh_path_join(path, sizeof(path), repo->mesh_dir,"refs")<0)
return -1;
dir=opendir(path);
if(!dir)
return -1;
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
if(names_add(&names,&n,&cap,entry->d_name)<0)
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
qsort(names, n, sizeof(*names), name_cmp);
*out=names;
*out_n=n;
return 0;
fail: {
int saved=errno;
if(dir)
closedir(dir);
names_free(names,n);
errno=saved;
return -1;
}
}
int branch_list(const struct mesh_repo*repo, FILE*out) {
char**names=NULL;
size_t n=0;
if(!repo||!out) {
errno=EINVAL;
return -1;
}
if(branch_collect(repo,&names,&n)<0)
return -1;
if(n==0) {
fprintf(out,"No branches.\n");
names_free(names,n);
return 0;
}
fprintf(out,"Branches (%zu):\n", n);
for(size_t i=0;i<n;i++) {
char hash[MESH_HASH_HEX+1];
char short_hash[13];
if(branch_read_ref(repo,names[i],hash)<0) {
int saved=errno;
names_free(names,n);
errno=saved;
return -1;
}
mesh_short_hash(short_hash, sizeof(short_hash), hash);
fprintf(out,"%c %s %s\n", strcmp(names[i],repo->branch)==0?'*':' ',
names[i], hash[0]?short_hash:"(empty)");
}
names_free(names,n);
return 0;
}
int branch_create(const struct mesh_repo*repo, const char*name,
const char*start) {
char path[MESH_PATH_MAX];
char target[MESH_HASH_HEX+1];
char short_hash[13];
struct stat st;
int lock_fd=-1;
int rc=-1;
if(!repo||!repo->valid||!mesh_ref_name_valid(name)) {
errno=EINVAL;
return -1;
}
if(branch_ref_path(repo,name,path,sizeof(path))<0)
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
if(branch_target(repo,start,target)<0)
goto done;
if(branch_write_ref(repo,name,target)<0)
goto done;
mesh_short_hash(short_hash, sizeof(short_hash), target);
printf("created branch %s at %s\n", name, short_hash);
rc=0;
done: {
int saved=errno;
close(lock_fd);
errno=saved;
}
return rc;
}
static void branch_refresh_status(struct mesh_repo*repo, const char*name) {
struct mesh_status status;
if(monitor_collect(repo,&status,"branch")<0)
return;
snprintf(status.message, sizeof(status.message),
"switched to branch %s", name);
(void)monitor_write(repo,&status);
}
int branch_switch(struct mesh_repo*repo, const char*name) {
struct mesh_graph graph;
char target[MESH_HASH_HEX+1];
char old[MESH_HASH_HEX+1];
char old_branch[64];
char message[MESH_MSG_MAX];
int lock_fd=-1;
int rc=-1;
if(!repo||!repo->valid||!mesh_ref_name_valid(name)) {
errno=EINVAL;
return -1;
}
graph_init(&graph);
lock_fd=mesh_repo_lock(repo);
if(lock_fd<0)
goto done;
if(branch_read_ref(repo,name,target)<0)
goto done;
if(graph_head(repo,old)<0||graph_load(repo,&graph)<0)
goto done;
if(!graph_find(&graph,target)) {
errno=EINVAL;
goto done;
}
mesh_strlcpy(old_branch, sizeof(old_branch), repo->branch);
if(!mesh_ref_name_valid(old_branch))
mesh_strlcpy(old_branch, sizeof(old_branch),"main");
if(branch_config_set(repo,name)<0)
goto done;
if(graph_set_head(repo,name,target)<0) {
int saved=errno;
(void)branch_config_set(repo,old_branch);
errno=saved;
goto done;
}
mesh_strlcpy(repo->branch, sizeof(repo->branch), name);
snprintf(message, sizeof(message),"branch: switch to %s", name);
if(reflog_append(repo,old,target,message)<0)
fprintf(stderr,"mesh: branch: warning: reflog append failed: %s\n",
strerror(errno));
rc=0;
done: {
int saved=errno;
if(lock_fd>=0)
close(lock_fd);
graph_free(&graph);
errno=saved;
}
if(rc==0) {
branch_refresh_status(repo,name);
printf("switched to branch '%s'\n", name);
}
return rc;
}
int branch_delete(const struct mesh_repo*repo, const char*name) {
char path[MESH_PATH_MAX];
int lock_fd=-1;
int rc=-1;
if(!repo||!repo->valid||!mesh_ref_name_valid(name)) {
errno=EINVAL;
return -1;
}
if(strcmp(name,repo->branch)==0) {
errno=EBUSY;
return -1;
}
if(branch_ref_path(repo,name,path,sizeof(path))<0)
return -1;
lock_fd=mesh_repo_lock(repo);
if(lock_fd<0)
return -1;
if(unlink(path)<0)
goto done;
printf("deleted branch %s\n", name);
rc=0;
done: {
int saved=errno;
close(lock_fd);
errno=saved;
}
return rc;
}
