/*
* admin_refs.c - validate every local ref against the snapshot graph.
* Refs are one hash per file. A ref that names a missing snapshot is a
* bootstrapping failure, not a warning, because restore and switch would fail.
*/
#include "core.h"
static int check_ref(const char*label,const char*path,
const struct mesh_graph*graph,size_t*failures) {
char*buf=NULL;
size_t len=0;
if(mesh_read_text(path,&buf,&len)<0) {
printf("FAIL %s: %s\n", label, strerror(errno));
(*failures)++;
return 0;
}
mesh_trim_newline(buf);
if(!mesh_hash_valid(buf)) {
printf("FAIL %s: invalid hash\n", label);
(*failures)++;
free(buf);
return 0;
}
if(!graph_find(graph, buf)) {
printf("FAIL %s: snapshot %s is missing\n", label, buf);
(*failures)++;
free(buf);
return 0;
}
printf("ok   %s -> %s\n", label, buf);
free(buf);
return 0;
}
static int check_dir(const struct mesh_repo*repo,const char*rel,
bool skip_tags,const struct mesh_graph*graph,size_t*failures) {
char path[MESH_PATH_MAX];
DIR*dir;
struct dirent*entry;
int fd;
int rc=0;
if(mesh_path_join(path, sizeof(path), repo->mesh_dir, rel)<0)
return -1;
dir=opendir(path);
if(!dir) {
if(errno==ENOENT)
return 0;
return -1;
}
fd=dirfd(dir);
if(fd<0) {
closedir(dir);
return -1;
}
while((errno=0, entry=readdir(dir))!=NULL) {
char child[MESH_PATH_MAX];
char label[MESH_PATH_MAX];
struct stat st;
if(strcmp(entry->d_name,".")==0||strcmp(entry->d_name,"..")==0)
continue;
if(skip_tags&&strcmp(entry->d_name,"tags")==0)
continue;
if(fstatat(fd, entry->d_name,&st, AT_SYMLINK_NOFOLLOW)<0) {
rc=-1;
break;
}
if(!S_ISREG(st.st_mode))
continue;
if(mesh_path_join(child, sizeof(child), path, entry->d_name)<0) {
rc=-1;
break;
}
snprintf(label, sizeof(label),"%s/%s", rel, entry->d_name);
if(check_ref(label, child, graph, failures)<0) {
rc=-1;
break;
}
}
if(rc==0&&errno!=0)
rc=-1;
if(closedir(dir)<0)
rc=-1;
return rc;
}
int admin_refs_run(const struct mesh_repo*repo) {
struct mesh_graph graph;
char head[MESH_HASH_HEX+1];
size_t failures=0;
int rc=1;
if(!repo||!repo->valid) {
errno=EINVAL;
return 1;
}
graph_init(&graph);
if(graph_load(repo,&graph)<0) {
fprintf(stderr,"mesh: admin refs: cannot read history: %s\n",
strerror(errno));
graph_free(&graph);
return 1;
}
if(check_dir(repo,"refs",true,&graph,&failures)<0||
check_dir(repo,"refs/tags",false,&graph,&failures)<0) {
fprintf(stderr,"mesh: admin refs: %s\n", strerror(errno));
goto done;
}
if(graph_head(repo,head)<0) {
fprintf(stderr,"mesh: admin refs: cannot read HEAD: %s\n",
strerror(errno));
goto done;
}
if(head[0]&&!graph_find(&graph, head)) {
printf("FAIL HEAD: snapshot %s is missing\n", head);
failures++;
} else {
printf("ok   HEAD -> %s\n", head[0]?head:"(empty)");
}
if(failures)
printf("refs: %zu failure(s)\n", failures);
else
printf("refs: all references are valid\n");
rc=failures?1:0;
done: {
int saved=errno;
graph_free(&graph);
errno=saved;
}
return rc;
}
