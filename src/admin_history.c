/*
* admin_history.c - validate the append-only history and the local reflog.
* History records are content-addressed, so a parse failure is a hard error.
* The reflog is local navigation state and may be empty without failing.
*/
#include "core.h"
static size_t pipe_count(const char*line) {
size_t count=0;
for(const char*p=line;*p;p++)
if(*p=='|')
count++;
return count;
}
static int check_history_text(const struct mesh_repo*repo,size_t*snapshots,
size_t*failures) {
char path[MESH_PATH_MAX];
char*buf=NULL;
char*save=NULL;
char*line;
size_t len=0;
if(mesh_path_join(path, sizeof(path), repo->mesh_dir,"history")<0)
return -1;
if(mesh_read_text(path,&buf,&len)<0) {
if(errno==ENOENT)
return 0;
return -1;
}
for(line=strtok_r(buf,"\n",&save);line;
line=strtok_r(NULL,"\n",&save)) {
size_t pipes;
if(line[0]=='\0')
continue;
(*snapshots)++;
pipes=pipe_count(line);
if(pipes==6||pipes==8)
continue;
printf("FAIL history line %zu has %zu fields\n", *snapshots,
pipes+1);
(*failures)++;
}
free(buf);
return 0;
}
static void check_reflog_line(char*line,size_t number,size_t*failures) {
char*field[5];
char*p=line;
char*end=NULL;
long long timestamp;
for(size_t i=0;i<4;i++) {
char*sep=strchr(p,'|');
if(!sep) {
printf("FAIL reflog line %zu is malformed\n", number);
(*failures)++;
return;
}
*sep='\0';
field[i]=p;
p=sep+1;
}
field[4]=p;
if(strchr(field[4],'|')) {
printf("FAIL reflog line %zu has extra fields\n", number);
(*failures)++;
return;
}
errno=0;
timestamp=strtoll(field[0],&end, 10);
if(errno!=0||end==field[0]||!end||*end!='\0'||
(field[1][0]&&!mesh_hash_valid(field[1]))||
(field[2][0]&&!mesh_hash_valid(field[2]))) {
printf("FAIL reflog line %zu is invalid\n", number);
(*failures)++;
}
(void)timestamp;
}
static int check_reflog(const struct mesh_repo*repo,size_t*lines,
size_t*failures) {
char path[MESH_PATH_MAX];
char*buf=NULL;
char*save=NULL;
char*line;
size_t len=0;
if(mesh_path_join(path, sizeof(path), repo->mesh_dir,
"logs/HEAD")<0)
return -1;
if(mesh_read_text(path,&buf,&len)<0) {
if(errno==ENOENT)
return 0;
return -1;
}
for(line=strtok_r(buf,"\n",&save);line;
line=strtok_r(NULL,"\n",&save)) {
if(line[0]=='\0')
continue;
(*lines)++;
check_reflog_line(line, *lines, failures);
}
free(buf);
return 0;
}
int admin_history_run(const struct mesh_repo*repo) {
struct mesh_graph graph;
size_t history_lines=0, reflog_lines=0, failures=0;
int rc=1;
if(!repo||!repo->valid) {
errno=EINVAL;
return 1;
}
graph_init(&graph);
if(graph_load(repo,&graph)<0) {
fprintf(stderr,"mesh: admin history: cannot parse history: %s\n",
strerror(errno));
graph_free(&graph);
return 1;
}
if(check_history_text(repo,&history_lines,&failures)<0||
check_reflog(repo,&reflog_lines,&failures)<0) {
fprintf(stderr,"mesh: admin history: %s\n", strerror(errno));
goto done;
}
if(history_lines!=graph.count) {
printf("FAIL history line count does not match graph\n");
failures++;
}
printf("history: %zu snapshot(s)\n", graph.count);
printf("reflog:  %zu movement(s)\n", reflog_lines);
printf("failures: %zu\n", failures);
rc=failures?1:0;
done: {
int saved=errno;
graph_free(&graph);
errno=saved;
}
return rc;
}
