/*
* admin_support.c - shared metadata and accounting for the admin modules.
* The registry is static so the command surface is auditable at compile time.
* Hash sets only own their strings; callers own the repository handles.
*/
#include "core.h"
static const struct mesh_admin_module admin_modules[]={
{"admin","Admin command router and help."},
{"admin_support","Registry, hash set and store accounting."},
{"admin_doctor","Repository layout and metadata health checks."},
{"admin_verify","Object and manifest integrity verification."},
{"admin_inventory","Store, ref and conflict inventory."},
{"admin_gc","Unreachable object collection with dry-run first."},
{"admin_refs","Branch, tag and HEAD reference validation."},
{"admin_status","Refresh the passive monitor status document."},
{"admin_audit","Append and read the local admin audit log."},
{"admin_quota","Report and enforce a store byte quota."},
{"admin_config","Validate local configuration and identity values."},
{"admin_history","Validate history and reflog records."},
{"admin_export","Export the snapshot graph as JSON."},
};
#define ADMIN_MODULE_COUNT (sizeof(admin_modules)/sizeof(admin_modules[0]))
const struct mesh_admin_module*admin_registry_find(const char*name) {
if(!name)
return NULL;
for(size_t i=0;i<ADMIN_MODULE_COUNT;i++)
if(strcmp(admin_modules[i].name, name)==0)
return &admin_modules[i];
return NULL;
}
int admin_registry_print(FILE*out) {
if(!out)
return -1;
for(size_t i=0;i<ADMIN_MODULE_COUNT;i++)
fprintf(out,"%-16s %s\n", admin_modules[i].name,
admin_modules[i].summary);
return 0;
}
void admin_hash_set_init(struct mesh_admin_hash_set*set) {
if(set)
memset(set, 0, sizeof(*set));
}
void admin_hash_set_free(struct mesh_admin_hash_set*set) {
if(!set)
return;
for(size_t i=0;i<set->n;i++)
free(set->v[i]);
free(set->v);
memset(set, 0, sizeof(*set));
}
bool admin_hash_set_has(const struct mesh_admin_hash_set*set,
const char*hash) {
if(!set||!hash)
return false;
for(size_t i=0;i<set->n;i++)
if(strcmp(set->v[i], hash)==0)
return true;
return false;
}
int admin_hash_set_add(struct mesh_admin_hash_set*set,const char*hash) {
char**v;
char*copy;
size_t cap;
if(!set||!mesh_hash_valid(hash)) {
errno=EINVAL;
return -1;
}
if(admin_hash_set_has(set, hash))
return 0;
if(set->n==set->cap) {
cap=set->cap?set->cap*2:64;
if(cap>SIZE_MAX/sizeof(*v)) {
errno=ENOMEM;
return -1;
}
v=realloc(set->v, cap*sizeof(*v));
if(!v)
return -1;
set->v=v;
set->cap=cap;
}
copy=strdup(hash);
if(!copy)
return -1;
set->v[set->n++]=copy;
return 0;
}
static int scan_plain_dir(const char*path, size_t*count, uint64_t*bytes) {
DIR*dir;
struct dirent*entry;
int fd;
int rc=0;
if(!path||!count||!bytes) {
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
if(fstatat(fd, entry->d_name,&st, AT_SYMLINK_NOFOLLOW)<0) {
rc=-1;
break;
}
if(!S_ISREG(st.st_mode))
continue;
(*count)++;
*bytes+=(uint64_t)st.st_size;
}
if(rc==0&&errno!=0)
rc=-1;
if(closedir(dir)<0)
rc=-1;
return rc;
}
static int scan_object_dirs(const char*path, struct mesh_store_usage*usage) {
DIR*dir;
struct dirent*entry;
int fd;
int rc=0;
if(!path||!usage) {
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
char child[MESH_PATH_MAX];
if(strcmp(entry->d_name,".")==0||strcmp(entry->d_name,"..")==0)
continue;
if(fstatat(fd, entry->d_name,&st, AT_SYMLINK_NOFOLLOW)<0) {
rc=-1;
break;
}
if(!S_ISDIR(st.st_mode))
continue;
if(mesh_path_join(child, sizeof(child), path, entry->d_name)<0||
scan_plain_dir(child,&usage->objects,&usage->object_bytes)<0) {
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
int admin_store_usage(const struct mesh_repo*repo,
struct mesh_store_usage*usage) {
char objects[MESH_PATH_MAX];
char trees[MESH_PATH_MAX];
if(!repo||!usage) {
errno=EINVAL;
return -1;
}
memset(usage, 0, sizeof(*usage));
if(mesh_path_join(objects, sizeof(objects), repo->mesh_dir,
"store/objects")<0||mesh_path_join(trees, sizeof(trees),
repo->mesh_dir,"store/trees")<0)
return -1;
if(scan_object_dirs(objects, usage)<0)
return -1;
return scan_plain_dir(trees,&usage->trees,&usage->tree_bytes);
}
