/*
* main.c - process entry point.
* Keep it boring: ignore SIGPIPE so a dead peer is an error value, not a
* signal, bring libcurl up once, hand argv to the CLI and clean up.
*/
#include "core.h"
#include <signal.h>
int main(int argc, char**argv) {
int rc;
signal(SIGPIPE, SIG_IGN);
if(net_init()<0)
return 1;
rc=cli_main(argc, argv);
net_shutdown();
return rc;
}
