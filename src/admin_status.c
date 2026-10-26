/*
* admin_status.c - rebuild the passive status document from live state.
* This is the repair path for a missing or stale status.json. It does not
* rescan the working tree; it reuses the same collector as the CLI commands.
*/
#include "core.h"
int admin_status_run(const struct mesh_repo*repo) {
struct mesh_status status;
if(!repo||!repo->valid) {
errno=EINVAL;
return 1;
}
if(monitor_collect(repo,&status,"admin")<0) {
fprintf(stderr,"mesh: admin status: cannot collect state: %s\n",
strerror(errno));
return 1;
}
mesh_strlcpy(status.message, sizeof(status.message),
"status refreshed by admin");
if(monitor_write(repo,&status)<0) {
fprintf(stderr,"mesh: admin status: cannot write state: %s\n",
strerror(errno));
return 1;
}
printf("status refreshed: %zu snapshot(s), %zu file(s), %zu conflict(s)\n",
status.snapshots, status.files, status.conflicts);
return 0;
}
