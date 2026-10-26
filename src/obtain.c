/*
* obtain.c - create a Mesh repository from a forge source archive.
* GitHub, GitLab, Gitea and their derivatives expose a repository tree as a
* gzip tar archive. Mesh resolves the forge URL, downloads that archive into a
* staging directory, extracts it without following links and initializes the
* .mesh skeleton there. The caller records the first snapshot and then commits
* the staging directory with one rename.
*/
#include "core.h"
#include <ctype.h>
#include <strings.h>
struct obtain_url {
char origin[MESH_PATH_MAX];
char authority[MESH_PATH_MAX];
char host[MESH_PATH_MAX];
char path[MESH_PATH_MAX];
};
static int obtain_copy(char*out, size_t cap, const char*in, size_t len) {
if(!out||cap==0||!in||len>=cap) {
errno=ENAMETOOLONG;
return -1;
}
memcpy(out, in, len);
out[len]='\0';
return 0;
}
static int obtain_split_url(const char*url, struct obtain_url*out) {
const char*scheme, *authority, *end, *path;
size_t scheme_len, authority_len, path_len;
if(!mesh_http_url_ok(url)||!out) {
errno=EINVAL;
return -1;
}
memset(out, 0, sizeof(*out));
scheme=strstr(url,"://");
scheme_len=(size_t)(scheme-url);
authority=scheme+3;
end=authority;
while(*end&&*end!='/'&&*end!='?'&&*end!='#')
end++;
if(end==authority||*end!='/')
return -1;
for(const char*p=authority;p<end;p++) {
unsigned char c=(unsigned char)*p;
if(c<=0x20||c==0x7f||c=='@') {
errno=EINVAL;
return -1;
}
}
authority_len=(size_t)(end-authority);
path=end;
path_len=strlen(path);
if(path_len>=sizeof(out->path)) {
errno=ENAMETOOLONG;
return -1;
}
if(memchr(path,'?',path_len)!=NULL||memchr(path,'#',path_len)!=NULL) {
errno=EINVAL;
return -1;
}
for(size_t i=0;i<path_len;i++) {
unsigned char c=(unsigned char)path[i];
if(c<=0x20||c==0x7f) {
errno=EINVAL;
return -1;
}
}
if(obtain_copy(out->authority, sizeof(out->authority),
authority, authority_len)<0)
return -1;
for(size_t i=0;i<authority_len;i++)
out->authority[i]=mesh_ascii_lower((char)(unsigned char)out->authority[i]);
if(snprintf(out->origin, sizeof(out->origin),"%.*s://%s",
(int)scheme_len, url, out->authority)>=(int)sizeof(out->origin)) {
errno=ENAMETOOLONG;
return -1;
}
const char*colon=strrchr(out->authority,':');
size_t host_len=colon?(size_t)(colon-out->authority):strlen(out->authority);
if(out->authority[0]=='[') {
const char*close=strchr(out->authority,']');
if(close)
host_len=(size_t)(close-out->authority+1);
}
if(host_len==0||host_len>=sizeof(out->host)) {
errno=EINVAL;
return -1;
}
memcpy(out->host, out->authority, host_len);
out->host[host_len]='\0';
if(obtain_copy(out->path, sizeof(out->path), path, path_len)<0)
return -1;
while(path_len>1&&out->path[path_len-1]=='/')
out->path[--path_len]='\0';
if(strcmp(out->path,"/")==0) {
errno=EINVAL;
return -1;
}
const char*p=out->path+1;
while(*p) {
const char*slash=strchr(p,'/');
size_t len=slash?(size_t)(slash-p):strlen(p);
if(len==0||(len==1&&p[0]=='.')||
(len==2&&p[0]=='.'&&p[1]=='.')) {
errno=EINVAL;
return -1;
}
if(!slash)
break;
p=slash+1;
}
return 0;
}
static int obtain_archive_authority(const char*url,
char*out, size_t cap) {
const char*scheme, *authority;
size_t len;
if(!mesh_http_url_ok(url)||!out||cap==0) {
errno=EINVAL;
return -1;
}
scheme=strstr(url,"://");
authority=scheme+3;
len=strcspn(authority,"/?#");
if(len==0)
return -1;
for(size_t i=0;i<len;i++) {
unsigned char c=(unsigned char)authority[i];
if(c<=0x20||c==0x7f||c=='@') {
errno=EINVAL;
return -1;
}
}
if(len>=cap) {
errno=ENAMETOOLONG;
return -1;
}
for(size_t i=0;i<len;i++)
out[i]=mesh_ascii_lower(authority[i]);
out[len]='\0';
return 0;
}
static int obtain_unquote_name(const char*in, char*out, size_t cap) {
size_t o=0;
for(size_t i=0;in[i];i++) {
unsigned char c=(unsigned char)in[i];
if(c=='%'&&isxdigit((unsigned char)in[i+1])&&
isxdigit((unsigned char)in[i+2])) {
int hi=isdigit((unsigned char)in[i+1])?
in[i+1]-'0':(tolower((unsigned char)in[i+1])-'a'+10);
int lo=isdigit((unsigned char)in[i+2])?
in[i+2]-'0':(tolower((unsigned char)in[i+2])-'a'+10);
c=(unsigned char)((hi<<4)|lo);
i+=2;
}
if(c=='/'||c<0x20||c==0x7f||o+1>=cap) {
errno=EINVAL;
return -1;
}
out[o++]=(char)c;
}
out[o]='\0';
if(o==0||strcmp(out,".")==0||strcmp(out,"..")==0) {
errno=EINVAL;
return -1;
}
return 0;
}
static int obtain_ref_valid(const char*ref) {
size_t len;
if(!ref||ref[0]=='\0')
return 0;
len=strlen(ref);
if(len>=MESH_PATH_MAX||ref[0]=='/'||ref[len-1]=='/'||
strstr(ref,"//")!=NULL||strstr(ref,"..")!=NULL)
return 0;
for(size_t i=0;i<len;i++) {
unsigned char c=(unsigned char)ref[i];
if(c<=0x20||c==0x7f||c=='\\'||c=='?'||c=='#')
return 0;
}
return 1;
}
static int obtain_encode(char*out, size_t cap, const char*src,
bool keep_slash) {
static const char digits[]="0123456789ABCDEF";
size_t o=0;
for(const unsigned char*p=(const unsigned char*)src;*p;p++) {
unsigned char c=*p;
bool plain=(c>='a'&&c<='z')||(c>='A'&&c<='Z')||
(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~'||
(keep_slash&&c=='/');
if(plain) {
if(o+1>=cap) {
errno=ENAMETOOLONG;
return -1;
}
out[o++]=(char)c;
} else {
if(o+3>=cap) {
errno=ENAMETOOLONG;
return -1;
}
out[o++]='%';
out[o++]=digits[c>>4];
out[o++]=digits[c&0x0f];
}
}
out[o]='\0';
return 0;
}
static int obtain_project_path(const char*path, char*out, size_t cap,
char*project, size_t project_cap) {
size_t len=strlen(path);
size_t last;
if(len>4&&strcmp(path+len-4,".git")==0)
len-=4;
if(len<2||path[0]!='/'||len>=cap) {
errno=EINVAL;
return -1;
}
last=len;
while(last>0&&path[last-1]!='/')
last--;
if(last==len||last<2||project_cap==0) {
errno=EINVAL;
return -1;
}
memcpy(out, path, len);
out[len]='\0';
if(obtain_unquote_name(out+last, project, project_cap)<0)
return -1;
return 0;
}
static size_t obtain_path_components(const char*path) {
size_t n=0;
const char*p=path;
while(*p=='/')
p++;
while(*p) {
n++;
const char*slash=strchr(p,'/');
if(!slash)
break;
p=slash+1;
}
return n;
}
int mesh_obtain_is_repo_url(const char*url) {
char clean[MESH_PATH_MAX];
struct obtain_url parsed;
size_t len, components;
const char*base;
const char*host;
if(!mesh_http_url_ok(url)) {
errno=EINVAL;
return -1;
}
len=strcspn(url,"?#");
if(len>=sizeof(clean)) {
errno=ENAMETOOLONG;
return -1;
}
memcpy(clean, url, len);
clean[len]='\0';
if(obtain_split_url(clean,&parsed)<0)
return 0;
components=obtain_path_components(parsed.path);
if(components<2)
return 0;
base=strrchr(parsed.path,'/');
base=base?base+1:parsed.path;
host=parsed.host;
if(strcmp(host,"github.com")==0||strcmp(host,"www.github.com")==0)
return components==2;
if(strcmp(host,"gitlab.com")==0)
return strstr(parsed.path,"/-/")==NULL;
if(components!=2)
return 0;
if(strlen(base)>4&&strcmp(base+strlen(base)-4,".git")==0)
return 1;
return strchr(base,'.')==NULL;
}
static int obtain_github(const struct obtain_url*parsed,
const char*ref, char*out, size_t cap) {
char encoded[MESH_PATH_MAX*3];
char path[MESH_PATH_MAX];
size_t len=strlen(parsed->path);
if(strcmp(parsed->host,"github.com")!=0&&
strcmp(parsed->host,"www.github.com")!=0) {
errno=ENOENT;
return -1;
}
if(len>4&&strcmp(parsed->path+len-4,".git")==0)
len-=4;
if(len<2||parsed->path[0]!='/') {
errno=EINVAL;
return -1;
}
memcpy(path, parsed->path, len);
path[len]='\0';
const char*p=path+1;
unsigned components=0;
while(*p) {
const char*slash=strchr(p,'/');
if(!slash) {
components++;
break;
}
components++;
p=slash+1;
}
if(components!=2) {
errno=EINVAL;
return -1;
}
if(obtain_encode(encoded, sizeof(encoded), ref, true)<0)
return -1;
if(snprintf(out, cap,"https://codeload.github.com%s/tar.gz/%s",
path, encoded)>=(int)cap) {
errno=ENAMETOOLONG;
return -1;
}
return 0;
}
static int obtain_gitlab(const struct obtain_url*parsed,
const char*ref, char*out, size_t cap) {
char encoded[MESH_PATH_MAX*3];
char path[MESH_PATH_MAX];
char name[256];
if(obtain_project_path(parsed->path, path, sizeof(path),
name, sizeof(name))<0)
return -1;
if(obtain_encode(encoded, sizeof(encoded), ref, false)<0)
return -1;
if(snprintf(out, cap,"%s%s/-/archive/%s/%s-archive.tar.gz",
parsed->origin, path, encoded, name)>=(int)cap) {
errno=ENAMETOOLONG;
return -1;
}
return 0;
}
static int obtain_gitea(const struct obtain_url*parsed,
const char*ref, char*out, size_t cap) {
char encoded[MESH_PATH_MAX*3];
char path[MESH_PATH_MAX];
char project[256];
if(obtain_project_path(parsed->path, path, sizeof(path),
project, sizeof(project))<0)
return -1;
if(obtain_encode(encoded, sizeof(encoded), ref, true)<0)
return -1;
if(snprintf(out, cap,"%s%s/archive/%s.tar.gz",
parsed->origin, path, encoded)>=(int)cap) {
errno=ENAMETOOLONG;
return -1;
}
return 0;
}
static int obtain_ref_plan(const struct obtain_url*parsed,
const char*ref, char*out, size_t cap) {
if(strcmp(parsed->host,"github.com")==0||
strcmp(parsed->host,"www.github.com")==0)
return obtain_github(parsed, ref, out, cap);
if(strcmp(parsed->host,"gitlab.com")==0)
return obtain_gitlab(parsed, ref, out, cap);
return obtain_gitea(parsed, ref, out, cap);
}
int mesh_obtain_plan(const char*url, const char*ref,
const char*archive, char*archive_url, size_t archive_cap,
char*dest_name, size_t dest_cap, char*authority, size_t authority_cap) {
struct obtain_url parsed;
const char*chosen;
char project[MESH_PATH_MAX];
char path[MESH_PATH_MAX];
if(!archive_url||archive_cap==0||!dest_name||dest_cap==0||
!authority||authority_cap==0) {
errno=EINVAL;
return -1;
}
archive_url[0]='\0';
dest_name[0]='\0';
authority[0]='\0';
if(obtain_split_url(url,&parsed)<0)
return -1;
if(obtain_project_path(parsed.path, path, sizeof(path),
project, sizeof(project))<0)
return -1;
if(strlen(project)>=dest_cap) {
errno=ENAMETOOLONG;
return -1;
}
snprintf(dest_name, dest_cap,"%s", project);
if(archive&&archive[0]) {
if(!mesh_http_url_ok(archive)) {
errno=EINVAL;
return -1;
}
chosen=archive;
} else {
const char*wanted=ref&&ref[0]?ref:"HEAD";
if(!obtain_ref_valid(wanted)) {
errno=EINVAL;
return -1;
}
if(obtain_ref_plan(&parsed, wanted, archive_url, archive_cap)<0)
return -1;
chosen=archive_url;
}
if(chosen!=archive_url) {
if(strlen(chosen)>=archive_cap) {
errno=ENAMETOOLONG;
return -1;
}
snprintf(archive_url, archive_cap,"%s", chosen);
}
if(obtain_archive_authority(archive_url, authority, authority_cap)<0)
return -1;
return 0;
}
static int obtain_dir_empty(const char*path, bool*empty) {
DIR*dir;
struct dirent*de;
int fd=open(path, O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
if(fd<0)
return -1;
dir=fdopendir(fd);
if(!dir) {
int saved=errno;
close(fd);
errno=saved;
return -1;
}
*empty=true;
while((errno=0, de=readdir(dir))!=NULL) {
if(strcmp(de->d_name,".")==0||strcmp(de->d_name,"..")==0)
continue;
*empty=false;
break;
}
int saved=errno;
if(closedir(dir)<0)
return -1;
errno=saved;
return saved==0?0:-1;
}
static int obtain_parent(char*out, size_t cap, const char*path) {
const char*slash=strrchr(path,'/');
if(!slash)
return obtain_copy(out, cap,".", 1);
if(slash==path)
return obtain_copy(out, cap,"/", 1);
return obtain_copy(out, cap, path, (size_t)(slash-path));
}
static int obtain_remove_at(int parent, const char*name) {
struct stat st;
int fd, rc;
if(fstatat(parent, name,&st, AT_SYMLINK_NOFOLLOW)<0)
return errno==ENOENT?0:-1;
if(!S_ISDIR(st.st_mode))
return unlinkat(parent, name, 0);
fd=openat(parent, name, O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
if(fd<0)
return -1;
DIR*dir=fdopendir(fd);
if(!dir) {
int saved=errno;
close(fd);
errno=saved;
return -1;
}
rc=0;
for(;;) {
errno=0;
struct dirent*de=readdir(dir);
if(!de) {
if(errno!=0)
rc=-1;
break;
}
if(strcmp(de->d_name,".")==0||strcmp(de->d_name,"..")==0)
continue;
if(obtain_remove_at(dirfd(dir), de->d_name)<0) {
rc=-1;
break;
}
}
int saved=errno;
closedir(dir);
if(rc==0&&unlinkat(parent, name, AT_REMOVEDIR)<0) {
saved=errno;
rc=-1;
}
errno=saved;
return rc;
}
void mesh_obtain_abort(const char*stage) {
char parent[MESH_PATH_MAX], base[MESH_PATH_MAX];
const char*slash;
int fd;
if(!stage||stage[0]=='\0')
return;
slash=strrchr(stage,'/');
if(!slash)
return;
if(obtain_parent(parent, sizeof(parent), stage)<0)
return;
snprintf(base, sizeof(base),"%s", slash+1);
fd=open(parent, O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
if(fd<0)
return;
if(obtain_remove_at(fd, base)<0)
fprintf(stderr,"mesh: obtain: cleanup failed for %s: %s\n",
stage, strerror(errno));
close(fd);
}
int mesh_obtain_commit(const char*stage, const char*final) {
char parent[MESH_PATH_MAX];
struct stat st;
bool empty=false;
int fd, rc=-1;
if(!stage||!final) {
errno=EINVAL;
return -1;
}
if(lstat(final,&st)==0) {
if(!S_ISDIR(st.st_mode)||obtain_dir_empty(final,&empty)<0||!empty) {
errno=EEXIST;
return -1;
}
if(rmdir(final)<0)
return -1;
} else if(errno!=ENOENT) {
return -1;
}
if(rename(stage, final)<0)
return -1;
if(obtain_parent(parent, sizeof(parent), final)<0)
return -1;
fd=open(parent, O_RDONLY|O_DIRECTORY|O_CLOEXEC);
if(fd<0)
return -1;
rc=mesh_fsync_dir(fd);
int saved=errno;
close(fd);
errno=saved;
return rc;
}
static int obtain_credentials(const struct mesh_repo*repo,
const char*authority, const char*token) {
char path[MESH_PATH_MAX];
char line[MESH_AUTH_MAX+512];
size_t len=strlen(token);
int n;
if(len==0||len>MESH_AUTH_MAX) {
errno=EINVAL;
return -1;
}
for(size_t i=0;i<len;i++) {
unsigned char c=(unsigned char)token[i];
if(c<0x21||c>0x7e) {
errno=EINVAL;
return -1;
}
}
if(mesh_path_join(path, sizeof(path), repo->mesh_dir,"credentials")<0)
return -1;
n=snprintf(line, sizeof(line),"%s %s\n", authority, token);
if(n<0||(size_t)n>=sizeof(line)) {
errno=ENAMETOOLONG;
return -1;
}
return mesh_write_atomic(path, line, (size_t)n, 0600);
}
int mesh_obtain_stage_ex(const char*url, const char*ref,
const char*archive, const char*dest,
const struct mesh_download_options*download,
char*stage, size_t stage_cap, char*final, size_t final_cap,
uint64_t*bytes) {
char archive_url[MESH_PATH_MAX];
char derived[256];
char authority[MESH_PATH_MAX];
char wanted[MESH_PATH_MAX];
char final_parent[MESH_PATH_MAX], resolved[MESH_PATH_MAX];
char template[MESH_PATH_MAX], archive_path[MESH_PATH_MAX];
char base[256];
struct mesh_repo source, target;
struct stat st;
const char*token;
bool have_source=false, empty=false;
int rc=-1;
if(!stage||stage_cap==0||!final||final_cap==0) {
errno=EINVAL;
return -1;
}
stage[0]='\0';
final[0]='\0';
if(bytes)
*bytes=0;
if(mesh_obtain_plan(url, ref, archive, archive_url, sizeof(archive_url),
derived, sizeof(derived), authority, sizeof(authority))<0)
return -1;
if(dest&&dest[0]) {
if(strlen(dest)>=sizeof(wanted)) {
errno=ENAMETOOLONG;
return -1;
}
snprintf(wanted, sizeof(wanted),"%s", dest);
} else {
snprintf(wanted, sizeof(wanted),"%s", derived);
}
size_t wlen=strlen(wanted);
while(wlen>1&&wanted[wlen-1]=='/')
wanted[--wlen]='\0';
if(strcmp(wanted,".")==0||strcmp(wanted,"..")==0||
wanted[0]=='\0'||wanted[wlen-1]=='/') {
errno=EINVAL;
return -1;
}
if(obtain_parent(final_parent, sizeof(final_parent), wanted)<0)
return -1;
const char*slash=strrchr(wanted,'/');
const char*name=slash?slash+1:wanted;
if(name[0]=='\0'||strcmp(name,".")==0||strcmp(name,"..")==0||
strlen(name)>=sizeof(base)) {
errno=EINVAL;
return -1;
}
if(!realpath(final_parent, resolved)) {
if(errno==ENOENT)
fprintf(stderr,"mesh: obtain: parent directory does not exist: %s\n",
final_parent);
return -1;
}
if(snprintf(final, final_cap,"%s/%s", resolved, name)>=(int)final_cap) {
errno=ENAMETOOLONG;
return -1;
}
if(lstat(final,&st)==0) {
if(!S_ISDIR(st.st_mode)||obtain_dir_empty(final,&empty)<0||!empty) {
fprintf(stderr,"mesh: obtain: destination exists and is not empty: %s\n",
final);
errno=EEXIST;
return -1;
}
} else if(errno!=ENOENT) {
return -1;
}
if(snprintf(template, sizeof(template),"%s/.mesh-obtain-XXXXXX",
resolved)>=(int)sizeof(template)) {
errno=ENAMETOOLONG;
return -1;
}
if(!mkdtemp(template)) {
return -1;
}
if(strlen(template)>=stage_cap) {
mesh_obtain_abort(template);
errno=ENAMETOOLONG;
return -1;
}
snprintf(stage, stage_cap,"%s", template);
if(mesh_repo_init_at(&target, stage)<0)
goto fail;
have_source=mesh_repo_open(&source)==0;
token=getenv("MESH_TOKEN");
if(download&&download->insecure) {
char auth[MESH_AUTH_MAX+64];
if(token&&token[0]) {
fprintf(stderr,"mesh: obtain: refusing to disable TLS verification "
"with MESH_TOKEN credentials\n");
errno=EPERM;
goto fail;
}
if(have_source) {
int auth_rc=net_auth_header(&source, archive_url, auth,
sizeof(auth));
if(auth_rc<0)
goto fail;
if(auth_rc==0) {
fprintf(stderr,"mesh: obtain: refusing to disable TLS "
"verification with repository credentials\n");
errno=EPERM;
goto fail;
}
}
}
if(download&&download->follow_redirects) {
char auth[MESH_AUTH_MAX+64];
if(token&&token[0]) {
fprintf(stderr,"mesh: obtain: refusing to follow redirects with "
"MESH_TOKEN credentials\n");
errno=EPERM;
goto fail;
}
if(have_source) {
int auth_rc=net_auth_header(&source, archive_url, auth,
sizeof(auth));
if(auth_rc<0)
goto fail;
if(auth_rc==0) {
fprintf(stderr,"mesh: obtain: refusing to follow redirects "
"with repository credentials\n");
errno=EPERM;
goto fail;
}
}
}
if(token&&token[0]) {
if(obtain_credentials(&target, authority, token)<0) {
fprintf(stderr,"mesh: obtain: invalid MESH_TOKEN\n");
goto fail;
}
have_source=false;
}
if(mesh_path_join(archive_path, sizeof(archive_path), target.mesh_dir,
"store/tmp/obtain.tar.gz")<0)
goto fail;
if(net_download_ex(have_source?&source:&target, archive_url, archive_path,
download, bytes, NULL, NULL)<0) {
fprintf(stderr,"mesh: obtain: download failed: %s\n", strerror(errno));
goto fail;
}
if(mesh_archive_extract_gzip(archive_path, stage)<0) {
fprintf(stderr,"mesh: obtain: extraction failed: %s\n", strerror(errno));
goto fail;
}
if(unlink(archive_path)<0&&errno!=ENOENT)
goto fail;
if(chmod(stage, 0755)<0)
goto fail;
rc=0;
return 0;
fail: {
int saved=errno;
if(stage[0])
mesh_obtain_abort(stage);
stage[0]='\0';
final[0]='\0';
errno=saved;
}
return rc;
}
int mesh_obtain_stage(const char*url, const char*ref,
const char*archive, const char*dest,
char*stage, size_t stage_cap, char*final, size_t final_cap,
uint64_t*bytes) {
return mesh_obtain_stage_ex(url, ref, archive, dest, NULL,
stage, stage_cap, final, final_cap, bytes);
}
