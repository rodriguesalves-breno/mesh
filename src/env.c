/*
* env.c - native environment automation.
* No shell scripts, no sed, no prayers. Mesh discovers the repository,
* provisions its skeleton and writes the PATH block directly into the
* user's shell startup files using POSIX file APIs. Every write is
* idempotent: the marker block is written once and once only.
*/
#include "core.h"
static int write_if_missing(const char*path, const char*data, size_t len,
mode_t mode) {
struct stat st;
if(lstat(path,&st)==0) {
if(!S_ISREG(st.st_mode)) {
errno=ELOOP;
return -1;
}
return 0;
}
if(errno!=ENOENT)
return -1;
return mesh_write_atomic(path, data, len, mode);
}
int mesh_config_branch(const struct mesh_repo*repo, char*out, size_t cap) {
char path[MESH_PATH_MAX], line[512];
FILE*fp;
if(!repo||!out||cap==0) {
errno=EINVAL;
return -1;
}
snprintf(out, cap,"main");
if(mesh_path_join(path, sizeof(path), repo->mesh_dir,"config")<0)
return -1;
fp=fopen(path,"r");
if(!fp)
return -1;
while(fgets(line, sizeof(line), fp)) {
mesh_trim_newline(line);
if(strncmp(line,"branch=", 7)==0&&line[7]!='\0') {
if(strlen(line+7)>=cap) {
fclose(fp);
errno=ENAMETOOLONG;
return -1;
}
snprintf(out, cap,"%s", line+7);
break;
}
}
if(ferror(fp)) {
fclose(fp);
return -1;
}
fclose(fp);
return 0;
}
/* Global identity lives where XDG applications keep their configuration.
* The local key=value file remains .mesh/config, so existing repositories
* keep working without a migration. */
static int config_global_path(char*out, size_t cap) {
const char*xdg=getenv("XDG_CONFIG_HOME");
const char*home=getenv("HOME");
if(xdg&&xdg[0]=='/')
return mesh_path_join(out, cap, xdg,"mesh/config");
if(home&&home[0]=='/')
return mesh_path_join(out, cap, home,".config/mesh/config");
errno=ENOENT;
return -1;
}
static int config_path(const struct mesh_repo*repo, bool global,
char*out, size_t cap) {
if(global)
return config_global_path(out, cap);
if(!repo||repo->mesh_dir[0]=='\0') {
errno=EINVAL;
return -1;
}
return mesh_path_join(out, cap, repo->mesh_dir,"config");
}
static int config_ensure_parent(const char*path) {
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
slash[1]='\0';
else
*slash='\0';
return mesh_mkdir_p(dir);
}
int mesh_config_get(const struct mesh_repo*repo, bool global,
const char*key, char*out, size_t cap) {
char path[MESH_PATH_MAX];
char*buf=NULL;
size_t len=0, key_len;
bool found=false;
int rc=-1;
if(!out||cap==0||!mesh_config_key_valid(key)) {
errno=EINVAL;
return -1;
}
out[0]='\0';
if(config_path(repo, global, path, sizeof(path))<0)
return -1;
if(mesh_read_text(path,&buf,&len)<0) {
if(errno==ENOENT)
return 0;
return -1;
}
if(memchr(buf,'\0',len)!=NULL) {
free(buf);
errno=EINVAL;
return -1;
}
key_len=strlen(key);
const char*p=buf;
const char*end=buf+len;
while(p<end) {
const char*nl=memchr(p,'\n',(size_t)(end-p));
size_t line_len=nl?(size_t)(nl-p):(size_t)(end-p);
if(line_len>0&&p[line_len-1]=='\r')
line_len--;
if(line_len>=key_len+1&&memcmp(p,key,key_len)==0&&
p[key_len]=='=') {
const char*value=p+key_len+1;
size_t value_len=line_len-key_len-1;
while(value_len>0&&value[0]==' ') {
value++;
value_len--;
}
while(value_len>0&&value[value_len-1]==' ')
value_len--;
if(value_len>=cap) {
errno=ENAMETOOLONG;
free(buf);
return -1;
}
memcpy(out, value, value_len);
out[value_len]='\0';
found=true;
}
p=nl?nl+1:end;
}
free(buf);
rc=found?1:0;
return rc;
}
int mesh_config_set(const struct mesh_repo*repo, bool global,
const char*key, const char*value) {
char path[MESH_PATH_MAX], clean[MESH_EMAIL_MAX+1];
char*existing=NULL;
char*rewritten=NULL;
const char*p, *end;
size_t key_len, clean_len, existing_len=0, out_len=0, cap_size;
size_t limit;
bool replaced=false;
int lock_fd=-1;
int rc=-1;
if(!value||!mesh_config_key_valid(key)) {
errno=EINVAL;
return -1;
}
limit=strcmp(key,"user.name")==0?MESH_AUTHOR_MAX:MESH_EMAIL_MAX;
p=value;
while(*p==' ')
p++;
clean_len=strlen(p);
while(clean_len>0&&p[clean_len-1]==' ')
clean_len--;
if(clean_len==0||clean_len>limit) {
errno=EINVAL;
return -1;
}
memcpy(clean,p,clean_len);
clean[clean_len]='\0';
if((strcmp(key,"user.name")==0&&!mesh_author_name_valid(clean))||
(strcmp(key,"user.email")==0&&!mesh_email_valid(clean))) {
errno=EINVAL;
return -1;
}
if(!global) {
if(!repo||!repo->valid) {
errno=EINVAL;
return -1;
}
lock_fd=mesh_repo_lock(repo);
if(lock_fd<0)
return -1;
}
if(config_path(repo, global, path, sizeof(path))<0)
goto done;
if(config_ensure_parent(path)<0)
goto done;
if(mesh_read_text(path,&existing,&existing_len)<0) {
if(errno!=ENOENT)
goto done;
existing=NULL;
existing_len=0;
}
if(existing&&memchr(existing,'\0',existing_len)!=NULL) {
errno=EINVAL;
goto done;
}
key_len=strlen(key);
if(existing_len>SIZE_MAX-key_len-clean_len-64) {
errno=EOVERFLOW;
goto done;
}
cap_size=existing_len+key_len+clean_len+64;
rewritten=malloc(cap_size);
if(!rewritten)
goto done;
p=existing?existing:"";
end=p+existing_len;
while(p<end) {
const char*nl=memchr(p,'\n',(size_t)(end-p));
size_t line_len=nl?(size_t)(nl-p):(size_t)(end-p);
size_t content_len=line_len;
if(content_len>0&&p[content_len-1]=='\r')
content_len--;
bool match=content_len>=key_len+1&&
memcmp(p,key,key_len)==0&&p[key_len]=='=';
if(match) {
if(!replaced) {
if(out_len+key_len+clean_len+2>cap_size) {
errno=ENOSPC;
goto done;
}
memcpy(rewritten+out_len,key,key_len);
out_len+=key_len;
rewritten[out_len++]='=';
memcpy(rewritten+out_len,clean,clean_len);
out_len+=clean_len;
rewritten[out_len++]='\n';
replaced=true;
}
} else {
if(out_len+content_len+1>cap_size) {
errno=ENOSPC;
goto done;
}
memcpy(rewritten+out_len,p,content_len);
out_len+=content_len;
rewritten[out_len++]='\n';
}
p=nl?nl+1:end;
}
if(!replaced) {
if(out_len+key_len+clean_len+2>cap_size) {
errno=ENOSPC;
goto done;
}
memcpy(rewritten+out_len,key,key_len);
out_len+=key_len;
rewritten[out_len++]='=';
memcpy(rewritten+out_len,clean,clean_len);
out_len+=clean_len;
rewritten[out_len++]='\n';
}
rc=mesh_write_atomic(path,rewritten,out_len,0644);
done: {
int saved=errno;
if(lock_fd>=0)
close(lock_fd);
free(existing);
free(rewritten);
errno=saved;
}
return rc;
}
int mesh_config_list(const struct mesh_repo*repo, bool global, FILE*out) {
char path[MESH_PATH_MAX];
char*buf=NULL;
size_t len=0;
int rc=-1;
if(!out) {
errno=EINVAL;
return -1;
}
if(config_path(repo, global, path, sizeof(path))<0)
return -1;
if(mesh_read_text(path,&buf,&len)<0) {
if(errno==ENOENT)
return 0;
return -1;
}
if(memchr(buf,'\0',len)!=NULL) {
free(buf);
errno=EINVAL;
return -1;
}
const char*p=buf;
const char*end=buf+len;
while(p<end) {
const char*nl=memchr(p,'\n',(size_t)(end-p));
size_t line_len=nl?(size_t)(nl-p):(size_t)(end-p);
if(line_len>0&&p[line_len-1]=='\r')
line_len--;
if(line_len>0&&p[0]!='#'&&memchr(p,'=',line_len)!=NULL) {
if(fwrite(p,1,line_len,out)!=line_len||fputc('\n',out)==EOF)
goto done;
}
p=nl?nl+1:end;
}
rc=0;
done: {
int saved=errno;
free(buf);
errno=saved;
}
return rc;
}
int mesh_identity_load(struct mesh_repo*repo) {
char name[MESH_AUTHOR_MAX+1];
char email[MESH_EMAIL_MAX+1];
const char*env_email;
if(!repo) {
errno=EINVAL;
return -1;
}
repo->author_name[0]='\0';
repo->author_email[0]='\0';
if(mesh_config_get(NULL,true,"user.name",name,sizeof(name))>0&&
mesh_author_name_valid(name))
mesh_strlcpy(repo->author_name,sizeof(repo->author_name),name);
if(mesh_config_get(NULL,true,"user.email",email,sizeof(email))>0&&
mesh_email_valid(email))
mesh_strlcpy(repo->author_email,sizeof(repo->author_email),email);
if(mesh_config_get(repo,false,"user.name",name,sizeof(name))>0&&
mesh_author_name_valid(name))
mesh_strlcpy(repo->author_name,sizeof(repo->author_name),name);
if(mesh_config_get(repo,false,"user.email",email,sizeof(email))>0&&
mesh_email_valid(email))
mesh_strlcpy(repo->author_email,sizeof(repo->author_email),email);
if(repo->author_email[0]=='\0'&&
(env_email=getenv("EMAIL"))!=NULL&&mesh_email_valid(env_email))
mesh_strlcpy(repo->author_email,sizeof(repo->author_email),env_email);
return 0;
}
int mesh_repo_open(struct mesh_repo*repo) {
char cwd[MESH_PATH_MAX];
char probe[MESH_PATH_MAX];
struct stat st;
if(!repo) {
errno=EINVAL;
return -1;
}
memset(repo, 0, sizeof(*repo));
if(!getcwd(cwd, sizeof(cwd)))
return -1;
for(;;) {
int n=snprintf(probe, sizeof(probe),"%s/.mesh", cwd);
if(n<0||(size_t)n>=sizeof(probe)) {
errno=ENAMETOOLONG;
return -1;
}
if(lstat(probe,&st)==0&&S_ISDIR(st.st_mode)) {
char config[MESH_PATH_MAX];
if(mesh_path_join(config, sizeof(config), probe,
"config")<0) {
return -1;
}
if(lstat(config,&st)==0&&S_ISREG(st.st_mode)) {
snprintf(repo->root, sizeof(repo->root),"%s",
cwd);
snprintf(repo->mesh_dir,
sizeof(repo->mesh_dir),"%s", probe);
if(mesh_config_branch(repo, repo->branch,
sizeof(repo->branch))<0)
return -1;
if(mesh_identity_load(repo)<0)
return -1;
repo->valid=true;
return 0;
}
}
if(strcmp(cwd,"/")==0)
break;
char*slash=strrchr(cwd,'/');
if(!slash)
break;
if(slash==cwd)
cwd[1]='\0';
else
*slash='\0';
}
errno=ENOENT;
return -1;
}
int mesh_repo_init(struct mesh_repo*repo) {
char cwd[MESH_PATH_MAX];
if(!repo) {
errno=EINVAL;
return -1;
}
if(mesh_repo_open(repo)==0)
return 1;
if(!getcwd(cwd, sizeof(cwd)))
return -1;
return mesh_repo_init_at(repo, cwd);
}
int mesh_repo_init_at(struct mesh_repo*repo, const char*root) {
static const char*dirs[]={
"","store","store/objects","store/trees","store/conflicts",
"store/remote","store/tmp","monitor","refs","refs/tags","logs"
};
static const char config[]=
"version="MESH_VERSION "\n"
"branch=main\n"
"icon="MESH_ICON "\n";
char cwd[MESH_PATH_MAX], path[MESH_PATH_MAX];
struct stat st;
if(!repo||!root||root[0]=='\0') {
errno=EINVAL;
return -1;
}
memset(repo, 0, sizeof(*repo));
if(root[0]=='/') {
if(strlen(root)>=sizeof(repo->root)) {
errno=ENAMETOOLONG;
return -1;
}
snprintf(repo->root, sizeof(repo->root),"%s", root);
} else {
if(!getcwd(cwd, sizeof(cwd)))
return -1;
if(mesh_path_join(repo->root, sizeof(repo->root), cwd, root)<0)
return -1;
}
if(mesh_path_join(repo->mesh_dir, sizeof(repo->mesh_dir),
repo->root,".mesh")<0)
return -1;
if(mesh_mkdir_one(repo->mesh_dir)<0)
return -1;
if(lstat(repo->mesh_dir,&st)<0||!S_ISDIR(st.st_mode)) {
errno=ENOTDIR;
return -1;
}
for(size_t i=0;i<sizeof(dirs)/sizeof(dirs[0]);i++) {
if(mesh_path_join(path, sizeof(path), repo->mesh_dir,
dirs[i])<0)
return -1;
if(mesh_mkdir_p(path)<0)
return -1;
}
if(mesh_path_join(path, sizeof(path), repo->mesh_dir,"config")<0)
return -1;
if(write_if_missing(path, config, sizeof(config)-1, 0644)<0)
return -1;
if(mesh_path_join(path, sizeof(path), repo->mesh_dir,"HEAD")<0)
return -1;
if(write_if_missing(path,"", 0, 0644)<0)
return -1;
if(mesh_path_join(path, sizeof(path), repo->mesh_dir,"history")<0)
return -1;
if(write_if_missing(path,"", 0, 0644)<0)
return -1;
if(mesh_path_join(path, sizeof(path), repo->mesh_dir,
"refs/main")<0)
return -1;
if(write_if_missing(path,"", 0, 0644)<0)
return -1;
snprintf(repo->branch, sizeof(repo->branch),"main");
if(mesh_identity_load(repo)<0)
return -1;
repo->valid=true;
return 0;
}
/* Shell integration */
static int append_block(const char*path, const char*block) {
char last='\0';
struct stat st;
size_t len=strlen(block);
int fd;
fd=open(path,O_RDWR|O_CREAT|O_APPEND|O_CLOEXEC|O_NOFOLLOW,0644);
if(fd<0)
return -1;
if(flock(fd,LOCK_EX)<0)
goto fail;
if(fstat(fd,&st)<0)
goto fail;
if(st.st_size>0&&pread(fd,&last,1,(off_t)st.st_size-1)<0)
goto fail;
if(st.st_size>0&&last!='\n'&&mesh_write_all(fd,"\n",1)<0)
goto fail;
if(mesh_write_all(fd,block,len)<0||fsync(fd)<0)
goto fail;
if(flock(fd,LOCK_UN)<0)
goto fail;
return close(fd);
fail: {
int saved=errno;
flock(fd,LOCK_UN);
close(fd);
errno=saved;
return -1;
}
}
/*
* Returns 1 when the block was written, 0 when the exact block was already
* present, -1 on a real I/O error.
* A stale block (same markers, different binary directory) is replaced
* instead of being left in front of the new PATH entry. The rewrite is
* atomic so an interrupted shell startup file update cannot truncate the
* user's file.
*/
static int inject_shell_file(const char*path, const char*bin_dir,
bool fish) {
char resolved[PATH_MAX];
char*existing=NULL;
size_t existing_len=0;
const char*begin;
const char*work=path;
struct stat link_st;
if(lstat(path,&link_st)==0) {
if(S_ISLNK(link_st.st_mode)) {
if(!realpath(path,resolved)) {
errno=ELOOP;
return -1;
}
work=resolved;
} else if(!S_ISREG(link_st.st_mode)) {
errno=EINVAL;
return -1;
}
} else if(errno!=ENOENT) {
return -1;
}
char block[4096];
char escaped[PATH_MAX*2];
size_t e=0;
int n;
for(const char*p=bin_dir;*p;p++) {
unsigned char c=(unsigned char)*p;
if(c<0x20||c==0x7f||c==':') {
errno=EINVAL;
return -1;
}
if(*p=='\\'||*p=='"'||*p=='$'||*p=='`') {
if(e+2>=sizeof(escaped)) {
errno=ENAMETOOLONG;
return -1;
}
escaped[e++]='\\';
}
if(e+1>=sizeof(escaped)) {
errno=ENAMETOOLONG;
return -1;
}
escaped[e++]=*p;
}
escaped[e]='\0';
if(fish) {
n=snprintf(block, sizeof(block),
"%s\n"
"# Managed by "MESH_NAME " "MESH_VERSION
" - remove this block to uninstall.\n"
"if not contains \"%s\" $PATH\n"
"    set -gx PATH \"%s\" $PATH\n"
"end\n"
"%s\n",
MESH_MARK_BEGIN, escaped, escaped, MESH_MARK_END);
} else {
n=snprintf(block, sizeof(block),
"%s\n"
"# Managed by "MESH_NAME " "MESH_VERSION
" - remove this block to uninstall.\n"
"case \":$PATH:\" in\n"
"    *\":%s:\"*) ;;\n"
"    *) PATH=\"%s:$PATH\" ;;\n"
"esac\n"
"export PATH\n"
"%s\n",
MESH_MARK_BEGIN, escaped, escaped, MESH_MARK_END);
}
if(n<0||(size_t)n>=sizeof(block)) {
errno=ENAMETOOLONG;
return -1;
}
if(mesh_read_text(work,&existing,&existing_len)<0) {
if(errno!=ENOENT)
return -1;
return append_block(work, block)==0?1:-1;
}
begin=strstr(existing, MESH_MARK_BEGIN);
if(begin&&strstr(begin, block)) {
free(existing);
return 0;
}
if(!begin) {
free(existing);
return append_block(work, block)==0?1:-1;
}
FILE*fp;
char*rewritten=NULL;
size_t rewritten_len=0;
const char*p=existing;
const char*b;
struct stat st;
mode_t mode=0644;
int rc;
fp=open_memstream(&rewritten,&rewritten_len);
if(!fp) {
free(existing);
return -1;
}
while((b=strstr(p, MESH_MARK_BEGIN))!=NULL) {
const char*end=strstr(b, MESH_MARK_END);
if(!end) {
fclose(fp);
free(rewritten);
free(existing);
errno=EINVAL;
return -1;
}
if(fwrite(p, 1, (size_t)(b-p), fp)!=
(size_t)(b-p)) {
fclose(fp);
free(rewritten);
free(existing);
errno=EIO;
return -1;
}
p=end+strlen(MESH_MARK_END);
if(*p=='\n')
p++;
}
if(fputs(p, fp)==EOF||fputs(block, fp)==EOF||
fflush(fp)==EOF||ferror(fp)) {
fclose(fp);
free(rewritten);
free(existing);
errno=EIO;
return -1;
}
if(fclose(fp)!=0) {
free(rewritten);
free(existing);
return -1;
}
if(stat(work,&st)==0)
mode=st.st_mode&0777;
rc=mesh_write_atomic(work, rewritten, rewritten_len,
mode);
free(rewritten);
free(existing);
return rc==0?1:-1;
}
int mesh_env_install(const struct mesh_repo*repo, const char*bin_dir,
unsigned*files_touched) {
char home[MESH_PATH_MAX], path[MESH_PATH_MAX], fish_dir[MESH_PATH_MAX];
char icon_src[MESH_PATH_MAX], icon_dst[MESH_PATH_MAX];
const char*home_env;
bool have_bash, have_zsh;
unsigned touched=0;
if(!repo||!repo->valid||!bin_dir||bin_dir[0]=='\0') {
errno=EINVAL;
return -1;
}
home_env=getenv("HOME");
if(!home_env||home_env[0]!='/') {
errno=ENOENT;
return -1;
}
if(snprintf(home, sizeof(home),"%s", home_env)>=(int)sizeof(home)) {
errno=ENAMETOOLONG;
return -1;
}
if(mesh_path_join(path, sizeof(path), home,".bashrc")<0)
return -1;
have_bash=access(path, F_OK)==0;
if(have_bash) {
int rc=inject_shell_file(path, bin_dir, false);
if(rc<0)
return -1;
touched+=(unsigned)rc;
}
if(mesh_path_join(path, sizeof(path), home,".zshrc")<0)
return -1;
have_zsh=access(path, F_OK)==0;
if(have_zsh) {
int rc=inject_shell_file(path, bin_dir, false);
if(rc<0)
return -1;
touched+=(unsigned)rc;
}
/* Neither file exists: leave the user a sane bash default. */
if(!have_bash&&!have_zsh) {
if(mesh_path_join(path, sizeof(path), home,".bashrc")<0)
return -1;
int rc=inject_shell_file(path, bin_dir, false);
if(rc<0)
return -1;
touched+=(unsigned)rc;
}
if(mesh_path_join(fish_dir, sizeof(fish_dir), home,
".config/fish")<0)
return -1;
if(mesh_path_join(path, sizeof(path), fish_dir,"config.fish")<0)
return -1;
if(access(path, F_OK)==0) {
int rc=inject_shell_file(path, bin_dir, true);
if(rc<0)
return -1;
touched+=(unsigned)rc;
}
/* Ship the icon next to the repository so the IDE manifest resolves. */
if(mesh_path_join(icon_src, sizeof(icon_src), bin_dir, MESH_ICON)==0&&
mesh_path_join(icon_dst, sizeof(icon_dst), repo->root,
MESH_ICON)==0) {
if(access(icon_src, R_OK)==0&&
access(icon_dst, F_OK)!=0&&
mesh_copy_path_to_path(icon_src, icon_dst, 0644)<0)
return -1;
}
if(files_touched)
*files_touched=touched;
return 0;
}
int mesh_write_vscode(const struct mesh_repo*repo) {
static const char settings[]=
"{\n"
"  \"files.associations\": {\n"
"    \"*.meshconfig\": \"json\",\n"
"    \"status.json\": \"json\"\n"
"  },\n"
"  \"workbench.iconTheme\": \"mesh-icons\",\n"
"  \"mesh.iconPath\": \"${workspaceFolder}/"MESH_ICON "\"\n"
"}\n";
char dir[MESH_PATH_MAX], path[MESH_PATH_MAX];
char*existing=NULL;
size_t len=0;
if(!repo||!repo->valid) {
errno=EINVAL;
return -1;
}
if(mesh_path_join(dir, sizeof(dir), repo->root,".vscode")<0)
return -1;
if(mesh_path_join(path, sizeof(path), dir,"settings.json")<0)
return -1;
if(mesh_read_text(path,&existing,&len)==0) {
bool managed=strstr(existing,"\"mesh.iconPath\"")!=NULL;
free(existing);
if(managed)
return 0;
/* Never destroy a user's hand-written editor settings. */
return 1;
}
if(errno!=ENOENT)
return -1;
if(mesh_mkdir_p(dir)<0)
return -1;
return mesh_write_atomic(path, settings, sizeof(settings)-1, 0644);
}
