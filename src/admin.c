/*
* admin.c - command router for the repository administration plane.
* The admin commands are read-mostly, explicit and separately testable. They
* share the existing repository, graph, monitor and object-store contracts.
*/
#include "core.h"
static void admin_usage(FILE*out) {
fputs("Mesh admin - repository administration\n"
"\n"
"Usage:\n"
"  mesh admin <command> [options]\n"
"\n"
"Commands:\n"
"  modules              List the admin modules and their purpose.\n"
"  doctor               Check repository layout and metadata.\n"
"  verify               Verify every reachable object and manifest.\n"
"  inventory [--json]   Count objects, trees, refs and conflicts.\n"
"  gc [--apply]         List or remove unreachable objects.\n"
"  refs                 Validate branches, tags and HEAD.\n"
"  status               Refresh .mesh/monitor/status.json.\n"
"  audit [message]      Append an audit entry or list the log.\n"
"  quota [bytes]        Show or set the store byte quota.\n"
"  config               Validate .mesh/config values.\n"
"  history              Validate history and reflog records.\n"
"  export [file|-]      Export graph metadata as JSON.\n"
"  help                 Show this text.\n", out);
}
static int admin_repo(struct mesh_repo*repo) {
if(mesh_repo_open(repo)<0) {
fprintf(stderr,
"mesh: admin: not a Mesh repository (run 'mesh start' first)\n");
return -1;
}
return 0;
}
static int no_extra(int argc,int limit) {
if(argc<=limit)
return 0;
fprintf(stderr,"mesh: admin: too many arguments\n");
return -1;
}
static int admin_gc_command(int argc,char**argv,const struct mesh_repo*repo) {
bool apply=false;
if(argc>4)
return 2;
if(argc==4) {
if(strcmp(argv[3],"--apply")!=0) {
fprintf(stderr,"mesh: admin gc: expected --apply\n");
return 2;
}
apply=true;
}
return admin_gc_run(repo, apply);
}
static int admin_inventory_command(int argc,char**argv,
const struct mesh_repo*repo) {
bool json=false;
if(argc>4)
return 2;
if(argc==4) {
if(strcmp(argv[3],"--json")!=0) {
fprintf(stderr,"mesh: admin inventory: expected --json\n");
return 2;
}
json=true;
}
return admin_inventory_run(repo, json);
}
static int admin_dispatch(int argc,char**argv,const struct mesh_repo*repo) {
const char*command=argv[2];
if(strcmp(command,"doctor")==0)
return no_extra(argc, 3)==0?admin_doctor_run(repo):2;
if(strcmp(command,"verify")==0)
return no_extra(argc, 3)==0?admin_verify_run(repo):2;
if(strcmp(command,"inventory")==0)
return admin_inventory_command(argc,argv,repo);
if(strcmp(command,"gc")==0)
return admin_gc_command(argc,argv,repo);
if(strcmp(command,"refs")==0)
return no_extra(argc, 3)==0?admin_refs_run(repo):2;
if(strcmp(command,"status")==0)
return no_extra(argc, 3)==0?admin_status_run(repo):2;
if(strcmp(command,"audit")==0) {
if(argc>4)
return 2;
return admin_audit_run(repo, argc==4?argv[3]:NULL);
}
if(strcmp(command,"quota")==0) {
if(argc>4)
return 2;
return admin_quota_run(repo, argc==4?argv[3]:NULL);
}
if(strcmp(command,"config")==0)
return no_extra(argc, 3)==0?admin_config_run(repo):2;
if(strcmp(command,"history")==0)
return no_extra(argc, 3)==0?admin_history_run(repo):2;
if(strcmp(command,"export")==0) {
if(argc>4)
return 2;
return admin_export_run(repo, argc==4?argv[3]:NULL);
}
fprintf(stderr,"mesh: admin: unknown command '%s'\n", command);
admin_usage(stderr);
return 2;
}
int admin_main(int argc,char**argv) {
struct mesh_repo repo;
if(argc<3) {
admin_usage(stdout);
return 0;
}
if(strcmp(argv[2],"help")==0) {
admin_usage(stdout);
return 0;
}
if(strcmp(argv[2],"modules")==0) {
if(argc>3)
return 2;
return admin_registry_print(stdout)<0?1:0;
}
if(admin_repo(&repo)<0)
return 1;
return admin_dispatch(argc,argv,&repo);
}
