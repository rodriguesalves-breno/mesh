/*
* admin_gc.c - mark reachable objects, then optionally unlink the rest.
* The default is a dry run. Deletion requires --apply, a non-empty history
* and the repository lock, so a mistyped command cannot erase a fresh store.
*/
#include "core.h"
static int mark_tree(const struct mesh_repo*repo,const char*tree,
struct mesh_admin_hash_set*trees,struct mesh_admin_hash_set*objects) {
struct mesh_file_list list;
if(admin_hash_set_has(trees,tree))
return 0;
if(admin_hash_set_add(trees,tree)<0)
return -1;
if(manifest_load(repo,tree,&list)<0)
return -1;
for(size_t i=0;i<list.n;i++) {
const struct mesh_file_entry*entry=&list.v[i];
if(entry->nchunks==0) {
if(admin_hash_set_add(objects,entry->hash)<0) {
file_list_free(&list);
return -1;
}
continue;
}
for(size_t c=0;c<entry->nchunks;c++)
if(admin_hash_set_add(objects,entry->chunks[c].hash)<0) {
file_list_free(&list);
return -1;
}
}
file_list_free(&list);
return 0;
}
static int gc_object_dir(const char*path,const struct mesh_admin_hash_set*objects,
bool apply,size_t*removed,uint64_t*bytes,size_t*dangling) {
DIR*dir;
struct dirent*entry;
int fd;
int rc=0;
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
char hash[MESH_HASH_HEX+1];
struct stat st;
if(strcmp(entry->d_name,".")==0||strcmp(entry->d_name,"..")==0)
continue;
if(strlen(entry->d_name)!=2)
continue;
int sub_open=openat(fd, entry->d_name,
O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
if(sub_open<0) {
rc=-1;
break;
}
DIR*sub=fdopendir(sub_open);
if(!sub) {
int saved=errno;
close(sub_open);
errno=saved;
rc=-1;
break;
}
int subfd=dirfd(sub);
if(subfd<0) {
closedir(sub);
rc=-1;
break;
}
struct dirent*file;
while((errno=0, file=readdir(sub))!=NULL) {
if(strcmp(file->d_name,".")==0||strcmp(file->d_name,"..")==0)
continue;
if(strlen(file->d_name)!=MESH_HASH_HEX-2)
continue;
snprintf(hash, sizeof(hash),"%.2s%s", entry->d_name,
file->d_name);
if(!mesh_hash_valid(hash))
continue;
if(fstatat(subfd, file->d_name,&st, AT_SYMLINK_NOFOLLOW)<0) {
rc=-1;
break;
}
if(!S_ISREG(st.st_mode)||admin_hash_set_has(objects, hash))
continue;
(*dangling)++;
if(!apply)
continue;
if(unlinkat(subfd, file->d_name,0)<0) {
rc=-1;
break;
}
(*removed)++;
*bytes+=(uint64_t)st.st_size;
}
if(rc==0&&errno!=0)
rc=-1;
if(closedir(sub)<0)
rc=-1;
if(rc!=0)
break;
}
if(rc==0&&errno!=0)
rc=-1;
if(closedir(dir)<0)
rc=-1;
return rc;
}
int admin_gc_run(const struct mesh_repo*repo,bool apply) {
struct mesh_graph graph;
struct mesh_admin_hash_set trees, objects;
char path[MESH_PATH_MAX];
size_t removed=0, dangling=0;
uint64_t bytes=0;
int lock_fd=-1;
int rc=1;
if(!repo||!repo->valid) {
errno=EINVAL;
return 1;
}
graph_init(&graph);
admin_hash_set_init(&trees);
admin_hash_set_init(&objects);
if(graph_load(repo,&graph)<0) {
fprintf(stderr,"mesh: admin gc: cannot read history: %s\n",
strerror(errno));
goto done;
}
if(graph.count==0&&apply) {
fprintf(stderr,"mesh: admin gc: refusing to apply with empty history\n");
goto done;
}
for(const struct mesh_node*node=graph.head;node;node=node->next)
if(mark_tree(repo,node->tree,&trees,&objects)<0) {
fprintf(stderr,"mesh: admin gc: cannot mark tree: %s\n",
strerror(errno));
goto done;
}
if(apply) {
lock_fd=mesh_repo_lock(repo);
if(lock_fd<0) {
fprintf(stderr,"mesh: admin gc: cannot lock repository: %s\n",
strerror(errno));
goto done;
}
}
if(mesh_path_join(path, sizeof(path), repo->mesh_dir,
"store/objects")<0)
goto done;
if(gc_object_dir(path,&objects,apply,&removed,&bytes,&dangling)<0) {
fprintf(stderr,"mesh: admin gc: cannot scan objects: %s\n",
strerror(errno));
goto done;
}
if(!apply)
printf("would remove: %zu object(s), %"PRIu64 " byte(s)\n",
dangling, bytes);
else
printf("removed: %zu object(s), %"PRIu64 " byte(s)\n", removed,
bytes);
printf("reachable: %zu object(s), %zu tree(s)\n", objects.n, trees.n);
rc=0;
done: {
int saved=errno;
if(lock_fd>=0)
close(lock_fd);
admin_hash_set_free(&trees);
admin_hash_set_free(&objects);
graph_free(&graph);
errno=saved;
}
return rc;
}
