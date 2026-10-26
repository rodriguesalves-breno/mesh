/*
* clean.c - remove working-tree files that no snapshot tracks.
* Tracking is defined by HEAD's manifest, not by an index. Ignored files are
* skipped unless the caller asks for them, so a dry run can explain exactly
* what a forced clean would delete.
*/
#include "core.h"
static int clean_walk(const struct mesh_repo*repo, const char*rel,
const struct mesh_file_list*tracked, const struct mesh_ignore*ignore,
bool include_ignored, struct mesh_file_list*out, unsigned depth) {
char full[MESH_PATH_MAX];
DIR*dir;
struct dirent*entry;
int rc=0;
if(depth>64) {
errno=ELOOP;
return -1;
}
if(mesh_path_join(full, sizeof(full), repo->root, rel)<0)
return -1;
dir=opendir(full);
if(!dir)
return -1;
while((errno=0, entry=readdir(dir))!=NULL) {
char child[MESH_PATH_MAX];
char path[MESH_PATH_MAX];
struct stat st;
bool is_dir;
if(strcmp(entry->d_name,".")==0||strcmp(entry->d_name,"..")==0)
continue;
if(mesh_component_reserved(entry->d_name,strlen(entry->d_name)))
continue;
if(mesh_path_join(child, sizeof(child), rel, entry->d_name)<0||
mesh_path_join(path, sizeof(path), repo->root, child)<0) {
rc=-1;
break;
}
if(!mesh_relpath_safe(child)) {
errno=EINVAL;
rc=-1;
break;
}
if(lstat(path,&st)<0) {
rc=-1;
break;
}
is_dir=S_ISDIR(st.st_mode);
bool ignored=!include_ignored&&ignore_match(ignore,child,is_dir);
if(ignored&&(!is_dir||ignore_can_prune(ignore)))
continue;
if(is_dir) {
if(clean_walk(repo,child,tracked,ignore,include_ignored,out,
depth+1)<0) {
rc=-1;
break;
}
continue;
}
if(!S_ISREG(st.st_mode)&&!S_ISLNK(st.st_mode))
continue;
if(manifest_find(tracked,child))
continue;
if(!file_list_add(out, child, (uint64_t)st.st_size, 0, 0, 0644,
S_ISLNK(st.st_mode)?MESH_KIND_SYMLINK:MESH_KIND_FILE)) {
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
static int clean_tracked(const struct mesh_repo*repo,
struct mesh_file_list*tracked, struct mesh_graph*graph) {
char head[MESH_HASH_HEX+1];
struct mesh_node*node;
if(graph_head(repo,head)<0)
return -1;
if(head[0]=='\0')
return 0;
if(graph_load(repo,graph)<0)
return -1;
node=graph_find(graph,head);
if(!node) {
errno=EINVAL;
return -1;
}
return manifest_load(repo,node->tree,tracked);
}
static void clean_note_failure(size_t*failed) {
if(failed)
(*failed)++;
}
int clean_run(const struct mesh_repo*repo, bool force, bool include_ignored,
size_t*removed, uint64_t*bytes, size_t*failed) {
struct mesh_file_list tracked, candidates;
struct mesh_graph graph;
struct mesh_ignore*ignore=NULL;
int lock_fd=-1;
int root_fd=-1;
int rc=-1;
if(removed)
*removed=0;
if(bytes)
*bytes=0;
if(failed)
*failed=0;
if(!repo||!repo->valid) {
errno=EINVAL;
return -1;
}
lock_fd=mesh_repo_lock(repo);
if(lock_fd<0)
return -1;
file_list_init(&tracked);
file_list_init(&candidates);
graph_init(&graph);
root_fd=open(repo->root,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
if(root_fd<0)
goto done;
if(clean_tracked(repo,&tracked,&graph)<0)
goto done;
if(!include_ignored&&ignore_load(repo,&ignore)<0)
goto done;
if(clean_walk(repo,"",&tracked,ignore,include_ignored,&candidates,0)<0)
goto done;
file_list_sort(&candidates);
for(size_t i=0;i<candidates.n;i++) {
struct mesh_file_entry*entry=&candidates.v[i];
if(!force) {
printf("Would remove %s\n", entry->path);
} else {
char leaf[MESH_PATH_MAX];
int parent=-1;
if(mesh_open_parent_at(root_fd, entry->path, false,&parent,leaf,
sizeof(leaf))<0) {
fprintf(stderr,"mesh: clean: cannot open %s: %s\n",
entry->path, strerror(errno));
clean_note_failure(failed);
continue;
}
if(unlinkat(parent,leaf,0)<0) {
fprintf(stderr,"mesh: clean: cannot remove %s: %s\n",
entry->path, strerror(errno));
clean_note_failure(failed);
close(parent);
continue;
}
if(mesh_fsync_dir(parent)<0) {
fprintf(stderr,"mesh: clean: cannot sync %s: %s\n",
entry->path, strerror(errno));
clean_note_failure(failed);
close(parent);
continue;
}
close(parent);
printf("Removed %s\n", entry->path);
}
if(removed)
(*removed)++;
if(bytes)
*bytes+=entry->size;
}
rc=0;
done: {
int saved=errno;
ignore_free(ignore);
file_list_free(&tracked);
file_list_free(&candidates);
graph_free(&graph);
if(root_fd>=0)
close(root_fd);
if(lock_fd>=0)
close(lock_fd);
errno=saved;
}
return rc;
}
