/*
* blake2b.c - BLAKE2b-512 (RFC 7693) plus a small thread pool for
* hashing files and byte ranges in parallel.
* The implementation is self contained on purpose: the kernel teaches
* that a build must not depend on a library that is only present on the
* distributor's favourite machine. Every context lives on the stack of
* its owner, so hashing is thread safe by construction.
*/
#include "core.h"
static const uint64_t blake2b_iv[8]={
0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL,
0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL
};
static const uint8_t blake2b_sigma[12][16]={
{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 },
{14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3 },
{11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4 },
{7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8 },
{9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13 },
{2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9 },
{12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11 },
{13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10 },
{6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5 },
{10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0 },
{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 },
{14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3 }
};
static inline uint64_t rotr64(uint64_t x, unsigned n) {
return (x>>n)|(x<<(64u-n));
}
static inline uint64_t load64(const uint8_t*p) {
return (uint64_t)p[0]|((uint64_t)p[1]<<8)|
((uint64_t)p[2]<<16)|((uint64_t)p[3]<<24)|
((uint64_t)p[4]<<32)|((uint64_t)p[5]<<40)|
((uint64_t)p[6]<<48)|((uint64_t)p[7]<<56);
}
static inline void store64(uint8_t*p, uint64_t v) {
p[0]=(uint8_t)v;
p[1]=(uint8_t)(v>>8);
p[2]=(uint8_t)(v>>16);
p[3]=(uint8_t)(v>>24);
p[4]=(uint8_t)(v>>32);
p[5]=(uint8_t)(v>>40);
p[6]=(uint8_t)(v>>48);
p[7]=(uint8_t)(v>>56);
}
#define MESH_G(r, i, a, b, c, d) \
do { \
a=a+b+m[blake2b_sigma[r][2*(i)+0]]; \
d=rotr64(d^a, 32); \
c=c+d; \
b=rotr64(b^c, 24); \
a=a+b+m[blake2b_sigma[r][2*(i)+1]]; \
d=rotr64(d^a, 16); \
c=c+d; \
b=rotr64(b^c, 63); \
} while(0)
#define MESH_ROUND(r) \
do { \
MESH_G(r, 0, v[0], v[4], v[8], v[12]); \
MESH_G(r, 1, v[1], v[5], v[9], v[13]); \
MESH_G(r, 2, v[2], v[6], v[10], v[14]); \
MESH_G(r, 3, v[3], v[7], v[11], v[15]); \
MESH_G(r, 4, v[0], v[5], v[10], v[15]); \
MESH_G(r, 5, v[1], v[6], v[11], v[12]); \
MESH_G(r, 6, v[2], v[7], v[8], v[13]); \
MESH_G(r, 7, v[3], v[4], v[9], v[14]); \
} while(0)
static void blake2b_compress(struct mesh_hash_ctx*ctx, const uint8_t block[128]) {
uint64_t m[16] = {0};
uint64_t v[16] = {0};
for(unsigned i=0;i<16;i++)
m[i]=load64(block+8*i);
for(unsigned i=0;i<8;i++) {
v[i]=ctx->h[i];
v[i+8]=blake2b_iv[i];
}
v[12]^=ctx->t[0];
v[13]^=ctx->t[1];
v[14]^=ctx->f[0];
v[15]^=ctx->f[1];
MESH_ROUND(0);
MESH_ROUND(1);
MESH_ROUND(2);
MESH_ROUND(3);
MESH_ROUND(4);
MESH_ROUND(5);
MESH_ROUND(6);
MESH_ROUND(7);
MESH_ROUND(8);
MESH_ROUND(9);
MESH_ROUND(10);
MESH_ROUND(11);
for(unsigned i=0;i<8;i++)
ctx->h[i]^=v[i]^v[i+8];
}
static void blake2b_init_ctx(struct mesh_hash_ctx*ctx, size_t outlen) {
memset(ctx, 0, sizeof(*ctx));
memcpy(ctx->h, blake2b_iv, sizeof(ctx->h));
/* Parameter block: digest length, key length 0, fanout 1, depth 1. */
ctx->h[0]^=0x01010000ULL^(uint64_t)outlen;
ctx->outlen=outlen;
}
static void blake2b_update_ctx(struct mesh_hash_ctx*ctx, const void*input,
size_t inlen) {
const uint8_t*in=input;
size_t left=ctx->buflen;
size_t fill=sizeof(ctx->buf)-left;
if(inlen==0)
return;
if(left>0&&inlen>fill) {
memcpy(ctx->buf+left, in, fill);
ctx->buflen=0;
ctx->t[0]+=sizeof(ctx->buf);
if(ctx->t[0]<sizeof(ctx->buf))
ctx->t[1]++;
blake2b_compress(ctx, ctx->buf);
in+=fill;
inlen-=fill;
left=0;
}
while(inlen>sizeof(ctx->buf)) {
ctx->t[0]+=sizeof(ctx->buf);
if(ctx->t[0]<sizeof(ctx->buf))
ctx->t[1]++;
blake2b_compress(ctx, in);
in+=sizeof(ctx->buf);
inlen-=sizeof(ctx->buf);
}
memcpy(ctx->buf+left, in, inlen);
ctx->buflen=left+inlen;
}
static void blake2b_final_ctx(struct mesh_hash_ctx*ctx,
uint8_t out[MESH_HASH_LEN]) {
uint8_t full[64];
ctx->t[0]+=ctx->buflen;
if(ctx->t[0]<ctx->buflen)
ctx->t[1]++;
ctx->f[0]=~(uint64_t)0;
memset(ctx->buf+ctx->buflen, 0, sizeof(ctx->buf)-ctx->buflen);
blake2b_compress(ctx, ctx->buf);
for(unsigned i=0;i<8;i++)
store64(full+8*i, ctx->h[i]);
memcpy(out, full, ctx->outlen);
memset(ctx, 0, sizeof(*ctx));
memset(full, 0, sizeof(full));
}
void blake2b_hash_buffer(const void*data, size_t len,
uint8_t out[MESH_HASH_LEN]) {
struct mesh_hash_ctx ctx;
if((!data&&len>0)||!out)
return;
blake2b_init_ctx(&ctx, MESH_HASH_LEN);
blake2b_update_ctx(&ctx, data, len);
blake2b_final_ctx(&ctx, out);
}
static void hash_to_hex(const uint8_t*hash, char hex[MESH_HASH_HEX+1]) {
static const char digits[]="0123456789abcdef";
for(size_t i=0;i<MESH_HASH_LEN;i++) {
hex[2*i]=digits[hash[i]>>4];
hex[2*i+1]=digits[hash[i]&0x0f];
}
hex[MESH_HASH_HEX]='\0';
}
void mesh_hash_init(struct mesh_hash_ctx*ctx) {
if(ctx)
blake2b_init_ctx(ctx, MESH_HASH_LEN);
}
void mesh_hash_update(struct mesh_hash_ctx*ctx, const void*data, size_t len) {
if(ctx)
blake2b_update_ctx(ctx, data, len);
}
void mesh_hash_final(struct mesh_hash_ctx*ctx, char hex[MESH_HASH_HEX+1]) {
uint8_t digest[MESH_HASH_LEN];
if(!ctx||!hex)
return;
blake2b_final_ctx(ctx, digest);
hash_to_hex(digest, hex);
memset(digest, 0, sizeof(digest));
}
int blake2b_hash_string(const char*s, char hex[MESH_HASH_HEX+1]) {
uint8_t digest[MESH_HASH_LEN];
if(!s||!hex) {
errno=EINVAL;
return -1;
}
blake2b_hash_buffer(s, strlen(s), digest);
hash_to_hex(digest, hex);
return 0;
}
int blake2b_hash_fd(int fd, uint64_t offset, uint64_t length,
char hex[MESH_HASH_HEX+1]) {
struct mesh_hash_ctx ctx;
uint8_t digest[MESH_HASH_LEN];
unsigned char*buf;
uint64_t left=length;
uint64_t pos=offset;
if(fd<0||!hex) {
errno=EINVAL;
return -1;
}
if(offset>(uint64_t)INT64_MAX||length>(uint64_t)INT64_MAX||
offset+length<offset||offset+length>(uint64_t)INT64_MAX) {
errno=EOVERFLOW;
return -1;
}
buf=malloc(MESH_IO_BUFFER);
if(!buf)
return -1;
blake2b_init_ctx(&ctx, MESH_HASH_LEN);
while(left>0) {
size_t want=left<MESH_IO_BUFFER?(size_t)left
:MESH_IO_BUFFER;
ssize_t r=pread(fd, buf, want, (off_t)pos);
if(r<0) {
if(errno==EINTR)
continue;
free(buf);
return -1;
}
if(r==0) {
free(buf);
errno=EIO;
return -1;
}
blake2b_update_ctx(&ctx, buf, (size_t)r);
pos+=(uint64_t)r;
left-=(uint64_t)r;
}
blake2b_final_ctx(&ctx, digest);
hash_to_hex(digest, hex);
free(buf);
return 0;
}

