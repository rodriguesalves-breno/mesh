/*
* reflog.c - append-only record of HEAD movements.
* The reflog is deliberately separate from .mesh/history. History describes
* immutable snapshots; the reflog describes local navigation between them,
* including branch switches that create no snapshot at all.
*/
#include "core.h"
#define MESH_REFLOG_LINE 2048u
static int reflog_path(const struct mesh_repo*repo, char*out, size_t cap) {
char dir[MESH_PATH_MAX];
if(!repo||!repo->valid||!out||cap==0) {
errno=EINVAL;
return -1;
}
if(mesh_path_join(dir, sizeof(dir), repo->mesh_dir,"logs")<0)
return -1;
if(mesh_mkdir_p(dir)<0)
return -1;
return mesh_path_join(out, cap, dir,"HEAD");
}
int reflog_append(const struct mesh_repo*repo, const char*old_hash,
const char*new_hash, const char*message) {
char path[MESH_PATH_MAX];
char old_clean[MESH_HASH_HEX+1];
char new_clean[MESH_HASH_HEX+1];
char author_name[MESH_AUTHOR_MAX+1];
char author_email[MESH_EMAIL_MAX+1];
char actor[MESH_AUTHOR_MAX+MESH_EMAIL_MAX+8];
char msg[MESH_MSG_MAX];
char line[MESH_REFLOG_LINE];
long long ts;
int fd, n;
if(!repo||!repo->valid) {
errno=EINVAL;
return -1;
}
if(old_hash&&*old_hash&&!mesh_hash_valid(old_hash)) {
errno=EINVAL;
return -1;
}
if(new_hash&&*new_hash&&!mesh_hash_valid(new_hash)) {
errno=EINVAL;
return -1;
}
mesh_sanitize_field(old_clean, sizeof(old_clean), old_hash?old_hash:"");
mesh_sanitize_field(new_clean, sizeof(new_clean), new_hash?new_hash:"");
mesh_sanitize_field(msg, sizeof(msg), message?message:"");
mesh_sanitize_field(author_name, sizeof(author_name), repo->author_name);
mesh_sanitize_field(author_email, sizeof(author_email),
repo->author_email);
if(mesh_author_name_valid(author_name)&&mesh_email_valid(author_email))
snprintf(actor, sizeof(actor),"%s <%s>", author_name, author_email);
else
snprintf(actor, sizeof(actor),"mesh");
ts=mesh_now_ns();
n=snprintf(line, sizeof(line),"%lld|%s|%s|%s|%s\n", ts, old_clean,
new_clean, actor, msg);
if(n<0||(size_t)n>=sizeof(line)) {
errno=ENAMETOOLONG;
return -1;
}
if(reflog_path(repo,path,sizeof(path))<0)
return -1;
fd=open(path,O_WRONLY|O_CREAT|O_APPEND|O_CLOEXEC|O_NOFOLLOW,0644);
if(fd<0)
return -1;
if(flock(fd, LOCK_EX)<0)
goto fail;
if(mesh_write_all(fd, line, (size_t)n)<0)
goto fail;
if(fsync(fd)<0)
goto fail;
if(flock(fd, LOCK_UN)<0)
goto fail;
if(close(fd)<0)
return -1;
return 0;
fail: {
int saved=errno;
(void)flock(fd, LOCK_UN);
close(fd);
errno=saved;
return -1;
}
}
struct reflog_entry {
long long timestamp;
const char*old_hash;
const char*new_hash;
const char*actor;
const char*message;
};
static bool reflog_parse(char*line, struct reflog_entry*out) {
char*field[5] = {NULL};
char*p=line;
char*end=NULL;
long long timestamp;
if(!line||!out)
return false;
for(size_t i=0;i<4;i++) {
char*sep=strchr(p,'|');
if(!sep)
return false;
*sep='\0';
field[i]=p;
p=sep+1;
}
field[4]=p;
if(strchr(field[4],'|'))
return false;
errno=0;
timestamp=strtoll(field[0],&end, 10);
if(errno!=0||end==field[0]||!end||*end!='\0')
return false;
if(field[1][0]&&!mesh_hash_valid(field[1]))
return false;
if(field[2][0]&&!mesh_hash_valid(field[2]))
return false;
out->timestamp=timestamp;
out->old_hash=field[1];
out->new_hash=field[2];
out->actor=field[3];
out->message=field[4];
return true;
}
int reflog_print(const struct mesh_repo*repo, FILE*out) {
char path[MESH_PATH_MAX];
char*buf=NULL;
size_t len=0;
char*save=NULL;
char*line;
struct reflog_entry*entries=NULL;
size_t n=0, cap=0;
if(!repo||!out) {
errno=EINVAL;
return -1;
}
if(mesh_path_join(path, sizeof(path), repo->mesh_dir,"logs/HEAD")<0)
return -1;
if(mesh_read_text(path,&buf,&len)<0) {
if(errno==ENOENT) {
fprintf(out,"No reflog entries.\n");
return 0;
}
return -1;
}
if(memchr(buf,'\0',len)!=NULL) {
free(buf);
errno=EINVAL;
return -1;
}
for(line=strtok_r(buf,"\n",&save);line;
line=strtok_r(NULL,"\n",&save)) {
struct reflog_entry entry;
if(!reflog_parse(line,&entry))
continue;
if(n==cap) {
size_t next=cap?cap*2:32;
struct reflog_entry*v;
if(next<cap||next>SIZE_MAX/sizeof(*v)) {
free(entries);
free(buf);
errno=ENOMEM;
return -1;
}
v=realloc(entries, next*sizeof(*v));
if(!v) {
free(entries);
free(buf);
return -1;
}
entries=v;
cap=next;
}
entries[n++]=entry;
}
if(n==0) {
fprintf(out,"No reflog entries.\n");
free(entries);
free(buf);
return 0;
}
fprintf(out,"Reflog (%zu entries):\n", n);
for(size_t i=n;i>0;i--) {
struct reflog_entry*entry=&entries[i-1];
char old_short[13], new_short[13], date[32];
struct tm tm;
time_t timestamp=(time_t)(entry->timestamp/1000000000LL);
mesh_short_hash(old_short, sizeof(old_short), entry->old_hash);
mesh_short_hash(new_short, sizeof(new_short), entry->new_hash);
snprintf(date, sizeof(date),"unknown");
if(localtime_r(&timestamp,&tm))
(void)strftime(date, sizeof(date),"%Y-%m-%d %H:%M:%S",&tm);
fprintf(out,"  %s -> %s  %s  %s", entry->old_hash[0]?old_short:"(empty)",
entry->new_hash[0]?new_short:"(empty)", date, entry->message);
if(entry->actor[0])
fprintf(out,"  (%s)", entry->actor);
fputc('\n',out);
}
free(entries);
free(buf);
return 0;
}
