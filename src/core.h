/*
* Mesh - unified core header.
* Every subsystem speaks through this header. The style is deliberately
* C17 + POSIX, no object orientation, no hidden allocation, no magic.
* Copyright (C) 2026 The Mesh Authors. GPL-2.0-only.
*/
#ifndef MESH_CORE_H
#define MESH_CORE_H
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/file.h>
#include <sys/time.h>
#include <limits.h>
#include <curl/curl.h>
/* Constants */
#define MESH_NAME "Mesh"
#define MESH_VERSION "1.0"
#define MESH_ICON "mesh_icon.svg"
#define MESH_HASH_LEN 64u /* BLAKE2b-512 bytes */
#define MESH_HASH_HEX (MESH_HASH_LEN*2u)
#define MESH_PATH_MAX 4096
#ifndef PATH_MAX
#define PATH_MAX MESH_PATH_MAX
#endif
#define MESH_CHUNK_SIZE (10u*1024u*1024u) /* 10 MiB */
#define MESH_CHUNK_WINDOW 4096u /* realignment */
#define MESH_LARGE_FILE (50u*1024u*1024u) /* 50 MiB */
#define MESH_IO_BUFFER (128u*1024u)
#define MESH_PATCH_LIMIT (4u*1024u*1024u)
#define MESH_MAX_THREADS 64u
#define MESH_MSG_MAX 512
#define MESH_AUTHOR_MAX 128
#define MESH_EMAIL_MAX 254
#define MESH_AUTH_MAX 4096
#define MESH_MARK_BEGIN "# >>> mesh path >>>"
#define MESH_MARK_END "# <<< mesh path <<<"
#define MESH_KIND_FILE 1u
#define MESH_KIND_SYMLINK 2u
#define MESH_MANIFEST_FIELDS 7u
/* Structural ceilings for manifests received from a peer. The 64 MiB HTTP
* body cap alone still allows hundreds of thousands of records, so the
* parser rejects pathological trees before allocating for them. */
#define MESH_MANIFEST_MAX_ENTRIES 262144u
#define MESH_MANIFEST_MAX_CHUNKS 1048576u
#define MESH_MANIFEST_MAX_CHUNKS_PER_FILE 262144u
/* Types */
struct mesh_repo {
char root[MESH_PATH_MAX];
char mesh_dir[MESH_PATH_MAX];
char branch[64];
/* Effective identity: local config overrides the global config. Empty
* fields mean the repository can still save, but the snapshot has no
* author metadata. */
char author_name[MESH_AUTHOR_MAX+1];
char author_email[MESH_EMAIL_MAX+1];
bool valid;
};
struct mesh_chunk {
char hash[MESH_HASH_HEX+1];
uint64_t offset;
uint64_t length;
};
struct mesh_chunk_list {
struct mesh_chunk*v;
size_t n;
size_t cap;
};
struct mesh_file_entry {
char*path; /* owned, relative to repository root */
char hash[MESH_HASH_HEX+1];
uint64_t size; /* file size or symlink target length */
int64_t mtime_sec; /* informational; nanoseconds are compared */
int64_t mtime_nsec;
uint32_t mode; /* permission bits (or 0777 for a symlink) */
uint32_t kind; /* MESH_KIND_FILE or MESH_KIND_SYMLINK */
uint64_t dev; /* populated by scan_dir(), not persisted */
uint64_t ino;
struct mesh_chunk*chunks; /* owned, NULL when the file is a blob */
size_t nchunks;
size_t chunk_capacity; /* allocation size for incremental manifest parsing */
};
struct mesh_file_list {
struct mesh_file_entry*v;
size_t n;
size_t cap;
};
struct mesh_changed {
size_t added;
size_t modified;
size_t deleted;
uint64_t bytes;
};
struct mesh_node {
char hash[MESH_HASH_HEX+1];
char tree[MESH_HASH_HEX+1];
char parent_1[MESH_HASH_HEX+1];
char parent_2[MESH_HASH_HEX+1];
char author_name[MESH_AUTHOR_MAX+1];
char author_email[MESH_EMAIL_MAX+1];
int64_t timestamp;
char*message; /* owned */
struct mesh_node*p1; /* resolved parent (older) */
struct mesh_node*p2;
struct mesh_node*next; /* next node towards the present */
};
struct mesh_graph {
struct mesh_node*head; /* newest snapshot, NULL when empty */
size_t count;
/* Sorted hash index used by graph_find(). It is built on load and
* invalidated after a commit; callers do not need to manage it. */
struct mesh_node**index;
size_t index_n;
};
struct mesh_hash_job {
char*path; /* owned by the caller; avoids 4 KiB per job */
uint64_t offset;
uint64_t length;
char hex[MESH_HASH_HEX+1];
int rc;
};
struct mesh_http {
char*data; /* NUL terminated response body */
size_t len;
size_t cap;
long status;
char remote_hash[MESH_HASH_HEX+1];
char remote_tree[MESH_HASH_HEX+1];
char remote_etag[256];
bool supports_cas;
bool truncated;
};
/* Optional transfer policy. A NULL pointer keeps the historical defaults:
* no redirects, the Mesh user agent and the standard timeout. */
struct mesh_transfer_stats {
long response_code;
long redirects;
double time_total;
double time_connect;
double time_starttransfer;
uint64_t bytes;
char url_effective[MESH_PATH_MAX];
char remote_ip[64];
char content_type[128];
};
struct mesh_download_options {
const char*const*headers; /* NULL terminated, never owned here */
const char*user_agent;
const char*referer;
const char*cookie; /* -b name=value */
const char*cookie_file; /* -b FILE */
const char*cookie_jar; /* -c FILE */
const char*proxy; /* -x URL */
const char*proxy_user; /* -U user:password */
const char*noproxy; /* --noproxy LIST */
struct mesh_transfer_stats*stats; /* optional, filled on return */
long connect_timeout;
long max_time;
uint64_t max_recv_speed;
uint64_t resume_from;
bool resume_from_set; /* -C N: truncate to N, then resume */
bool follow_redirects;
bool compressed;
bool insecure; /* -k: disable TLS verification (warned by the CLI) */
bool fail; /* -f: fail on HTTP errors in HEAD mode */
};
/* Opaque parsed .meshignore rules. See ignore.c. */
struct mesh_ignore;
struct mesh_admin_module {
const char*name;
const char*summary;
};
struct mesh_admin_hash_set {
char**v;
size_t n;
size_t cap;
};
struct mesh_store_usage {
size_t objects;
size_t trees;
uint64_t object_bytes;
uint64_t tree_bytes;
};
struct mesh_status {
char operation[32];
char message[MESH_MSG_MAX];
char author_name[MESH_AUTHOR_MAX+1];
char author_email[MESH_EMAIL_MAX+1];
char last_path[MESH_PATH_MAX];
char download_hash[MESH_HASH_HEX+1];
char head[MESH_HASH_HEX+1];
char parent[MESH_HASH_HEX+1];
char tree[MESH_HASH_HEX+1];
char branch[64];
size_t snapshots;
size_t files;
size_t chunks;
size_t conflicts;
uint64_t bytes_tracked;
uint64_t download_bytes;
uint64_t resumed_from;
struct mesh_changed changed;
};
/* Small, side-effect free helpers shared by every translation unit. */
/* static inline keeps the build free of an extra utility object. */
static inline long long mesh_now(void) {
return (long long)time(NULL);
}
static inline long long mesh_now_ns(void) {
struct timespec ts;
if(clock_gettime(CLOCK_REALTIME,&ts)<0)
return mesh_now()*1000000000LL;
return (long long)ts.tv_sec*1000000000LL+(long long)ts.tv_nsec;
}
static inline void mesh_trim_newline(char*s) {
size_t len;
if(!s)
return;
len=strlen(s);
while(len>0&&(s[len-1]=='\n'||s[len-1]=='\r'))
s[--len]='\0';
}
static inline void mesh_short_hash(char*dst, size_t cap, const char*hash) {
if(!dst||cap==0)
return;
snprintf(dst, cap,"%.12s", hash?hash:"");
}
/*
* Join base and rel into dst. Returns 0 or -1 with errno set. Never
* truncates silently: the caller gets ENAMETOOLONG instead of a path
* that points to the wrong file. That is the whole game.
*/
static inline int mesh_path_join(char*dst, size_t cap, const char*base,
const char*rel) {
int n;
if(!dst||cap==0)
return -1;
if(!base)
base=".";
if(!rel||rel[0]=='\0') {
n=snprintf(dst, cap,"%s", base);
} else if(base[0]=='\0'||(base[0]=='.'&&base[1]=='\0')) {
n=snprintf(dst, cap,"%s", rel);
} else {
size_t blen=strlen(base);
const char*sep=(base[blen-1]=='/')?"":"/";
n=snprintf(dst, cap,"%s%s%s", base, sep, rel);
}
if(n<0||(size_t)n>=cap) {
if(cap)
dst[0]='\0';
errno=ENAMETOOLONG;
return -1;
}
return 0;
}
static inline int mesh_mkdir_one(const char*path) {
struct stat st;
if(mkdir(path, 0755)==0)
return 0;
if(errno!=EEXIST)
return -1;
if(lstat(path,&st)<0)
return -1;
if(S_ISLNK(st.st_mode)) {
errno=ELOOP;
return -1;
}
if(!S_ISDIR(st.st_mode)) {
errno=ENOTDIR;
return -1;
}
return 0;
}
static inline int mesh_mkdir_p(const char*path) {
char tmp[MESH_PATH_MAX];
size_t len;
if(!path||path[0]=='\0'||strlen(path)>=sizeof(tmp)) {
errno=ENAMETOOLONG;
return -1;
}
memcpy(tmp, path, strlen(path)+1);
len=strlen(tmp);
while(len>1&&tmp[len-1]=='/')
tmp[--len]='\0';
char*p=tmp;
if(*p=='/')
p++;
for(;*p;p++) {
if(*p!='/')
continue;
*p='\0';
if(mesh_mkdir_one(tmp)<0)
return -1;
*p='/';
}
return mesh_mkdir_one(tmp);
}
static inline int mesh_json_escape(const char*in, char*out, size_t cap) {
size_t o=0;
if(!out||cap==0)
return -1;
if(!in)
in="";
while(*in) {
const unsigned char c=(unsigned char)*in;
const char*rep=NULL;
char tmp[8];
size_t need=1;
if(c<0x80) {
in++;
switch(c) {
case '"':
rep="\\\"";
break;
case '\\':
rep="\\\\";
break;
case '\n':
rep="\\n";
break;
case '\r':
rep="\\r";
break;
case '\t':
rep="\\t";
break;
default:
if(c<0x20) {
snprintf(tmp,sizeof(tmp),"\\u%04x",c);
rep=tmp;
}
break;
}
} else {
size_t n=0;
if((c&0xe0)==0xc0)
n=2;
else if((c&0xf0)==0xe0)
n=3;
else if((c&0xf8)==0xf0)
n=4;
bool valid=n>0;
for(size_t i=1;valid&&i<n;i++) {
unsigned char next=(unsigned char)in[i];
if(next=='\0'||(next&0xc0)!=0x80)
valid=false;
}
if(valid&&n==2&&c<0xc2)
valid=false;
if(valid&&n==3) {
unsigned char second=(unsigned char)in[1];
if((c==0xe0&&second<0xa0)||
(c==0xed&&second>=0xa0))
valid=false;
}
if(valid&&n==4) {
unsigned char second=(unsigned char)in[1];
if((c==0xf0&&second<0x90)||
(c>0xf4||(c==0xf4&&second>0x8f)))
valid=false;
}
if(valid) {
need=n;
in+=n;
} else {
snprintf(tmp,sizeof(tmp),"\\u%04x",c);
rep=tmp;
in++;
}
}
if(rep)
need=strlen(rep);
if(o+need>=cap) {
out[0]='\0';
errno=ENOSPC;
return -1;
}
if(rep)
memcpy(out+o,rep,need);
else
memcpy(out+o,in-need,need);
o+=need;
}
out[o]='\0';
return 0;
}
/*
* Replace bytes that would corrupt the pipe-delimited history format.
* History is a line protocol; one stray newline must never be able to
* forge a snapshot.
*/
static inline void mesh_sanitize_field(char*dst, size_t cap, const char*src) {
size_t o=0;
if(!dst||cap==0)
return;
if(!src)
src="";
for(;*src&&o+1<cap;src++) {
unsigned char c=(unsigned char)*src;
if(c=='|'||c=='\n'||c=='\r'||c=='\t'||c<0x20||c==0x7f)
c=' ';
dst[o++]=(char)c;
}
dst[o]='\0';
}
static inline bool mesh_author_name_valid(const char*name) {
size_t len;
bool visible=false;
if(!name)
return false;
len=strlen(name);
if(len==0||len>MESH_AUTHOR_MAX)
return false;
for(const unsigned char*p=(const unsigned char*)name;*p;p++) {
if(*p<0x20||*p==0x7f||*p=='|')
return false;
if(*p!=' ')
visible=true;
}
return visible;
}
static inline bool mesh_email_valid(const char*email) {
const char*at=NULL;
size_t count=0, len;
if(!email||email[0]=='\0')
return false;
len=strlen(email);
if(len>MESH_EMAIL_MAX)
return false;
for(const unsigned char*p=(const unsigned char*)email;*p;p++) {
if(*p<=0x20||*p==0x7f||*p=='|'||*p=='<'||*p=='>'||
*p=='/'||*p=='\\')
return false;
if(*p=='@') {
at=(const char*)p;
count++;
}
}
return count==1&&at!=email&&at[1]!='\0';
}
static inline bool mesh_config_key_valid(const char*key) {
return key&&(strcmp(key,"user.name")==0||
strcmp(key,"user.email")==0);
}
/*
* Branch and tag names share one namespace rule. A slash is rejected so a
* ref is always one regular file below .mesh/refs, which keeps listing and
* deletion immune to nested path tricks.
*/
static inline bool mesh_ref_name_valid(const char*name) {
size_t len;
if(!name)
return false;
len=strlen(name);
if(len==0||len>=64||strcmp(name,".")==0||strcmp(name,"..")==0)
return false;
if(name[0]=='-'||name[len-1]=='.'||name[len-1]=='/')
return false;
if(strstr(name,"..")||strstr(name,"@{")||strstr(name,".lock"))
return false;
for(const unsigned char*p=(const unsigned char*)name;*p;p++) {
if(*p<=0x20||*p==0x7f||*p=='/'||*p=='\\'||*p=='|'||*p=='~'||
*p=='^'||*p==':'||*p=='?'||*p=='*'||*p=='['||*p==']')
return false;
}
return true;
}
static inline int mesh_write_all(int fd,const void*data,size_t len);
static inline int mesh_path_no_symlink(const char*path);
/*
* Repository-relative paths must stay inside the repository. Absolute
* paths, empty components, "." and ".." are rejected before a single byte
* is written to disk. The version-control metadata directories are a
* reserved namespace: a remote manifest must never be able to write
* .git/hooks or .mesh/credentials. A remote manifest is untrusted input.
*/
static inline char mesh_ascii_lower(char c) {
return (c>='A'&&c<='Z')?(char)(c+('a'-'A')):c;
}
bool mesh_unicode_valid_utf8(const char*text);
int mesh_unicode_key(const char*text,char**key);
/* The transfer layer accepts only the two schemes libcurl is allowed to use. */
static inline bool mesh_http_url_ok(const char*url) {
if(!url)
return false;
if(strncmp(url,"https://", 8)==0&&url[8]!='\0')
return true;
if(strncmp(url,"http://", 7)==0&&url[7]!='\0')
return true;
return false;
}
static inline bool mesh_component_reserved(const char*p, size_t len) {
if(len==5&&p[0]=='.'&&
mesh_ascii_lower(p[1])=='m'&&mesh_ascii_lower(p[2])=='e'&&
mesh_ascii_lower(p[3])=='s'&&mesh_ascii_lower(p[4])=='h')
return true;
return len==4&&p[0]=='.'&&
mesh_ascii_lower(p[1])=='g'&&mesh_ascii_lower(p[2])=='i'&&
mesh_ascii_lower(p[3])=='t';
}
static inline bool mesh_relpath_safe(const char*path) {
const char*p;
size_t path_len;
if(!path||path[0]=='\0'||path[0]=='/'||
(path_len=strlen(path))>=MESH_PATH_MAX||path[path_len-1]=='/')
return false;
if(!mesh_unicode_valid_utf8(path))
return false;
for(const unsigned char*q=(const unsigned char*)path;*q;q++)
if(*q<0x20||*q==0x7f)
return false;
p=path;
while(*p) {
const char*slash=strchr(p,'/');
size_t len=slash?(size_t)(slash-p):strlen(p);
if(len==0||(len==1&&p[0]=='.')||
(len==2&&p[0]=='.'&&p[1]=='.')||
mesh_component_reserved(p, len))
return false;
if(!slash)
break;
p=slash+1;
}
return true;
}
/* Copy the POSIX timestamp without depending on a non-portable field name. */
static inline void mesh_stat_mtime(const struct stat*st, int64_t*sec,
int64_t*nsec) {
#if defined(__APPLE__)||defined(__NetBSD__)
*sec=(int64_t)st->st_mtimespec.tv_sec;
*nsec=(int64_t)st->st_mtimespec.tv_nsec;
#else
*sec=(int64_t)st->st_mtim.tv_sec;
*nsec=(int64_t)st->st_mtim.tv_nsec;
#endif
}
static inline int mesh_fsync_dir(int fd) {
return fsync(fd);
}
/* Open a parent directory one component at a time with O_NOFOLLOW. */
static inline int mesh_open_parent_at(int root_fd, const char*rel,
bool create, int*parent_fd, char*leaf, size_t leaf_cap) {
char work[MESH_PATH_MAX];
char*p;
int fd;
if(root_fd<0||!rel||!mesh_relpath_safe(rel)||!parent_fd||
!leaf||leaf_cap==0) {
errno=EINVAL;
return -1;
}
memcpy(work, rel, strlen(rel)+1);
fd=openat(root_fd,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
if(fd<0)
return -1;
p=work;
for(;;) {
char*slash=strchr(p,'/');
if(!slash) {
if(strlen(p)>=leaf_cap) {
close(fd);
errno=ENAMETOOLONG;
return -1;
}
memcpy(leaf, p, strlen(p)+1);
*parent_fd=fd;
return 0;
}
*slash='\0';
if(slash==p||strcmp(p,".")==0||strcmp(p,"..")==0) {
close(fd);
errno=EINVAL;
return -1;
}
int next=openat(fd,p,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
if(next<0&&errno==ENOENT&&create) {
if(mkdirat(fd,p,0755)<0&&errno!=EEXIST) {
int saved=errno;
close(fd);
errno=saved;
return -1;
}
next=openat(fd,p,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
}
if(next<0) {
int saved=errno;
close(fd);
errno=saved;
return -1;
}
close(fd);
fd=next;
p=slash+1;
}
}
static inline int mesh_temp_name(char*out, size_t cap, unsigned attempt) {
int n=snprintf(out, cap,".mesh-tmp-%ld-%u",(long)getpid(),attempt);
if(n<0||(size_t)n>=cap) {
errno=ENAMETOOLONG;
return -1;
}
return 0;
}
static inline int mesh_copy_fd_to_fd(int in, int out, uint64_t length) {
unsigned char buf[MESH_IO_BUFFER];
bool until_eof=(length==UINT64_MAX);
while(until_eof||length>0) {
size_t want=length<sizeof(buf)?(size_t)length:sizeof(buf);
ssize_t r=read(in, buf, want);
if(r<0) {
if(errno==EINTR)
continue;
return -1;
}
if(r==0) {
if(until_eof)
return 0;
errno=EIO;
return -1;
}
if(mesh_write_all(out, buf, (size_t)r)<0)
return -1;
if(!until_eof)
length-=(uint64_t)r;
}
return 0;
}
static inline int mesh_write_at_rel(int root_fd, const char*rel,
const void*data, size_t len, mode_t mode) {
char leaf[MESH_PATH_MAX],tmp[128]="";
int parent=-1,fd=-1,rc=-1;
if(!data&&len>0) {
errno=EINVAL;
return -1;
}
if(mesh_open_parent_at(root_fd, rel, true,&parent,leaf,sizeof(leaf))<0)
return -1;
for(unsigned i=0;i<32;i++) {
if(mesh_temp_name(tmp, sizeof(tmp), i)<0)
goto done;
fd=openat(parent,tmp,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|
O_NOFOLLOW,0600);
if(fd>=0)
break;
if(errno!=EEXIST)
goto done;
}
if(fd<0) {
errno=EEXIST;
goto done;
}
if(fchmod(fd, mode)<0||mesh_write_all(fd, data, len)<0||
fsync(fd)<0)
goto done;
if(close(fd)<0) {
fd=-1;
goto done;
}
fd=-1;
if(renameat(parent,tmp,parent,leaf)<0)
goto done;
if(mesh_fsync_dir(parent)<0)
goto done;
rc=0;
done: {
int saved=errno;
if(fd>=0)
close(fd);
if(rc!=0&&tmp[0])
unlinkat(parent,tmp,0);
close(parent);
errno=saved;
}
return rc;
}
static inline int mesh_copy_to_rel(int root_fd, const char*rel,
const char*src, mode_t mode) {
char leaf[MESH_PATH_MAX],tmp[128]="";
int parent=-1,in=-1,out=-1,rc=-1;
if(!src) {
errno=EINVAL;
return -1;
}
if(mesh_open_parent_at(root_fd, rel, true,&parent,leaf,sizeof(leaf))<0)
return -1;
in=open(src,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
if(in<0)
goto done;
for(unsigned i=0;i<32;i++) {
if(mesh_temp_name(tmp, sizeof(tmp), i)<0)
goto done;
out=openat(parent,tmp,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|
O_NOFOLLOW,0600);
if(out>=0)
break;
if(errno!=EEXIST)
goto done;
}
if(out<0) {
errno=EEXIST;
goto done;
}
if(fchmod(out, mode)<0||mesh_copy_fd_to_fd(in,out,UINT64_MAX)<0)
goto done;
if(fsync(out)<0||close(out)<0) {
out=-1;
goto done;
}
out=-1;
if(renameat(parent,tmp,parent,leaf)<0)
goto done;
if(mesh_fsync_dir(parent)<0)
goto done;
rc=0;
done: {
int saved=errno;
if(in>=0)
close(in);
if(out>=0)
close(out);
if(rc!=0&&tmp[0])
unlinkat(parent,tmp,0);
close(parent);
errno=saved;
}
return rc;
}
static inline int mesh_symlink_at_rel(int root_fd, const char*rel,
const char*target, mode_t mode) {
char leaf[MESH_PATH_MAX],tmp[128]="";
int parent=-1,rc=-1;
(void)mode;
if(!target||!target[0]||strlen(target)>=MESH_PATH_MAX) {
errno=EINVAL;
return -1;
}
if(mesh_open_parent_at(root_fd, rel, true,&parent,leaf,sizeof(leaf))<0)
return -1;
bool made=false;
for(unsigned i=0;i<32;i++) {
if(mesh_temp_name(tmp, sizeof(tmp), i)<0)
goto done;
if(symlinkat(target,parent,tmp)==0) {
made=true;
break;
}
if(errno!=EEXIST)
goto done;
}
if(!made) {
errno=EEXIST;
goto done;
}
if(renameat(parent,tmp,parent,leaf)<0)
goto done;
if(mesh_fsync_dir(parent)<0)
goto done;
rc=0;
done: {
int saved=errno;
if(rc!=0&&tmp[0])
unlinkat(parent,tmp,0);
close(parent);
errno=saved;
}
return rc;
}
static inline int mesh_set_times_at(int parent_fd, const char*leaf,
int64_t sec, int64_t nsec, bool nofollow) {
struct timespec ts[2];
if(nsec<0||nsec>=1000000000LL) {
errno=EINVAL;
return -1;
}
ts[0].tv_sec=(time_t)sec;
ts[0].tv_nsec=(long)nsec;
ts[1]=ts[0];
return utimensat(parent_fd,leaf,ts,
nofollow?AT_SYMLINK_NOFOLLOW:0);
}
static inline int mesh_write_all(int fd,const void*data,size_t len) {
const char*p=data;
while(len>0) {
ssize_t w=write(fd, p, len);
if(w<0) {
if(errno==EINTR)
continue;
return -1;
}
p+=w;
len-=(size_t)w;
}
return 0;
}
static inline int mesh_fsync_parent(const char*path) {
char dir[MESH_PATH_MAX];
char*slash;
int fd, rc;
if(!path||strlen(path)>=sizeof(dir)) {
errno=EINVAL;
return -1;
}
memcpy(dir,path,strlen(path)+1);
slash=strrchr(dir,'/');
if(!slash)
return 0;
if(slash==dir)
slash[1]='\0';
else
*slash='\0';
fd=open(dir,O_RDONLY|O_DIRECTORY|O_CLOEXEC);
if(fd<0)
return -1;
rc=fsync(fd);
if(close(fd)<0)
rc=-1;
return rc;
}
static inline int mesh_write_atomic(const char*path, const void*data,
size_t len, mode_t mode) {
char tmp[MESH_PATH_MAX];
int fd=-1, n;
if(mesh_path_no_symlink(path)<0)
return -1;
n=snprintf(tmp, sizeof(tmp),"%s.tmp.XXXXXX", path);
if(!path||n<0||(size_t)n>=sizeof(tmp)) {
errno=ENAMETOOLONG;
return -1;
}
fd=mkstemp(tmp);
if(fd<0)
return -1;
if(fchmod(fd, mode)<0)
goto fail;
if(mesh_write_all(fd, data, len)<0)
goto fail;
if(fsync(fd)<0)
goto fail;
if(close(fd)<0) {
fd=-1;
goto fail;
}
fd=-1;
if(rename(tmp, path)<0)
goto fail;
return mesh_fsync_parent(path);
fail: {
int saved=errno;
if(fd>=0)
close(fd);
unlink(tmp);
errno=saved;
return -1;
}
}
static inline int mesh_read_text(const char*path, char**out, size_t*out_len) {
int fd;
struct stat st;
char*buf;
size_t len, got=0;
if(!out)
return -1;
*out=NULL;
if(out_len)
*out_len=0;
fd=open(path, O_RDONLY|O_NOFOLLOW);
if(fd<0)
return -1;
if(fstat(fd,&st)<0)
goto fail;
if(!S_ISREG(st.st_mode)||st.st_size<0) {
errno=EINVAL;
goto fail;
}
if((uint64_t)st.st_size>(uint64_t)(64u*1024u*1024u)) {
errno=EFBIG;
goto fail;
}
len=(size_t)st.st_size;
buf=malloc(len+1);
if(!buf)
goto fail;
while(got<len) {
ssize_t r=read(fd, buf+got, len-got);
if(r<0) {
if(errno==EINTR)
continue;
free(buf);
goto fail;
}
if(r==0)
break;
got+=(size_t)r;
}
close(fd);
buf[got]='\0';
*out=buf;
if(out_len)
*out_len=got;
return 0;
fail: {
int saved=errno;
close(fd);
errno=saved;
return -1;
}
}
static inline void mesh_strlcpy(char*dst, size_t cap, const char*src) {
size_t len;
if(!dst||cap==0)
return;
if(!src)
src="";
len=strlen(src);
if(len>=cap)
len=cap-1;
memcpy(dst, src, len);
dst[len]='\0';
}
/* One byte formatter for CLI output, monitor JSON and admin reports. */
static inline void mesh_format_bytes(uint64_t bytes,char*out,size_t cap) {
static const char*units[]={"B","KiB","MiB","GiB","TiB"};
double value=(double)bytes;
size_t unit=0;
if(!out||cap==0)
return;
while(value>=1024.0&&unit+1<sizeof(units)/sizeof(units[0])) {
value/=1024.0;
unit++;
}
if(unit==0)
snprintf(out, cap,"%"PRIu64 " B", bytes);
else
snprintf(out, cap,"%.1f %s", value, units[unit]);
}
static inline bool mesh_hash_valid(const char*hash) {
if(!hash||strlen(hash)!=MESH_HASH_HEX)
return false;
for(size_t i=0;i<MESH_HASH_HEX;i++) {
char c=hash[i];
if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))
return false;
}
return true;
}
/* Reject symlinks in every existing component of a destination path. */
static inline int mesh_path_no_symlink(const char*path) {
char probe[MESH_PATH_MAX];
char*p;
if(!path||path[0]=='\0'||strlen(path)>=sizeof(probe)) {
errno=EINVAL;
return -1;
}
memcpy(probe, path, strlen(path)+1);
p=probe;
if(*p=='/')
p++;
for(;;) {
struct stat st;
char*slash=strchr(p,'/');
if(slash)
*slash='\0';
errno=0;
if(probe[0]!='\0'&&lstat(probe,&st)==0&&
S_ISLNK(st.st_mode)) {
errno=ELOOP;
return -1;
}
if(errno!=0&&errno!=ENOENT&&errno!=ENOTDIR)
return -1;
if(!slash)
break;
*slash='/';
p=slash+1;
if(*p=='\0')
break;
}
return 0;
}
static inline int mesh_repo_lock(const struct mesh_repo*repo) {
char path[MESH_PATH_MAX];
int fd;
if(!repo||!repo->valid||
mesh_path_join(path, sizeof(path), repo->mesh_dir,"lock")<0)
return -1;
fd=open(path,O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0644);
if(fd<0)
return -1;
if(flock(fd, LOCK_EX)<0) {
int saved=errno;
close(fd);
errno=saved;
return -1;
}
return fd;
}
static inline int mesh_object_path(const struct mesh_repo*repo,
const char*hash, char*out, size_t cap) {
size_t dlen;
size_t need;
if(!repo||!out||!mesh_hash_valid(hash)) {
errno=EINVAL;
if(out&&cap)
out[0]='\0';
return -1;
}
dlen=strlen(repo->mesh_dir);
/* "/store/objects/" (15) + 2 prefix + "/" + 126 suffix */
need=dlen+15+2+1+(MESH_HASH_HEX-2);
if(need+1>cap) {
errno=ENAMETOOLONG;
out[0]='\0';
return -1;
}
memcpy(out, repo->mesh_dir, dlen);
memcpy(out+dlen,"/store/objects/", 15);
memcpy(out+dlen+15, hash, 2);
out[dlen+17]='/';
memcpy(out+dlen+18, hash+2, MESH_HASH_HEX-2);
out[dlen+18+MESH_HASH_HEX-2]='\0';
return 0;
}
static inline int mesh_object_exists(const struct mesh_repo*repo,
const char*hash) {
char path[MESH_PATH_MAX];
struct stat st;
if(!mesh_hash_valid(hash)) {
errno=EINVAL;
return 0;
}
if(mesh_object_path(repo, hash, path, sizeof(path))<0)
return 0;
return stat(path,&st)==0&&S_ISREG(st.st_mode);
}
static inline int mesh_copy_fd_to_stream(int fd, uint64_t length, FILE*out) {
unsigned char buf[MESH_IO_BUFFER];
while(length>0) {
size_t want=length<sizeof(buf)?(size_t)length:sizeof(buf);
ssize_t r=read(fd, buf, want);
if(r<0) {
if(errno==EINTR)
continue;
return -1;
}
if(r==0) {
errno=EIO;
return -1;
}
if(fwrite(buf, 1, (size_t)r, out)!=(size_t)r)
return -1;
length-=(uint64_t)r;
}
return 0;
}
static inline int mesh_copy_path_to_path(const char*src, const char*dst,
mode_t mode) {
int in=-1, out=-1, rc=-1;
char tmp[MESH_PATH_MAX];
unsigned char buf[MESH_IO_BUFFER];
int n;
if(mesh_path_no_symlink(dst)<0)
return -1;
n=snprintf(tmp, sizeof(tmp),"%s.tmp.XXXXXX", dst);
if(n<0||(size_t)n>=sizeof(tmp)) {
errno=ENAMETOOLONG;
return -1;
}
in=open(src, O_RDONLY|O_NOFOLLOW);
if(in<0)
return -1;
out=mkstemp(tmp);
if(out<0)
goto done;
if(fchmod(out, mode)<0)
goto done;
for(;;) {
ssize_t r=read(in, buf, sizeof(buf));
ssize_t off=0;
if(r<0) {
if(errno==EINTR)
continue;
goto done;
}
if(r==0)
break;
while(off<r) {
ssize_t w=write(out, buf+off, (size_t)(r-off));
if(w<0) {
if(errno==EINTR)
continue;
goto done;
}
off+=w;
}
}
if(fsync(out)<0)
goto done;
if(close(out)<0) {
out=-1;
goto done;
}
out=-1;
if(rename(tmp, dst)<0)
goto done;
if(mesh_fsync_parent(dst)<0)
goto done;
rc=0;
done: {
int saved=errno;
if(in>=0)
close(in);
if(out>=0)
close(out);
if(rc!=0)
unlink(tmp);
errno=saved;
}
return rc;
}
static inline int mesh_self_dir(char*out, size_t cap) {
ssize_t n;
if(!out||cap<2)
return -1;
n=readlink("/proc/self/exe", out, cap-1);
if(n>0&&(size_t)n<cap) {
char*slash;
out[n]='\0';
slash=strrchr(out,'/');
if(slash) {
if(slash==out)
slash[1]='\0';
else
*slash='\0';
return 0;
}
}
if(getcwd(out, cap))
return 0;
out[0]='\0';
return -1;
}
/* cli.c */
int cli_main(int argc, char**argv);
/* blake2b.c */
struct mesh_hash_ctx {
uint64_t h[8];
uint64_t t[2];
uint64_t f[2];
uint8_t buf[128];
size_t buflen;
size_t outlen;
};
void mesh_hash_init(struct mesh_hash_ctx*ctx);
void mesh_hash_update(struct mesh_hash_ctx*ctx, const void*data, size_t len);
void mesh_hash_final(struct mesh_hash_ctx*ctx, char hex[MESH_HASH_HEX+1]);
void blake2b_hash_buffer(const void*data, size_t len,
uint8_t out[MESH_HASH_LEN]);
int blake2b_hash_string(const char*s, char hex[MESH_HASH_HEX+1]);
int blake2b_hash_file(const char*path, char hex[MESH_HASH_HEX+1]);
int blake2b_hash_fd(int fd, uint64_t offset, uint64_t length,
char hex[MESH_HASH_HEX+1]);
int blake2b_hash_fd_layout(int fd, uint64_t size,
struct mesh_chunk*chunks, size_t nchunks,
char full_hex[MESH_HASH_HEX+1]);
int blake2b_hash_jobs(struct mesh_hash_job*jobs, size_t count,
unsigned threads);
unsigned mesh_threads(void);
/* env.c */
int mesh_repo_open(struct mesh_repo*repo);
int mesh_repo_init(struct mesh_repo*repo);
int mesh_repo_init_at(struct mesh_repo*repo, const char*root);
int mesh_env_install(const struct mesh_repo*repo, const char*bin_dir,
unsigned*files_touched);
int mesh_write_vscode(const struct mesh_repo*repo);
int mesh_config_branch(const struct mesh_repo*repo, char*out, size_t cap);
int mesh_config_get(const struct mesh_repo*repo, bool global,
const char*key, char*out, size_t cap);
int mesh_config_set(const struct mesh_repo*repo, bool global,
const char*key, const char*value);
int mesh_config_list(const struct mesh_repo*repo, bool global, FILE*out);
int mesh_identity_load(struct mesh_repo*repo);
/* chunking.c */
void chunk_list_init(struct mesh_chunk_list*list);
void chunk_list_free(struct mesh_chunk_list*list);
int chunk_scan_fd(int fd, uint64_t size, struct mesh_chunk_list*out,
char full_hash[MESH_HASH_HEX+1]);
int object_store_fd(const struct mesh_repo*repo, const char*hash,
int fd, uint64_t offset, uint64_t length);
int object_store_blob(const struct mesh_repo*repo, const char*hash,
const void*data, size_t len);
int object_verify(const struct mesh_repo*repo, const char*hash,
uint64_t*size);
int object_read(const struct mesh_repo*repo, const char*hash, char**out,
size_t*out_len);
/* graph.c */
void file_list_init(struct mesh_file_list*list);
void file_list_free(struct mesh_file_list*list);
struct mesh_file_entry*file_list_add(struct mesh_file_list*list,
const char*path, uint64_t size,
int64_t mtime_sec, int64_t mtime_nsec,
uint32_t mode, uint32_t kind);
void file_list_sort(struct mesh_file_list*list);
const struct mesh_file_entry*manifest_find(const struct mesh_file_list*list,
const char*path);
int manifest_save(const struct mesh_repo*repo,
const struct mesh_file_list*list,
char tree_hash[MESH_HASH_HEX+1]);
int manifest_load(const struct mesh_repo*repo, const char*tree_hash,
struct mesh_file_list*out);
int manifest_stream_path(const struct mesh_repo*repo, const char*tree_hash,
const char*path, const char*dest, mode_t mode);
int manifest_materialize(const struct mesh_repo*repo,
const char*tree_hash, const char*path);
int manifest_materialize_entry(const struct mesh_repo*repo,
const struct mesh_file_entry*entry);
int manifest_diff(const struct mesh_file_list*old_list,
const struct mesh_file_list*new_list,
struct mesh_changed*out);
size_t manifest_chunk_count(const struct mesh_file_list*list);
void graph_init(struct mesh_graph*graph);
void graph_free(struct mesh_graph*graph);
int graph_load(const struct mesh_repo*repo, struct mesh_graph*graph);
int graph_commit(const struct mesh_repo*repo, struct mesh_graph*graph,
const char*tree, const char*p1, const char*p2,
const char*branch, const char*message,
char out_hash[MESH_HASH_HEX+1]);
struct mesh_node*graph_find(const struct mesh_graph*graph,
const char*hash);
int graph_resolve(const struct mesh_graph*graph, const char*arg,
const char*head, char out[MESH_HASH_HEX+1]);
void graph_print(const struct mesh_graph*graph, FILE*out);
int graph_head(const struct mesh_repo*repo, char out[MESH_HASH_HEX+1]);
int graph_set_head(const struct mesh_repo*repo, const char*branch,
const char*hash);
/* conflict.c */
int conflict_stage(const struct mesh_repo*repo, const char*relpath,
const char*alpha_src, const char*beta_src);
int conflict_stage_typed(const struct mesh_repo*repo,const char*relpath,
const char*alpha_src,const char*beta_src,uint32_t alpha_kind,
mode_t alpha_mode,uint32_t beta_kind,mode_t beta_mode);
int conflict_count(const struct mesh_repo*repo);
int conflict_list(const struct mesh_repo*repo);
int conflict_choose(const struct mesh_repo*repo, const char*variant,
size_t*resolved);
/* monitor.c */
int monitor_collect(const struct mesh_repo*repo, struct mesh_status*status,
const char*operation);
int monitor_write(const struct mesh_repo*repo,
const struct mesh_status*status);
int monitor_read(const struct mesh_repo*repo, char**json, size_t*len);
int monitor_print(const struct mesh_repo*repo);
/* bridge.c */
int bridge_write_patch(const struct mesh_repo*repo, const char*old_tree,
const char*new_tree, const char*dest);
/* git_coexist.c */
int git_coexist_report(const struct mesh_repo*repo);
/* ignore.c */
int ignore_load(const struct mesh_repo*repo, struct mesh_ignore**out);
void ignore_free(struct mesh_ignore*rules);
bool ignore_match(const struct mesh_ignore*rules, const char*rel,
bool is_dir);
bool ignore_can_prune(const struct mesh_ignore*rules);
int ignore_list(const struct mesh_repo*repo, FILE*out);
/* branch.c */
int branch_list(const struct mesh_repo*repo, FILE*out);
int branch_create(const struct mesh_repo*repo, const char*name,
const char*start);
int branch_switch(struct mesh_repo*repo, const char*name);
int branch_delete(const struct mesh_repo*repo, const char*name);
/* tag.c */
int tag_list(const struct mesh_repo*repo, FILE*out);
int tag_create(const struct mesh_repo*repo, const char*name,
const char*target);
int tag_delete(const struct mesh_repo*repo, const char*name);
/* reflog.c */
int reflog_append(const struct mesh_repo*repo, const char*old_hash,
const char*new_hash, const char*message);
int reflog_print(const struct mesh_repo*repo, FILE*out);
/* clean.c */
int clean_run(const struct mesh_repo*repo, bool force, bool include_ignored,
size_t*removed, uint64_t*bytes, size_t*failed);
/* obtain.c */
int mesh_obtain_is_repo_url(const char*url);
int mesh_obtain_plan(const char*url, const char*ref,
const char*archive, char*archive_url, size_t archive_cap,
char*dest_name, size_t dest_cap, char*authority, size_t authority_cap);
int mesh_obtain_stage(const char*url, const char*ref,
const char*archive, const char*dest,
char*stage, size_t stage_cap, char*final, size_t final_cap,
uint64_t*bytes);
int mesh_obtain_stage_ex(const char*url, const char*ref,
const char*archive, const char*dest,
const struct mesh_download_options*download,
char*stage, size_t stage_cap, char*final, size_t final_cap,
uint64_t*bytes);
int mesh_obtain_commit(const char*stage, const char*final);
void mesh_obtain_abort(const char*stage);
/* inflate.c */
typedef int (*mesh_sink_fn)(void*user, const unsigned char*data, size_t len);
int mesh_inflate_gzip(int fd, mesh_sink_fn sink, void*user);
/* archive.c */
int mesh_archive_extract_gzip(const char*archive, const char*dest);
/* network.c */
int net_init(void);
void net_shutdown(void);
int net_download(const struct mesh_repo*repo, const char*url,
const char*dest, uint64_t*downloaded, uint64_t*resumed,
char hash[MESH_HASH_HEX+1]);
int net_download_ex(const struct mesh_repo*repo, const char*url,
const char*dest, const struct mesh_download_options*options,
uint64_t*downloaded, uint64_t*resumed,
char hash[MESH_HASH_HEX+1]);
int net_stream_ex(const struct mesh_repo*repo, const char*url,
const struct mesh_download_options*options,
uint64_t*downloaded, char hash[MESH_HASH_HEX+1]);
int net_head_ex(const struct mesh_repo*repo, const char*url,
const struct mesh_download_options*options, FILE*out);
int net_get(const struct mesh_repo*repo, const char*url,
const char*const*headers, struct mesh_http*resp);
int net_post_json(const struct mesh_repo*repo, const char*url,
const char*json, struct mesh_http*resp);
int net_post_json_if_match(const struct mesh_repo*repo, const char*url,
const char*json, const char*etag, struct mesh_http*resp);
int net_put_file(const struct mesh_repo*repo, const char*url,
const char*src);
int net_auth_header(const struct mesh_repo*repo, const char*url,
char*out, size_t cap);
void mesh_http_init(struct mesh_http*resp);
void mesh_http_free(struct mesh_http*resp);
const char*net_error(CURLcode code);
/* admin.c */
int admin_main(int argc,char**argv);
/* admin_support.c */
const struct mesh_admin_module*admin_registry_find(const char*name);
int admin_registry_print(FILE*out);
void admin_hash_set_init(struct mesh_admin_hash_set*set);
void admin_hash_set_free(struct mesh_admin_hash_set*set);
bool admin_hash_set_has(const struct mesh_admin_hash_set*set,
const char*hash);
int admin_hash_set_add(struct mesh_admin_hash_set*set,const char*hash);
int admin_store_usage(const struct mesh_repo*repo,
struct mesh_store_usage*usage);
/* admin_doctor.c */
int admin_doctor_run(const struct mesh_repo*repo);
/* admin_verify.c */
int admin_verify_run(const struct mesh_repo*repo);
/* admin_inventory.c */
int admin_inventory_run(const struct mesh_repo*repo,bool json);
/* admin_gc.c */
int admin_gc_run(const struct mesh_repo*repo,bool apply);
/* admin_refs.c */
int admin_refs_run(const struct mesh_repo*repo);
/* admin_status.c */
int admin_status_run(const struct mesh_repo*repo);
/* admin_audit.c */
int admin_audit_run(const struct mesh_repo*repo,const char*message);
/* admin_quota.c */
int admin_quota_run(const struct mesh_repo*repo,const char*limit);
/* admin_config.c */
int admin_config_run(const struct mesh_repo*repo);
/* admin_history.c */
int admin_history_run(const struct mesh_repo*repo);
/* admin_export.c */
int admin_export_run(const struct mesh_repo*repo,const char*dest);
#endif /* MESH_CORE_H */
