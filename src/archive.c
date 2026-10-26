/*
* archive.c - safe streaming extraction of forge tar.gz archives.
* The forge endpoints return a tar stream inside gzip. Names, sizes and
* timestamps come from an untrusted source, so every entry is normalized,
* checked with mesh_relpath_safe() and materialized below one open directory
* descriptor with O_NOFOLLOW. No path in the archive can reach .mesh, .git or
* the parent of the destination.
*/
#include "core.h"
#define TAR_BLOCK 512u
#define TAR_META_MAX (1024u*1024u)
#define TAR_MAX_BYTES (64ull*1024u*1024u*1024u)
#define TAR_MAX_ENTRIES 1000000u
enum tar_state {
TAR_HEADER,
TAR_DATA,
TAR_PAD,
TAR_META,
TAR_DONE
};
struct tar_ctx {
int root_fd;
enum tar_state state;
unsigned char hdr[TAR_BLOCK];
size_t hdr_len;
uint64_t data_left;
uint64_t data_size;
uint64_t pad_left;
int fd;
int parent_fd;
char temp[128];
char leaf[MESH_PATH_MAX];
mode_t mode;
bool have_time;
int64_t mtime_sec;
int64_t mtime_nsec;
unsigned char*meta;
size_t meta_len;
size_t meta_cap;
char meta_type;
char root[256];
size_t root_len;
bool root_set;
bool ended;
uint64_t total;
size_t entries;
char pax_path[MESH_PATH_MAX];
char pax_link[MESH_PATH_MAX];
char gnu_path[MESH_PATH_MAX];
char gnu_link[MESH_PATH_MAX];
bool pax_have_size;
uint64_t pax_size;
};
static int tar_octal(const unsigned char*field, size_t len, uint64_t*out) {
size_t i=0;
uint64_t value=0;
while(i<len&&(field[i]==' '||field[i]=='\0'))
i++;
if(i<len&&(field[i]&0x80u)) {
value=(uint64_t)(field[i]&0x7fu);
for(i++;i<len;i++) {
if(value>(UINT64_MAX-field[i])/256u) {
errno=EOVERFLOW;
return -1;
}
value=(value<<8)|field[i];
}
*out=value;
return 0;
}
for(;i<len&&field[i]!=' '&&field[i]!='\0';i++) {
if(field[i]<'0'||field[i]>'7')
return -1;
uint64_t digit=(uint64_t)(field[i]-'0');
if(value>(UINT64_MAX-digit)/8u) {
errno=EOVERFLOW;
return -1;
}
value=(value<<3)|digit;
}
*out=value;
return 0;
}
static int tar_name(const unsigned char*field, size_t len,
char*out, size_t cap) {
size_t n=0;
while(n<len&&field[n]!='\0')
n++;
if(n>=cap) {
errno=ENAMETOOLONG;
return -1;
}
memcpy(out, field, n);
out[n]='\0';
return 0;
}
static int tar_text(const unsigned char*data, size_t len,
char*out, size_t cap) {
size_t n=len;
while(n>0&&data[n-1]=='\0')
n--;
if(n==0||memchr(data,'\0',n)!=NULL||n>=cap) {
errno=EINVAL;
return -1;
}
memcpy(out, data, n);
out[n]='\0';
return 0;
}
static int tar_checksum(const unsigned char*hdr, uint64_t stored) {
unsigned sum=0, signed_sum=0;
for(size_t i=0;i<TAR_BLOCK;i++) {
unsigned char c=hdr[i];
if(i>=148&&i<156)
c=' ';
sum+=c;
signed_sum+=(unsigned)(int)(signed char)c;
}
return stored==sum||stored==signed_sum?0:-1;
}
static int tar_meta_apply(struct tar_ctx*ctx) {
size_t at=0;
if(ctx->meta_type=='g') {
free(ctx->meta);
ctx->meta=NULL;
ctx->meta_len=0;
return 0;
}
if(ctx->meta_type=='L'||ctx->meta_type=='K') {
if(tar_text(ctx->meta, ctx->meta_len,
ctx->meta_type=='L'?ctx->gnu_path:ctx->gnu_link,
MESH_PATH_MAX)<0)
return -1;
free(ctx->meta);
ctx->meta=NULL;
ctx->meta_len=0;
return 0;
}
while(at<ctx->meta_len) {
size_t len=0, start, rec_len;
size_t space;
char key[32];
const unsigned char*rec;
while(at+len<ctx->meta_len&&ctx->meta[at+len]>='0'&&
ctx->meta[at+len]<='9') {
len++;
if(len>20) {
errno=EINVAL;
return -1;
}
}
if(len==0) {
errno=EINVAL;
return -1;
}
char number[24];
if(len>=sizeof(number)) {
errno=EINVAL;
return -1;
}
memcpy(number, ctx->meta+at, len);
number[len]='\0';
unsigned long long parsed=strtoull(number, NULL, 10);
if(parsed==0||parsed>ctx->meta_len-at) {
errno=EINVAL;
return -1;
}
rec_len=(size_t)parsed;
if(len>=rec_len||ctx->meta[at+rec_len-1]!='\n') {
errno=EINVAL;
return -1;
}
space=len;
if(ctx->meta[at+space]!=' ') {
errno=EINVAL;
return -1;
}
start=space+1;
rec=ctx->meta+at+start;
size_t body=rec_len-len-2;
const unsigned char*eq=memchr(rec,'=',body);
if(!eq) {
errno=EINVAL;
return -1;
}
size_t key_len=(size_t)(eq-rec);
if(key_len==0||key_len>=sizeof(key)) {
errno=EINVAL;
return -1;
}
memcpy(key, rec, key_len);
key[key_len]='\0';
const char*value=(const char*)eq+1;
size_t value_len=body-key_len-1;
if(memchr(value,'\0',value_len)!=NULL) {
errno=EINVAL;
return -1;
}
if(strcmp(key,"path")==0) {
if(value_len>=sizeof(ctx->pax_path)) {
errno=ENAMETOOLONG;
return -1;
}
memcpy(ctx->pax_path, value, value_len);
ctx->pax_path[value_len]='\0';
} else if(strcmp(key,"linkpath")==0) {
if(value_len>=sizeof(ctx->pax_link)) {
errno=ENAMETOOLONG;
return -1;
}
memcpy(ctx->pax_link, value, value_len);
ctx->pax_link[value_len]='\0';
} else if(strcmp(key,"size")==0) {
char tmp[32];
if(value_len==0||value_len>=sizeof(tmp)) {
errno=EINVAL;
return -1;
}
memcpy(tmp, value, value_len);
tmp[value_len]='\0';
char*endp=NULL;
errno=0;
unsigned long long size=strtoull(tmp,&endp,10);
if(errno==ERANGE||!endp||endp==tmp||*endp!='\0'||tmp[0]=='-'||
size>(unsigned long long)UINT64_MAX) {
errno=EINVAL;
return -1;
}
ctx->pax_size=(uint64_t)size;
ctx->pax_have_size=true;
} else if(strcmp(key,"mtime")==0) {
char tmp[32];
if(value_len==0||value_len>=sizeof(tmp)) {
errno=EINVAL;
return -1;
}
memcpy(tmp, value, value_len);
tmp[value_len]='\0';
char*endp=NULL;
errno=0;
long long mtime=strtoll(tmp,&endp,10);
if(errno==ERANGE||!endp||endp==tmp||
(*endp!='\0'&&*endp!='.')||
(*endp=='.'&&(!endp[1]||strspn(endp+1,"0123456789")!=strlen(endp+1)))) {
errno=EINVAL;
return -1;
}
ctx->mtime_sec=(int64_t)mtime;
ctx->mtime_nsec=0;
ctx->have_time=true;
}
at+=rec_len;
}
free(ctx->meta);
ctx->meta=NULL;
ctx->meta_len=0;
return 0;
}
static int tar_path(struct tar_ctx*ctx, const char*name, char*out, size_t cap) {
char clean[MESH_PATH_MAX];
size_t len;
if(!name||name[0]=='\0') {
errno=EINVAL;
return -1;
}
if(name[0]=='/'||strlen(name)>=sizeof(clean)) {
errno=EINVAL;
return -1;
}
snprintf(clean, sizeof(clean),"%s", name);
len=strlen(clean);
while(len>1&&clean[len-1]=='/')
clean[--len]='\0';
if(strncmp(clean,"./",2)==0) {
memmove(clean, clean+2, strlen(clean+2)+1);
len=strlen(clean);
}
if(clean[0]=='\0'||strcmp(clean,".")==0) {
errno=EINVAL;
return -1;
}
if(!ctx->root_set) {
char*slash=strchr(clean,'/');
size_t name_len=slash?(size_t)(slash-clean):strlen(clean);
if(name_len==0||name_len>=sizeof(ctx->root)||
(name_len==1&&clean[0]=='.')||
(name_len==2&&clean[0]=='.'&&clean[1]=='.')) {
errno=EINVAL;
return -1;
}
for(size_t i=0;i<name_len;i++) {
unsigned char c=(unsigned char)clean[i];
if(c<0x20||c==0x7f) {
errno=EINVAL;
return -1;
}
}
memcpy(ctx->root, clean, name_len);
ctx->root[name_len]='\0';
ctx->root_len=name_len;
ctx->root_set=true;
return 1;
}
if(strncmp(clean, ctx->root, ctx->root_len)!=0) {
errno=EINVAL;
return -1;
}
if(clean[ctx->root_len]=='\0')
return 1;
if(clean[ctx->root_len]!='/') {
errno=EINVAL;
return -1;
}
const char*rel=clean+ctx->root_len+1;
if(rel[0]=='\0')
return 1;
if(!mesh_relpath_safe(rel)) {
errno=EINVAL;
return -1;
}
if(strlen(rel)>=cap) {
errno=ENAMETOOLONG;
return -1;
}
snprintf(out, cap,"%s", rel);
return 0;
}
static int tar_finish_file(struct tar_ctx*ctx) {
char leaf[MESH_PATH_MAX],temp[128];
int fd=ctx->fd, parent=ctx->parent_fd;
mode_t mode=ctx->mode&0777;
struct timespec ts[2];
bool have=ctx->have_time;
int64_t sec=ctx->mtime_sec, nsec=ctx->mtime_nsec;
int rc=-1;
if(mode==0)
mode=0644;
memcpy(leaf, ctx->leaf, strlen(ctx->leaf)+1);
memcpy(temp, ctx->temp, strlen(ctx->temp)+1);
ctx->fd=-1;
ctx->parent_fd=-1;
ts[0].tv_sec=(time_t)sec;
ts[0].tv_nsec=(long)nsec;
ts[1]=ts[0];
if(fchmod(fd, mode)<0)
goto done;
if(have&&futimens(fd, ts)<0)
goto done;
if(fsync(fd)<0)
goto done;
if(close(fd)<0) {
fd=-1;
goto done;
}
fd=-1;
if(renameat(parent,temp,parent,leaf)<0)
goto done;
rc=0;
done: {
int saved=errno;
if(fd>=0)
close(fd);
if(rc!=0) {
unlinkat(parent,temp,0);
close(parent);
errno=saved;
return -1;
}
if(mesh_fsync_dir(parent)<0) {
saved=errno;
close(parent);
errno=saved;
return -1;
}
close(parent);
errno=saved;
}
return rc;
}
static int tar_open_file(struct tar_ctx*ctx, const char*rel,
uint64_t size, mode_t mode, bool have_time,
int64_t sec, int64_t nsec) {
int rc=-1;
if(size>TAR_MAX_BYTES) {
errno=EFBIG;
return -1;
}
if(mesh_open_parent_at(ctx->root_fd, rel, true,&ctx->parent_fd,
ctx->leaf, sizeof(ctx->leaf))<0)
return -1;
for(unsigned i=0;i<32;i++) {
if(mesh_temp_name(ctx->temp, sizeof(ctx->temp), i)<0)
goto done;
ctx->fd=openat(ctx->parent_fd, ctx->temp,
O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
if(ctx->fd>=0)
break;
if(errno!=EEXIST)
goto done;
}
if(ctx->fd<0) {
errno=EEXIST;
goto done;
}
ctx->mode=mode;
ctx->have_time=have_time;
ctx->mtime_sec=sec;
ctx->mtime_nsec=nsec;
if(size==0) {
rc=tar_finish_file(ctx);
return rc;
}
ctx->data_left=size;
ctx->data_size=size;
ctx->state=TAR_DATA;
return 0;
done: {
int saved=errno;
if(ctx->fd>=0)
close(ctx->fd);
if(ctx->parent_fd>=0)
close(ctx->parent_fd);
ctx->fd=-1;
ctx->parent_fd=-1;
errno=saved;
}
return rc;
}
static int tar_open_dir(struct tar_ctx*ctx, const char*rel, mode_t mode,
bool have_time, int64_t sec, int64_t nsec) {
char leaf[MESH_PATH_MAX];
int parent=-1, fd=-1, rc=-1;
struct timespec ts[2];
if(mesh_open_parent_at(ctx->root_fd, rel, true,&parent,leaf,
sizeof(leaf))<0)
return -1;
if(mkdirat(parent, leaf, 0700)<0&&errno!=EEXIST)
goto done;
fd=openat(parent, leaf, O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
if(fd<0)
goto done;
mode=mode&0777;
if(mode==0)
mode=0755;
if(fchmod(fd, mode)<0)
goto done;
if(have_time) {
ts[0].tv_sec=(time_t)sec;
ts[0].tv_nsec=(long)nsec;
ts[1]=ts[0];
if(futimens(fd, ts)<0)
goto done;
}
if(mesh_fsync_dir(fd)<0||mesh_fsync_dir(parent)<0)
goto done;
rc=0;
done: {
int saved=errno;
if(fd>=0)
close(fd);
if(parent>=0)
close(parent);
errno=saved;
}
return rc;
}
static int tar_open_link(struct tar_ctx*ctx, const char*rel,
const char*target, mode_t mode, bool have_time,
int64_t sec, int64_t nsec) {
int parent=-1, rc;
if(!target||target[0]=='\0'||strlen(target)>=MESH_PATH_MAX) {
errno=EINVAL;
return -1;
}
rc=mesh_symlink_at_rel(ctx->root_fd, rel, target, mode);
if(rc<0||!have_time)
return rc;
if(mesh_open_parent_at(ctx->root_fd, rel, false,&parent,ctx->leaf,
sizeof(ctx->leaf))<0)
return -1;
rc=mesh_set_times_at(parent, ctx->leaf, sec, nsec, true);
close(parent);
return rc;
}
static int tar_meta_start(struct tar_ctx*ctx, uint64_t size) {
if(size>TAR_META_MAX) {
errno=EFBIG;
return -1;
}
if(size>0) {
ctx->meta=malloc((size_t)size);
if(!ctx->meta)
return -1;
} else {
ctx->meta=NULL;
}
ctx->meta_len=0;
ctx->meta_cap=(size_t)size;
ctx->data_size=size;
ctx->data_left=size;
ctx->state=TAR_META;
if(size==0) {
if(tar_meta_apply(ctx)<0)
return -1;
ctx->state=TAR_HEADER;
}
return 0;
}
static int tar_real(struct tar_ctx*ctx, const char*name, char type,
const char*link, uint64_t size, mode_t mode,
bool have_time, int64_t sec, int64_t nsec) {
char rel[MESH_PATH_MAX];
char target[MESH_PATH_MAX];
int path_rc;
path_rc=tar_path(ctx, name, rel, sizeof(rel));
if(path_rc<0)
return -1;
if(path_rc>0) {
if(type!='5'&&size>0) {
errno=EINVAL;
return -1;
}
ctx->state=TAR_PAD;
ctx->data_left=0;
ctx->pad_left=(TAR_BLOCK-(size%TAR_BLOCK))%TAR_BLOCK;
return 0;
}
ctx->entries++;
if(ctx->entries>TAR_MAX_ENTRIES) {
errno=EFBIG;
return -1;
}
if(ctx->pax_have_size)
size=ctx->pax_size;
if(type=='5')
return tar_open_dir(ctx, rel, mode, have_time, sec, nsec);
if(type=='0'||type=='\0'||type=='7')
return tar_open_file(ctx, rel, size, mode, have_time, sec, nsec);
if(type=='2') {
if(ctx->pax_link[0])
snprintf(target, sizeof(target),"%s", ctx->pax_link);
else if(ctx->gnu_link[0])
snprintf(target, sizeof(target),"%s", ctx->gnu_link);
else if(link[0])
snprintf(target, sizeof(target),"%s", link);
else {
errno=EINVAL;
return -1;
}
if(size>0) {
errno=EINVAL;
return -1;
}
return tar_open_link(ctx, rel, target, mode, have_time, sec, nsec);
}
if(type=='1') {
fprintf(stderr,"mesh: obtain: hard links are not supported\n");
errno=ENOTSUP;
return -1;
}
fprintf(stderr,"mesh: obtain: unsupported archive entry type '%c'\n", type);
errno=ENOTSUP;
return -1;
}
static int tar_header(struct tar_ctx*ctx) {
unsigned char type;
char name[MESH_PATH_MAX];
char prefix[156];
char link[101];
uint64_t size=0, mode=0, mtime=0, stored=0;
bool have_time=true;
if(ctx->hdr[0]=='\0') {
ctx->ended=true;
ctx->state=TAR_DONE;
return 0;
}
if(tar_octal(ctx->hdr+148, 8,&stored)<0||
tar_checksum(ctx->hdr, stored)<0) {
fprintf(stderr,"mesh: obtain: corrupt tar header\n");
errno=EINVAL;
return -1;
}
if(tar_name(ctx->hdr, 100, name, sizeof(name))<0||
tar_octal(ctx->hdr+124, 12,&size)<0||
tar_octal(ctx->hdr+100, 8,&mode)<0)
return -1;
if(tar_octal(ctx->hdr+136, 12,&mtime)<0) {
have_time=false;
mtime=0;
}
type=ctx->hdr[156];
if(type=='x'||type=='g'||type=='L'||type=='K') {
ctx->meta_type=(char)type;
return tar_meta_start(ctx, size);
}
if(ctx->hdr[257]=='u'&&ctx->hdr[258]=='s'&&ctx->hdr[259]=='t'&&
ctx->hdr[260]=='a'&&ctx->hdr[261]=='r') {
if(tar_name(ctx->hdr+345, 155, prefix, sizeof(prefix))<0)
return -1;
if(prefix[0]) {
char joined[MESH_PATH_MAX];
int n=snprintf(joined, sizeof(joined),"%s/%s", prefix, name);
if(n<0||(size_t)n>=sizeof(joined)) {
errno=ENAMETOOLONG;
return -1;
}
snprintf(name, sizeof(name),"%s", joined);
}
}
if(tar_name(ctx->hdr+157, 100, link, sizeof(link))<0)
return -1;
if(ctx->pax_path[0])
snprintf(name, sizeof(name),"%s", ctx->pax_path);
else if(ctx->gnu_path[0])
snprintf(name, sizeof(name),"%s", ctx->gnu_path);
if(ctx->pax_have_size)
size=ctx->pax_size;
int rc=tar_real(ctx, name, (char)type, link, size, (mode_t)mode,
have_time, (int64_t)mtime, 0);
ctx->pax_path[0]='\0';
ctx->pax_link[0]='\0';
ctx->gnu_path[0]='\0';
ctx->gnu_link[0]='\0';
ctx->pax_have_size=false;
if(rc<0)
return -1;
if(ctx->state==TAR_PAD&&ctx->pad_left==0)
ctx->state=TAR_HEADER;
return 0;
}
static int tar_feed(void*user, const unsigned char*data, size_t len) {
struct tar_ctx*ctx=user;
if(ctx->total>SIZE_MAX-len||ctx->total+len>TAR_MAX_BYTES) {
errno=EFBIG;
return -1;
}
ctx->total+=len;
while(len>0) {
size_t take;
if(ctx->state==TAR_DONE)
return 0;
if(ctx->state==TAR_HEADER) {
take=TAR_BLOCK-ctx->hdr_len;
if(take>len)
take=len;
memcpy(ctx->hdr+ctx->hdr_len, data, take);
ctx->hdr_len+=take;
data+=take;
len-=take;
if(ctx->hdr_len<TAR_BLOCK)
continue;
ctx->hdr_len=0;
if(tar_header(ctx)<0)
return -1;
continue;
}
if(ctx->state==TAR_PAD) {
take=ctx->pad_left<SIZE_MAX?(size_t)ctx->pad_left:SIZE_MAX;
if(take>len)
take=len;
data+=take;
len-=take;
ctx->pad_left-=take;
if(ctx->pad_left==0)
ctx->state=TAR_HEADER;
continue;
}
if(ctx->state==TAR_DATA||ctx->state==TAR_META) {
take=ctx->data_left<SIZE_MAX?(size_t)ctx->data_left:SIZE_MAX;
if(take>len)
take=len;
if(ctx->state==TAR_DATA) {
if(mesh_write_all(ctx->fd, data, take)<0)
return -1;
} else {
if(ctx->meta_len+take>ctx->meta_cap) {
errno=EINVAL;
return -1;
}
memcpy(ctx->meta+ctx->meta_len, data, take);
ctx->meta_len+=take;
}
data+=take;
len-=take;
ctx->data_left-=take;
if(ctx->data_left>0)
continue;
if(ctx->state==TAR_DATA) {
if(tar_finish_file(ctx)<0)
return -1;
} else if(tar_meta_apply(ctx)<0) {
return -1;
}
ctx->pad_left=(TAR_BLOCK-(ctx->data_size%TAR_BLOCK))%TAR_BLOCK;
ctx->state=ctx->pad_left?TAR_PAD:TAR_HEADER;
continue;
}
}
return 0;
}
static int tar_finish(struct tar_ctx*ctx) {
if(ctx->fd>=0||ctx->parent_fd>=0) {
errno=EINVAL;
return -1;
}
if (ctx->meta) {
free(ctx->meta);
ctx->meta = NULL;
}
if(ctx->state!=TAR_DONE||!ctx->ended||ctx->root_set==false) {
fprintf(stderr,"mesh: obtain: truncated or empty tar archive\n");
errno=EINVAL;
return -1;
}
return 0;
}
int mesh_archive_extract_gzip(const char*archive, const char*dest) {
struct tar_ctx ctx;
int archive_fd=-1, root_fd=-1, rc=-1;
if(!archive||!dest) {
errno=EINVAL;
return -1;
}
memset(&ctx, 0, sizeof(ctx));
ctx.fd=-1;
ctx.parent_fd=-1;
ctx.state=TAR_HEADER;
archive_fd=open(archive, O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
if(archive_fd<0)
return -1;
root_fd=open(dest, O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
if(root_fd<0)
goto done;
ctx.root_fd=root_fd;
if(mesh_inflate_gzip(archive_fd, tar_feed,&ctx)<0)
goto done;
if(tar_finish(&ctx)<0)
goto done;
rc=0;
done: {
int saved=errno;
if(ctx.fd>=0)
close(ctx.fd);
if(ctx.parent_fd>=0)
close(ctx.parent_fd);
free(ctx.meta);
if(root_fd>=0)
close(root_fd);
if(archive_fd>=0)
close(archive_fd);
errno=saved;
}
return rc;
}
