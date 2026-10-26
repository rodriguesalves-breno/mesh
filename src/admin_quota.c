/*
* admin_quota.c - a byte ceiling for the immutable store.
* The quota is advisory metadata, not a filesystem enforcement hook. It gives
* operators and CI a single number to check before a large save or transfer.
*/
#include "core.h"
static int quota_path(const struct mesh_repo*repo,char*out,size_t cap) {
return mesh_path_join(out, cap, repo->mesh_dir,"admin/quota");
}
static int parse_bytes(const char*text,uint64_t*out) {
char*end=NULL;
unsigned long long value;
if(!text||!*text||text[0]=='+'||text[0]=='-') {
errno=EINVAL;
return -1;
}
errno=0;
value=strtoull(text,&end, 10);
if(errno==ERANGE||end==text||!end||*end!='\0') {
errno=EINVAL;
return -1;
}
*out=(uint64_t)value;
return 0;
}
static int write_limit(const struct mesh_repo*repo,uint64_t limit) {
char dir[MESH_PATH_MAX];
char path[MESH_PATH_MAX];
char text[64];
int n;
if(mesh_path_join(dir, sizeof(dir), repo->mesh_dir,"admin")<0||
mesh_mkdir_p(dir)<0||quota_path(repo, path, sizeof(path))<0)
return -1;
n=snprintf(text, sizeof(text),"%"PRIu64 "\n", limit);
if(n<0||(size_t)n>=sizeof(text)) {
errno=ENAMETOOLONG;
return -1;
}
return mesh_write_atomic(path, text, (size_t)n, 0644);
}
static int read_limit(const struct mesh_repo*repo,uint64_t*limit,bool*set) {
char path[MESH_PATH_MAX];
char*buf=NULL;
size_t len=0;
*set=false;
*limit=0;
if(quota_path(repo, path, sizeof(path))<0)
return -1;
if(mesh_read_text(path,&buf,&len)<0) {
if(errno==ENOENT)
return 0;
return -1;
}
mesh_trim_newline(buf);
if(buf[0]=='\0') {
free(buf);
return 0;
}
if(parse_bytes(buf, limit)<0) {
free(buf);
return -1;
}
free(buf);
*set=true;
return 0;
}
int admin_quota_run(const struct mesh_repo*repo,const char*limit) {
struct mesh_store_usage usage;
uint64_t configured=0;
uint64_t used;
bool set=false;
if(!repo||!repo->valid) {
errno=EINVAL;
return 1;
}
if(limit) {
if(parse_bytes(limit,&configured)<0||write_limit(repo, configured)<0) {
fprintf(stderr,"mesh: admin quota: invalid limit\n");
return 1;
}
printf("quota set: %"PRIu64 " byte(s)\n", configured);
return 0;
}
if(read_limit(repo,&configured,&set)<0||
admin_store_usage(repo,&usage)<0) {
fprintf(stderr,"mesh: admin quota: %s\n", strerror(errno));
return 1;
}
used=usage.object_bytes+usage.tree_bytes;
if(!set) {
printf("quota: not configured\n");
printf("used:  %"PRIu64 " byte(s)\n", used);
return 0;
}
printf("quota: %"PRIu64 " byte(s)\n", configured);
printf("used:  %"PRIu64 " byte(s)\n", used);
if(used>configured) {
printf("status: exceeded\n");
return 1;
}
printf("status: within limit\n");
return 0;
}
