/*
* admin_verify.c - hash every object reachable from the snapshot graph.
* A manifest is verified before any object is trusted. Chunked files are
* verified through their chunk objects, exactly as materialization reads them.
*/
#include "core.h"
static int verify_object(const struct mesh_repo*repo,const char*hash,
struct mesh_admin_hash_set*objects,size_t*checked,size_t*failures) {
if(admin_hash_set_has(objects,hash))
return 0;
if(admin_hash_set_add(objects,hash)<0)
return -1;
if(object_verify(repo,hash,NULL)<0) {
printf("FAIL object %s: %s\n", hash, strerror(errno));
(*failures)++;
return 0;
}
(*checked)++;
return 0;
}
static int verify_tree(const struct mesh_repo*repo,const char*tree,
struct mesh_admin_hash_set*trees,struct mesh_admin_hash_set*objects,
size_t*checked,size_t*failures) {
struct mesh_file_list list;
if(admin_hash_set_has(trees,tree))
return 0;
if(admin_hash_set_add(trees,tree)<0)
return -1;
if(manifest_load(repo,tree,&list)<0) {
printf("FAIL tree %s: %s\n", tree, strerror(errno));
(*failures)++;
return 0;
}
for(size_t i=0;i<list.n;i++) {
const struct mesh_file_entry*entry=&list.v[i];
if(entry->nchunks==0) {
if(verify_object(repo,entry->hash,objects,checked,failures)<0) {
file_list_free(&list);
return -1;
}
continue;
}
for(size_t c=0;c<entry->nchunks;c++)
if(verify_object(repo,entry->chunks[c].hash,objects,checked,
failures)<0) {
file_list_free(&list);
return -1;
}
}
file_list_free(&list);
return 0;
}
int admin_verify_run(const struct mesh_repo*repo) {
struct mesh_graph graph;
struct mesh_admin_hash_set trees, objects;
size_t checked=0, failures=0;
int rc=1;
if(!repo||!repo->valid) {
errno=EINVAL;
return 1;
}
graph_init(&graph);
admin_hash_set_init(&trees);
admin_hash_set_init(&objects);
printf("Mesh admin verify\n");
if(graph_load(repo,&graph)<0) {
printf("FAIL history: %s\n", strerror(errno));
goto done;
}
for(const struct mesh_node*node=graph.head;node;node=node->next)
if(verify_tree(repo,node->tree,&trees,&objects,&checked,
&failures)<0) {
printf("FAIL verify: %s\n", strerror(errno));
goto done;
}
printf("verified: %zu object(s), %zu tree(s), %zu snapshot(s)\n",
checked, trees.n, graph.count);
printf("failures: %zu\n", failures);
rc=failures?1:0;
done: {
int saved=errno;
admin_hash_set_free(&trees);
admin_hash_set_free(&objects);
graph_free(&graph);
errno=saved;
}
return rc;
}
