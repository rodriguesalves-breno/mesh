/*
* chunking.c - pragmatic content-defined chunking and the immutable
* object store.
* Files below 50 MiB are one blob. Above that we cut stable 10 MiB
* blocks and, only when a boundary looks wrong, look ahead at most 4 KiB
* for a NUL or a newline. No rolling polynomials, no academic windows:
* just predictable I/O that a spinning disk and a cheap SSD both like.
*/
#include "core.h"
void chunk_list_init(struct mesh_chunk_list*list) {
if(!list)
return;
memset(list, 0, sizeof(*list));
}
void chunk_list_free(struct mesh_chunk_list*list) {
if(!list)
return;
free(list->v);
list->v=NULL;
list->n=0;
list->cap=0;
}
static struct mesh_chunk*chunk_push(struct mesh_chunk_list*list,
uint64_t offset, uint64_t length) {
struct mesh_chunk*chunk;
if(list->n==list->cap) {
size_t cap;
struct mesh_chunk*v;
if(list->cap>SIZE_MAX/2) {
errno=ENOMEM;
return NULL;
}
cap=list->cap?list->cap*2:8;
if(cap>SIZE_MAX/sizeof(*v)) {
errno=ENOMEM;
return NULL;
}
v=realloc(list->v, cap*sizeof(*v));
if(!v)
return NULL;
list->v=v;
list->cap=cap;
}
chunk=&list->v[list->n++];
memset(chunk, 0, sizeof(*chunk));
chunk->offset=offset;
chunk->length=length;
return chunk;
}
/*
* Return the end of the block starting at 'off'. The nominal edge is
* 10 MiB. If bytes remain beyond it, a short forward scan looks for a
* natural boundary so that an inserted byte does not cascade through
* every following chunk.
*/
static int chunk_boundary(int fd, uint64_t off, uint64_t size, uint64_t*end) {
uint64_t nominal=off+MESH_CHUNK_SIZE;
uint8_t window[MESH_CHUNK_WINDOW];
ssize_t got;
if(nominal>=size) {
*end=size;
return 0;
}
*end=nominal;
do {
got=pread(fd, window, sizeof(window), (off_t)nominal);
} while(got<0&&errno==EINTR);
if(got<0)
return -1;
for(ssize_t i=0;i<got;i++) {
if(window[i]=='\n'||window[i]=='\0') {
*end=nominal+(uint64_t)i+1;
if(*end>size)
*end=size;
break;
}
}
return 0;
}
int chunk_scan_fd(int fd, uint64_t size, struct mesh_chunk_list*out,
char full_hash[MESH_HASH_HEX+1]) {
uint64_t off=0;
if(fd<0||!out||!full_hash) {
errno=EINVAL;
return -1;
}
chunk_list_init(out);
full_hash[0]='\0';
if(size<MESH_LARGE_FILE) {
if(blake2b_hash_fd_layout(fd,size,NULL,0,full_hash)<0) {
chunk_list_free(out);
return -1;
}
return 0;
}
while(off<size) {
uint64_t end=0;
if(chunk_boundary(fd,off,size,&end)<0)
goto fail;
if(end<=off||end>size) {
errno=EIO;
goto fail;
}
if(!chunk_push(out,off,end-off))
goto fail;
off=end;
}
if(blake2b_hash_fd_layout(fd,size,out->v,out->n,full_hash)<0)
goto fail;
return 0;
fail: {
int saved=errno;
chunk_list_free(out);
full_hash[0]='\0';
errno=saved;
return -1;
}
}
/* Object store */
static int valid_hash(const char*hash) {
return mesh_hash_valid(hash)?1:0;
}
int object_verify(const struct mesh_repo*repo, const char*hash,
uint64_t*size) {
char path[MESH_PATH_MAX];
char actual[MESH_HASH_HEX+1];
struct stat st;
if(!repo||!valid_hash(hash)) {
errno=EINVAL;
return -1;
}
if(mesh_object_path(repo, hash, path, sizeof(path))<0)
return -1;
if(lstat(path,&st)<0)
return -1;
if(!S_ISREG(st.st_mode)||st.st_size<0) {
errno=EINVAL;
return -1;
}
if(blake2b_hash_file(path, actual)<0)
return -1;
if(strcmp(actual, hash)!=0) {
errno=EIO;
return -1;
}
if(size)
*size=(uint64_t)st.st_size;
return 0;
}
int object_store_fd(const struct mesh_repo*repo, const char*hash,
int in, uint64_t offset, uint64_t length) {
char dir[MESH_PATH_MAX], final[MESH_PATH_MAX], tmp[MESH_PATH_MAX];
char actual[MESH_HASH_HEX+1];
int out=-1, rc=-1;
uint64_t left=length;
unsigned char*buf=NULL;
int n;
if(!repo||!valid_hash(hash)||in<0||
offset>(uint64_t)INT64_MAX||length>(uint64_t)INT64_MAX||
offset+length<offset||offset+length>(uint64_t)INT64_MAX) {
errno=EINVAL;
return -1;
}
if(mesh_object_exists(repo, hash)&&object_verify(repo, hash, NULL)==0)
return 0;
n=snprintf(dir, sizeof(dir),"%s/store/objects/%.2s", repo->mesh_dir,
hash);
if(n<0||(size_t)n>=sizeof(dir)) {
errno=ENAMETOOLONG;
return -1;
}
if(mesh_mkdir_p(dir)<0)
return -1;
if(mesh_object_path(repo, hash, final, sizeof(final))<0)
return -1;
n=snprintf(tmp, sizeof(tmp),"%s.tmp.XXXXXX", final);
if(n<0||(size_t)n>=sizeof(tmp)) {
errno=ENAMETOOLONG;
return -1;
}
out=mkstemp(tmp);
if(out<0)
goto done;
if(fchmod(out,0444)<0)
goto done;
buf=malloc(MESH_IO_BUFFER);
if(!buf)
goto done;
while(left>0) {
size_t want=left<MESH_IO_BUFFER?(size_t)left
:MESH_IO_BUFFER;
ssize_t r=pread(in, buf, want, (off_t)offset);
ssize_t off=0;
if(r<0) {
if(errno==EINTR)
continue;
goto done;
}
if(r==0) {
errno=EIO;
goto done;
}
while(off<r) {
ssize_t w=write(out, buf+off, (size_t)(r-off));
if(w<0) {
if(errno==EINTR)
continue;
goto done;
}
off+=w;
}
offset+=(uint64_t)r;
left-=(uint64_t)r;
}
if(fsync(out)<0)
goto done;
if(blake2b_hash_fd(out, 0, length, actual)<0||
strcmp(actual, hash)!=0) {
errno=EAGAIN;
goto done;
}
if(close(out)<0) {
out=-1;
goto done;
}
out=-1;
if(rename(tmp, final)<0)
goto done;
if(mesh_fsync_parent(final)<0)
goto done;
rc=0;
done: {
int saved=errno;
if(out>=0)
close(out);
free(buf);
if(rc!=0)
unlink(tmp);
errno=saved;
}
return rc;
}
int object_store_blob(const struct mesh_repo*repo, const char*hash,
const void*data, size_t len) {
char dir[MESH_PATH_MAX], final[MESH_PATH_MAX];
int n;
char actual[MESH_HASH_HEX+1];
uint8_t digest[MESH_HASH_LEN];
static const char digits[]="0123456789abcdef";
if(!repo||!valid_hash(hash)||(!data&&len>0)) {
errno=EINVAL;
return -1;
}
blake2b_hash_buffer(data,len,digest);
for(size_t i=0;i<MESH_HASH_LEN;i++) {
actual[2*i]=digits[digest[i]>>4];
actual[2*i+1]=digits[digest[i]&0x0f];
}
actual[MESH_HASH_HEX]='\0';
if(strcmp(actual,hash)!=0) {
errno=EINVAL;
return -1;
}
if(mesh_object_exists(repo, hash)&&object_verify(repo, hash, NULL)==0)
return 0;
n=snprintf(dir, sizeof(dir),"%s/store/objects/%.2s", repo->mesh_dir,
hash);
if(n<0||(size_t)n>=sizeof(dir)) {
errno=ENAMETOOLONG;
return -1;
}
if(mesh_mkdir_p(dir)<0)
return -1;
if(mesh_object_path(repo, hash, final, sizeof(final))<0)
return -1;
return mesh_write_atomic(final, data, len, 0444);
}
int object_read(const struct mesh_repo*repo, const char*hash, char**out,
size_t*out_len) {
char path[MESH_PATH_MAX];
if(!repo||!valid_hash(hash)||!out) {
errno=EINVAL;
return -1;
}
if(object_verify(repo, hash, NULL)<0)
return -1;
if(mesh_object_path(repo, hash, path, sizeof(path))<0)
return -1;
return mesh_read_text(path, out, out_len);
}
