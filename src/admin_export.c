/*
* admin_export.c - render the snapshot graph as one stable JSON document.
* This is the read-only bridge for dashboards and the Python tools. It never
* exports object contents, only graph metadata.
*/
#include "core.h"
static int export_node(FILE*out,const struct mesh_node*node) {
char name[MESH_AUTHOR_MAX*6+1];
char email[MESH_EMAIL_MAX*6+1];
char message[MESH_MSG_MAX*6+1];
if(mesh_json_escape(node->author_name, name, sizeof(name))<0||
mesh_json_escape(node->author_email, email, sizeof(email))<0||
mesh_json_escape(node->message?node->message:"", message,
sizeof(message))<0)
return -1;
fprintf(out,"{\"hash\":\"%s\",\"tree\":\"%s\",\"parent_1\":\"%s\","
"\"parent_2\":\"%s\",\"timestamp\":%lld,\"author_name\":\"%s\","
"\"author_email\":\"%s\",\"message\":\"%s\"}", node->hash,
node->tree, node->parent_1, node->parent_2,
(long long)node->timestamp, name, email, message);
return 0;
}
int admin_export_run(const struct mesh_repo*repo,const char*dest) {
struct mesh_graph graph;
char root[MESH_PATH_MAX*2+1];
char branch[64*6+1];
char head[MESH_HASH_HEX+1];
char*json=NULL;
size_t len=0;
FILE*fp;
int rc=1;
if(!repo||!repo->valid) {
errno=EINVAL;
return 1;
}
graph_init(&graph);
if(graph_load(repo,&graph)<0||graph_head(repo,head)<0) {
fprintf(stderr,"mesh: admin export: cannot read repository: %s\n",
strerror(errno));
graph_free(&graph);
return 1;
}
if(mesh_json_escape(repo->root, root, sizeof(root))<0||
mesh_json_escape(repo->branch, branch, sizeof(branch))<0) {
fprintf(stderr,"mesh: admin export: cannot encode metadata\n");
graph_free(&graph);
return 1;
}
fp=open_memstream(&json,&len);
if(!fp) {
graph_free(&graph);
return 1;
}
fprintf(fp,"{\"name\":\"Mesh\",\"version\":\"1.0\",\"root\":\"%s\","
"\"branch\":\"%s\",\"head\":\"%s\",\"snapshots\":[", root, branch,
head);
for(const struct mesh_node*node=graph.head;node;node=node->next) {
if(node!=graph.head)
fputc(',', fp);
if(export_node(fp, node)<0)
goto done;
}
fputs("]}\n", fp);
if(ferror(fp))
goto done;
if(fclose(fp)!=0) {
fp=NULL;
goto done;
}
fp=NULL;
if(!dest||strcmp(dest,"-")==0) {
if(fwrite(json, 1, len, stdout)!=len)
goto done;
} else if(mesh_write_atomic(dest, json, len, 0644)<0) {
goto done;
}
rc=0;
done: {
int saved=errno;
if(fp)
fclose(fp);
free(json);
graph_free(&graph);
errno=saved;
}
if(rc!=0)
fprintf(stderr,"mesh: admin export: %s\n", strerror(errno));
return rc;
}
