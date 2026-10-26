/*
* ignore.c - .meshignore parsing and path matching.
* A rule is evaluated against the relative path and every ancestor
* directory. That makes an ignored directory suppress its contents without
* forcing the scanner to materialise a second path list. Negated rules are
* evaluated in file order; the last matching rule wins.
*/
#include "core.h"
#define MESH_IGNORE_MAX_RULES 65536u
struct mesh_ignore_rule {
char*pattern;
bool negated;
bool dir_only;
bool anchored;
bool full_path;
};
struct mesh_ignore {
struct mesh_ignore_rule*v;
size_t n;
size_t cap;
bool has_negation;
};
static bool glob_match(const char*pat, const char*text);
static bool glob_match_star(const char*pat, const char*text) {
for(;;text++) {
if(*pat == *text && glob_match(pat, text))
return true;
if(*text=='\0'||*text=='/')
return false;
}
}
static bool glob_match_double(const char*pat, const char*text) {
if(*pat=='/') {
if(glob_match(pat+1,text))
return true;
pat++;
}
for(;;text++) {
if(glob_match(pat,text))
return true;
if(*text=='\0')
return false;
}
}
static bool glob_match(const char*pat, const char*text) {
while(*pat) {
if(*pat=='\\'&&pat[1]) {
pat++;
if(*text!=*pat)
return false;
pat++;
text++;
continue;
}
if(*pat=='*') {
if(pat[1]=='*')
return glob_match_double(pat+2,text);
return glob_match_star(pat+1,text);
}
if(*pat=='?') {
if(!*text||*text=='/')
return false;
pat++;
text++;
continue;
}
if(*text!=*pat)
return false;
pat++;
text++;
}
return *text=='\0';
}
static void ignore_rule_free(struct mesh_ignore_rule*rule) {
if(!rule)
return;
free(rule->pattern);
memset(rule, 0, sizeof(*rule));
}
void ignore_free(struct mesh_ignore*rules) {
if(!rules)
return;
for(size_t i=0;i<rules->n;i++)
ignore_rule_free(&rules->v[i]);
free(rules->v);
free(rules);
}
static int ignore_rule_add(struct mesh_ignore*rules, const char*pattern,
size_t len, bool negated, bool dir_only, bool anchored) {
struct mesh_ignore_rule*rule;
if(!rules||!pattern||len==0||len>=MESH_PATH_MAX) {
errno=EINVAL;
return -1;
}
if(rules->n>=MESH_IGNORE_MAX_RULES) {
errno=EFBIG;
return -1;
}
if(rules->n==rules->cap) {
size_t cap=rules->cap?rules->cap*2:16;
struct mesh_ignore_rule*v;
if(cap<rules->cap||cap>SIZE_MAX/sizeof(*v)) {
errno=ENOMEM;
return -1;
}
v=realloc(rules->v, cap*sizeof(*v));
if(!v)
return -1;
rules->v=v;
rules->cap=cap;
}
rule=&rules->v[rules->n];
memset(rule, 0, sizeof(*rule));
rule->pattern=malloc(len+1);
if(!rule->pattern)
return -1;
memcpy(rule->pattern, pattern, len);
rule->pattern[len]='\0';
rule->negated=negated;
rule->dir_only=dir_only;
rule->anchored=anchored;
rule->full_path=anchored||strchr(rule->pattern,'/')!=NULL;
rules->n++;
if(negated)
rules->has_negation=true;
return 0;
}
int ignore_load(const struct mesh_repo*repo, struct mesh_ignore**out) {
char path[MESH_PATH_MAX];
char*buf=NULL;
size_t len=0;
struct mesh_ignore*rules=NULL;
const char*p, *end;
int rc=-1;
if(!repo||!repo->valid||!out) {
errno=EINVAL;
return -1;
}
*out=NULL;
if(mesh_path_join(path, sizeof(path), repo->root,".meshignore")<0)
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
rules=calloc(1, sizeof(*rules));
if(!rules) {
free(buf);
return -1;
}
p=buf;
end=buf+len;
while(p<end) {
const char*nl=memchr(p,'\n',(size_t)(end-p));
size_t line_len=nl?(size_t)(nl-p):(size_t)(end-p);
const char*line=p;
size_t n=line_len;
bool negated=false, dir_only=false, anchored=false;
if(n>0&&line[n-1]=='\r')
n--;
while(n>0&&(line[0]==' '||line[0]=='\t')) {
line++;
n--;
}
while(n>0&&(line[n-1]==' '||line[n-1]=='\t'))
n--;
p=nl?nl+1:end;
if(n==0||line[0]=='#')
continue;
if(line[0]=='\\'&&n>1&&(line[1]=='#'||line[1]=='!')) {
line++;
n--;
} else if(line[0]=='!') {
negated=true;
line++;
n--;
}
if(n==0)
continue;
if(line[n-1]=='/') {
dir_only=true;
n--;
}
if(n>0&&line[0]=='/') {
anchored=true;
line++;
n--;
}
if(n==0)
continue;
if(n>=MESH_PATH_MAX) {
errno=ENAMETOOLONG;
goto done;
}
if(ignore_rule_add(rules, line, n, negated, dir_only, anchored)<0)
goto done;
}
if(rules->n==0) {
ignore_free(rules);
rules=NULL;
}
*out=rules;
rc=0;
done: {
int saved=errno;
free(buf);
if(rc!=0)
ignore_free(rules);
errno=saved;
}
return rc;
}
static bool ignore_rule_matches(const struct mesh_ignore_rule*rule,
const char*path, bool is_dir) {
const char*base;
if(!rule||!path||!*path)
return false;
if(rule->dir_only&&!is_dir)
return false;
if(rule->full_path)
return glob_match(rule->pattern,path);
base=strrchr(path,'/');
return glob_match(rule->pattern,base?base+1:path);
}
static bool ignore_rule_hits(const struct mesh_ignore_rule*rule,
const char*rel, bool is_dir) {
char buf[MESH_PATH_MAX];
size_t len;
if(!rule||!rel)
return false;
len=strlen(rel);
if(len==0||len>=sizeof(buf))
return false;
memcpy(buf, rel, len+1);
if(ignore_rule_matches(rule,buf,is_dir))
return true;
for(char*p=buf;*p;p++) {
if(*p!='/')
continue;
*p='\0';
if(ignore_rule_matches(rule,buf,true)) {
*p='/';
return true;
}
*p='/';
}
return false;
}
bool ignore_match(const struct mesh_ignore*rules, const char*rel,
bool is_dir) {
bool ignored=false;
if(!rules||!rel||rel[0]=='\0')
return false;
/* The rule file is repository metadata, not a candidate for its own
* patterns. It must remain available to explain the next snapshot. */
if(strcmp(rel,".meshignore")==0)
return false;
for(size_t i=0;i<rules->n;i++) {
if(ignore_rule_hits(&rules->v[i],rel,is_dir))
ignored=!rules->v[i].negated;
}
return ignored;
}
bool ignore_can_prune(const struct mesh_ignore*rules) {
return !rules||!rules->has_negation;
}
int ignore_list(const struct mesh_repo*repo, FILE*out) {
struct mesh_ignore*rules=NULL;
if(!repo||!out) {
errno=EINVAL;
return -1;
}
if(ignore_load(repo,&rules)<0)
return -1;
if(!rules||rules->n==0) {
fprintf(out,"No ignore rules.\n");
return 0;
}
fprintf(out,"Ignore rules (%zu):\n", rules->n);
for(size_t i=0;i<rules->n;i++) {
struct mesh_ignore_rule*rule=&rules->v[i];
fprintf(out,"  %s%s%s%s\n", rule->negated?"!":"",
rule->anchored?"/":"", rule->pattern,
rule->dir_only?"/":"");
}
ignore_free(rules);
return 0;
}