/*
* Hash a file and its chunk layout in one sequential pass over one
* descriptor. The full digest and every chunk digest therefore describe
* exactly the same bytes; a writer cannot swap the file between two
* independent hashing passes.
*/
int blake2b_hash_fd_layout(int fd, uint64_t size,
struct mesh_chunk*chunks, size_t nchunks,
char full_hex[MESH_HASH_HEX+1]) {
struct mesh_hash_ctx full, part;
uint8_t digest[MESH_HASH_LEN];
unsigned char*buf=NULL;
uint64_t pos=0, expected=0;
size_t ci=0;
int rc=-1;
if(fd<0||!full_hex||(nchunks>0&&!chunks)||
size>(uint64_t)INT64_MAX) {
errno=EINVAL;
return -1;
}
for(size_t i=0;i<nchunks;i++) {
if(!mesh_hash_valid(chunks[i].hash)&&chunks[i].hash[0]!='\0')
return -1;
if(chunks[i].offset!=expected||chunks[i].length==0||
chunks[i].length>size-expected) {
errno=EINVAL;
return -1;
}
expected+=chunks[i].length;
}
if(nchunks>0&&expected!=size) {
errno=EINVAL;
return -1;
}
buf=malloc(MESH_IO_BUFFER);
if(!buf)
return -1;
blake2b_init_ctx(&full, MESH_HASH_LEN);
if(nchunks>0)
blake2b_init_ctx(&part, MESH_HASH_LEN);
while(pos<size) {
size_t want=size-pos<MESH_IO_BUFFER?(size_t)(size-pos)
:MESH_IO_BUFFER;
ssize_t r=pread(fd, buf, want, (off_t)pos);
if(r<0) {
if(errno==EINTR)
continue;
goto done;
}
if(r==0) {
errno=EIO;
goto done;
}
blake2b_update_ctx(&full, buf, (size_t)r);
size_t off=0;
while(off<(size_t)r&&nchunks>0) {
uint64_t left=chunks[ci].offset+chunks[ci].length-pos-off;
size_t take=(size_t)(left<(uint64_t)((size_t)r-off)?left:
(uint64_t)((size_t)r-off));
blake2b_update_ctx(&part, buf+off, take);
off+=take;
if(pos+off==chunks[ci].offset+chunks[ci].length) {
blake2b_final_ctx(&part, digest);
hash_to_hex(digest, chunks[ci].hash);
ci++;
if(ci<nchunks)
blake2b_init_ctx(&part, MESH_HASH_LEN);
}
}
pos+=(uint64_t)r;
}
if(pos!=size||ci!=nchunks) {
errno=EIO;
goto done;
}
blake2b_final_ctx(&full, digest);
hash_to_hex(digest, full_hex);
rc=0;
done: {
int saved=errno;
free(buf);
errno=saved;
}
return rc;
}

