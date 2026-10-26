/*
* admin_audit.c - a local append-only record of operator actions.
* The log is line-oriented: timestamp|actor|message. It deliberately lives
* below .mesh/logs and is never part of a snapshot or a network transfer.
*/
#include "core.h"
static int audit_path(const struct mesh_repo*repo,char*out,size_t cap) {
return mesh_path_join(out, cap, repo->mesh_dir,"logs/admin.log");
}
static int audit_list(const struct mesh_repo*repo) {
char path[MESH_PATH_MAX];
char*buf=NULL;
size_t len=0;
if(audit_path(repo, path, sizeof(path))<0)
return -1;
if(mesh_read_text(path,&buf,&len)<0) {
if(errno==ENOENT) {
printf("No audit entries.\n");
return 0;
}
return -1;
}
printf("Admin audit log (%zu bytes)\n", len);
fputs(buf, stdout);
if(len>0&&buf[len-1]!='\n')
fputc('\n', stdout);
free(buf);
return 0;
}
static int audit_append(const struct mesh_repo*repo,const char*message) {
char dir[MESH_PATH_MAX];
char path[MESH_PATH_MAX];
char actor[MESH_AUTHOR_MAX+1];
char msg[MESH_MSG_MAX];
char line[MESH_MSG_MAX+MESH_AUTHOR_MAX+128];
int fd=-1;
int n;
int rc=-1;
if(!message||message[0]=='\0') {
errno=EINVAL;
return -1;
}
if(strlen(message)>=sizeof(msg)) {
errno=ENAMETOOLONG;
return -1;
}
mesh_sanitize_field(actor, sizeof(actor),
repo->author_name[0]?repo->author_name:"mesh");
mesh_sanitize_field(msg, sizeof(msg), message);
if(mesh_path_join(dir, sizeof(dir), repo->mesh_dir,"logs")<0||
mesh_mkdir_p(dir)<0||audit_path(repo, path, sizeof(path))<0)
return -1;
n=snprintf(line, sizeof(line),"%lld|%s|%s\n", mesh_now(), actor, msg);
if(n<0||(size_t)n>=sizeof(line)) {
errno=ENAMETOOLONG;
return -1;
}
fd=open(path,O_WRONLY|O_CREAT|O_APPEND|O_CLOEXEC|O_NOFOLLOW,0644);
if(fd<0)
return -1;
if(flock(fd, LOCK_EX)<0)
goto done;
if(mesh_write_all(fd, line, (size_t)n)<0||fsync(fd)<0)
goto done;
rc=0;
done: {
int saved=errno;
flock(fd, LOCK_UN);
close(fd);
errno=saved;
}
return rc;
}
int admin_audit_run(const struct mesh_repo*repo,const char*message) {
if(!repo||!repo->valid) {
errno=EINVAL;
return 1;
}
if(!message||message[0]=='\0') {
if(audit_list(repo)<0) {
fprintf(stderr,"mesh: admin audit: %s\n", strerror(errno));
return 1;
}
return 0;
}
if(audit_append(repo, message)<0) {
fprintf(stderr,"mesh: admin audit: %s\n", strerror(errno));
return 1;
}
printf("audit entry appended\n");
return 0;
}
