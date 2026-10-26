/*
* admin_inventory.c - count what a repository currently owns.
* The inventory is read-only. It distinguishes objects, manifests, refs and
* staged conflicts so a future garbage collector has a stable baseline.
*/
#include "core.h"
static int count_refs(const char*path,bool skip_tags,size_t*count) {
DIR*dir;
struct dirent*entry;
int fd;
int rc=0;
if(!path||!count) {
errno=EINVAL;
return -1;
}
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
struct stat st;
if(strcmp(entry->d_name,".")==0||strcmp(entry->d_name,"..")==0)
continue;
if(skip_tags&&strcmp(entry->d_name,"tags")==0)
continue;
if(fstatat(fd, entry->d_name,&st, AT_SYMLINK_NOFOLLOW)<0) {
rc=-1;
break;
}
if(S_ISREG(st.st_mode))
(*count)++;
}
if(rc==0&&errno!=0)
rc=-1;
if(closedir(dir)<0)
rc=-1;
return rc;
}
int admin_inventory_run(const struct mesh_repo*repo,bool json) {
struct mesh_graph graph;
struct mesh_store_usage usage;
char refs[MESH_PATH_MAX];
char tags[MESH_PATH_MAX];
char pretty[32];
size_t branches=0, tags_count=0, conflicts=0;
int rc=1;
if(!repo||!repo->valid) {
errno=EINVAL;
return 1;
}
graph_init(&graph);
if(graph_load(repo,&graph)<0) {
fprintf(stderr,"mesh: admin inventory: cannot read history: %s\n",
strerror(errno));
graph_free(&graph);
return 1;
}
if(mesh_path_join(refs, sizeof(refs), repo->mesh_dir,"refs")<0||
mesh_path_join(tags, sizeof(tags), repo->mesh_dir,"refs/tags")<0)
goto done;
if(count_refs(refs, true,&branches)<0||count_refs(tags, false,
&tags_count)<0||admin_store_usage(repo,&usage)<0)
goto done;
conflicts=(size_t)conflict_count(repo);
mesh_format_bytes(usage.object_bytes+usage.tree_bytes, pretty, sizeof(pretty));
if(json) {
printf("{\"snapshots\":%zu,\"objects\":%zu,\"trees\":%zu,"
"\"branches\":%zu,\"tags\":%zu,\"conflicts\":%zu,"
"\"object_bytes\":%"PRIu64 ",\"tree_bytes\":%"PRIu64 "}\n",
graph.count, usage.objects, usage.trees, branches, tags_count,
conflicts, usage.object_bytes, usage.tree_bytes);
} else {
printf("snapshots: %zu\n", graph.count);
printf("objects:   %zu (%s)\n", usage.objects, pretty);
printf("trees:     %zu\n", usage.trees);
printf("branches:  %zu\n", branches);
printf("tags:      %zu\n", tags_count);
printf("conflicts: %zu\n", conflicts);
printf("bytes:     %"PRIu64 "\n",
usage.object_bytes+usage.tree_bytes);
}
rc=0;
done: {
int saved=errno;
graph_free(&graph);
errno=saved;
}
if(rc!=0)
fprintf(stderr,"mesh: admin inventory: %s\n", strerror(errno));
return rc;
}
