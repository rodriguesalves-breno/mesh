/*
* network.c - libcurl engine for downloads, JSON APIs and object
* transfer.
* Two rules shape this file:
* 1. A broken TCP stream must never cost the bytes already on disk.
* Large transfers resume with a Range request driven by stat().
* 2. JSON API responses live in memory. The disk is for user data,
* not for temporary HTTP bodies.
*/
#include "core.h"
#include <strings.h>
#define MESH_HTTP_MAX_BODY (64u*1024u*1024u)
static int auth_authority(const char*url, char*out, size_t cap) {
const char*start, *end, *at=NULL;
size_t len;
if(!url||!out||cap==0) {
errno=EINVAL;
return -1;
}
start=strstr(url,"://");
if(!start) {
errno=EINVAL;
return -1;
}
start+=3;
end=start;
while(*end&&*end!='/'&&*end!='?'&&*end!='#')
end++;
for(const char*p=start;p<end;p++)
if(*p=='@')
at=p;
if(at)
start=at+1;
len=(size_t)(end-start);
if(len==0||len>=cap) {
errno=EINVAL;
return -1;
}
for(size_t i=0;i<len;i++) {
unsigned char c=(unsigned char)start[i];
out[i]=(char)((c>='A'&&c<='Z')?c+32:c);
}
out[len]='\0';
return 0;
}
static int auth_token_ok(const char*token, size_t len) {
if(len==0||len>MESH_AUTH_MAX)
return 0;
for(size_t i=0;i<len;i++) {
unsigned char c=(unsigned char)token[i];
if(c<0x21||c>0x7e)
return 0;
}
return 1;
}
static int auth_transport_ok(const struct mesh_repo*repo, const char*url) {
char auth[MESH_AUTH_MAX+64];
int rc;
if(!repo||!repo->valid)
return 0;
rc=net_auth_header(repo, url, auth, sizeof(auth));
if(rc<0)
return -1;
if(rc==0&&strncmp(url,"https://", 8)!=0) {
errno=EPERM;
fprintf(stderr,"mesh: network: refusing to send credentials over HTTP\n");
return -1;
}
return 0;
}
int net_init(void) {
static bool ready;
CURLcode code;
if(ready)
return 0;
code=curl_global_init(CURL_GLOBAL_DEFAULT);
if(code!=CURLE_OK) {
fprintf(stderr,"mesh: network: %s\n", net_error(code));
return -1;
}
ready=true;
return 0;
}
void net_shutdown(void) {
curl_global_cleanup();
}
const char*net_error(CURLcode code) {
return curl_easy_strerror(code);
}
void mesh_http_init(struct mesh_http*resp) {
if(!resp)
return;
memset(resp, 0, sizeof(*resp));
}
void mesh_http_free(struct mesh_http*resp) {
if(!resp)
return;
free(resp->data);
memset(resp, 0, sizeof(*resp));
}
/*
* Credentials are one entry per line. The modern form is
* "host token"; a single "token" line is the legacy wildcard form. Host
* matching happens before any header is built so a token stored for one peer
* is never sent to another origin. URLs without an authority cannot match a
* scoped entry.
*/
int net_auth_header(const struct mesh_repo*repo, const char*url,
char*out, size_t cap) {
char path[MESH_PATH_MAX];
char authority[MESH_PATH_MAX];
char fallback[MESH_AUTH_MAX+1]="";
char*buf=NULL;
char*save=NULL;
char*line;
bool have_authority=false;
int n;
if(!out||cap==0) {
errno=EINVAL;
return -1;
}
out[0]='\0';
if(!repo||!repo->valid)
return 1;
if(url&&auth_authority(url,authority,sizeof(authority))<0)
return -1;
have_authority=url!=NULL;
if(mesh_path_join(path, sizeof(path), repo->mesh_dir,"credentials")<
0)
return -1;
if(mesh_read_text(path,&buf,NULL)<0) {
if(errno==ENOENT)
return 1;
return -1;
}
line=strtok_r(buf,"\n",&save);
while(line) {
char*host=line;
char*token;
char*sep;
mesh_trim_newline(line);
sep=strchr(line,' ');
if(sep) {
*sep='\0';
token=sep+1;
while(*token==' ')
token++;
} else {
host=(char*)"*";
token=line;
}
if(!*token||strchr(token,' ')) {
free(buf);
errno=EINVAL;
return -1;
}
if(!auth_token_ok(token,strlen(token))) {
free(buf);
errno=EINVAL;
return -1;
}
if(!have_authority||strcmp(host,"*")==0) {
if(!have_authority&&!fallback[0]) {
snprintf(fallback,sizeof(fallback),"%s",token);
}
if(have_authority)
snprintf(fallback,sizeof(fallback),"%s",token);
} else if(strcasecmp(host,authority)==0) {
n=snprintf(out,cap,"Authorization: Bearer %s",token);
free(buf);
if(n<0||(size_t)n>=cap) {
out[0]='\0';
errno=ENAMETOOLONG;
return -1;
}
return 0;
}
line=strtok_r(NULL,"\n",&save);
}
free(buf);
if(!fallback[0])
return 1;
n=snprintf(out,cap,"Authorization: Bearer %s",fallback);
if(n<0||(size_t)n>=cap) {
out[0]='\0';
errno=ENAMETOOLONG;
return -1;
}
return 0;
}
static int list_append(struct curl_slist**list, const char*value) {
struct curl_slist*next=curl_slist_append(*list, value);
if(!next) {
curl_slist_free_all(*list);
*list=NULL;
errno=ENOMEM;
return -1;
}
*list=next;
return 0;
}
static struct curl_slist*build_headers(const struct mesh_repo*repo,
const char*url, const char*const*extra,
bool json) {
struct curl_slist*list=NULL;
char auth[MESH_AUTH_MAX+64];
if(json&&list_append(&list,"Content-Type: application/json")<0)
return NULL;
/* Suppress the pointless "Expect: 100-continue" round trip. */
if(list_append(&list,"Expect:")<0)
return NULL;
if(repo&&repo->valid) {
int rc=net_auth_header(repo, url, auth, sizeof(auth));
if(rc<0) {
curl_slist_free_all(list);
return NULL;
}
if(rc==0&&list_append(&list, auth)<0)
return NULL;
}
if(extra) {
for(size_t i=0;extra[i];i++) {
if(list_append(&list, extra[i])<0)
return NULL;
}
}
return list;
}
static void report(CURLcode code) {
fprintf(stderr,"mesh: network: %s\n", curl_easy_strerror(code));
}
static void transfer_stats_fill(CURL*easy, struct mesh_transfer_stats*stats,
uint64_t fallback_bytes) {
long status=0, redirects=0;
double total=0, connect=0, start=0;
curl_off_t bytes=0;
char*text=NULL;
if(!stats)
return;
memset(stats, 0, sizeof(*stats));
if(!easy)
return;
(void)curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
(void)curl_easy_getinfo(easy, CURLINFO_REDIRECT_COUNT, &redirects);
(void)curl_easy_getinfo(easy, CURLINFO_TOTAL_TIME, &total);
(void)curl_easy_getinfo(easy, CURLINFO_CONNECT_TIME, &connect);
(void)curl_easy_getinfo(easy, CURLINFO_STARTTRANSFER_TIME, &start);
stats->response_code=status;
stats->redirects=redirects;
stats->time_total=total;
stats->time_connect=connect;
stats->time_starttransfer=start;
if(curl_easy_getinfo(easy, CURLINFO_SIZE_DOWNLOAD_T, &bytes)==CURLE_OK&&
bytes>=0)
stats->bytes=(uint64_t)bytes;
else
stats->bytes=fallback_bytes;
if(curl_easy_getinfo(easy, CURLINFO_EFFECTIVE_URL, &text)==CURLE_OK&&text)
mesh_strlcpy(stats->url_effective, sizeof(stats->url_effective), text);
if(curl_easy_getinfo(easy, CURLINFO_PRIMARY_IP, &text)==CURLE_OK&&text)
mesh_strlcpy(stats->remote_ip, sizeof(stats->remote_ip), text);
if(curl_easy_getinfo(easy, CURLINFO_CONTENT_TYPE, &text)==CURLE_OK&&text)
mesh_strlcpy(stats->content_type, sizeof(stats->content_type), text);
}
static long status_line_code(const char*buffer, size_t len) {
size_t i=0;
long code=0;
bool any=false;
if(len<6||strncmp(buffer, "HTTP/", 5)!=0)
return 0;
while(i<len&&buffer[i]!=' ')
i++;
i++;
while(i<len&&buffer[i]>='0'&&buffer[i]<='9') {
code=code*10+(buffer[i]-'0');
any=true;
i++;
}
return any?code:0;
}
static size_t memory_cb(char*ptr, size_t size, size_t nmemb, void*userdata) {
struct mesh_http*resp=userdata;
size_t n;
if(size!=0&&nmemb>SIZE_MAX/size)
return 0;
n=size*nmemb;
if(n==0)
return 0;
if(n>MESH_HTTP_MAX_BODY||resp->len>MESH_HTTP_MAX_BODY-n-1) {
resp->truncated=true;
return 0;
}
if(resp->len+n+1>resp->cap) {
size_t cap=resp->cap?resp->cap:4096;
char*data;
while(cap<resp->len+n+1) {
if(cap>MESH_HTTP_MAX_BODY/2) {
cap=MESH_HTTP_MAX_BODY;
break;
}
cap*=2;
}
data=realloc(resp->data, cap);
if(!data)
return 0;
resp->data=data;
resp->cap=cap;
}
memcpy(resp->data+resp->len, ptr, n);
resp->len+=n;
resp->data[resp->len]='\0';
return n;
}
static void header_value(const char*buf, size_t len, const char*name,
char*out, size_t cap) {
size_t nlen=strlen(name);
const char*p;
size_t o=0;
if(len<=nlen||strncasecmp(buf, name, nlen)!=0)
return;
p=buf+nlen;
if(*p!=':')
return;
p++;
while(p<buf+len&&(*p==' '||*p=='\t'))
p++;
while(p<buf+len&&*p!='\r'&&*p!='\n'&&o+1<cap)
out[o++]=*p++;
out[o]='\0';
}
static size_t header_cb(char*buffer, size_t size, size_t nitems, void*userdata) {
struct mesh_http*resp=userdata;
size_t len=size*nitems;
char cas[32]={0};
header_value(buffer, len,"X-Mesh-Hash", resp->remote_hash,
sizeof(resp->remote_hash));
header_value(buffer, len,"X-Mesh-Tree", resp->remote_tree,
sizeof(resp->remote_tree));
header_value(buffer,len,"ETag",resp->remote_etag,
sizeof(resp->remote_etag));
header_value(buffer,len,"X-Mesh-CAS",cas,sizeof(cas));
if(strcasecmp(cas,"if-match")==0)
resp->supports_cas=true;
return len;
}
static size_t sink_cb(char*ptr, size_t size, size_t nmemb, void*userdata) {
(void)ptr;
(void)userdata;
return size*nmemb;
}
static size_t file_cb(char*ptr, size_t size, size_t nmemb, void*userdata) {
if(size!=0&&nmemb>SIZE_MAX/size)
return 0;
return fwrite(ptr, 1, size*nmemb, userdata);
}
static bool header_text_ok(const char*text) {
if(!text)
return true;
for(const unsigned char*p=(const unsigned char*)text;*p;p++)
if(*p=='\r'||*p=='\n'||*p==0x7f||*p<0x20)
return false;
return true;
}
static bool extra_headers_ok(const char*const*headers) {
if(!headers)
return true;
for(size_t i=0;headers[i];i++) {
if(headers[i][0]=='\0'||!header_text_ok(headers[i]))
return false;
}
return true;
}
static int configure_transfer(CURL*easy, const char*url,
const struct mesh_download_options*options) {
const char*agent="Mesh/"MESH_VERSION;
const char*referer=NULL;
long connect=20L;
long max_time=0L;
uint64_t rate=0;
bool follow=false, compressed=false, insecure=false;
const char*cookie=NULL, *cookie_file=NULL, *cookie_jar=NULL;
const char*proxy=NULL, *proxy_user=NULL, *noproxy=NULL;
CURLcode code;
if(options) {
if(options->user_agent&&options->user_agent[0])
agent=options->user_agent;
referer=options->referer;
if(options->connect_timeout>0)
connect=options->connect_timeout;
max_time=options->max_time;
rate=options->max_recv_speed;
follow=options->follow_redirects;
compressed=options->compressed;
insecure=options->insecure;
cookie=options->cookie;
cookie_file=options->cookie_file;
cookie_jar=options->cookie_jar;
proxy=options->proxy;
proxy_user=options->proxy_user;
noproxy=options->noproxy;
}
if((code=curl_easy_setopt(easy, CURLOPT_URL, url))!=CURLE_OK||
(code=curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION,
follow?1L:0L))!=CURLE_OK||
(code=curl_easy_setopt(easy, CURLOPT_MAXREDIRS, 5L))!=CURLE_OK||
(code=curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L))!=CURLE_OK||
(code=curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, connect))!=
CURLE_OK||
(code=curl_easy_setopt(easy, CURLOPT_LOW_SPEED_LIMIT, 512L))!=
CURLE_OK||
(code=curl_easy_setopt(easy, CURLOPT_LOW_SPEED_TIME, 60L))!=
CURLE_OK||
(code=curl_easy_setopt(easy, CURLOPT_USERAGENT, agent))!=CURLE_OK) {
report(code);
return -1;
}
#if LIBCURL_VERSION_NUM >= 0x075500
if((code=curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR,
"http,https"))!=CURLE_OK||
(code=curl_easy_setopt(easy, CURLOPT_REDIR_PROTOCOLS_STR,
strncmp(url,"https://",8)==0?"https":"http,https"))!=CURLE_OK) {
report(code);
return -1;
}
#else
if((code=curl_easy_setopt(easy, CURLOPT_PROTOCOLS,
(long)(CURLPROTO_HTTP|CURLPROTO_HTTPS)))!=CURLE_OK||
(code=curl_easy_setopt(easy, CURLOPT_REDIR_PROTOCOLS,
(long)(strncmp(url,"https://",8)==0?CURLPROTO_HTTPS:
CURLPROTO_HTTP|CURLPROTO_HTTPS)))!=CURLE_OK) {
report(code);
return -1;
}
#endif
if(referer&&referer[0]&&
(code=curl_easy_setopt(easy, CURLOPT_REFERER, referer))!=CURLE_OK) {
report(code);
return -1;
}
if(max_time>0&&
(code=curl_easy_setopt(easy, CURLOPT_TIMEOUT, max_time))!=CURLE_OK) {
report(code);
return -1;
}
if(rate>0&&(code=curl_easy_setopt(easy, CURLOPT_MAX_RECV_SPEED_LARGE,
(curl_off_t)rate))!=CURLE_OK) {
report(code);
return -1;
}
if(compressed&&(code=curl_easy_setopt(easy, CURLOPT_ACCEPT_ENCODING,
""))!=CURLE_OK) {
report(code);
return -1;
}
if(cookie&&
(code=curl_easy_setopt(easy, CURLOPT_COOKIE, cookie))!=CURLE_OK) {
report(code);
return -1;
}
if(cookie_file&&
(code=curl_easy_setopt(easy, CURLOPT_COOKIEFILE, cookie_file))!=CURLE_OK) {
report(code);
return -1;
}
if(cookie_jar&&
(code=curl_easy_setopt(easy, CURLOPT_COOKIEJAR, cookie_jar))!=CURLE_OK) {
report(code);
return -1;
}
if(proxy&&
(code=curl_easy_setopt(easy, CURLOPT_PROXY, proxy))!=CURLE_OK) {
report(code);
return -1;
}
if(proxy_user&&
(code=curl_easy_setopt(easy, CURLOPT_PROXYUSERPWD, proxy_user))!=CURLE_OK) {
report(code);
return -1;
}
if(noproxy&&
(code=curl_easy_setopt(easy, CURLOPT_NOPROXY, noproxy))!=CURLE_OK) {
report(code);
return -1;
}
if(insecure&&
((code=curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 0L))!=CURLE_OK||
(code=curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 0L))!=CURLE_OK)) {
report(code);
return -1;
}
return 0;
}
static int transfer_options_ok(const struct mesh_download_options*options) {
if(!options)
return 0;
if(!extra_headers_ok(options->headers)||
!header_text_ok(options->user_agent)||
!header_text_ok(options->referer)||
!header_text_ok(options->cookie)||
!header_text_ok(options->cookie_file)||
!header_text_ok(options->cookie_jar)||
!header_text_ok(options->proxy)||
!header_text_ok(options->proxy_user)||
!header_text_ok(options->noproxy)||
(options->cookie_file&&options->cookie_file[0]=='\0')||
(options->cookie_jar&&options->cookie_jar[0]=='\0')) {
errno=EINVAL;
return -1;
}
return 0;
}
/*
* Resolve the offset used by a resumable transfer. Automatic resume trusts
* the existing file size. An explicit -C N truncates a longer file to N and
* refuses to extend a shorter one: claiming bytes the peer never sent would
* turn a partial download into a corrupt file.
*/
static int prepare_resume(const char*dest,
const struct mesh_download_options*options, uint64_t*resume) {
struct stat st;
*resume=0;
if(options&&options->resume_from_set) {
if(lstat(dest,&st)==0) {
if(!S_ISREG(st.st_mode)||st.st_size<0) {
errno=EINVAL;
return -1;
}
if((uint64_t)st.st_size<options->resume_from) {
fprintf(stderr,
"mesh: download: resume offset %"PRIu64
" exceeds the local file size\n",
options->resume_from);
errno=EINVAL;
return -1;
}
if((uint64_t)st.st_size>options->resume_from) {
int fd=open(dest, O_WRONLY|O_CLOEXEC|O_NOFOLLOW);
if(fd<0)
return -1;
if(ftruncate(fd,(off_t)options->resume_from)<0) {
int saved=errno;
close(fd);
errno=saved;
return -1;
}
if(close(fd)<0)
return -1;
}
*resume=options->resume_from;
return 0;
}
if(errno!=ENOENT)
return -1;
if(options->resume_from!=0) {
fprintf(stderr,
"mesh: download: cannot resume at %"PRIu64
" without a local file\n", options->resume_from);
errno=ENOENT;
return -1;
}
return 0;
}
if(lstat(dest,&st)==0) {
if(!S_ISREG(st.st_mode)||st.st_size<0) {
errno=EINVAL;
return -1;
}
if(st.st_size>0)
*resume=(uint64_t)st.st_size;
} else if(errno!=ENOENT) {
return -1;
}
return 0;
}
int net_download(const struct mesh_repo*repo, const char*url,
const char*dest, uint64_t*downloaded, uint64_t*resumed,
char hash[MESH_HASH_HEX+1]) {
return net_download_ex(repo, url, dest, NULL, downloaded, resumed, hash);
}
int net_download_ex(const struct mesh_repo*repo, const char*url,
const char*dest, const struct mesh_download_options*options,
uint64_t*downloaded, uint64_t*resumed,
char hash[MESH_HASH_HEX+1]) {
struct stat st;
uint64_t resume=0;
CURL*easy=NULL;
struct curl_slist*headers=NULL;
FILE*fp=NULL;
CURLcode code;
long status=0;
bool used_resume=false;
int attempt, rc=-1;
if(options&&options->stats)
memset(options->stats, 0, sizeof(*options->stats));
if(!mesh_http_url_ok(url)||!dest) {
errno=EINVAL;
return -1;
}
if(transfer_options_ok(options)<0)
return -1;
if(auth_transport_ok(repo, url)<0||mesh_path_no_symlink(dest)<0)
return -1;
if(downloaded)
*downloaded=0;
if(resumed)
*resumed=0;
if(hash)
hash[0]='\0';
if(prepare_resume(dest, options, &resume)<0)
return -1;
headers=build_headers(repo, url, options?options->headers:NULL, false);
if(!headers)
return -1;
for(attempt=0;attempt<2;attempt++) {
bool restart=(attempt>0);
int flags=O_WRONLY|O_CREAT|O_CLOEXEC|O_NOFOLLOW|
(restart?O_TRUNC:O_APPEND);
int dfd=open(dest,flags,0644);
if(dfd<0)
goto done;
fp=fdopen(dfd,restart?"wb":"ab");
if(!fp) {
int saved=errno;
close(dfd);
errno=saved;
goto done;
}
easy=curl_easy_init();
if(!easy) {
errno=ENOMEM;
goto done;
}
if(configure_transfer(easy, url, options)<0)
goto done;
if((code=curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers))!=
CURLE_OK) {
report(code);
goto done;
}
if((code=curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION,
file_cb))!=CURLE_OK) {
report(code);
goto done;
}
/* fwrite(3) is the callback: stable, simple, bounded memory. */
if((code=curl_easy_setopt(easy, CURLOPT_WRITEDATA, fp))!=
CURLE_OK) {
report(code);
goto done;
}
if((code=curl_easy_setopt(easy, CURLOPT_FAILONERROR, 0L))!=
CURLE_OK) {
report(code);
goto done;
}
if(!restart&&resume>0) {
if((code=curl_easy_setopt(easy,
CURLOPT_RESUME_FROM_LARGE,
(curl_off_t)resume))!=
CURLE_OK) {
report(code);
goto done;
}
}
status=0;
code=curl_easy_perform(easy);
(void)curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE,&status);
if(status==416&&!restart&&resume>0) {
/* Retry from zero: 416 alone does not prove the local file is complete. */
if(fclose(fp)!=0) {
fp=NULL;
goto done;
}
fp=NULL;
if(unlink(dest)<0&&errno!=ENOENT)
goto done;
curl_easy_cleanup(easy);
easy=NULL;
resume=0;
continue;
}
if(code!=CURLE_OK) {
if(fp) {
fclose(fp);
fp=NULL;
}
if(!restart&&resume>0&&code==CURLE_RANGE_ERROR) {
fprintf(stderr,
"mesh: download: server ignored the range, "
"restarting the transfer\n");
curl_easy_cleanup(easy);
easy=NULL;
resume=0;
continue;
}
report(code);
goto done;
}
if(status<200||status>=300) {
/*
* An error response is not a resumable prefix. Drop the bytes
* it produced, or rewind to the offset that was requested.
*/
if(fp) {
int dfd=fileno(fp);
if(dfd>=0&&resume>0)
if (ftruncate(dfd, (off_t)resume) == -1) {}
fclose(fp);
fp=NULL;
}
if(resume==0)
(void)unlink(dest);
fprintf(stderr,"mesh: download: HTTP %ld\n", status);
goto done;
}
/*
* Some servers ignore Range and answer 200 with
* the whole body. Never append that to a partial
* file: restart cleanly.
*/
if(!restart&&resume>0&&status==200) {
fprintf(stderr,
"mesh: download: server ignored the range, "
"restarting the transfer\n");
if(fclose(fp)!=0) {
fp=NULL;
goto done;
}
fp=NULL;
curl_easy_cleanup(easy);
easy=NULL;
resume=0;
used_resume=false;
continue;
}
if(fflush(fp)==EOF) {
perror("mesh: download");
goto done;
}
if(fsync(fileno(fp))<0) {
perror("mesh: download");
goto done;
}
if(fclose(fp)!=0) {
fp=NULL;
perror("mesh: download");
goto done;
}
fp=NULL;
used_resume=!restart&&resume>0;
rc=0;
break;
}
if(rc==0) {
if(lstat(dest,&st)<0||!S_ISREG(st.st_mode)) {
errno=EIO;
rc=-1;
goto done;
}
if(downloaded)
*downloaded=(uint64_t)st.st_size;
if(resumed)
*resumed=used_resume?resume:0;
if(hash&&blake2b_hash_file(dest, hash)<0)
rc=-1;
}
done: {
int saved=errno;
if(options&&options->stats)
transfer_stats_fill(easy, options->stats,
downloaded?*downloaded:0);
if(fp)
fclose(fp);
if(easy)
curl_easy_cleanup(easy);
curl_slist_free_all(headers);
errno=saved;
}
return rc;
}
struct stream_state {
struct mesh_hash_ctx hash;
long status;
bool discard;
uint64_t bytes;
};
static size_t stream_header_cb(char*buffer, size_t size, size_t nitems,
void*userdata) {
struct stream_state*state=userdata;
size_t len=size*nitems;
long code=status_line_code(buffer, len);
if(code>0) {
state->status=code;
/* Mesh never keeps an HTTP error body. A redirect is only an
* intermediate response; the final 2xx clears the flag. */
state->discard=(code<200||code>=300);
}
return len;
}
static size_t stream_cb(char*ptr, size_t size, size_t nmemb, void*userdata) {
struct stream_state*state=userdata;
size_t n;
if(size!=0&&nmemb>SIZE_MAX/size)
return 0;
n=size*nmemb;
if(n==0)
return 0;
if(state->discard)
return n;
if(fwrite(ptr, 1, n, stdout)!=n)
return 0;
mesh_hash_update(&state->hash, ptr, n);
state->bytes+=n;
return n;
}
int net_stream_ex(const struct mesh_repo*repo, const char*url,
const struct mesh_download_options*options,
uint64_t*downloaded, char hash[MESH_HASH_HEX+1]) {
struct stream_state state;
CURL*easy=NULL;
struct curl_slist*headers=NULL;
CURLcode code;
long status=0;
int rc=-1;
if(options&&options->stats)
memset(options->stats, 0, sizeof(*options->stats));
if(!mesh_http_url_ok(url)) {
errno=EINVAL;
return -1;
}
if(transfer_options_ok(options)<0)
return -1;
if(auth_transport_ok(repo, url)<0)
return -1;
if(downloaded)
*downloaded=0;
if(hash)
hash[0]='\0';
memset(&state, 0, sizeof(state));
mesh_hash_init(&state.hash);
headers=build_headers(repo, url, options?options->headers:NULL, false);
if(!headers)
return -1;
easy=curl_easy_init();
if(!easy) {
errno=ENOMEM;
goto done;
}
if(configure_transfer(easy, url, options)<0)
goto done;
if((code=curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers))!=
CURLE_OK||
(code=curl_easy_setopt(easy, CURLOPT_HTTPGET, 1L))!=CURLE_OK||
(code=curl_easy_setopt(easy, CURLOPT_FAILONERROR, 0L))!=CURLE_OK||
(code=curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, stream_cb))!=
CURLE_OK||
(code=curl_easy_setopt(easy, CURLOPT_WRITEDATA, &state))!=CURLE_OK||
(code=curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION,
stream_header_cb))!=CURLE_OK||
(code=curl_easy_setopt(easy, CURLOPT_HEADERDATA, &state))!=CURLE_OK) {
report(code);
goto done;
}
code=curl_easy_perform(easy);
(void)curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
if(code!=CURLE_OK) {
report(code);
goto done;
}
if(status==0)
status=state.status;
if(status<200||status>=300) {
fprintf(stderr,"mesh: download: HTTP %ld\n", status);
errno=EIO;
goto done;
}
if(fflush(stdout)==EOF) {
perror("mesh: download");
goto done;
}
if(hash)
mesh_hash_final(&state.hash, hash);
if(downloaded)
*downloaded=state.bytes;
rc=0;
done: {
int saved=errno;
if(options&&options->stats) {
transfer_stats_fill(easy, options->stats, state.bytes);
options->stats->bytes=state.bytes;
}
if(easy)
curl_easy_cleanup(easy);
curl_slist_free_all(headers);
errno=saved;
}
return rc;
}
static size_t head_header_cb(char*buffer, size_t size, size_t nitems,
void*userdata) {
FILE*out=userdata;
size_t len=size*nitems;
if(out&&fwrite(buffer, 1, len, out)!=len)
return 0;
return len;
}
int net_head_ex(const struct mesh_repo*repo, const char*url,
const struct mesh_download_options*options, FILE*out) {
CURL*easy=NULL;
struct curl_slist*headers=NULL;
CURLcode code;
long status=0;
int rc=-1;
if(options&&options->stats)
memset(options->stats, 0, sizeof(*options->stats));
if(!mesh_http_url_ok(url)) {
errno=EINVAL;
return -1;
}
if(!out)
out=stdout;
if(transfer_options_ok(options)<0)
return -1;
if(auth_transport_ok(repo, url)<0)
return -1;
headers=build_headers(repo, url, options?options->headers:NULL, false);
if(!headers)
return -1;
easy=curl_easy_init();
if(!easy) {
errno=ENOMEM;
goto done;
}
if(configure_transfer(easy, url, options)<0)
goto done;
if((code=curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers))!=
CURLE_OK||
(code=curl_easy_setopt(easy, CURLOPT_NOBODY, 1L))!=CURLE_OK||
(code=curl_easy_setopt(easy, CURLOPT_FAILONERROR, 0L))!=CURLE_OK||
(code=curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, sink_cb))!=
CURLE_OK||
(code=curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, head_header_cb))!=
CURLE_OK||
(code=curl_easy_setopt(easy, CURLOPT_HEADERDATA, out))!=CURLE_OK) {
report(code);
goto done;
}
code=curl_easy_perform(easy);
(void)curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
if(code!=CURLE_OK) {
report(code);
goto done;
}
if(fflush(out)==EOF) {
perror("mesh: download");
goto done;
}
if(options&&options->fail&&(status<200||status>=300)) {
fprintf(stderr,"mesh: download: HTTP %ld\n", status);
errno=EIO;
goto done;
}
rc=0;
done: {
int saved=errno;
if(options&&options->stats) {
transfer_stats_fill(easy, options->stats, 0);
options->stats->bytes=0;
}
if(easy)
curl_easy_cleanup(easy);
curl_slist_free_all(headers);
errno=saved;
}
return rc;
}
int net_get(const struct mesh_repo*repo, const char*url,
const char*const*extra, struct mesh_http*resp) {
CURL*easy=NULL;
struct curl_slist*headers=NULL;
CURLcode code;
long status=0;
int rc=-1;
if(!mesh_http_url_ok(url)||!resp) {
errno=EINVAL;
return -1;
}
if(auth_transport_ok(repo, url)<0)
return -1;
mesh_http_init(resp);
resp->data=calloc(1, 1);
if(!resp->data)
return -1;
resp->cap=1;
headers=build_headers(repo, url, extra, false);
if(!headers)
goto done;
easy=curl_easy_init();
if(!easy) {
errno=ENOMEM;
goto done;
}
if(configure_transfer(easy, url, NULL)<0)
goto done;
if((code=curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers))!=
CURLE_OK||
(code=curl_easy_setopt(easy, CURLOPT_HTTPGET, 1L))!=CURLE_OK||
(code=curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION,
memory_cb))!=CURLE_OK||
(code=curl_easy_setopt(easy, CURLOPT_WRITEDATA, resp))!=
CURLE_OK||
(code=curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION,
header_cb))!=CURLE_OK||
(code=curl_easy_setopt(easy, CURLOPT_HEADERDATA, resp))!=
CURLE_OK) {
report(code);
goto done;
}
code=curl_easy_perform(easy);
if(code!=CURLE_OK) {
report(code);
goto done;
}
(void)curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE,&status);
resp->status=status;
rc=0;
done: {
int saved=errno;
if(easy)
curl_easy_cleanup(easy);
curl_slist_free_all(headers);
if(rc!=0)
mesh_http_free(resp);
errno=saved;
}
return rc;
}
int net_post_json_if_match(const struct mesh_repo*repo,const char*url,
const char*json,const char*etag,struct mesh_http*resp) {
CURL*easy=NULL;
struct curl_slist*headers=NULL;
CURLcode code;
long status=0;
size_t payload;
char if_match_header[320];
const char*extra[2]={NULL,NULL};
int rc=-1;
if(!mesh_http_url_ok(url)||!json||!resp) {
errno=EINVAL;
return -1;
}
if(auth_transport_ok(repo, url)<0)
return -1;
if(etag) {
if(!etag[0]||strlen(etag)>=sizeof(if_match_header)-11||
!header_text_ok(etag)) {
errno=EINVAL;
return -1;
}
int n=snprintf(if_match_header,sizeof(if_match_header),"If-Match: %s",etag);
if(n<0||(size_t)n>=sizeof(if_match_header)) {
errno=ENAMETOOLONG;
return -1;
}
extra[0]=if_match_header;
}
payload=strlen(json);
mesh_http_init(resp);
resp->data=calloc(1, 1);
if(!resp->data)
return -1;
resp->cap=1;
headers=build_headers(repo, url, etag?extra:NULL, true);
if(!headers)
goto done;
easy=curl_easy_init();
if(!easy) {
errno=ENOMEM;
goto done;
}
if(configure_transfer(easy, url, NULL)<0)
goto done;
if((code=curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers))!=
CURLE_OK||
(code=curl_easy_setopt(easy, CURLOPT_POST, 1L))!=CURLE_OK||
(code=curl_easy_setopt(easy, CURLOPT_POSTFIELDS, json))!=
CURLE_OK||
(code=curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE_LARGE,
(curl_off_t)payload))!=CURLE_OK||
(code=curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION,
memory_cb))!=CURLE_OK||
(code=curl_easy_setopt(easy, CURLOPT_WRITEDATA, resp))!=
CURLE_OK||
(code=curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION,
header_cb))!=CURLE_OK||
(code=curl_easy_setopt(easy, CURLOPT_HEADERDATA, resp))!=
CURLE_OK) {
report(code);
goto done;
}
code=curl_easy_perform(easy);
if(code!=CURLE_OK) {
report(code);
goto done;
}
(void)curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE,&status);
resp->status=status;
rc=0;
done: {
int saved=errno;
if(easy)
curl_easy_cleanup(easy);
curl_slist_free_all(headers);
if(rc!=0)
mesh_http_free(resp);
errno=saved;
}
	return rc;
}
int net_post_json(const struct mesh_repo*repo,const char*url,
const char*json,struct mesh_http*resp) {
return net_post_json_if_match(repo,url,json,NULL,resp);
}
int net_put_file(const struct mesh_repo*repo, const char*url,
const char*src) {
CURL*easy=NULL;
struct curl_slist*headers=NULL;
struct stat st;
FILE*fp=NULL;
CURLcode code;
long status=0;
int rc=-1;
if(!mesh_http_url_ok(url)||!src) {
errno=EINVAL;
return -1;
}
if(auth_transport_ok(repo, url)<0)
return -1;
int sfd=open(src,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
if(sfd<0)
return -1;
fp=fdopen(sfd,"rb");
if(!fp) {
int saved=errno;
close(sfd);
errno=saved;
return -1;
}
if(fstat(fileno(fp),&st)<0)
goto done;
if(!S_ISREG(st.st_mode)||st.st_size<0) {
errno=EINVAL;
goto done;
}
headers=build_headers(repo, url, NULL, false);
if(!headers)
goto done;
if(list_append(&headers,"Content-Type: application/octet-stream")<0)
goto done;
easy=curl_easy_init();
if(!easy) {
errno=ENOMEM;
goto done;
}
if(configure_transfer(easy, url, NULL)<0)
goto done;
if((code=curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers))!=
CURLE_OK||
(code=curl_easy_setopt(easy, CURLOPT_UPLOAD, 1L))!=CURLE_OK||
(code=curl_easy_setopt(easy, CURLOPT_READDATA, fp))!=CURLE_OK||
(code=curl_easy_setopt(easy, CURLOPT_INFILESIZE_LARGE,
(curl_off_t)st.st_size))!=CURLE_OK||
(code=curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION,
sink_cb))!=CURLE_OK) {
report(code);
goto done;
}
code=curl_easy_perform(easy);
if(code!=CURLE_OK) {
report(code);
goto done;
}
(void)curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE,&status);
if(status<200||status>=300) {
fprintf(stderr,"mesh: put: HTTP %ld\n", status);
goto done;
}
rc=0;
done: {
int saved=errno;
if(easy)
curl_easy_cleanup(easy);
curl_slist_free_all(headers);
if(fp)
fclose(fp);
errno=saved;
}
return rc;
}