int blake2b_hash_file(const char*path, char hex[MESH_HASH_HEX+1]) {
struct stat st;
int fd, rc;
if(!path||!hex) {
errno=EINVAL;
return -1;
}
fd=open(path, O_RDONLY|O_NOFOLLOW);
if(fd<0)
return -1;
if(fstat(fd,&st)<0) {
close(fd);
return -1;
}
if(!S_ISREG(st.st_mode)||st.st_size<0) {
close(fd);
errno=EINVAL;
return -1;
}
rc=blake2b_hash_fd(fd, 0, (uint64_t)st.st_size, hex);
close(fd);
return rc;
}
unsigned mesh_threads(void) {
long n=sysconf(_SC_NPROCESSORS_ONLN);
if(n<1)
n=1;
if(n>(long)MESH_MAX_THREADS)
n=(long)MESH_MAX_THREADS;
return (unsigned)n;
}
struct hash_pool {
struct mesh_hash_job*jobs;
size_t count;
size_t next;
pthread_mutex_t lock;
};
static void*hash_worker(void*arg) {
struct hash_pool*pool=arg;
for(;;) {
size_t i;
int fd;
pthread_mutex_lock(&pool->lock);
i=pool->next;
if(i>=pool->count) {
pthread_mutex_unlock(&pool->lock);
break;
}
pool->next++;
pthread_mutex_unlock(&pool->lock);
pool->jobs[i].rc=-1;
fd=open(pool->jobs[i].path, O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
if(fd<0)
continue;
pool->jobs[i].rc=blake2b_hash_fd(fd, pool->jobs[i].offset,
pool->jobs[i].length,
pool->jobs[i].hex);
close(fd);
}
return NULL;
}
int blake2b_hash_jobs(struct mesh_hash_job*jobs, size_t count,
unsigned threads) {
struct hash_pool pool;
pthread_t tids[MESH_MAX_THREADS];
unsigned nthreads, started=0;
int failures=0;
if(!jobs||count==0)
return 0;
if(threads==0)
threads=mesh_threads();
if(threads>MESH_MAX_THREADS)
threads=MESH_MAX_THREADS;
nthreads=(count<threads)?(unsigned)count:threads;
if(nthreads==0)
nthreads=1;
pool.jobs=jobs;
pool.count=count;
pool.next=0;
if(pthread_mutex_init(&pool.lock, NULL)!=0) {
errno=EAGAIN;
return -1;
}
for(unsigned i=0;i<nthreads;i++) {
if(pthread_create(&tids[i], NULL, hash_worker,&pool)!=0)
break;
started++;
}
/* The calling thread pulls its weight as well. */
hash_worker(&pool);
for(unsigned i=0;i<started;i++)
pthread_join(tids[i], NULL);
pthread_mutex_destroy(&pool.lock);
for(size_t i=0;i<count;i++) {
if(jobs[i].rc!=0)
failures++;
}
if(failures>0) {
errno=EIO;
return -1;
}
return 0;
}
