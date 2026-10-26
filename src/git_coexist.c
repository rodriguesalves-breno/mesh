/*
* git_coexist.c - read-only detection of Git metadata beside Mesh metadata.
* Mesh is a companion tool: this module never invokes Git, changes .git, or
* treats Git's object database as Mesh storage.
*/
#include "core.h"
#include <sys/stat.h>
#include <unistd.h>

int git_coexist_report(const struct mesh_repo*repo) {
char path[MESH_PATH_MAX];
struct stat st;
int n;
if(!repo||!repo->valid||!repo->root[0]) {
errno=EINVAL;
return -1;
}
n=snprintf(path,sizeof(path),"%s/.git",repo->root);
if(n<0||(size_t)n>=sizeof(path)) {
errno=ENAMETOOLONG;
return -1;
}
if(lstat(path,&st)<0) {
if(errno==ENOENT) {
printf("Git repository: not detected at Mesh root.\n");
printf("Mesh metadata: %s/.mesh (unchanged)\n",repo->root);
return 0;
}
return -1;
}
if(S_ISDIR(st.st_mode)) {
printf("Git repository: detected (.git directory; metadata not opened).\n");
} else if(S_ISREG(st.st_mode)) {
/* Git worktrees and submodules commonly use a .git pointer file. */
int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
char line[256];
ssize_t got;
if(fd<0)
return -1;
got=read(fd,line,sizeof(line)-1);
int saved=errno;
close(fd);
errno=saved;
if(got<0)
return -1;
line[got]='\0';
if(got>=7&&memcmp(line,"gitdir:",7)==0)
printf("Git repository: detected (.git pointer file; target not followed).\n");
else
printf("Git metadata: .git is a regular file, but not a recognized pointer; left untouched.\n");
} else if(S_ISLNK(st.st_mode)) {
printf("Git metadata: .git is a symlink; not followed or modified.\n");
} else {
printf("Git metadata: .git has an unsupported filesystem type; left untouched.\n");
}
printf("Mesh metadata: %s/.mesh (independent store).\n",repo->root);
printf("Coexistence: Mesh operations do not modify Git refs, index, objects, or configuration.\n");
return 0;
}
