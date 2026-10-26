/*
* conflict.c - conflict isolation without code pollution.
* Mesh never writes <<<<<<< markers into a user's source tree. When two
* versions of the same path collide, the working file stays byte for
* byte as it was and two pure variants are parked in
* .mesh/store/conflicts/ as <stem>.alpha and <stem>.beta. The user
* chooses with 'mesh choose alpha|beta'.
*
* The v1 index records each escaped repository-relative path plus endpoint
* kinds and modes. Legacy path-only entries remain readable as file pairs.
* Variant file names are derived from the path, never from text supplied by
* the index. A forged index therefore cannot point the resolver at an
* arbitrary file inside (or outside) the repository.
*/
#include "core.h"

static int escape_index(const char*in, char*out, size_t cap) {
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
static int unescape_index(const char*in, char*out, size_t cap) {
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
static int index_path(const struct mesh_repo*repo, char*out, size_t cap) {
return mesh_path_join(out, cap, repo->mesh_dir,
"store/conflicts/index");
}
static int read_index(const struct mesh_repo*repo, char**out, size_t*len) {
char path[MESH_PATH_MAX];
if(index_path(repo, path, sizeof(path))<0)
return -1;
return mesh_read_text(path, out, len);
}
static bool index_has_path(const char*index, const char*escaped) {
const char*p=index;
size_t wanted;
if(!index||!escaped)
return false;
wanted=strlen(escaped);
while(*p) {
const char*nl=strchr(p,'\n');
size_t len=nl?(size_t)(nl-p):strlen(p);
const char*path=p;
const char*tab=memchr(p,'\t',len);
if(tab) {
path=tab+1;
len-=(size_t)(path-p);
}
if(len==wanted&&memcmp(path,escaped,wanted)==0)
return true;
if(!nl)
break;
p=nl+1;
}
return false;
}
/* Accept the historical key<TAB>path format but never trust the key. */
static const char*index_entry_path(const char*line) {
const char*tab=strchr(line,'\t');
return tab?tab+1:line;
}
/* Return 1 for the typed v1 record, 0 for a legacy file/file record. */
static int index_entry_types(const char*line,const char*variant,
uint32_t*kind,mode_t*mode) {
char header[48],*save=NULL,*part[5],*token;
size_t len;
unsigned long alpha_mode,beta_mode;
if(!line||!variant||!kind||!mode)
return -1;
*kind=MESH_KIND_FILE;
*mode=0644;
if(strncmp(line,"v1 ",3)!=0)
return 0;
const char*tab=strchr(line,'\t');
if(!tab)
return -1;
len=(size_t)(tab-line);
if(len>=sizeof(header))
return -1;
memcpy(header,line,len);
header[len]='\0';
size_t n=0;
for(token=strtok_r(header," ",&save);token;
token=strtok_r(NULL," ",&save)) {
if(n>=5)
return -1;
part[n++]=token;
}
if(n!=5||strcmp(part[0],"v1")!=0||strlen(part[1])!=1||
strlen(part[3])!=1||
(part[1][0]!='f'&&part[1][0]!='l')||
(part[3][0]!='f'&&part[3][0]!='l'))
return -1;
char*end=NULL;
errno=0;
alpha_mode=strtoul(part[2],&end,8);
if(errno||!end||*end||alpha_mode>0777)
return -1;
errno=0;
beta_mode=strtoul(part[4],&end,8);
if(errno||!end||*end||beta_mode>0777)
return -1;
if(strcmp(variant,"alpha")==0) {
*kind=part[1][0]=='l'?MESH_KIND_SYMLINK:MESH_KIND_FILE;
*mode=(mode_t)alpha_mode;
} else {
*kind=part[3][0]=='l'?MESH_KIND_SYMLINK:MESH_KIND_FILE;
*mode=(mode_t)beta_mode;
}
return 1;
}
/*
* Build a deterministic, filesystem-safe stem. The hash of the full path
* makes collisions practically impossible and, more importantly, means
* the variant location cannot be influenced by a forged index entry.
*/
static int conflict_stem(const char*relpath, char*out, size_t cap) {
static const char digits[]="0123456789abcdef";
uint8_t digest[MESH_HASH_LEN];
const char*base=strrchr(relpath,'/');
char safe[81];
size_t o=0;
int n;
base=base?base+1:relpath;
blake2b_hash_buffer(relpath,strlen(relpath),digest);
for(size_t i=0;i<8;i++) {
safe[o++]=digits[digest[i]>>4];
safe[o++]=digits[digest[i]&0x0f];
}
safe[o++]='-';
for(size_t i=0;base[i]&&o<sizeof(safe)-1;i++) {
unsigned char c=(unsigned char)base[i];
safe[o++]=(c=='.'||c=='-'||c=='_'||
(c>='0'&&c<='9')||(c>='A'&&c<='Z')||
(c>='a'&&c<='z'))?(char)c:'_';
}
safe[o]='\0';
n=snprintf(out,cap,"%s",safe);
if(n<0||(size_t)n>=cap) {
errno=ENAMETOOLONG;
return -1;
}
return 0;
}
static int variant_path(const struct mesh_repo*repo, const char*stem,
const char*variant, char*out, size_t cap) {
char rel[256];
int n;
n=snprintf(rel,sizeof(rel),"store/conflicts/%s.%s",stem,variant);
if(n<0||(size_t)n>=sizeof(rel)) {
errno=ENAMETOOLONG;
return -1;
}
return mesh_path_join(out,cap,repo->mesh_dir,rel);
}
int conflict_stage_typed(const struct mesh_repo*repo,const char*relpath,
const char*alpha_src,const char*beta_src,uint32_t alpha_kind,
mode_t alpha_mode,uint32_t beta_kind,mode_t beta_mode) {
char dir[MESH_PATH_MAX], alpha[MESH_PATH_MAX], beta[MESH_PATH_MAX];
char index_file[MESH_PATH_MAX], escaped[MESH_PATH_MAX*2+1];
char line[MESH_PATH_MAX*2+64], stem[128];
char*index=NULL;
size_t index_len=0;
int fd=-1,n;
if(!repo||!repo->valid||!relpath||!alpha_src||!beta_src||
!mesh_relpath_safe(relpath)||
(alpha_kind!=MESH_KIND_FILE&&alpha_kind!=MESH_KIND_SYMLINK)||
(beta_kind!=MESH_KIND_FILE&&beta_kind!=MESH_KIND_SYMLINK)||
(alpha_mode&~0777u)||(beta_mode&~0777u)) {
errno=EINVAL;
return -1;
}
if(escape_index(relpath,escaped,sizeof(escaped))<0||
conflict_stem(relpath,stem,sizeof(stem))<0)
return -1;
if(mesh_path_join(dir,sizeof(dir),repo->mesh_dir,
"store/conflicts")<0||mesh_mkdir_p(dir)<0)
return -1;
if(variant_path(repo,stem,"alpha",alpha,sizeof(alpha))<0||
variant_path(repo,stem,"beta",beta,sizeof(beta))<0)
return -1;
if(mesh_path_no_symlink(alpha)<0||mesh_path_no_symlink(beta)<0)
return -1;
if(mesh_copy_path_to_path(alpha_src,alpha,alpha_mode)<0)
return -1;
if(mesh_copy_path_to_path(beta_src,beta,beta_mode)<0) {
int saved=errno;
unlink(alpha);
errno=saved;
return -1;
}
if(read_index(repo,&index,&index_len)==0) {
bool present=index_has_path(index,escaped);
free(index);
if(present)
return 0;
} else if(errno!=ENOENT) {
return -1;
}
n=snprintf(line,sizeof(line),"v1 %c %03o %c %03o\t%s\n",
alpha_kind==MESH_KIND_SYMLINK?'l':'f',alpha_mode,
beta_kind==MESH_KIND_SYMLINK?'l':'f',beta_mode,escaped);
if(n<0||(size_t)n>=sizeof(line)) {
errno=ENAMETOOLONG;
return -1;
}
if(index_path(repo,index_file,sizeof(index_file))<0)
return -1;
fd=open(index_file,O_WRONLY|O_CREAT|O_APPEND|O_CLOEXEC|O_NOFOLLOW,
0644);
if(fd<0)
return -1;
if(flock(fd,LOCK_EX)<0) {
int saved=errno;
close(fd);
errno=saved;
return -1;
}
if(mesh_write_all(fd,line,(size_t)n)<0||fsync(fd)<0) {
int saved=errno;
flock(fd,LOCK_UN);
close(fd);
errno=saved;
return -1;
}
flock(fd,LOCK_UN);
return close(fd);
}
int conflict_stage(const struct mesh_repo*repo,const char*relpath,
const char*alpha_src,const char*beta_src) {
struct stat ast,bst;
mode_t alpha_mode=0644,beta_mode=0644;
if(alpha_src&&lstat(alpha_src,&ast)==0&&S_ISREG(ast.st_mode))
alpha_mode=(mode_t)(ast.st_mode&0777);
if(beta_src&&lstat(beta_src,&bst)==0&&S_ISREG(bst.st_mode))
beta_mode=(mode_t)(bst.st_mode&0777);
return conflict_stage_typed(repo,relpath,alpha_src,beta_src,
MESH_KIND_FILE,alpha_mode,MESH_KIND_FILE,beta_mode);
}
int conflict_count(const struct mesh_repo*repo) {
char*index=NULL;
size_t len=0;
int count=0;
if(!repo||!repo->valid)
return 0;
if(read_index(repo,&index,&len)<0)
return 0;
char*save=NULL;
for(char*line=strtok_r(index,"\n",&save);line;
line=strtok_r(NULL,"\n",&save)) {
char rel[MESH_PATH_MAX];
const char*path=index_entry_path(line);
if(path[0]&&unescape_index(path,rel,sizeof(rel))==0&&
mesh_relpath_safe(rel))
count++;
}
free(index);
return count;
}
int conflict_list(const struct mesh_repo*repo) {
char*index=NULL;
size_t len=0;
if(!repo||!repo->valid) {
errno=EINVAL;
return -1;
}
if(read_index(repo,&index,&len)<0) {
if(errno==ENOENT) {
printf("No conflicts.\n");
return 0;
}
return -1;
}
if(len==0) {
free(index);
printf("No conflicts.\n");
return 0;
}
printf("Conflicts staged in %s/store/conflicts:\n", repo->mesh_dir);
char*save=NULL;
int shown=0;
for(char*line=strtok_r(index,"\n",&save);line;
line=strtok_r(NULL,"\n",&save)) {
char rel[MESH_PATH_MAX];
const char*path=index_entry_path(line);
if(path[0]&&unescape_index(path,rel,sizeof(rel))==0&&
mesh_relpath_safe(rel)) {
uint32_t alpha_kind,beta_kind;
mode_t alpha_mode,beta_mode;
int alpha_typed=index_entry_types(line,"alpha",&alpha_kind,&alpha_mode);
int beta_typed=index_entry_types(line,"beta",&beta_kind,&beta_mode);
if(alpha_typed>0&&beta_typed>0)
printf("  %s  [alpha %s %03o]  [beta %s %03o]\n",rel,
alpha_kind==MESH_KIND_SYMLINK?"symlink":"file",alpha_mode,
beta_kind==MESH_KIND_SYMLINK?"symlink":"file",beta_mode);
else
printf("  %s  [alpha]  [beta]\n", rel);
shown++;
}
}
if(shown==0)
printf("No conflicts.\n");
free(index);
return 0;
}
int conflict_choose(const struct mesh_repo*repo, const char*variant,
size_t*resolved) {
char*index=NULL;
size_t len=0;
char*keep=NULL;
size_t keep_len=0;
FILE*keep_fp=NULL;
char index_file[MESH_PATH_MAX];
char**chosen=NULL;
size_t chosen_cap=0;
size_t done=0, failed=0;
int lock=-1;
if(resolved)
*resolved=0;
if(!repo||!repo->valid||!variant) {
errno=EINVAL;
return -1;
}
if(strcmp(variant,"alpha")!=0&&strcmp(variant,"beta")!=0) {
errno=EINVAL;
return -1;
}
lock=mesh_repo_lock(repo);
if(lock<0)
return -1;
if(read_index(repo,&index,&len)<0) {
if(errno==ENOENT) {
close(lock);
return 0;
}
goto fail;
}
if(len==0) {
free(index);
close(lock);
return 0;
}
keep_fp=open_memstream(&keep,&keep_len);
if(!keep_fp)
goto fail;
char*save=NULL;
for(char*line=strtok_r(index,"\n",&save);line;
line=strtok_r(NULL,"\n",&save)) {
char rel[MESH_PATH_MAX],stem[128];
char src[MESH_PATH_MAX],other[MESH_PATH_MAX];
uint32_t selected_kind;
mode_t selected_mode;
int typed=index_entry_types(line,variant,&selected_kind,&selected_mode);
const char*path=index_entry_path(line);
if(!path[0])
continue;
if(typed<0) {
fprintf(keep_fp,"%s\n",line);
failed++;
continue;
}
if(unescape_index(path,rel,sizeof(rel))<0||
!mesh_relpath_safe(rel)||
conflict_stem(rel,stem,sizeof(stem))<0||
variant_path(repo,stem,variant,src,sizeof(src))<0||
variant_path(repo,stem,strcmp(variant,"alpha")==0?
"beta":"alpha",other,sizeof(other))<0) {
fprintf(keep_fp,"%s\n",line);
failed++;
continue;
}
bool duplicate=false;
for(size_t i=0;i<done;i++)
if(strcmp(chosen[i],src)==0) {
duplicate=true;
break;
}
if(duplicate)
continue;
int root=open(repo->root,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
if(root<0) {
fprintf(keep_fp,"%s\n",line);
failed++;
continue;
}
struct stat st;
bool ok=lstat(src,&st)==0&&S_ISREG(st.st_mode)&&
st.st_size>=0;
if(ok) {
if(typed==0)
selected_mode=(mode_t)(st.st_mode&0777);
if(selected_kind==MESH_KIND_FILE)
ok=mesh_copy_to_rel(root,rel,src,selected_mode)==0;
else {
char*target=NULL;
size_t target_len=0;
if(mesh_read_text(src,&target,&target_len)==0&&target_len>0&&
target_len<MESH_PATH_MAX&&memchr(target,'\0',target_len)==NULL)
ok=mesh_symlink_at_rel(root,rel,target,selected_mode)==0;
else
ok=false;
free(target);
}
}
close(root);
if(!ok) {
fprintf(keep_fp,"%s\n",line);
failed++;
continue;
}
if(chosen_cap<done+1) {
size_t cap=chosen_cap?chosen_cap*2:16;
char**next=realloc(chosen,cap*sizeof(*next));
if(!next) {
fprintf(keep_fp,"%s\n",line);
failed++;
continue;
}
chosen=next;
chosen_cap=cap;
}
chosen[done]=strdup(src);
if(!chosen[done]) {
fprintf(keep_fp,"%s\n",line);
failed++;
continue;
}
done++;
}
if(fflush(keep_fp)==EOF||ferror(keep_fp))
goto fail;
if(fclose(keep_fp)!=0) {
keep_fp=NULL;
goto fail;
}
keep_fp=NULL;
if(index_path(repo,index_file,sizeof(index_file))<0)
goto fail;
if(mesh_write_atomic(index_file,keep?keep:"",keep_len,0644)<0)
goto fail;
for(size_t i=0;i<done;i++) {
char other[MESH_PATH_MAX];
char*dot=strrchr(chosen[i],'.');
if(dot&&(size_t)(dot-chosen[i])+8<sizeof(other)) {
int n=snprintf(other,sizeof(other),"%.*s.alpha",
(int)(dot-chosen[i]),chosen[i]);
if(n>0&&(size_t)n<sizeof(other))
unlink(other);
n=snprintf(other,sizeof(other),"%.*s.beta",
(int)(dot-chosen[i]),chosen[i]);
if(n>0&&(size_t)n<sizeof(other))
unlink(other);
}
free(chosen[i]);
}
free(chosen);
free(keep);
free(index);
close(lock);
if(resolved)
*resolved=done;
if(failed>0) {
errno=EIO;
return -1;
}
return 0;
fail: {
int saved=errno;
if(keep_fp)
fclose(keep_fp);
for(size_t i=0;i<done;i++)
free(chosen?chosen[i]:NULL);
free(chosen);
free(keep);
free(index);
if(lock>=0)
close(lock);
errno=saved;
return -1;
}
}
