/*
* admin_config.c - validate the local key=value configuration.
* Mesh accepts unknown keys for forward compatibility, but a malformed
* version, branch, icon or identity is reported as a concrete failure.
*/
#include "core.h"
static bool icon_valid(const char*value) {
if(!value||!value[0]||strchr(value,'/')||strchr(value,'\\'))
return false;
for(const unsigned char*p=(const unsigned char*)value;*p;p++)
if(*p<0x20||*p==0x7f)
return false;
return true;
}
static void check_pair(const char*key,const char*value,size_t*errors,
size_t*warnings) {
if(strcmp(key,"version")==0) {
if(strcmp(value,"1.0")!=0) {
printf("FAIL version must be 1.0, got '%s'\n", value);
(*errors)++;
}
return;
}
if(strcmp(key,"branch")==0) {
if(!mesh_ref_name_valid(value)) {
printf("FAIL branch is invalid: '%s'\n", value);
(*errors)++;
}
return;
}
if(strcmp(key,"icon")==0) {
if(!icon_valid(value)) {
printf("FAIL icon is invalid: '%s'\n", value);
(*errors)++;
}
return;
}
if(strcmp(key,"user.name")==0) {
if(!mesh_author_name_valid(value)) {
printf("FAIL user.name is invalid\n");
(*errors)++;
}
return;
}
if(strcmp(key,"user.email")==0) {
if(!mesh_email_valid(value)) {
printf("FAIL user.email is invalid\n");
(*errors)++;
}
return;
}
printf("WARN unknown key: %s\n", key);
(*warnings)++;
}
int admin_config_run(const struct mesh_repo*repo) {
char path[MESH_PATH_MAX];
char*buf=NULL;
char*save=NULL;
char*line;
size_t len=0;
size_t errors=0, warnings=0, keys=0;
if(!repo||!repo->valid) {
errno=EINVAL;
return 1;
}
if(mesh_path_join(path, sizeof(path), repo->mesh_dir,"config")<0)
return 1;
if(mesh_read_text(path,&buf,&len)<0) {
fprintf(stderr,"mesh: admin config: cannot read config: %s\n",
strerror(errno));
return 1;
}
for(line=strtok_r(buf,"\n",&save);line;
line=strtok_r(NULL,"\n",&save)) {
char*eq;
if(line[0]=='\0'||line[0]=='#')
continue;
eq=strchr(line,'=');
if(!eq) {
printf("FAIL malformed line: %s\n", line);
errors++;
continue;
}
*eq='\0';
if(eq==line||eq[1]=='\0') {
printf("FAIL empty key or value: %s\n", line);
errors++;
continue;
}
keys++;
check_pair(line, eq+1,&errors,&warnings);
}
free(buf);
if(keys==0) {
printf("FAIL config is empty\n");
errors++;
}
printf("config: %zu key(s), %zu warning(s), %zu failure(s)\n", keys,
warnings, errors);
return errors?1:0;
}
