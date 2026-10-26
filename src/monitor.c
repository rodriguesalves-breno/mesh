/*
* monitor.c - the passive status surface consumed by GUIs and IDE
* extensions. One small JSON document, rewritten atomically. No daemon,
* no socket, no polling loop: a UI reads a file when it feels like it.
*/
#include "core.h"
#include <ctype.h>
int monitor_collect(const struct mesh_repo*repo, struct mesh_status*status,
const char*operation) {
struct mesh_graph graph;
char head[MESH_HASH_HEX+1];
if(!repo||!repo->valid||!status) {
errno=EINVAL;
return -1;
}
memset(status, 0, sizeof(*status));
snprintf(status->branch, sizeof(status->branch),"%s",
repo->branch[0]?repo->branch:"main");
mesh_sanitize_field(status->operation, sizeof(status->operation),
operation?operation:"check");
status->conflicts=(size_t)conflict_count(repo);
if(graph_head(repo, head)<0)
return -1;
if(head[0]=='\0')
return 0;
graph_init(&graph);
if(graph_load(repo,&graph)<0)
return -1;
status->snapshots=graph.count;
struct mesh_node*node=graph_find(&graph, head);
if(!node) {
graph_free(&graph);
errno=EINVAL;
return -1;
}
snprintf(status->head, sizeof(status->head),"%s", node->hash);
snprintf(status->tree, sizeof(status->tree),"%s", node->tree);
snprintf(status->parent, sizeof(status->parent),"%s",
node->parent_1);
snprintf(status->author_name, sizeof(status->author_name),"%s",
node->author_name);
snprintf(status->author_email, sizeof(status->author_email),"%s",
node->author_email);
graph_free(&graph);
struct mesh_file_list list;
if(manifest_load(repo, status->tree,&list)<0)
return -1;
status->files=list.n;
status->chunks=manifest_chunk_count(&list);
for(size_t i=0;i<list.n;i++)
status->bytes_tracked+=list.v[i].size;
file_list_free(&list);
return 0;
}
int monitor_write(const struct mesh_repo*repo,
const struct mesh_status*status) {
char root_esc[MESH_PATH_MAX*6+1];
char repo_name_esc[MESH_PATH_MAX*6+1];
char path_esc[MESH_PATH_MAX*6+1];
char msg_esc[MESH_MSG_MAX*6+1];
char author_name_esc[MESH_AUTHOR_MAX*6+1];
char author_email_esc[MESH_EMAIL_MAX*6+1];
char op_esc[32*6+1];
char branch_esc[64*6+1];
char target[MESH_PATH_MAX];
const char*repo_name;
char*json;
size_t cap=2*(MESH_PATH_MAX*6+1)+sizeof(msg_esc)+
sizeof(author_name_esc)+sizeof(author_email_esc)+4096;
size_t off=0;
int n;
if(!repo||!repo->valid||!status) {
errno=EINVAL;
return -1;
}
repo_name=strrchr(repo->root,'/');
repo_name=repo_name?repo_name+1:repo->root;
if(mesh_json_escape(repo->root, root_esc, sizeof(root_esc))<0||
mesh_json_escape(repo_name, repo_name_esc, sizeof(repo_name_esc))<0||
mesh_json_escape(status->last_path, path_esc, sizeof(path_esc))<0||
mesh_json_escape(status->message, msg_esc, sizeof(msg_esc))<0||
mesh_json_escape(status->author_name, author_name_esc,
sizeof(author_name_esc))<0||
mesh_json_escape(status->author_email, author_email_esc,
sizeof(author_email_esc))<0||
mesh_json_escape(status->operation, op_esc, sizeof(op_esc))<0||
mesh_json_escape(status->branch, branch_esc,
sizeof(branch_esc))<0)
return -1;
json=malloc(cap);
if(!json)
return -1;
#define APPEND(...) \
do { \
n=snprintf(json+off, cap-off, __VA_ARGS__); \
if(n<0||(size_t)n>=cap-off) { \
free(json); \
errno=ENOSPC; \
return -1; \
} \
off+=(size_t)n; \
} while(0)
APPEND("{\n");
APPEND("  \"mesh_version\": \""MESH_VERSION "\",\n");
APPEND("  \"repo_root\": \"%s\",\n", root_esc);
APPEND("  \"repo_name\": \"%s\",\n", repo_name_esc);
APPEND("  \"branch\": \"%s\",\n", branch_esc);
APPEND("  \"head\": \"%s\",\n", status->head);
APPEND("  \"head_short\": \"%.12s\",\n", status->head);
APPEND("  \"parent\": \"%s\",\n", status->parent);
APPEND("  \"tree\": \"%s\",\n", status->tree);
APPEND("  \"snapshots\": %zu,\n", status->snapshots);
APPEND("  \"files\": %zu,\n", status->files);
APPEND("  \"bytes\": %"PRIu64 ",\n", status->bytes_tracked);
APPEND("  \"chunks\": %zu,\n", status->chunks);
APPEND("  \"conflicts\": %zu,\n", status->conflicts);
APPEND("  \"state\": \"%s\",\n",
status->conflicts>0?"conflicted":"ready");
APPEND("  \"changed\": {\"added\": %zu, \"modified\": %zu, "
"\"deleted\": %zu, \"bytes\": %"PRIu64 "},\n",
status->changed.added, status->changed.modified,
status->changed.deleted, status->changed.bytes);
APPEND("  \"last_operation\": \"%s\",\n", op_esc);
APPEND("  \"message\": \"%s\",\n", msg_esc);
APPEND("  \"author_name\": \"%s\",\n", author_name_esc);
APPEND("  \"author_email\": \"%s\",\n", author_email_esc);
APPEND("  \"last_path\": \"%s\",\n", path_esc);
APPEND("  \"download_bytes\": %"PRIu64 ",\n",
status->download_bytes);
APPEND("  \"resumed_from\": %"PRIu64 ",\n", status->resumed_from);
APPEND("  \"download_hash\": \"%s\",\n", status->download_hash);
APPEND("  \"timestamp\": %lld,\n", mesh_now());
APPEND("  \"repo_icon_source\": \""MESH_ICON "\"\n");
APPEND("}\n");
#undef APPEND
if(mesh_path_join(target, sizeof(target), repo->mesh_dir,
"monitor/status.json")<0) {
free(json);
return -1;
}
n=mesh_write_atomic(target, json, off, 0644);
free(json);
return n;
}
int monitor_read(const struct mesh_repo*repo, char**json, size_t*len) {
char path[MESH_PATH_MAX];
if(!repo||!json) {
errno=EINVAL;
return -1;
}
if(mesh_path_join(path, sizeof(path), repo->mesh_dir,
"monitor/status.json")<0)
return -1;
return mesh_read_text(path, json, len);
}
static char json_unescape_char(char c) {
switch(c) {
case 'n':
return '\n';
case 'r':
return '\r';
case 't':
return '\t';
case '"':
return '"';
case '\\':
return '\\';
default:
return c;
}
}
static const char*json_ws(const char*p) {
while(*p&&isspace((unsigned char)*p))
p++;
return p;
}
static const char*json_string_end(const char*p) {
if(*p!='"')
return NULL;
p++;
while(*p) {
if(*p=='\\') {
if(!*++p)
return NULL;
p++;
continue;
}
if(*p++=='"')
return p;
}
return NULL;
}
static const char*json_skip_value(const char*p, unsigned depth) {
const char*end;
if(depth>32)
return NULL;
p=json_ws(p);
if(*p=='"') {
end=json_string_end(p);
return end?json_ws(end):NULL;
}
if(*p=='{') {
p++;
for(;;) {
p=json_ws(p);
if(*p=='}')
return p+1;
if(*p!='"')
return NULL;
end=json_string_end(p);
if(!end)
return NULL;
p=json_ws(end);
if(*p!=':')
return NULL;
p=json_skip_value(p+1,depth+1);
if(!p)
return NULL;
p=json_ws(p);
if(*p==',') {
p++;
continue;
}
if(*p!='}')
return NULL;
return p+1;
}
}
if(*p=='[') {
p++;
for(;;) {
p=json_ws(p);
if(*p==']')
return p+1;
p=json_skip_value(p,depth+1);
if(!p)
return NULL;
p=json_ws(p);
if(*p==',') {
p++;
continue;
}
if(*p!=']')
return NULL;
return p+1;
}
}
while(*p&&*p!=','&&*p!='}'&&*p!=']'&&
!isspace((unsigned char)*p))
p++;
return p;
}
static bool json_field(const char*json, const char*key, char*out,
size_t cap) {
const char*p;
if(!json||!key||!out||cap==0)
return false;
p=json_ws(json);
if(*p!='{')
return false;
p++;
for(;;) {
char name[128];
size_t o=0;
const char*end;
p=json_ws(p);
if(*p=='}')
return false;
if(*p!='"')
return false;
p++;
while(*p&&*p!='"') {
char c=*p++;
if(c=='\\'&&*p) {
c=json_unescape_char(*p);
p++;
}
if(o+1<sizeof(name))
name[o++]=c;
}
if(*p!='"')
return false;
p++;
name[o]='\0';
p=json_ws(p);
if(*p!=':')
return false;
p=json_ws(p+1);
if(strcmp(name,key)==0) {
o=0;
if(*p=='"') {
p++;
while(*p&&*p!='"') {
char c=*p++;
if(c=='\\'&&*p) {
c=json_unescape_char(*p);
p++;
}
if(o+1<cap)
out[o++]=c;
}
if(*p!='"')
return false;
} else {
while(*p&&*p!=','&&*p!='}'&&
!isspace((unsigned char)*p)) {
if(o+1<cap)
out[o++]=*p;
p++;
}
}
out[o]='\0';
return true;
}
end=json_skip_value(p,0);
if(!end)
return false;
p=end;
if(*p==',') {
p++;
continue;
}
if(*p=='}')
return false;
return false;
}
}
int monitor_print(const struct mesh_repo*repo) {
char*json=NULL;
char root[MESH_PATH_MAX]="-";
char branch[64]="main";
char head[MESH_HASH_HEX+1]="";
char tree[MESH_HASH_HEX+1]="";
char parent[MESH_HASH_HEX+1]="";
char files[32]="0", bytes[32]="0", snapshots[32]="0";
char chunks[32]="0", conflicts[32]="0";
char operation[32]="-";
char state[32]="ready";
char message[MESH_MSG_MAX]="";
char author_name[MESH_AUTHOR_MAX+1]="";
char author_email[MESH_EMAIL_MAX+1]="";
char status_path[MESH_PATH_MAX];
unsigned long long file_count, snap_count;
if(!repo||!repo->valid) {
errno=EINVAL;
return -1;
}
if(monitor_read(repo,&json, NULL)<0) {
struct mesh_status status;
if(errno!=ENOENT)
return -1;
if(monitor_collect(repo,&status,"check")<0)
return -1;
if(monitor_write(repo,&status)<0)
return -1;
if(monitor_read(repo,&json, NULL)<0)
return -1;
}
/* status.json is a cache. If HEAD moved without a successful status
* write (for example a failed monitor_collect), the cached file is
* stale; rebuild it instead of reporting an empty or old snapshot. */
char current_head[MESH_HASH_HEX+1]="";
char stored_head[MESH_HASH_HEX+1]="";
if(graph_head(repo,current_head)<0) {
free(json);
return -1;
}
if(!json_field(json,"head",stored_head,sizeof(stored_head))||
strcmp(stored_head,current_head)!=0) {
struct mesh_status status;
free(json);
json=NULL;
if(monitor_collect(repo,&status,"check")<0)
return -1;
if(monitor_write(repo,&status)<0)
return -1;
if(monitor_read(repo,&json, NULL)<0)
return -1;
}
if(!json_field(json,"repo_root", root, sizeof(root))||
!json_field(json,"branch", branch, sizeof(branch))||
!json_field(json,"state", state, sizeof(state))||
!json_field(json,"last_operation", operation, sizeof(operation))||
!json_field(json,"author_name", author_name, sizeof(author_name))||
!json_field(json,"author_email", author_email, sizeof(author_email))) {
struct mesh_status status;
free(json);
json=NULL;
if(monitor_collect(repo,&status,"check")<0||
monitor_write(repo,&status)<0||
monitor_read(repo,&json, NULL)<0)
return -1;
}
json_field(json,"repo_root", root, sizeof(root));
json_field(json,"branch", branch, sizeof(branch));
json_field(json,"head", head, sizeof(head));
json_field(json,"tree", tree, sizeof(tree));
json_field(json,"parent", parent, sizeof(parent));
json_field(json,"snapshots", snapshots, sizeof(snapshots));
json_field(json,"files", files, sizeof(files));
json_field(json,"bytes", bytes, sizeof(bytes));
json_field(json,"chunks", chunks, sizeof(chunks));
json_field(json,"conflicts", conflicts, sizeof(conflicts));
json_field(json,"last_operation", operation, sizeof(operation));
json_field(json,"state", state, sizeof(state));
json_field(json,"message", message, sizeof(message));
json_field(json,"author_name", author_name, sizeof(author_name));
json_field(json,"author_email", author_email, sizeof(author_email));
file_count=strtoull(files, NULL, 10);
snap_count=strtoull(snapshots, NULL, 10);
char pretty[32];
mesh_format_bytes(strtoull(bytes, NULL, 10), pretty, sizeof(pretty));
printf("Mesh repository : %s\n", root);
printf("Branch          : %s\n", branch);
printf("Head            : %s\n", head[0]?head:"(no snapshot)");
printf("Tree            : %s\n", tree[0]?tree:"-");
printf("Parent          : %s\n", parent[0]?parent:"-");
printf("Snapshots       : %llu\n", snap_count);
printf("Tracked files   : %llu (%s)\n", file_count, pretty);
printf("Chunks          : %s\n", chunks);
printf("Conflicts       : %s\n", conflicts);
printf("State           : %s\n", state);
printf("Last operation  : %s\n", operation);
printf("Message         : %s\n", message);
if(author_name[0]&&author_email[0])
printf("Author          : %s <%s>\n", author_name, author_email);
else
printf("Author          : -\n");
printf("Icon source     : "MESH_ICON "\n");
if(mesh_path_join(status_path, sizeof(status_path), repo->mesh_dir,
"monitor/status.json")<0)
snprintf(status_path, sizeof(status_path),"status.json");
printf("Status file     : %s\n", status_path);
free(json);
return 0;
}
