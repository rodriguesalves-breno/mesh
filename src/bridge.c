/*
* bridge.c - export a Mesh snapshot difference as a Git-compatible
* unified patch.
* The patch is intentionally conservative: for text files it emits a
* full replacement hunk for each changed file. That is boring, valid
* unified diff and 'git apply' eats it without complaining. Binary and
* very large files are declared as differing instead of being inlined.
*/
#include "core.h"
static size_t count_lines(const char*data, size_t len) {
size_t lines=0;
for(size_t i=0;i<len;i++) {
if(data[i]=='\n')
lines++;
}
if(len>0&&data[len-1]!='\n')
lines++;
return lines;
}
static void emit_lines(FILE*fp, char prefix, const char*data, size_t len) {
size_t i=0;
while(i<len) {
const char*nl=memchr(data+i,'\n', len-i);
size_t chunk=nl?(size_t)(nl-(data+i))+1:len-i;
fputc(prefix, fp);
fwrite(data+i, 1, chunk, fp);
if(!nl)
fputs("\n\\ No newline at end of file\n", fp);
i+=chunk;
}
}
static bool binary_data(const char*data, size_t len) {
return len>MESH_PATCH_LIMIT||
(len>0&&memchr(data,'\0', len)!=NULL);
}
/*
* Read one manifest entry without reloading the whole tree for every
* file. bridge.c owns this small reader because it walks two trees.
*/
static int entry_read(const struct mesh_repo*repo,
const struct mesh_file_entry*entry, char**out,
size_t*len) {
char*buf=NULL;
if(!entry||!out) {
errno=EINVAL;
return -1;
}
*out=NULL;
if(len)
*len=0;
if(entry->size>=(uint64_t)SIZE_MAX) {
errno=EFBIG;
return -1;
}
if(entry->nchunks==0) {
return object_read(repo, entry->hash, out, len);
}
buf=malloc((size_t)entry->size+1);
if(!buf)
return -1;
uint64_t expected=0;
for(size_t i=0;i<entry->nchunks;i++) {
const struct mesh_chunk*chunk=&entry->chunks[i];
char*part=NULL;
size_t part_len=0;
if(chunk->offset!=expected||
chunk->length>entry->size-expected) {
free(buf);
errno=EINVAL;
return -1;
}
if(object_read(repo, chunk->hash,&part,
&part_len)<0) {
free(buf);
return -1;
}
if((uint64_t)part_len!=chunk->length) {
free(part);
free(buf);
errno=EIO;
return -1;
}
memcpy(buf+(size_t)chunk->offset, part, part_len);
free(part);
expected+=chunk->length;
}
if(expected!=entry->size) {
free(buf);
errno=EIO;
return -1;
}
uint8_t digest[MESH_HASH_LEN];
char actual[MESH_HASH_HEX+1];
static const char digits[]="0123456789abcdef";
blake2b_hash_buffer(buf, (size_t)entry->size, digest);
for(size_t i=0;i<MESH_HASH_LEN;i++) {
actual[2*i]=digits[digest[i]>>4];
actual[2*i+1]=digits[digest[i]&0x0f];
}
actual[MESH_HASH_HEX]='\0';
if(strcmp(actual, entry->hash)!=0) {
free(buf);
errno=EIO;
return -1;
}
buf[entry->size]='\0';
*out=buf;
if(len)
*len=(size_t)entry->size;
return 0;
}
static const char*git_mode(const struct mesh_file_entry*entry) {
if(!entry)
return "100644";
if(entry->kind==MESH_KIND_SYMLINK)
return "120000";
return (entry->mode&0111)?"100755":"100644";
}
static int emit_one(FILE*fp, const struct mesh_repo*repo,
const struct mesh_file_entry*old_entry,
const struct mesh_file_entry*new_entry) {
const struct mesh_file_entry*entry=new_entry?new_entry:old_entry;
const char*path;
if(!entry) {
errno=EINVAL;
return -1;
}
path=entry->path;
char*old_data=NULL,*new_data=NULL;
size_t old_len=0, new_len=0;
size_t old_lines, new_lines;
bool is_new=old_entry==NULL;
bool is_del=new_entry==NULL;
bool changed;
int rc=-1;
if(strpbrk(path,"\t\n\r\\\"")) {
fprintf(stderr,"mesh: patch: skipping path unsupported by "
"unified diff: %s\n", path);
return 0;
}
changed=is_new||is_del||
strcmp(old_entry->hash, new_entry->hash)!=0||
old_entry->mode!=new_entry->mode||
old_entry->kind!=new_entry->kind;
if(!changed)
return 0;
if((old_entry&&old_entry->size>MESH_PATCH_LIMIT)||
(new_entry&&new_entry->size>MESH_PATCH_LIMIT)) {
fprintf(stderr,
"mesh: patch: large change omitted: %s\n", path);
return 0;
}
if(!is_new) {
if(entry_read(repo, old_entry,&old_data,&old_len)<0)
goto done;
}
if(!is_del) {
if(entry_read(repo, new_entry,&new_data,&new_len)<0)
goto done;
}
if(binary_data(old_data, old_len)||binary_data(new_data, new_len)) {
/*
* A real Git binary patch needs base85 + zlib. Emitting a
* half-truth would make 'git apply' reject the whole file,
* so the binary change is reported and left out.
*/
fprintf(stderr,
"mesh: patch: binary change omitted: %s\n", path);
rc=0;
goto done;
}
fprintf(fp,"diff --git a/%s b/%s\n", path, path);
if(is_new)
fprintf(fp,"new file mode %s\n",git_mode(new_entry));
if(is_del)
fprintf(fp,"deleted file mode %s\n",git_mode(old_entry));
if(!is_new&&!is_del&&
(strcmp(git_mode(old_entry),git_mode(new_entry))!=0))
fprintf(fp,"old mode %s\nnew mode %s\n",
git_mode(old_entry),git_mode(new_entry));
old_lines=count_lines(old_data, old_len);
new_lines=count_lines(new_data, new_len);
/*
* An empty file has no lines to patch. Git represents creation and
* deletion of empty files with the mode header only.
*/
if(old_lines==0&&new_lines==0) {
rc=0;
goto done;
}
fprintf(fp,"--- %s%s\n", is_new?"/dev/null":"a/",
is_new?"":path);
fprintf(fp,"+++ %s%s\n", is_del?"/dev/null":"b/",
is_del?"":path);
fprintf(fp,"@@ -%d,%zu +%d,%zu @@\n",
old_lines?1:0, old_lines, new_lines?1:0, new_lines);
emit_lines(fp,'-', old_data, old_len);
emit_lines(fp,'+', new_data, new_len);
rc=0;
done: {
int saved=errno;
free(old_data);
free(new_data);
errno=saved;
}
return rc;
}
int bridge_write_patch(const struct mesh_repo*repo, const char*old_tree,
const char*new_tree, const char*dest) {
struct mesh_file_list old_list, new_list;
char tmp[MESH_PATH_MAX];
FILE*fp=NULL;
size_t i=0, j=0;
int rc=-1;
if(!repo||!new_tree||!dest) {
errno=EINVAL;
return -1;
}
file_list_init(&old_list);
file_list_init(&new_list);
if(old_tree&&old_tree[0]&&manifest_load(repo, old_tree,&old_list)<0)
goto done;
if(manifest_load(repo, new_tree,&new_list)<0)
goto done;
if(snprintf(tmp, sizeof(tmp),"%s.tmp.%ld", dest, (long)getpid())>=
(int)sizeof(tmp)) {
errno=ENAMETOOLONG;
goto done;
}
fp=fopen(tmp,"wb");
if(!fp)
goto done;
while(i<old_list.n||j<new_list.n) {
int cmp;
if(i>=old_list.n)
cmp=1;
else if(j>=new_list.n)
cmp=-1;
else
cmp=strcmp(old_list.v[i].path, new_list.v[j].path);
if(cmp<0) {
if(emit_one(fp, repo,&old_list.v[i], NULL)<0)
goto done;
i++;
} else if(cmp>0) {
if(emit_one(fp, repo, NULL,&new_list.v[j])<0)
goto done;
j++;
} else {
if(emit_one(fp, repo,&old_list.v[i],
&new_list.v[j])<0)
goto done;
i++;
j++;
}
}
if(fflush(fp)==EOF||ferror(fp))
goto done;
if(fsync(fileno(fp))<0)
goto done;
if(fclose(fp)!=0) {
fp=NULL;
goto done;
}
fp=NULL;
if(rename(tmp, dest)<0)
goto done;
rc=0;
done: {
int saved=errno;
if(fp)
fclose(fp);
if(rc!=0)
unlink(tmp);
file_list_free(&old_list);
file_list_free(&new_list);
errno=saved;
}
return rc;
}
