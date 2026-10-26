/*
* graph.c - tree manifests, the content index and the reverse history
* linked list.
* A snapshot is a node. A node owns a tree hash (a manifest of every
* tracked file and its chunks) and up to two parents. Traversal is a
* plain walk over ->next pointers; no graph library, no allocation
* storms.
*/
#include "core.h"
#include <inttypes.h>
#include <limits.h>
/* File lists */
void file_list_init(struct mesh_file_list*list) {
if(!list)
return;
memset(list, 0, sizeof(*list));
}
void file_list_free(struct mesh_file_list*list) {
if(!list)
return;
for(size_t i=0;i<list->n;i++) {
free(list->v[i].path);
free(list->v[i].chunks);
}
free(list->v);
memset(list, 0, sizeof(*list));
}
struct mesh_file_entry*file_list_add(struct mesh_file_list*list,
const char*path, uint64_t size,
int64_t mtime_sec, int64_t mtime_nsec,
uint32_t mode, uint32_t kind) {
struct mesh_file_entry*entry;
if(!list||!path) {
errno=EINVAL;
return NULL;
}
if(list->n==list->cap) {
size_t cap;
struct mesh_file_entry*v;
if(list->cap>SIZE_MAX/2) {
errno=ENOMEM;
return NULL;
}
cap=list->cap?list->cap*2:64;
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
entry=&list->v[list->n];
memset(entry, 0, sizeof(*entry));
entry->path=strdup(path);
if(!entry->path)
return NULL;
entry->size=size;
entry->mtime_sec=mtime_sec;
entry->mtime_nsec=mtime_nsec;
entry->mode=mode;
entry->kind=kind;
list->n++;
return entry;
}
static int entry_cmp(const void*a, const void*b) {
const struct mesh_file_entry*ea=a;
const struct mesh_file_entry*eb=b;
return strcmp(ea->path, eb->path);
}
void file_list_sort(struct mesh_file_list*list) {
if(list&&list->n>1)
qsort(list->v, list->n, sizeof(list->v[0]), entry_cmp);
}

const struct mesh_file_entry*manifest_find(const struct mesh_file_list*list,
const char*path) {
size_t lo=0, hi;
if(!list||!path)
return NULL;
hi=list->n;
while(lo<hi) {
size_t mid=lo+(hi-lo)/2;
int cmp=strcmp(list->v[mid].path, path);
if(cmp==0)
return &list->v[mid];
if(cmp<0)
lo=mid+1;
else
hi=mid;
}
return NULL;
}
size_t manifest_chunk_count(const struct mesh_file_list*list) {
size_t total=0;
if(!list)
return 0;
for(size_t i=0;i<list->n;i++)
total+=list->v[i].nchunks;
return total;
}
/* Manifest encoding */
/*
* Newlines and backslashes inside a path are escaped so a manifest stays
* one record per line. Spaces are legal in the terminal field.
*/
static int escape_path(const char*in, char*out, size_t cap) {
size_t o=0;
for(;*in;in++) {
const char*rep=NULL;
switch(*in) {
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
break;
}
if(rep) {
if(o+2>=cap) {
errno=ENAMETOOLONG;
return -1;
}
out[o++]=rep[0];
out[o++]=rep[1];
} else {
if(o+1>=cap) {
errno=ENAMETOOLONG;
return -1;
}
out[o++]=*in;
}
}
out[o]='\0';
return 0;
}
static int unescape_path(const char*in, char*out, size_t cap) {
size_t o=0;
while(*in) {
char c=*in++;
if(c=='\\'&&*in) {
switch(*in) {
case '\\':
c='\\';
in++;
break;
case 'n':
c='\n';
in++;
break;
case 'r':
c='\r';
in++;
break;
case 't':
c='\t';
in++;
break;
default:
break;
}
}
if(o+1>=cap) {
errno=ENAMETOOLONG;
return -1;
}
out[o++]=c;
}
out[o]='\0';
return 0;
}
static int entry_ptr_cmp(const void*a, const void*b) {
const struct mesh_file_entry*const*ea=a;
const struct mesh_file_entry*const*eb=b;
return strcmp((*ea)->path, (*eb)->path);
}
struct mesh_path_key { char*value; };
static int path_key_cmp(const void*a,const void*b) {
const struct mesh_path_key*ka=a;
const struct mesh_path_key*kb=b;
return strcmp(ka->value,kb->value);
}
static int manifest_paths_unique(const struct mesh_file_list*list) {
struct mesh_path_key*keys;
if(!list||list->n<2)
return 0;
if(list->n>SIZE_MAX/sizeof(*keys)) {
errno=ENOMEM;
return -1;
}
keys=calloc(list->n,sizeof(*keys));
if(!keys)
return -1;
for(size_t i=0;i<list->n;i++) {
if(mesh_unicode_key(list->v[i].path,&keys[i].value)<0) {
for(size_t j=0;j<i;j++)
free(keys[j].value);
free(keys);
return -1;
}
}
qsort(keys,list->n,sizeof(*keys),path_key_cmp);
for(size_t i=1;i<list->n;i++) {
if(strcmp(keys[i-1].value,keys[i].value)==0) {
for(size_t j=0;j<list->n;j++)
free(keys[j].value);
free(keys);
errno=EEXIST;
return -1;
}
}
for(size_t i=0;i<list->n;i++)
free(keys[i].value);
free(keys);
return 0;
}
static int manifest_entry_valid(const struct mesh_file_entry*entry) {
uint64_t expected=0;
if(!entry||!mesh_relpath_safe(entry->path)||
(entry->nchunks>0&&!entry->chunks)||
!mesh_hash_valid(entry->hash)||
entry->mtime_nsec<0||entry->mtime_nsec>=1000000000LL||
(entry->mode&~0777u)!=0)
return 0;
if(entry->kind==MESH_KIND_SYMLINK)
return entry->nchunks==0&&entry->size>0&&
entry->size<MESH_PATH_MAX;
if(entry->kind!=MESH_KIND_FILE)
return 0;
for(size_t i=0;i<entry->nchunks;i++) {
const struct mesh_chunk*chunk=&entry->chunks[i];
if(!mesh_hash_valid(chunk->hash)||chunk->length==0||
chunk->offset!=expected||chunk->offset>entry->size||
chunk->length>entry->size-chunk->offset)
return 0;
expected+=chunk->length;
}
return entry->nchunks==0||expected==entry->size;
}
int manifest_save(const struct mesh_repo*repo,
const struct mesh_file_list*list,
char tree_hash[MESH_HASH_HEX+1]) {
const struct mesh_file_entry**sorted=NULL;
char trees[MESH_PATH_MAX],tmp[MESH_PATH_MAX]={0},tree_path[MESH_PATH_MAX];
char line[MESH_PATH_MAX*2+256];
size_t total_bytes=0;
struct mesh_hash_ctx hash;
FILE*fp=NULL;
int fd=-1,rc=-1;
if(!repo||!list||!tree_hash) {
errno=EINVAL;
return -1;
}
if(list->n>MESH_MANIFEST_MAX_ENTRIES) {
errno=E2BIG;
return -1;
}
for(size_t i=0;i<list->n;i++) {
if(!manifest_entry_valid(&list->v[i])) {
errno=EINVAL;
return -1;
}
}
if(manifest_paths_unique(list)<0)
return -1;
if(list->n>0) {
sorted=malloc(list->n*sizeof(*sorted));
if(!sorted)
return -1;
for(size_t i=0;i<list->n;i++)
sorted[i]=&list->v[i];
qsort(sorted,list->n,sizeof(*sorted),entry_ptr_cmp);
}
size_t total_chunks=0;
for(size_t i=0;i<list->n;i++) {
if(list->v[i].nchunks>MESH_MANIFEST_MAX_CHUNKS_PER_FILE||
total_chunks>MESH_MANIFEST_MAX_CHUNKS-list->v[i].nchunks) {
free(sorted);
errno=E2BIG;
return -1;
}
total_chunks+=list->v[i].nchunks;
}
if(mesh_path_join(trees,sizeof(trees),repo->mesh_dir,"store/trees")<0||
mesh_mkdir_p(trees)<0||mesh_path_no_symlink(trees)<0||
mesh_path_join(tmp,sizeof(tmp),trees,".manifest.XXXXXX")<0)
goto done;
fd=mkstemp(tmp);
if(fd<0)
goto done;
fp=fdopen(fd,"wb");
if(!fp)
goto done;
fd=-1;
mesh_hash_init(&hash);
for(size_t i=0;i<list->n;i++) {
const struct mesh_file_entry*e=sorted[i];
char escaped[MESH_PATH_MAX*2];
if(escape_path(e->path,escaped,sizeof(escaped))<0)
goto done;
int n=snprintf(line,sizeof(line),"F %s %"PRIu64" %"PRId64" %"PRId64
" %03o %c %s\n",e->hash,e->size,e->mtime_sec,e->mtime_nsec,
e->mode,e->kind==MESH_KIND_SYMLINK?'l':'f',escaped);
if(n<0||(size_t)n>=sizeof(line)) {
errno=EOVERFLOW;
goto done;
}
if((size_t)n>64u*1024u*1024u-total_bytes) {
errno=EFBIG;
goto done;
}
if(fwrite(line,1,(size_t)n,fp)!=(size_t)n)
goto done;
mesh_hash_update(&hash,line,(size_t)n);
total_bytes+=(size_t)n;
for(size_t c=0;c<e->nchunks;c++) {
n=snprintf(line,sizeof(line),"C %s %"PRIu64" %"PRIu64"\n",
e->chunks[c].hash,e->chunks[c].offset,e->chunks[c].length);
if(n<0||(size_t)n>=sizeof(line)) {
errno=EOVERFLOW;
goto done;
}
if((size_t)n>64u*1024u*1024u-total_bytes) {
errno=EFBIG;
goto done;
}
if(fwrite(line,1,(size_t)n,fp)!=(size_t)n)
goto done;
mesh_hash_update(&hash,line,(size_t)n);
total_bytes+=(size_t)n;
}
}
if(fflush(fp)==EOF||ferror(fp)||fsync(fileno(fp))<0)
goto done;
if(fclose(fp)!=0) {
fp=NULL;
goto done;
}
fp=NULL;
mesh_hash_final(&hash,tree_hash);
char name[MESH_HASH_HEX+32];
snprintf(name,sizeof(name),"store/trees/%s",tree_hash);
if(mesh_path_join(tree_path,sizeof(tree_path),repo->mesh_dir,name)<0||
rename(tmp,tree_path)<0||mesh_fsync_parent(tree_path)<0)
goto done;
rc=0;
done: {
int saved=errno;
if(fp)
fclose(fp);
else if(fd>=0)
close(fd);
if(rc<0&&tmp[0])
unlink(tmp);
free(sorted);
errno=saved;
}
return rc;
}
static int parse_u64_field(const char*text, uint64_t*out) {
char*end=NULL;
unsigned long long value;
if(!text||!*text||text[0]=='+'||text[0]=='-') {
errno=EINVAL;
return -1;
}
errno=0;
value=strtoull(text,&end, 10);
if(errno==ERANGE||end==text||!end||*end!='\0'||
value>UINT64_MAX) {
errno=EINVAL;
return -1;
}
*out=(uint64_t)value;
return 0;
}
static int parse_i64_field(const char*text, int64_t*out) {
char*end=NULL;
long long value;
const char*p=text;
if(!text||!*text)
goto invalid;
if(*p=='-')
p++;
if(!*p)
goto invalid;
for(;*p;p++)
if(*p<'0'||*p>'9')
goto invalid;
errno=0;
value=strtoll(text,&end, 10);
if(errno==ERANGE||end==text||!end||*end!='\0')
goto invalid;
*out=(int64_t)value;
return 0;
invalid:
errno=EINVAL;
return -1;
}
static int parse_mode_field(const char*text, uint32_t*out) {
char*end=NULL;
unsigned long value;
if(!text||!*text)
goto invalid;
for(const char*p=text;*p;p++)
if(*p<'0'||*p>'7')
goto invalid;
errno=0;
value=strtoul(text,&end, 8);
if(errno==ERANGE||end==text||!end||*end!='\0'||value>0777)
goto invalid;
*out=(uint32_t)value;
return 0;
invalid:
errno=EINVAL;
return -1;
}
int manifest_load(const struct mesh_repo*repo, const char*tree_hash,
struct mesh_file_list*out) {
char path[MESH_PATH_MAX];
char*buf=NULL;
char*save=NULL;
char*line;
char actual[MESH_HASH_HEX+1];
size_t buf_len=0;
size_t total_chunks=0;
int rc=-1;
if(!repo||!mesh_hash_valid(tree_hash)||!out) {
errno=EINVAL;
return -1;
}
file_list_init(out);
char name[MESH_HASH_HEX+32];
snprintf(name, sizeof(name),"store/trees/%s", tree_hash);
if(mesh_path_join(path, sizeof(path), repo->mesh_dir, name)<0)
return -1;
if(mesh_read_text(path,&buf,&buf_len)<0)
return -1;
if(memchr(buf,'\0',buf_len)!=NULL) {
errno=EINVAL;
goto done;
}
uint8_t digest[MESH_HASH_LEN];
static const char digits[]="0123456789abcdef";
blake2b_hash_buffer(buf, buf_len, digest);
for(size_t i=0;i<MESH_HASH_LEN;i++) {
actual[2*i]=digits[digest[i]>>4];
actual[2*i+1]=digits[digest[i]&0x0f];
}
actual[MESH_HASH_HEX]='\0';
if(strcmp(actual, tree_hash)!=0) {
errno=EIO;
goto done;
}
for(line=strtok_r(buf,"\n",&save);line;
line=strtok_r(NULL,"\n",&save)) {
if(line[0]=='F'&&line[1]==' ') {
char*rest;
char*sep;
char hash_field[MESH_HASH_HEX+1];
char size_field[32];
char meta[96];
char*parts[4];
char*meta_q;
char*path_field;
uint64_t size;
int64_t sec=0,nsec=0;
uint32_t mode=0644,kind=MESH_KIND_FILE;
char real[MESH_PATH_MAX];
struct mesh_file_entry*entry;
int nf=0;
bool modern=false;
rest=line+2;
sep=strchr(rest,' ');
if(!sep)
goto invalid;
*sep='\0';
if(strlen(rest)!=MESH_HASH_HEX)
goto invalid;
memcpy(hash_field,rest,MESH_HASH_HEX+1);
rest=sep+1;
sep=strchr(rest,' ');
if(!sep)
goto invalid;
*sep='\0';
if(strlen(rest)>=sizeof(size_field))
goto invalid;
memcpy(size_field,rest,strlen(rest)+1);
rest=sep+1;
if(!mesh_hash_valid(hash_field)||parse_u64_field(size_field,&size)<0)
goto invalid;
/* Identify the modern seven-field form without copying or truncating
* the path. The metadata prefix is bounded; the path itself may be up
* to MESH_PATH_MAX-1 bytes and stays in the line buffer. */
char*scan=rest;
while(nf<4) {
sep=strchr(scan,' ');
if(!sep)
break;
parts[nf++]=scan;
scan=sep+1;
}
if(nf==4) {
size_t metalen=(size_t)(scan-rest);
if(metalen>=sizeof(meta))
goto invalid;
memcpy(meta,rest,metalen);
meta[metalen]='\0';
meta_q=meta;
nf=0;
while(nf<4) {
sep=strchr(meta_q,' ');
if(!sep)
break;
*sep='\0';
parts[nf++]=meta_q;
meta_q=sep+1;
}
if(nf==4&&parse_i64_field(parts[0],&sec)==0&&
parse_i64_field(parts[1],&nsec)==0&&
parse_mode_field(parts[2],&mode)==0) {
if(strcmp(parts[3],"f")==0)
kind=MESH_KIND_FILE;
else if(strcmp(parts[3],"l")==0)
kind=MESH_KIND_SYMLINK;
else
goto invalid;
modern=true;
}
}
if(modern)
path_field=scan;
else {
sec=0;
nsec=0;
mode=0644;
kind=MESH_KIND_FILE;
path_field=rest;
}
if(path_field[0]=='\0'||
unescape_path(path_field, real, sizeof(real))<0||
!mesh_relpath_safe(real))
goto invalid;
if(out->n>=MESH_MANIFEST_MAX_ENTRIES)
goto invalid;
entry=file_list_add(out, real, size, sec, nsec, mode, kind);
if(!entry)
goto done;
memcpy(entry->hash, hash_field, MESH_HASH_HEX+1);
} else if(line[0]=='C'&&line[1]==' ') {
char*hash=line+2;
char*offset_text=strchr(hash,' ');
char*length_text;
uint64_t offset, length;
struct mesh_file_entry*entry;
struct mesh_chunk*chunks;
	uint64_t expected;
if(out->n==0||!offset_text)
goto invalid;
*offset_text++='\0';
if(!mesh_hash_valid(hash))
goto invalid;
length_text=strchr(offset_text,' ');
if(!length_text)
goto invalid;
*length_text++='\0';
if(strchr(length_text,' ')||
parse_u64_field(offset_text,&offset)<0||
parse_u64_field(length_text,&length)<0||length==0)
goto invalid;
entry=&out->v[out->n-1];
if(entry->kind!=MESH_KIND_FILE)
goto invalid;
expected=0;
if(entry->nchunks>0) {
const struct mesh_chunk*previous=&entry->chunks[entry->nchunks-1];
if(previous->length>UINT64_MAX-previous->offset)
goto invalid;
expected=previous->offset+previous->length;
}
if(offset!=expected||offset>entry->size||
length>entry->size-offset||
entry->nchunks==SIZE_MAX||
entry->nchunks>=MESH_MANIFEST_MAX_CHUNKS_PER_FILE||
total_chunks>=MESH_MANIFEST_MAX_CHUNKS)
goto invalid;
if(entry->nchunks==entry->chunk_capacity) {
size_t capacity=entry->chunk_capacity?entry->chunk_capacity*2:8;
if(capacity>MESH_MANIFEST_MAX_CHUNKS_PER_FILE)
capacity=MESH_MANIFEST_MAX_CHUNKS_PER_FILE;
if(capacity>SIZE_MAX/sizeof(*chunks))
goto invalid;
chunks=realloc(entry->chunks,capacity*sizeof(*chunks));
if(!chunks)
goto done;
entry->chunks=chunks;
entry->chunk_capacity=capacity;
}
memcpy(entry->chunks[entry->nchunks].hash, hash,
MESH_HASH_HEX+1);
entry->chunks[entry->nchunks].offset=offset;
entry->chunks[entry->nchunks].length=length;
entry->nchunks++;
total_chunks++;
} else if(line[0]!='\0') {
goto invalid;
}
}
for(size_t i=0;i<out->n;i++)
if(!manifest_entry_valid(&out->v[i]))
goto invalid;
	if(manifest_paths_unique(out)<0)
	goto invalid;
if(out->n>1)
qsort(out->v, out->n, sizeof(out->v[0]), entry_cmp);
rc=0;
goto done;
invalid:
errno=EINVAL;
done: {
int saved_errno=errno;
free(buf);
if(rc!=0)
file_list_free(out);
errno=saved_errno;
}
return rc;
}
/* Content access */
static int ensure_parent_dir(const char*path) {
char dir[MESH_PATH_MAX];
char*slash;
if(strlen(path)>=sizeof(dir)) {
errno=ENAMETOOLONG;
return -1;
}
memcpy(dir, path, strlen(path)+1);
slash=strrchr(dir,'/');
if(!slash)
return 0;
if(slash==dir)
return 0;
*slash='\0';
return mesh_mkdir_p(dir);
}
static int entry_copy_to_fd(const struct mesh_repo*repo,
const struct mesh_file_entry*entry, int out) {
uint64_t expected=0;
if(entry->kind==MESH_KIND_SYMLINK||entry->nchunks==0) {
char path[MESH_PATH_MAX];
struct stat st;
int fd;
uint64_t object_size;
if(object_verify(repo,entry->hash,&object_size)<0)
return -1;
if(object_size!=entry->size) {
errno=EIO;
return -1;
}
if(mesh_object_path(repo,entry->hash,path,sizeof(path))<0)
return -1;
fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
if(fd<0)
return -1;
if(fstat(fd,&st)<0||!S_ISREG(st.st_mode)||
(uint64_t)st.st_size!=entry->size) {
int saved=errno;
close(fd);
errno=saved?saved:EIO;
return -1;
}
int rc=mesh_copy_fd_to_fd(fd,out,entry->size);
if(rc<0) {
int saved=errno;
close(fd);
errno=saved;
return -1;
}
return close(fd);
}
for(size_t i=0;i<entry->nchunks;i++) {
const struct mesh_chunk*chunk=&entry->chunks[i];
char obj[MESH_PATH_MAX];
struct stat st;
int fd;
if(chunk->offset!=expected||
chunk->length>entry->size-expected) {
errno=EINVAL;
return -1;
}
if(object_verify(repo,chunk->hash,NULL)<0||
mesh_object_path(repo,chunk->hash,obj,sizeof(obj))<0) {
if(errno==0)
errno=EIO;
return -1;
}
fd=open(obj,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
if(fd<0)
return -1;
if(fstat(fd,&st)<0||!S_ISREG(st.st_mode)||
(uint64_t)st.st_size!=chunk->length) {
close(fd);
errno=EIO;
return -1;
}
if(mesh_copy_fd_to_fd(fd,out,chunk->length)<0) {
int saved=errno;
close(fd);
errno=saved;
return -1;
}
close(fd);
expected+=chunk->length;
}
if(expected!=entry->size) {
errno=EIO;
return -1;
}
return 0;
}
int manifest_stream_path(const struct mesh_repo*repo, const char*tree_hash,
const char*path, const char*dest, mode_t mode) {
struct mesh_file_list list;
const struct mesh_file_entry*entry;
char tmp[MESH_PATH_MAX]="";
char actual[MESH_HASH_HEX+1];
int fd=-1;
int n;
int rc=-1;
if(!repo||!tree_hash||!path||!dest) {
errno=EINVAL;
return -1;
}
if(manifest_load(repo, tree_hash,&list)<0)
return -1;
entry=manifest_find(&list, path);
if(!entry) {
errno=ENOENT;
goto done;
}
if(mesh_path_no_symlink(dest)<0||ensure_parent_dir(dest)<0)
goto done;
n=snprintf(tmp, sizeof(tmp),"%s.tmp.XXXXXX", dest);
if(n<0||(size_t)n>=sizeof(tmp)) {
errno=ENAMETOOLONG;
goto done;
}
fd=mkstemp(tmp);
if(fd<0)
goto done;
if(fchmod(fd, mode)<0||entry_copy_to_fd(repo,entry,fd)<0||
fsync(fd)<0)
goto done;
if(blake2b_hash_fd(fd,0,entry->size,actual)<0||
strcmp(actual,entry->hash)!=0) {
errno=EIO;
goto done;
}
if(close(fd)<0) {
fd=-1;
goto done;
}
fd=-1;
if(rename(tmp,dest)<0)
goto done;
rc=0;
done: {
int saved=errno;
if(fd>=0)
close(fd);
if(rc!=0&&tmp[0])
unlink(tmp);
file_list_free(&list);
errno=saved;
}
return rc;
}
int manifest_materialize_entry(const struct mesh_repo*repo,
const struct mesh_file_entry*entry) {
const char*path;
char leaf[MESH_PATH_MAX],tmp[128]="";
char actual[MESH_HASH_HEX+1];
int root=-1,parent=-1,fd=-1,rc=-1;
if(!repo||!repo->valid||!entry||
!mesh_relpath_safe(entry->path)) {
errno=EINVAL;
return -1;
}
path=entry->path;
root=open(repo->root,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
if(root<0)
goto done;
if(entry->kind==MESH_KIND_SYMLINK) {
char*target=NULL;
size_t len=0;
if(object_read(repo,entry->hash,&target,&len)<0)
goto done;
if((uint64_t)len!=entry->size||memchr(target,'\0',len)!=NULL) {
free(target);
errno=EIO;
goto done;
}
if(mesh_symlink_at_rel(root,path,target,entry->mode)<0) {
free(target);
goto done;
}
free(target);
if(mesh_open_parent_at(root,path,false,&parent,leaf,
sizeof(leaf))<0)
goto done;
if(mesh_set_times_at(parent,leaf,entry->mtime_sec,
entry->mtime_nsec,true)<0)
goto done;
rc=0;
goto done;
}
if(mesh_open_parent_at(root,path,true,&parent,leaf,sizeof(leaf))<0)
goto done;
for(unsigned i=0;i<32;i++) {
if(mesh_temp_name(tmp,sizeof(tmp),i)<0)
goto done;
fd=openat(parent,tmp,O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC|
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
if(fchmod(fd,entry->mode)<0||entry_copy_to_fd(repo,entry,fd)<0||
fsync(fd)<0)
goto done;
if(blake2b_hash_fd(fd,0,entry->size,actual)<0||
strcmp(actual,entry->hash)!=0) {
errno=EIO;
goto done;
}
struct timespec ts[2];
ts[0].tv_sec=(time_t)entry->mtime_sec;
ts[0].tv_nsec=(long)entry->mtime_nsec;
ts[1]=ts[0];
if(futimens(fd,ts)<0)
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
if(parent>=0)
close(parent);
if(root>=0)
close(root);
if(rc!=0&&tmp[0]&&parent>=0)
unlinkat(parent,tmp,0);
errno=saved;
}
return rc;
}
int manifest_materialize(const struct mesh_repo*repo,
const char*tree_hash, const char*path) {
struct mesh_file_list list;
const struct mesh_file_entry*entry;
int rc;
if(!repo||!tree_hash||!path) {
errno=EINVAL;
return -1;
}
if(manifest_load(repo,tree_hash,&list)<0)
return -1;
entry=manifest_find(&list,path);
if(!entry) {
errno=ENOENT;
file_list_free(&list);
return -1;
}
rc=manifest_materialize_entry(repo,entry);
int saved=errno;
file_list_free(&list);
errno=saved;
return rc;
}
int manifest_diff(const struct mesh_file_list*old_list,
const struct mesh_file_list*new_list,
struct mesh_changed*out) {
const struct mesh_file_entry**a=NULL,**b=NULL;
size_t i=0, j=0;
if(!out)
return -1;
memset(out, 0, sizeof(*out));
if(!old_list||!new_list)
return -1;
if(old_list->n>0) {
if(old_list->n>SIZE_MAX/sizeof(*a)) {
errno=ENOMEM;
return -1;
}
a=malloc(old_list->n*sizeof(*a));
if(!a)
return -1;
for(size_t k=0;k<old_list->n;k++)
a[k]=&old_list->v[k];
qsort(a, old_list->n, sizeof(*a), entry_ptr_cmp);
}
if(new_list->n>0) {
if(new_list->n>SIZE_MAX/sizeof(*b)) {
free(a);
errno=ENOMEM;
return -1;
}
b=malloc(new_list->n*sizeof(*b));
if(!b) {
free(a);
return -1;
}
for(size_t k=0;k<new_list->n;k++)
b[k]=&new_list->v[k];
qsort(b, new_list->n, sizeof(*b), entry_ptr_cmp);
}
while(i<old_list->n||j<new_list->n) {
int cmp;
if(i>=old_list->n) {
if(!b) {
free(a);
free(b);
errno=ENOMEM;
return -1;
}
cmp=1;
} else if(j>=new_list->n) {
if(!a) {
free(a);
free(b);
errno=ENOMEM;
return -1;
}
cmp=-1;
} else {
if(!a||!b) {
free(a);
free(b);
errno=ENOMEM;
return -1;
}
cmp=strcmp(a[i]->path, b[j]->path);
}
if(cmp<0) {
out->deleted++;
i++;
} else if(cmp>0) {
out->added++;
out->bytes+=b[j]->size;
j++;
} else {
if(strcmp(a[i]->hash, b[j]->hash)!=0||
a[i]->mode!=b[j]->mode||a[i]->kind!=b[j]->kind) {
out->modified++;
out->bytes+=b[j]->size;
}
i++;
j++;
}
}
free(a);
free(b);
return 0;
}
/* History graph */
void graph_init(struct mesh_graph*graph) {
if(graph)
memset(graph, 0, sizeof(*graph));
}
void graph_free(struct mesh_graph*graph) {
struct mesh_node*node;
if(!graph)
return;
node=graph->head;
while(node) {
struct mesh_node*next=node->next;
free(node->message);
free(node);
node=next;
}
free(graph->index);
memset(graph, 0, sizeof(*graph));
}
static int node_hash_cmp(const void*a, const void*b) {
const struct mesh_node*na=*(const struct mesh_node*const*)a;
const struct mesh_node*nb=*(const struct mesh_node*const*)b;
return strcmp(na->hash, nb->hash);
}
static int hash_key_cmp(const void*key, const void*elem) {
const char*hash=key;
const struct mesh_node*node=*(const struct mesh_node*const*)elem;
return strcmp(hash, node->hash);
}
static int graph_build_index(struct mesh_graph*graph) {
struct mesh_node*node;
size_t i=0;
free(graph->index);
graph->index=NULL;
graph->index_n=0;
if(graph->count==0)
return 0;
graph->index=malloc(graph->count*sizeof(*graph->index));
if(!graph->index)
return -1;
for(node=graph->head;node;node=node->next)
graph->index[i++]=node;
qsort(graph->index, graph->count, sizeof(*graph->index),
node_hash_cmp);
for(i=1;i<graph->count;i++) {
if(strcmp(graph->index[i-1]->hash, graph->index[i]->hash)==0) {
free(graph->index);
graph->index=NULL;
errno=EINVAL;
return -1;
}
}
graph->index_n=graph->count;
return 0;
}
struct mesh_node*graph_find(const struct mesh_graph*graph, const char*hash) {
struct mesh_node*node;
if(!graph||!hash)
return NULL;
if(graph->index&&graph->index_n==graph->count) {
struct mesh_node*const*found=bsearch(hash, graph->index,
graph->index_n, sizeof(*graph->index), hash_key_cmp);
return found?*found:NULL;
}
for(node=graph->head;node;node=node->next) {
if(strcmp(node->hash, hash)==0)
return node;
}
return NULL;
}
int graph_resolve(const struct mesh_graph*graph, const char*arg,
const char*head, char out[MESH_HASH_HEX+1]) {
const struct mesh_node*hit=NULL;
if(!graph||!arg||!*arg||!out) {
errno=EINVAL;
return -1;
}
if(strcmp(arg,"HEAD")==0) {
if(!head||!head[0]) {
errno=ENOENT;
return -1;
}
arg=head;
}
if(mesh_hash_valid(arg)) {
if(!graph_find(graph,arg)) {
errno=ENOENT;
return -1;
}
snprintf(out, MESH_HASH_HEX+1,"%s", arg);
return 0;
}
size_t len=strlen(arg);
if(len<4||len>=MESH_HASH_HEX) {
errno=EINVAL;
return -1;
}
for(const struct mesh_node*node=graph->head;node;node=node->next) {
if(strncmp(node->hash,arg,len)==0) {
if(hit) {
errno=EINVAL;
return -1;
}
hit=node;
}
}
if(!hit) {
errno=ENOENT;
return -1;
}
snprintf(out, MESH_HASH_HEX+1,"%s", hit->hash);
return 0;
}
static int hex_field_ok(const char*s) {
return mesh_hash_valid(s)?1:0;
}
static int branch_field_ok(const char*branch) {
if(!branch||!branch[0]||strcmp(branch,".")==0||
strcmp(branch,"..")==0||strlen(branch)>=64)
return 0;
for(const unsigned char*p=(const unsigned char*)branch;*p;p++)
if(*p<0x20||*p==0x7f||*p=='/'||*p=='\\')
return 0;
return 1;
}
int graph_load(const struct mesh_repo*repo, struct mesh_graph*graph) {
char path[MESH_PATH_MAX];
char*buf=NULL;
char*save=NULL;
char*line;
size_t buf_len=0;
if(!repo||!graph) {
errno=EINVAL;
return -1;
}
graph_init(graph);
if(snprintf(path, sizeof(path),"%s/history", repo->mesh_dir)>=
(int)sizeof(path)) {
errno=ENAMETOOLONG;
return -1;
}
if(mesh_read_text(path,&buf,&buf_len)<0) {
if(errno==ENOENT)
return 0;
return -1;
}
if(memchr(buf,'\0',buf_len)!=NULL) {
free(buf);
errno=EINVAL;
return -1;
}
for(line=strtok_r(buf,"\n",&save);line;
line=strtok_r(NULL,"\n",&save)) {
char*field[9]={0};
char*p=line;
struct mesh_node*node;
char signature[MESH_MSG_MAX+MESH_AUTHOR_MAX+MESH_EMAIL_MAX+4096];
char calculated[MESH_HASH_HEX+1];
int64_t timestamp;
size_t fields=7;
int n;
if(line[0]=='\0')
continue;
for(int i=0;i<6;i++) {
char*sep=strchr(p,'|');
if(!sep)
goto fail;
*sep='\0';
field[i]=p;
p=sep+1;
}
field[6]=p;
/* Version 1.0 wrote seven fields. New snapshots append the author name
* and e-mail. The message is sanitized, so a pipe here can only start
* the optional author fields. */
if(strchr(field[6],'|')) {
char*sep=strchr(field[6],'|');
*sep='\0';
field[7]=sep+1;
sep=strchr(field[7],'|');
if(!sep)
goto fail;
*sep='\0';
field[8]=sep+1;
if(strchr(field[8],'|'))
goto fail;
fields=9;
}
if(!hex_field_ok(field[0])||!hex_field_ok(field[1])||
!branch_field_ok(field[5])||
parse_i64_field(field[2],&timestamp)<0)
goto fail;
if((field[3][0]&&!hex_field_ok(field[3]))||
(field[4][0]&&!hex_field_ok(field[4]))||
strcmp(field[0], field[3])==0||
strcmp(field[0], field[4])==0)
goto fail;
if(fields==9&&(!mesh_author_name_valid(field[7])||
!mesh_email_valid(field[8])))
goto fail;
if(fields==9)
n=snprintf(signature, sizeof(signature),
"%s|%s|%s|%s|%lld|%s|%s|%s",
field[1], field[3][0]?field[3]:"-",
field[4][0]?field[4]:"-", field[5],
(long long)timestamp, field[6], field[7], field[8]);
else
n=snprintf(signature, sizeof(signature),
"%s|%s|%s|%s|%lld|%s",
field[1], field[3][0]?field[3]:"-",
field[4][0]?field[4]:"-", field[5],
(long long)timestamp, field[6]);
if(n<0||(size_t)n>=sizeof(signature)||
blake2b_hash_string(signature, calculated)<0||
strcmp(calculated, field[0])!=0)
goto fail;
node=calloc(1, sizeof(*node));
if(!node)
goto fail;
snprintf(node->hash, sizeof(node->hash),"%s", field[0]);
snprintf(node->tree, sizeof(node->tree),"%s", field[1]);
node->timestamp=timestamp;
snprintf(node->parent_1, sizeof(node->parent_1),"%s", field[3]);
snprintf(node->parent_2, sizeof(node->parent_2),"%s", field[4]);
if(fields==9) {
snprintf(node->author_name, sizeof(node->author_name),
"%s", field[7]);
snprintf(node->author_email, sizeof(node->author_email),
"%s", field[8]);
}
node->message=strdup(field[6]);
if(!node->message) {
free(node);
goto fail;
}
node->next=graph->head;
graph->head=node;
graph->count++;
}
if(graph_build_index(graph)<0)
goto fail;
for(struct mesh_node*node=graph->head;node;node=node->next) {
if(node->parent_1[0]) {
node->p1=graph_find(graph, node->parent_1);
if(!node->p1)
goto fail;
}
if(node->parent_2[0]) {
node->p2=graph_find(graph, node->parent_2);
if(!node->p2)
goto fail;
}
}
free(buf);
return 0;
fail: {
int saved=errno;
free(buf);
graph_free(graph);
errno=saved;
return -1;
}
}
int graph_commit(const struct mesh_repo*repo, struct mesh_graph*graph,
const char*tree, const char*p1, const char*p2,
const char*branch, const char*message,
char out_hash[MESH_HASH_HEX+1]) {
char signature[MESH_MSG_MAX+MESH_AUTHOR_MAX+MESH_EMAIL_MAX+4096];
char msg[MESH_MSG_MAX];
char author_name[MESH_AUTHOR_MAX+1];
char author_email[MESH_EMAIL_MAX+1];
char br[64];
char history[MESH_PATH_MAX];
char line[MESH_HASH_HEX*3+MESH_MSG_MAX+MESH_AUTHOR_MAX+
MESH_EMAIL_MAX+256];
char hex[MESH_HASH_HEX+1];
struct mesh_node*node;
bool has_author;
long long ts;
off_t before=0;
int fd, n;
if(!repo||!graph||!hex_field_ok(tree)||!out_hash||
(p1&&*p1&&!hex_field_ok(p1))||
(p2&&*p2&&!hex_field_ok(p2))) {
errno=EINVAL;
return -1;
}
mesh_sanitize_field(msg, sizeof(msg), message);
mesh_sanitize_field(br, sizeof(br), branch&&*branch?branch:"main");
if(!br[0])
snprintf(br, sizeof(br),"main");
if(!branch_field_ok(br)) {
errno=EINVAL;
return -1;
}
mesh_sanitize_field(author_name, sizeof(author_name),
repo->author_name);
mesh_sanitize_field(author_email, sizeof(author_email),
repo->author_email);
has_author=mesh_author_name_valid(author_name)&&
mesh_email_valid(author_email);
ts=mesh_now_ns();
for(unsigned attempt=0;attempt<1024;attempt++,ts++) {
if(has_author)
n=snprintf(signature,sizeof(signature),
"%s|%s|%s|%s|%lld|%s|%s|%s",
tree,p1&&*p1?p1:"-",p2&&*p2?p2:"-",br,ts,msg,
author_name,author_email);
else
n=snprintf(signature,sizeof(signature),
"%s|%s|%s|%s|%lld|%s",
tree,p1&&*p1?p1:"-",p2&&*p2?p2:"-",br,ts,msg);
if(n<0||(size_t)n>=sizeof(signature)) {
errno=ENAMETOOLONG;
return -1;
}
if(blake2b_hash_string(signature,hex)<0)
return -1;
if(!graph_find(graph,hex))
break;
}
if(graph_find(graph,hex)) {
errno=EEXIST;
return -1;
}
if(has_author)
n=snprintf(line, sizeof(line),
"%s|%s|%lld|%s|%s|%s|%s|%s|%s\n", hex, tree,
ts, p1&&*p1?p1:"", p2&&*p2?p2:"", br, msg,
author_name, author_email);
else
n=snprintf(line, sizeof(line),"%s|%s|%lld|%s|%s|%s|%s\n", hex, tree,
ts, p1&&*p1?p1:"", p2&&*p2?p2:"", br, msg);
if(n<0||(size_t)n>=sizeof(line)) {
errno=ENAMETOOLONG;
return -1;
}
if(snprintf(history, sizeof(history),"%s/history", repo->mesh_dir)>=
(int)sizeof(history)) {
errno=ENAMETOOLONG;
return -1;
}
fd=open(history,O_WRONLY|O_CREAT|O_APPEND|O_CLOEXEC|O_NOFOLLOW,
0644);
if(fd<0)
return -1;
if(flock(fd, LOCK_EX)<0) {
close(fd);
return -1;
}
struct stat st;
if(fstat(fd,&st)<0) {
int saved=errno;
flock(fd, LOCK_UN);
close(fd);
errno=saved;
return -1;
}
before=st.st_size;
if(mesh_write_all(fd, line, (size_t)n)<0) {
int saved=errno;
if (ftruncate(fd, before) == -1) {}
flock(fd, LOCK_UN);
close(fd);
errno=saved;
return -1;
}
if(fsync(fd)<0) {
int saved=errno;
if (ftruncate(fd, before) == -1) {}
flock(fd, LOCK_UN);
close(fd);
errno=saved;
return -1;
}
flock(fd, LOCK_UN);
if(close(fd)<0)
return -1;
node=calloc(1, sizeof(*node));
if(!node)
return -1;
snprintf(node->hash, sizeof(node->hash),"%s", hex);
snprintf(node->tree, sizeof(node->tree),"%s", tree);
snprintf(node->parent_1, sizeof(node->parent_1),"%s",
p1&&*p1?p1:"");
snprintf(node->parent_2, sizeof(node->parent_2),"%s",
p2&&*p2?p2:"");
if(has_author) {
snprintf(node->author_name, sizeof(node->author_name),"%s",
author_name);
snprintf(node->author_email, sizeof(node->author_email),"%s",
author_email);
}
node->timestamp=ts;
node->message=strdup(msg);
if(!node->message) {
free(node);
return -1;
}
node->p1=p1&&*p1?graph_find(graph, p1):NULL;
node->p2=p2&&*p2?graph_find(graph, p2):NULL;
node->next=graph->head;
graph->head=node;
graph->count++;
free(graph->index);
graph->index=NULL;
graph->index_n=0;
memcpy(out_hash, hex, sizeof(hex));
return 0;
}
void graph_print(const struct mesh_graph*graph, FILE*out) {
const struct mesh_node*node;
if(!graph||!out)
return;
if(!graph->head) {
fprintf(out,"No snapshots yet.\n");
return;
}
fprintf(out,"History (%zu snapshot%s):\n", graph->count,
graph->count==1?"":"s");
for(node=graph->head;node;node=node->next) {
char short_hash[13];
char date[32];
struct tm tm;
time_t timestamp;
mesh_short_hash(short_hash, sizeof(short_hash), node->hash);
snprintf(date, sizeof(date),"unknown");
timestamp=(time_t)(node->timestamp/1000000000LL);
if(localtime_r(&timestamp,&tm))
(void)strftime(date, sizeof(date),"%Y-%m-%d %H:%M:%S",&tm);
fprintf(out,"  %s  %s  [%s]  %s\n", short_hash, date,
node->hash[0]?"snapshot":"?", node->message);
if(node->author_name[0]&&node->author_email[0])
fprintf(out,"              by %s <%s>\n", node->author_name,
node->author_email);
if(node->parent_2[0]) {
char a[13], b[13];
mesh_short_hash(a, sizeof(a), node->parent_1);
mesh_short_hash(b, sizeof(b), node->parent_2);
fprintf(out,"              merge of %s and %s\n", a,
b);
}
}
}
int graph_head(const struct mesh_repo*repo, char out[MESH_HASH_HEX+1]) {
char path[MESH_PATH_MAX];
char*buf=NULL;
size_t len=0;
int rc=-1;
if(!repo||!out) {
errno=EINVAL;
return -1;
}
out[0]='\0';
if(snprintf(path, sizeof(path),"%s/HEAD", repo->mesh_dir)>=
(int)sizeof(path)) {
errno=ENAMETOOLONG;
return -1;
}
if(mesh_read_text(path,&buf,&len)<0) {
if(errno==ENOENT)
return 0;
return -1;
}
if(memchr(buf,'\0',len)!=NULL) {
errno=EINVAL;
goto done;
}
mesh_trim_newline(buf);
if(buf[0]!='\0') {
if(!hex_field_ok(buf)) {
errno=EINVAL;
goto done;
}
snprintf(out, MESH_HASH_HEX+1,"%s", buf);
}
rc=0;
done: {
int saved=errno;
free(buf);
errno=saved;
}
return rc;
}
int graph_set_head(const struct mesh_repo*repo, const char*branch,
const char*hash) {
char path[MESH_PATH_MAX];
char value[MESH_HASH_HEX+2];
char safe[64];
if(!repo||!hash) {
errno=EINVAL;
return -1;
}
if(hash[0]!='\0'&&!hex_field_ok(hash)) {
errno=EINVAL;
return -1;
}
mesh_sanitize_field(safe, sizeof(safe), branch&&*branch?branch:"main");
if(!safe[0]||strchr(safe,'/')||strcmp(safe,".")==0||
strcmp(safe,"..")==0) {
snprintf(safe, sizeof(safe),"main");
}
snprintf(value, sizeof(value),"%s\n", hash);
if(snprintf(path, sizeof(path),"%s/HEAD", repo->mesh_dir)>=
(int)sizeof(path)) {
errno=ENAMETOOLONG;
return -1;
}
if(mesh_write_atomic(path, value, strlen(value), 0644)<0)
return -1;
if(snprintf(path, sizeof(path),"%s/refs/%s", repo->mesh_dir, safe)>=
(int)sizeof(path)) {
errno=ENAMETOOLONG;
return -1;
}
return mesh_write_atomic(path, value, strlen(value), 0644);
}
